#include "mii_detail_panel.h"

#include "ctr_log.h"
#include "ctr_network.h"
#include "ctr_ui.h"
#include "mii_image_fetch.h"
#include "png_texture.h"

#include <3ds.h>
#include <citro2d.h>

#include <algorithm>
#include <string>
#include <vector>

namespace MiiDetailPanel {

namespace {

constexpr int TOP_W = 400, TOP_H = 240;
// Fixed display height (not a separate fetch-vs-display size, so no
// upscale blur - see IMAGE_REQUEST_PX below), bottom-aligned rather than
// top-aligned (IMAGE_Y below) so the Mii's body still reads as extending
// off the bottom edge of the screen even though it's now smaller than the
// full TOP_H. Width is derived from the actual fetched image's own aspect
// ratio at draw time (see Draw()) and the result is horizontally centered
// - not hardcoded, since a non-square source (unlikely for a "face"
// render, but not guaranteed) should still end up centered rather than
// stretched.
constexpr int IMAGE_HEIGHT_PX = 210;
constexpr int IMAGE_REQUEST_PX = IMAGE_HEIGHT_PX; // fetch at native res for that display height, no upscale blur
constexpr int IMAGE_Y = TOP_H - IMAGE_HEIGHT_PX;

// Windowed prefetch, not just the single focused Mii: main.cpp's own
// startup preload fills the first INITIAL_PRELOAD_COUNT up front behind a
// "Loading..." screen, and as focus moves outside the currently cached
// window, this replenishes WINDOW_STEP (6) at a time (evicting the
// WINDOW_STEP farthest-from-focus entries) rather than one at a time - see
// this file's own comment on WINDOW_SIZE's memory math for why 35, not
// more.
//
// WINDOW_SIZE=35 sized against this app's own measured free linear heap on
// Old3DS (~23-24MB, under this CIA's own SystemMode:64MB - see
// meta/README-cia.md, the same 64MB a .3dsx already got from Homebrew
// Launcher's own Prod default, so this isn't a bigger budget than before):
// each 210px portrait needs a 256x256 POT texture (210 rounds up to the
// next power of two) = 256*256*4 = 256KB. 35 of those is 8.75MB, still
// leaving comfortable headroom (~14-15MB) for CFL_DB data, AnimatedBg's own
// texture, SFX/BGM, the GPU command buffer, and text buffers. This cap is
// deliberately independent of how many Miis actually exist - across all
// three tabs this app can have up to 200 from CFL_DB.dat alone (100
// Library + 100 embedded CFRA "recent Miis", see ctr_mii_db.h) plus 100
// from the Friend List, 300 total addressable Miis - caching all of them
// at once would need ~76.8MB, more than this app's *entire* memory budget
// on its own; the whole point of ImageCache's eviction (see its own
// comment) is that the cache never needs to hold more than WINDOW_SIZE
// regardless of how large the underlying list is. A background-worker-
// thread redesign (fetch+decode off the main thread, GPU upload still
// main-thread-only) was tried on top of this once the portrait fetch moved
// from libcurl+mbedtls to libctru's own httpc (ruling out the old memory-
// pressure theory for *why* threading kept crashing), on the theory that
// removing that pressure might make threading viable - it still failed on
// real hardware (silent process exit partway through the first real-usage
// frames, no crash screen, no dump - consistent with an uncaught
// exception/abort rather than a hardware fault, but never fully root-
// caused). Reverted back to this synchronous design, which is the one
// actually confirmed stable across this project's entire history - see
// this file's own comment on Update()'s fetch pacing below.
constexpr int WINDOW_SIZE = 35;
constexpr int WINDOW_STEP = 6;

// Cache keys are composite: tab * kTabIndexStride + index within that tab's
// list - ported from the Wii U sibling build's own composeImageKey()
// (main.cpp) - rather than each tab getting its own separate cache. This is
// what lets a shared WINDOW_SIZE cap serve two tabs: an entry's tab is
// recovered from the key (key / kTabIndexStride) for the eviction
// preference below, and (key % kTabIndexStride) recovers the plain index.
// 10000 comfortably exceeds this format's own 100-Mii-per-source max, same
// margin the Wii U build uses.
constexpr int kTabIndexStride = 10000;

int ComposeKey(Tab tab, int index) { return static_cast<int>(tab) * kTabIndexStride + index; }

u32 Gray() { return C2D_Color32(170, 170, 180, 255); }

// Keyed by the composite (tab, index) key above - shared across both tabs
// so switching tabs doesn't have to evict the tab being left; entries only
// get evicted once the shared cap is actually exceeded (EvictToFit below),
// same as the Wii U build's own multi-tab image cache.
class ImageCache {
public:
    void SetCap(size_t cap) { cap_ = cap; }

