#include "sfx.h"

#include <3ds.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace Sfx {

namespace {

constexpr int SOUND_COUNT = 4;

constexpr std::array<const char *, SOUND_COUNT> kSoundPaths = {
    "romfs:/sfx/back.wav",    // Sound::Back
    "romfs:/sfx/confirm.wav", // Sound::Confirm
    "romfs:/sfx/dpad.wav",    // Sound::Dpad
    "romfs:/sfx/tab.wav",     // Sound::Tab
};

constexpr const char *kBgmPath = "romfs:/bgm/bgm.wav";

// ndsp channels 0..kSfxChannelCount-1 rotate for short SFX (ndspChnReset()
// before each use so a new Play() always starts immediately rather than
// queuing behind whatever that channel played last); the channel right
// after them is reserved solely for BGM so the two never fight over the
// same channel state.
constexpr int kSfxChannelCount = 8;
constexpr int kBgmChannel = kSfxChannelCount;

// One decoded clip: interleaved native-endian PCM16 in a linearAlloc'd
// buffer (ndsp wave buffers must live in linear/physical memory), kept for
// the app's lifetime (SFX) or until playback finishes (BGM - see Update()).
struct Clip {
    int16_t *pcm = nullptr; // linearAlloc'd
    u32 frames = 0;         // sample-frames (per channel), not raw halfword count
    u32 sampleRate = 0;
    bool stereo = false;
};

std::array<Clip, SOUND_COUNT> g_clips;
Clip g_bgmClip;
bool g_initialized = false;
int g_nextSfxChannel = 0;

// One in-flight SFX ndspWaveBuf, tracked purely so Update()'s reaper can
// free it once ndsp reports it done - the DSP itself needs no further
// per-frame attention once a wave buffer is queued.
std::mutex g_activeMutex;
std::vector<ndspWaveBuf *> g_active;

ndspWaveBuf *g_bgmWaveBuf = nullptr;
bool g_bgmPlaying = false;

void ReapFinishedVoices() {
    for (size_t i = 0; i < g_active.size();) {
        if (g_active[i]->status == NDSP_WBUF_DONE) {
            delete g_active[i];
            g_active[i] = g_active.back();
            g_active.pop_back();
        } else {
            ++i;
        }
    }
}

// Reads a little-endian u16/u32 out of a byte buffer at `offset` - WAV's own
// on-disk byte order (RIFF is always little-endian), which conveniently
// matches ARM's native endianness, so the PCM sample data itself needs no
// byte-swapping at all (unlike the Wii U build's AX path).
uint16_t ReadU16LE(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t ReadU32LE(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Minimal RIFF/WAVE parser - just enough to find the "fmt " and "data"
// chunks of a plain, uncompressed PCM8 or PCM16 WAV file (there's no SDL2 on
// 3DS to lean on, unlike the Wii U build's SDL_LoadWAV()) - unrecognized
// chunks in between (e.g. a "LIST"/INFO metadata chunk, which some WAV
// exporters add) are skipped over, not treated as an error. PCM8 is
// upconverted to PCM16 below (ndsp only takes PCM16) since not every export
// tool defaults to 16-bit. Anything else (compressed formats, >2 channels,
// malformed chunks) is rejected, not guessed at.
void LoadClip(const char *path, Clip &outClip) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize < 44) {
        fclose(f);
        return;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
    size_t readBytes = fread(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    if (readBytes != bytes.size()) return;

    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return;

    uint16_t channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t *dataChunk = nullptr;
    uint32_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const uint8_t *chunkId = bytes.data() + pos;
        uint32_t chunkSize = ReadU32LE(bytes.data() + pos + 4);
        const uint8_t *chunkBody = bytes.data() + pos + 8;
        if (pos + 8 + chunkSize > bytes.size()) break;

        if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
            uint16_t audioFormat = ReadU16LE(chunkBody + 0);
            channels = ReadU16LE(chunkBody + 2);
            sampleRate = ReadU32LE(chunkBody + 4);
            bitsPerSample = ReadU16LE(chunkBody + 14);
            if (audioFormat != 1 /* PCM */) return; // WAVE_FORMAT_EXTENSIBLE etc. not supported
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataChunk = chunkBody;
            dataSize = chunkSize;
        }

        pos += 8 + chunkSize + (chunkSize & 1); // chunks are word-aligned
    }

    if (!dataChunk || (bitsPerSample != 16 && bitsPerSample != 8) || (channels != 1 && channels != 2) ||
        sampleRate == 0)
        return;

    uint32_t bytesPerSample = bitsPerSample / 8;
    uint32_t frames = dataSize / (bytesPerSample * channels);
    if (frames == 0) return;

    int16_t *pcm = static_cast<int16_t *>(linearAlloc(static_cast<size_t>(frames) * channels * sizeof(int16_t)));
    if (!pcm) return;
    if (bitsPerSample == 16) {
        std::memcpy(pcm, dataChunk, static_cast<size_t>(frames) * channels * sizeof(int16_t));
    } else {
        // 8-bit WAV samples are unsigned, centered at 128 (unlike 16-bit's
        // signed centered-at-0) - ndsp only takes PCM16, so each sample is
        // widened here: subtract the 128 bias, then scale the result up
        // into the 16-bit range.
        size_t sampleCount = static_cast<size_t>(frames) * channels;
        for (size_t i = 0; i < sampleCount; i++) {
            pcm[i] = static_cast<int16_t>((static_cast<int>(dataChunk[i]) - 128) * 256);
        }
    }
    DSP_FlushDataCache(pcm, static_cast<u32>(frames) * channels * sizeof(int16_t));

    outClip.pcm = pcm;
    outClip.frames = frames;
    outClip.sampleRate = sampleRate;
    outClip.stereo = channels == 2;
}

void FreeClip(Clip &clip) {
    if (clip.pcm) linearFree(clip.pcm);
    clip = Clip{};
}

} // namespace

