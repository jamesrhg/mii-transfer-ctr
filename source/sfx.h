#pragma once

// Short UI sound effects, bundled as WAV under romfs:/sfx/, plus one longer
// BGM track under romfs:/bgm/. Played via libctru's ndsp (the 3DS's DSP
// audio service) - the 3DS equivalent of the Wii U build's direct Cafe AX
// driving (see that file's own comment for why it bypassed SDL2's audio
// subsystem; ndsp is the analogous "talk to the platform's real audio
// service directly" layer here, there being no SDL2 on 3DS at all).
//
// Unlike the Wii U build (which had to target the GamePad/TV separately and
// byte-swap PCM to big-endian for AX), the 3DS has one audio output and
// native little-endian PCM16, so this is considerably simpler: each clip is
// decoded once into a linearAlloc'd PCM16 buffer, and Play() fires it on a
// rotating pool of ndsp channels (see sfx.cpp).
namespace Sfx {

enum class Sound {
    Back,    // leaving the Mii details/server screen back to the list
    Confirm, // a Mii selection is confirmed (A on a focused list row)
    Dpad,    // D-pad focus movement in the list
    Tab,     // switching tabs (L/R) - loaded, not wired to a call site yet
};

// Initializes ndsp and loads every clip (all Sound values, plus the BGM
// track) from romfs. Returns false (non-fatal - Play()/PlayBgm() are then a
// safe no-op) if audio couldn't be initialized or nothing loaded.
bool Init();

// Exits ndsp and frees loaded clips.
void Shutdown();

// Call once per frame. Only does anything while the BGM track is still
// playing: once it's finished (it never loops - see PlayBgm()), frees its
// ndsp wave buffer and unloads its decoded PCM from memory.
void Update();

// Starts `sound` playing from the beginning, mixed in with whatever else is
// currently playing.
void Play(Sound sound);

// Starts romfs:/bgm/bgm.wav playing once, from the beginning, with no
// looping - meant to be called once, at app startup.
void PlayBgm();

} // namespace Sfx