    size_t Count() const { return entries_.size(); }

    PngTexture::Texture *Find(int key) {
        for (Entry &entry : entries_) {
            if (entry.key == key) return &entry.tex;
        }
        return nullptr;
    }

    bool Has(int key) const {
        for (const Entry &entry : entries_) {
            if (entry.key == key) return true;
        }
        return false;
    }

    void Insert(int key, PngTexture::Texture tex) { entries_.push_back({key, tex}); }

    // Evicts entries until back at/under cap_, preferring entries that
    // belong to a tab other than `activeTab` before touching same-tab
    // entries at all, and among same-tab entries preferring whichever is
    // farthest (by plain index) from `center` - the currently focused
    // index. Ported from the Wii U build's own per-frame eviction loop
    // (main.cpp) - see this file's own comment on why cache keys are
    // composite in the first place.
    void EvictToFit(int activeTab, int center) {
        while (entries_.size() > cap_) {
            size_t farthestIdx = 0;
            long long farthestScore = -1;
            for (size_t i = 0; i < entries_.size(); i++) {
                int entryTab = entries_[i].key / kTabIndexStride;
                int entryIndex = entries_[i].key % kTabIndexStride;
                long long score = (entryTab == activeTab) ? std::abs(entryIndex - center)
                                                            : static_cast<long long>(kTabIndexStride) * 10;
                if (score > farthestScore) {
                    farthestScore = score;
                    farthestIdx = i;
                }
            }
            PngTexture::Free(&entries_[farthestIdx].tex);
            entries_[farthestIdx] = entries_.back();
            entries_.pop_back();
        }
    }