bool Init() {
    if (g_initialized) return true;

    if (R_FAILED(ndspInit())) return false;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    for (int i = 0; i < SOUND_COUNT; i++) {
        LoadClip(kSoundPaths[static_cast<size_t>(i)], g_clips[static_cast<size_t>(i)]);
    }
    LoadClip(kBgmPath, g_bgmClip);

    g_initialized = true;

    bool anyLoaded = g_bgmClip.frames != 0;
    for (const Clip &clip : g_clips) anyLoaded = anyLoaded || clip.frames != 0;
    return anyLoaded;
}

void Shutdown() {
    if (!g_initialized) return;

    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        for (int ch = 0; ch < kSfxChannelCount; ch++) ndspChnWaveBufClear(ch);
        for (ndspWaveBuf *wb : g_active) delete wb;
        g_active.clear();

        ndspChnWaveBufClear(kBgmChannel);
        if (g_bgmWaveBuf) {
            delete g_bgmWaveBuf;
            g_bgmWaveBuf = nullptr;
        }
        g_bgmPlaying = false;
    }

    for (Clip &clip : g_clips) FreeClip(clip);
    FreeClip(g_bgmClip);

    ndspExit();
    g_initialized = false;
}

void Play(Sound sound) {
    if (!g_initialized) return;
    size_t idx = static_cast<size_t>(sound);
    if (idx >= SOUND_COUNT) return;
    const Clip &clip = g_clips[idx];
    if (clip.frames == 0) return;

    std::lock_guard<std::mutex> lock(g_activeMutex);
    ReapFinishedVoices();

    int channel = g_nextSfxChannel;
    g_nextSfxChannel = (g_nextSfxChannel + 1) % kSfxChannelCount;

    ndspChnReset(channel);
    ndspChnSetInterp(channel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(channel, static_cast<float>(clip.sampleRate));
    ndspChnSetFormat(channel, clip.stereo ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12] = {};
    mix[0] = mix[1] = 1.0f; // full volume, front left/right
    ndspChnSetMix(channel, mix);

    ndspWaveBuf *wb = new ndspWaveBuf{};
    wb->data_pcm16 = clip.pcm;
    wb->nsamples = clip.frames;
    wb->looping = false;
    ndspChnWaveBufAdd(channel, wb);

    g_active.push_back(wb);
}

void PlayBgm() {
    if (!g_initialized) return;
    if (g_bgmClip.frames == 0) return;

    std::lock_guard<std::mutex> lock(g_activeMutex);

    ndspChnReset(kBgmChannel);
    ndspChnSetInterp(kBgmChannel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(kBgmChannel, static_cast<float>(g_bgmClip.sampleRate));
    ndspChnSetFormat(kBgmChannel, g_bgmClip.stereo ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12] = {};
    mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(kBgmChannel, mix);

    g_bgmWaveBuf = new ndspWaveBuf{};
    g_bgmWaveBuf->data_pcm16 = g_bgmClip.pcm;
    g_bgmWaveBuf->nsamples = g_bgmClip.frames;
    g_bgmWaveBuf->looping = false;
    ndspChnWaveBufAdd(kBgmChannel, g_bgmWaveBuf);

    g_bgmPlaying = true;
}

void Update() {
    if (!g_bgmPlaying) return;

    std::lock_guard<std::mutex> lock(g_activeMutex);
    if (!g_bgmWaveBuf || g_bgmWaveBuf->status != NDSP_WBUF_DONE) return;

    delete g_bgmWaveBuf;
    g_bgmWaveBuf = nullptr;
    g_bgmPlaying = false;

    // The decoded PCM has done its one and only job - BGM never loops or
    // replays - so it's not worth keeping resident for the rest of the
    // session.
    FreeClip(g_bgmClip);
}

} // namespace Sfx