    void Clear() {
        for (Entry &entry : entries_) PngTexture::Free(&entry.tex);
        entries_.clear();
    }

private:
    size_t cap_ = WINDOW_SIZE;
    struct Entry {
        int key;
        PngTexture::Texture tex;
    };
    std::vector<Entry> entries_;
};

ImageCache g_cache;

Tab g_activeTab = Tab::Library;
int g_focusedIndex = -1;
PngTexture::Texture *g_tex = nullptr;
// Only for the transition-logging in Draw() below - not app logic.
bool g_lastDrawHadValidTex = false;

// Per-tab window start (-1 = no window built yet for that tab). Indexed by
// static_cast<int>(Tab).
int g_windowStart[3] = {-1, -1, -1};
// Composite keys (see ComposeKey()) still needing a fetch in the *current*
// tab's window, closest-to-focus first - only ever holds entries for
// g_activeTab, since only one tab's worth of requests is ever fetched at a
// time (matches the Wii U build's own comment on this).
std::vector<int> g_pendingFetch;

// Set on any failed fetch attempt (network error, or a downloaded file that
// failed to decode), cleared the moment a later one succeeds - see
// LastFetchFailed()'s own comment. g_lastFetchFailedAtMs also drives the
// retry backoff below - see Update()'s own comment on why that exists at
// all.
bool g_lastFetchFailed = false;
u64 g_lastFetchFailedAtMs = 0;
// How long to wait after a failed fetch before attempting another one at
// all (any key, not just the one that just failed - see Update()'s own
// comment). Confirmed on-device as necessary, not just a nicety: with no
// internet, every attempt fails the same way, and the previous code (no
// backoff at all) re-queued and re-attempted the focused Mii's fetch every
// single Update() call via the safety net below - each attempt still a
// real blocking httpc call that can take real seconds to time out with no
// network to fail fast against, so the app was seconds-per-frame at best,
// indistinguishable from a hang/crash to the user.
constexpr u64 kRetryBackoffMs = 4000;

void RebuildWindow(Tab tab, const std::vector<Ver3MiiDecoded> &miis, int windowStart) {
    CtrLog::Printf("MiiDetailPanel: RebuildWindow tab=%d windowStart=%d miis.size()=%zu", static_cast<int>(tab),
                    windowStart, miis.size());
    g_windowStart[static_cast<int>(tab)] = windowStart;
    int lastIndex = static_cast<int>(miis.size()) - 1;
    int windowEnd = std::min(lastIndex, windowStart + WINDOW_SIZE - 1);

    g_pendingFetch.clear();
    for (int i = windowStart; i <= windowEnd; i++) {
        int key = ComposeKey(tab, i);
        if (!g_cache.Has(key)) g_pendingFetch.push_back(key);
    }
    std::sort(g_pendingFetch.begin(), g_pendingFetch.end(), [](int a, int b) {
        return std::abs((a % kTabIndexStride) - g_focusedIndex) < std::abs((b % kTabIndexStride) - g_focusedIndex);
    });
    CtrLog::Printf("MiiDetailPanel: RebuildWindow done, %zu pending", g_pendingFetch.size());
}

} // namespace

void SetMaxCachedPortraits(size_t maxPortraits) { g_cache.SetCap(maxPortraits); }

void Init() {}

// Diagnostic only, transition-logged (not every Update() call, which runs
// every single frame regardless of whether anything changed) - this is
// specifically to catch the moment the user's own D-pad/touch scrolling
// moves focus, since that itself is otherwise completely unlogged.
Tab g_lastLoggedTab = Tab::Library;
int g_lastLoggedFocusedIndex = -2; // -2, not -1: -1 is itself a real "no focus" value

void Update(Tab tab, const std::vector<Ver3MiiDecoded> &miis, int focusedIndex) {
    if (tab != g_lastLoggedTab || focusedIndex != g_lastLoggedFocusedIndex) {
        CtrLog::Printf("MiiDetailPanel::Update: focus now tab=%d index=%d (miis.size()=%zu)",
                        static_cast<int>(tab), focusedIndex, miis.size());
        g_lastLoggedTab = tab;
        g_lastLoggedFocusedIndex = focusedIndex;
    }

    // g_pendingFetch is only ever supposed to hold keys for whichever tab
    // is currently active (see its own comment) - but it's only actually
    // refreshed by RebuildWindow() below, which only runs when *this* tab's
    // own window needs to move. Switching to a tab whose window happens to
    // already be fully cached from a previous visit skips that rebuild
    // entirely, so without this check the *previous* tab's stale keys would
    // otherwise survive into this call and get decomposed against the
    // wrong tab's `miis` list a few lines down.
    if (tab != g_activeTab) g_pendingFetch.clear();
    g_activeTab = tab;
    g_focusedIndex = focusedIndex;
    int tabIdx = static_cast<int>(tab);

    if (miis.empty() || focusedIndex < 0) {
        g_tex = nullptr;
        g_pendingFetch.clear();
        return;
    }

    int windowStart = g_windowStart[tabIdx];
    if (windowStart < 0) {
        // First call ever for this tab - snap directly to the
        // WINDOW_STEP-aligned window containing focusedIndex.
        RebuildWindow(tab, miis, (focusedIndex / WINDOW_STEP) * WINDOW_STEP);
    } else if (focusedIndex < windowStart || focusedIndex >= windowStart + WINDOW_SIZE) {
        // Slide by WINDOW_STEP at a time from the *current* window, not a
        // fresh snap from focusedIndex alone - a fresh snap would jump by a
        // full WINDOW_SIZE when crossing the boundary by just one Mii.
        int newStart = windowStart;
        while (focusedIndex < newStart) newStart -= WINDOW_STEP;
        while (focusedIndex >= newStart + WINDOW_SIZE) newStart += WINDOW_STEP;
        RebuildWindow(tab, miis, std::max(0, newStart));
    }

    // Guarantees forward progress even when nothing above just rebuilt the
    // window: EvictToFit() (below, and via the *other* tab's own Update()
    // calls while this tab was inactive) can silently evict an entry that's
    // still technically inside this tab's own remembered window - the
    // window-move checks above only refresh g_pendingFetch when the window
    // itself shifts, so a cache entry lost that way would otherwise never
    // get re-requested at all, leaving whatever's currently focused stuck
    // showing the loading placeholder forever (confirmed via on-device
    // logging: g_tex stayed null for 60+ consecutive frames leading up to a
    // crash - switching tabs back and forth evicted a Library entry still
    // inside Library's own committed window, and nothing ever noticed).
    int focusedKey = ComposeKey(tab, focusedIndex);
    if (!g_cache.Has(focusedKey) &&
        std::find(g_pendingFetch.begin(), g_pendingFetch.end(), focusedKey) == g_pendingFetch.end()) {
        g_pendingFetch.insert(g_pendingFetch.begin(), focusedKey);
    }

    // One fetch per Update() call, synchronous/blocking (never backgrounded
    // onto another thread) - see this file's own top comment on WINDOW_SIZE
    // for why: every attempt at threading this (several different
    // coordination mechanisms, most recently a persistent worker thread on
    // top of libctru's own httpc) crashed or silently died on real
    // hardware. Pacing one fetch per call rather than draining the whole
    // window in one call is what keeps a single call from blocking for the
    // window's entire fetch time - the render loop still gets a frame in
    // between each fetch, so the UI stays responsive (if not instantly
    // populated) even though nothing here is actually concurrent.
    //
    // Gated on the retry backoff (see kRetryBackoffMs's own comment) -
    // skipped entirely, not just this key, while a previous failure is
    // still cooling down: g_pendingFetch is left untouched either way, so
    // whatever's at the front just gets tried again once the backoff
    // passes, same as if this whole block had simply run a few frames
    // later.
    bool backoffActive = g_lastFetchFailed && (osGetTime() - g_lastFetchFailedAtMs) < kRetryBackoffMs;
    if (!g_pendingFetch.empty() && !backoffActive) {
        int key = g_pendingFetch.front();
        g_pendingFetch.erase(g_pendingFetch.begin());

        // Checked fresh before every single attempt, not just once at
        // startup (see main.cpp's own Screen::NetworkGate, which only
        // checks this before the list is ever shown at all) - confirmed
        // on-device that turning the wireless switch off *while already
        // browsing the list*, then scrolling to an uncached Mii, reliably
        // exits the whole app the moment a fetch is actually attempted
        // against it - not a graceful Result-code failure this function's
        // own error handling below could ever catch, since that only
        // handles errors httpc itself reports back normally. Skipping the
        // call entirely whenever the switch is confirmed off sidesteps
        // whatever that failure mode actually is, rather than trying to
        // catch it after the fact.
        PngTexture::Texture tex{};
        MiiImageFetcher::Result fetchResult;
        if (CtrNetwork::IsWirelessSwitchOff()) {
            CtrLog::Printf("MiiDetailPanel: skipping fetch key=%d, wireless switch is off", key);
        } else {
            int idx = key % kTabIndexStride;
            CtrLog::Printf("MiiDetailPanel: fetching key=%d idx=%d (miis.size()=%zu)", key, idx, miis.size());
            fetchResult = MiiImageFetcher::FetchImageBlocking(miis[static_cast<size_t>(idx)].storeData, IMAGE_REQUEST_PX);
            CtrLog::Printf("MiiDetailPanel: fetch key=%d success=%d bytes=%zu", key, fetchResult.success,
                            fetchResult.pngBytes.size());
            if (fetchResult.success) {
                tex = PngTexture::LoadFromMemory(fetchResult.pngBytes.data(), fetchResult.pngBytes.size());
                CtrLog::Printf("MiiDetailPanel: decode key=%d valid=%d %ux%u", key, tex.valid,
                                tex.valid ? tex.image.subtex->width : 0, tex.valid ? tex.image.subtex->height : 0);
            }
        }
        // A successfully-decoded but suspiciously tiny image (e.g. a 1x1
        // "not found"/error placeholder some servers hand back with a real
        // 200 status instead of a proper face render - PngTexture's own
        // decode has no way to tell that apart from a real image on its
        // own, since it's only responsible for PNG bytes -> pixels, not
        // knowing what any particular caller actually expects to receive)
        // is still garbage for this specific caller's purposes - rejected
        // here, not in PngTexture itself (which is shared by AnimatedBg/the
        // author icon/etc, some of which are legitimately small), the same
        // as an outright download/decode failure: not cached, not drawn -
        // confirmed on-device as the cause of a solid-colored block
        // (a 1x1 texture stretched across the full portrait display size)
        // in place of the intended "..." placeholder.
        constexpr u16 kMinValidPortraitPx = 16;
        if (tex.valid && (tex.image.subtex->width < kMinValidPortraitPx || tex.image.subtex->height < kMinValidPortraitPx)) {
            CtrLog::Printf("MiiDetailPanel: rejecting key=%d, too small (%ux%u)", key, tex.image.subtex->width,
                            tex.image.subtex->height);
            PngTexture::Free(&tex);
        }
        if (tex.valid) {
            g_cache.Insert(key, tex);
            g_lastFetchFailed = false;
            CtrLog::Printf("MiiDetailPanel: cached key=%d (cache now has %zu entries)", key, g_cache.Count());
        } else {
            if (!g_lastFetchFailed) CtrLog::Printf("MiiDetailPanel: entering fetch backoff (key=%d failed)", key);
            g_lastFetchFailed = true;
            g_lastFetchFailedAtMs = osGetTime();
        }
    }

    // Shared cap across all tabs - see ImageCache::EvictToFit()'s own
    // comment for the eviction preference (other-tab entries go first).
    g_cache.EvictToFit(tabIdx, focusedIndex);

    g_tex = g_cache.Find(focusedKey);
}

void Draw(float eyeOffsetPx) {
    if (g_focusedIndex < 0) return;

    // Transition-only (not per-frame) - avoids the exact per-draw-call
    // logging volume that self-inflicted this app's own worst hang, way
    // earlier in its history (see ctr_log.h's own comment) - but still
    // gives visibility into exactly when the focused portrait becomes
    // available/unavailable, which matters for diagnosing a fetch-failure
    // bug specifically.
    bool nowValid = g_tex && g_tex->valid;
    if (nowValid != g_lastDrawHadValidTex) {
        CtrLog::Printf("MiiDetailPanel::Draw: g_tex now %s (tab=%d index=%d)", nowValid ? "VALID" : "invalid",
                        static_cast<int>(g_activeTab), g_focusedIndex);
        g_lastDrawHadValidTex = nowValid;
    }

    // No background rect behind the portrait - the fetched PNG has its own
    // alpha channel, and drawing an opaque rect here would defeat that,
    // hiding AnimatedBg (drawn separately by main.cpp, at zero parallax so
    // it stays flat - see this file's header comment on stereo 3D) behind a
    // solid color instead of letting it show through.
    if (g_tex && g_tex->valid) {
        // Height fixed at IMAGE_HEIGHT_PX (see that constant's own
        // comment); width follows the source texture's own aspect ratio at
        // that same scale, then centered horizontally (before
        // eyeOffsetPx's own shift) - not just re-using IMAGE_REQUEST_PX for
        // both axes, in case the server ever hands back something not
        // perfectly square.
        float scale = static_cast<float>(IMAGE_HEIGHT_PX) / static_cast<float>(g_tex->image.subtex->height);
        float displayWidth = static_cast<float>(g_tex->image.subtex->width) * scale;
        float imageX = (static_cast<float>(TOP_W) - displayWidth) / 2.0f + eyeOffsetPx;
        C2D_DrawImageAt(g_tex->image, imageX, IMAGE_Y, 0.0f, nullptr, scale, scale);
    } else {
        // Animated "..." placeholder while the focused Mii's portrait
        // hasn't finished fetching yet. osGetTime() is a u64 millisecond
        // timestamp large enough that truncating it to a 32-bit int before
        // this arithmetic can silently produce a negative value (console-
        // uptime-dependent) - the negative-modulo result would then hit
        // std::string(size_t count, char)'s unsigned count parameter as a
        // huge implicit-converted value and throw std::length_error. Do the
        // division/modulo in u64 space (always non-negative) and only cast
        // the small [0,2] result down to int at the very end.
        int dotCount = 1 + static_cast<int>((osGetTime() / 400) % 3);
        std::string dots(dotCount, '.');
        constexpr float kDotsScale = 1.0f;
        float dotsW = 0.0f, dotsH = 0.0f;
        CtrUi::MeasureText(kDotsScale, dots.c_str(), &dotsW, &dotsH);
        CtrUi::DrawText(static_cast<float>(TOP_W) / 2.0f - dotsW / 2.0f + eyeOffsetPx,
                         static_cast<float>(IMAGE_Y) + static_cast<float>(IMAGE_HEIGHT_PX) / 2.0f - dotsH / 2.0f,
                         kDotsScale, Gray(), dots.c_str());
    }
}

bool LastFetchFailed() { return g_lastFetchFailed; }

void Shutdown() {
    g_cache.Clear();
    g_pendingFetch.clear();
    g_windowStart[0] = g_windowStart[1] = g_windowStart[2] = -1;
    g_focusedIndex = -1;
    g_lastFetchFailed = false;
    g_lastFetchFailedAtMs = 0;
}

} // namespace MiiDetailPanel
