// Floor build: read CFL_DB.dat's main array (Library tab), its embedded
// CFRA "recent Miis" section (RecentGames tab - Miis picked in *other*
// titles, e.g. friend-request/NPC pickers - see ctr_mii_db.h), and FRD's
// friend list (Friends tab, see ctr_friends.h), each its own scrollable
// list on the bottom screen with a triangle cursor (D-pad up/down) pointing
// at the focused name, L/R cycling between the three tabs (scroll/focus
// preserved per tab), show the focused Mii's portrait (fetched over HTTP,
// see mii_detail_panel.h) on the top screen with real stereoscopic 3D
// depth, updating every time the cursor moves - MiiDetailPanel actually
// prefetches a whole window of upcoming portraits, not just the focused
// one, paced to one fetch per frame, shared between all three tabs (see its
// own comment for the memory budget and cache-sharing scheme) - and run the
// local HTTP file server (mii_http_server.h), showing its address top-left
// of the top screen once ready. ACT is used only for the current console
// user's own Mii name (shown in the Friends tab header). The bottom screen
// also takes touch: tapping a row focuses it (same as moving the D-pad
// cursor there, including the top-screen portrait prefetch that follows),
// tapping the already-focused row again opens its details screen (same as
// KEY_A), and tapping either scroll arrow pages the list a full
// VISIBLE_ROWS at once rather than moving it by a single row - see the main
// loop's own touch-handling block for specifics. No per-Mii info text
// beyond the dedicated details screen. Earlier rounds of
// the portrait feature crashed on-device several times over trying to
// background the fetch onto a separate thread (several different
// coordination mechanisms, all crashed) - it's synchronous/blocking now
// (paced per-frame, not per-request, to avoid one long freeze), which has
// proven stable.
#include <citro2d.h>
#include <3ds.h>

#include <algorithm>
#include <exception>
#include <malloc.h>
#include <string>
#include <vector>

#include "animated_bg.h"
#include "ctr_account.h"
#include "ctr_friends.h"
#include "ctr_log.h"
#include "ctr_mii_db.h"
#include "ctr_network.h"
#include "ctr_ui.h"
#include "mii_detail_panel.h"
#include "mii_http_server.h"
#include "mii_image_fetch.h"
#include "png_texture.h"
#include "sfx.h"
#include "ver3_mii.h"

#include <cstdio>

// Overrides libctru's crt0 weak default of a mere 32KB for the *main*
// thread's stack - see stack_adjust.s. Kept even in this stripped-down
// build since it's a real, confirmed-correct fix regardless of what else
// gets simplified away.
extern "C" u32 __stacksize__ = 512 * 1024;

namespace {

constexpr int TOP_W = 400, TOP_H = 240;
constexpr int BOTTOM_W = 320, BOTTOM_H = 240;
// The bottom screen is horizontally centered beneath the (wider) top screen
// on real hardware - see AnimatedBg::Draw()'s own comment on why this is
// what makes the two screens' tiles line up as one continuous background
// instead of two independently-tiled copies.
constexpr float BOTTOM_WORLD_OFFSET_X = static_cast<float>(TOP_W - BOTTOM_W) / 2.0f;

// TAB_HEADER_Y/SCALE: the "Mii Library (N Miis) - L/R: Switch Tab"/"...'s
// Friend's Miis (N Miis) - L/R: Switch Tab"/"Recent Miis from games (N
// Miis) - L/R: Switch Tab" line above everything else, telling the user
// which of the three tabs they're on, how many Miis are in it, and how to
// switch - see TabHeaderText(). Wrapped (DrawTextWrapped),
// not a single DrawText call: at this scale, the Friends tab's line
// (possessive Mii name + fixed text) routinely doesn't fit in BOTTOM_W on
// one line, and there's no reliable way to measure a wrapped block's real
// height in advance (see ctr_ui.h's own comment on DrawTextWrapped) - so
// TAB_HEADER_RESERVED_LINES fixes a generous 2-line budget instead, and
// everything below (the scroll arrows, LIST_TOP) is placed far enough down
// to always clear that budget whether the header actually wrapped or not.
constexpr float TAB_HEADER_SCALE = 0.55f;
constexpr float TAB_HEADER_Y = 4.0f;
constexpr int TAB_HEADER_RESERVED_LINES = 2;
constexpr float TAB_HEADER_LINE_HEIGHT = 30.0f * TAB_HEADER_SCALE; // system font: 30px tall at scale 1.0
constexpr float TAB_HEADER_BOTTOM_Y =
    TAB_HEADER_Y + static_cast<float>(TAB_HEADER_RESERVED_LINES) * TAB_HEADER_LINE_HEIGHT;

constexpr int LIST_TOP = static_cast<int>(TAB_HEADER_BOTTOM_Y) + 22;
constexpr int LIST_LEFT = 24; // leaves room for the cursor triangle to its left
constexpr float TAB_HEADER_WRAP_WIDTH = static_cast<float>(BOTTOM_W - 2 * LIST_LEFT);
constexpr int ROW_HEIGHT = 24;
constexpr int LIST_BOTTOM_MARGIN = 14;
constexpr int VISIBLE_ROWS = (BOTTOM_H - LIST_TOP - LIST_BOTTOM_MARGIN) / ROW_HEIGHT;
constexpr float SCROLL_ARROW_X = (static_cast<float>(LIST_LEFT) + static_cast<float>(BOTTOM_W)) / 2.0f;
constexpr float SCROLL_ARROW_TOP_Y = TAB_HEADER_BOTTOM_Y + 10.0f;
constexpr float SCROLL_ARROW_BOTTOM_Y = static_cast<float>(BOTTOM_H) - 8.0f;
// Touch hit box around each scroll arrow - deliberately much bigger than the
// arrow's own drawn triangle (halfW=7.0f/halfH=5.0f in DrawScrollArrow()),
// a stylus-only 14x10px target is too small to reliably hit with a finger.
constexpr float SCROLL_ARROW_HIT_HALF_W = 24.0f;
constexpr float SCROLL_ARROW_HIT_HALF_H = 14.0f;
constexpr float NAME_SCALE = 0.55f;
// Screen::NetworkGate's own error text ("Please enable wireless
// communications..." / "Please make sure to have set up an internet
// connection...") - deliberately bigger than every other on-screen label,
// since it's the one thing on that screen and needs to read clearly at a
// glance, not be hunted for.
constexpr float NETWORK_ERROR_SCALE = 0.65f;
constexpr float LOADING_SCALE = 0.6f;

constexpr uint16_t HTTP_SERVER_PORT = 8080;
constexpr float SERVER_LABEL_SCALE = 0.7f;
constexpr float SERVER_ADDRESS_SCALE = 0.9f;
constexpr float BOTTOM_HINT_SCALE = 0.6f;
// Top-left corner reminder shown on the top screen during the normal list/
// portrait view (not the dedicated server screen itself, which already
// shows its own status text) - lets the user discover the X-to-host feature
// without having to be told out of band.
constexpr float TOP_HINT_SCALE = 0.55f;
constexpr float TOP_HINT_X = 8.0f;
constexpr float TOP_HINT_Y = 6.0f;

// App version, top-right corner of the top screen, one row below the
// X-to-host reminder above rather than sharing its row - that reminder grew
// wide enough (button icon + "Host local Mii server  " + icon + "Return to
// HOME Menu") to run into the version text when both were on the same line.
// A notch bigger than TOP_HINT_SCALE (system font glyph height is 30px at
// scale 1.0, so +0.05 is +1.5px) rather than matching it exactly - legible
// on its own at a glance, not just readable once you already know it's
// there.
constexpr float TOP_VERSION_SCALE = 0.6f;
constexpr float TOP_VERSION_MARGIN_X = 8.0f;
constexpr float TOP_VERSION_Y = TOP_HINT_Y + 30.0f * TOP_HINT_SCALE + 4.0f;
constexpr const char *APP_VERSION_TEXT = "v1.0.0";

// Portrait-fetch error banner, directly below the X-to-host reminder and
// version text rows above - shown whenever MiiDetailPanel::LastFetchFailed()
// is true (a download or decode failure, most likely no internet), cleared
// automatically the moment a later fetch succeeds - see that function's own
// comment. Red, distinct from every other on-screen text color, since this
// is the one thing on screen actually reporting an error rather than just
// status.
constexpr float TOP_FETCH_ERROR_SCALE = 0.43f; // 2px smaller than before (system font glyph height is 30px at scale 1.0)
constexpr float TOP_FETCH_ERROR_Y = TOP_VERSION_Y + 30.0f * TOP_VERSION_SCALE + 4.0f;

// A-to-open Mii details screen (bottom screen only - the top screen keeps
// showing the same live stereo portrait it always does, since focus doesn't
// change and nothing new needs fetching).
constexpr float DETAIL_LEFT = 14.0f;
constexpr float DETAIL_TOP = 8.0f;
constexpr float DETAIL_NAME_SCALE = 0.7f;
constexpr float DETAIL_AUTHOR_SCALE = 0.55f;
constexpr float DETAIL_LINE_SCALE = 0.45f;
constexpr float DETAIL_LINE_GAP = 3.0f;
constexpr float DETAIL_SECTION_GAP = 6.0f;

u32 Gray() { return C2D_Color32(170, 170, 180, 255); }

// Max per-eye horizontal shift (pixels) for the Mii portrait's stereoscopic
// pop-out, at full 3D slider depth - only the portrait gets this (see
// MiiDetailPanel::Draw()'s own eyeOffsetPx parameter); AnimatedBg is always
// drawn identically to both eyes (zero parallax) so it reads as flat, at
// the screen's own depth, with the portrait floating in front of it.
// Tuned by feel, not derived from anything - adjust directly if it looks
// too subtle or too aggressive on real hardware.
constexpr float MAX_3D_DEPTH_PX = 8.0f;

// Deliberately less than MiiDetailPanel's own WINDOW_SIZE (35, see its own
// comment for the memory budget behind that number) - this is how many
// portraits to block-load up front, behind the "Loading..." screen, before
// ever showing the real list/portrait UI; the remaining slots up to
// WINDOW_SIZE fill in gradually, WINDOW_STEP (6) at a time, as the user
// actually scrolls, rather than making the startup wait longer than it
// needs to be for portraits that might not even get looked at right away.
// Each one is a real, blocking network fetch + decode (see
// MiiDetailPanel::Update()'s own comment on why nothing here is
// backgrounded) - lowered from 20 to 10 after it made startup noticeably
// slow to get through, then raised back up (15, now 30) anyway per explicit
// request (trading some of that responsiveness back for more portraits
// ready up-front - the "(n/N)" progress counter on the loading screen keeps
// this from feeling stuck even at the higher count).
constexpr int INITIAL_PRELOAD_COUNT = 30;

// "No HOME" icon (romfs:/homeDisallowed.png) animation timing, shown on a
// rejected HOME press during the loading screen - see RunInitialPreload()'s
// own comment. Frame-counted (this app's render loop is SYNCDRAW-paced, a
// steady 60fps, so frames are already the natural unit here) rather than
// wall-clock time, matching the platform's own documented convention for
// this icon: a linear fade curve, 5 frames fading in, 60 frames held fully
// visible, 20 frames fading back out - plays once per press; a press while
// it's already playing is ignored rather than restarting it, so rapid
// repeated presses can't spam or extend it.
constexpr int kHomeIconFadeInFrames = 5;
constexpr int kHomeIconHoldFrames = 60;
constexpr int kHomeIconFadeOutFrames = 20;
constexpr int kHomeIconTotalFrames = kHomeIconFadeInFrames + kHomeIconHoldFrames + kHomeIconFadeOutFrames;

// Linear ease in/hold/ease out, per kHomeIcon*Frames above. `frame` is
// 0-based, expected in [0, kHomeIconTotalFrames).
float HomeIconAlpha(int frame) {
    if (frame < kHomeIconFadeInFrames) return static_cast<float>(frame) / static_cast<float>(kHomeIconFadeInFrames);
    if (frame < kHomeIconFadeInFrames + kHomeIconHoldFrames) return 1.0f;
    int fadeOutFrame = frame - kHomeIconFadeInFrames - kHomeIconHoldFrames;
    return 1.0f - static_cast<float>(fadeOutFrame) / static_cast<float>(kHomeIconFadeOutFrames);
}

// A small right-pointing triangle, vertically centered on a ROW_HEIGHT-tall
// row starting at rowTop, sitting just left of LIST_LEFT.
void DrawCursorTriangle(int rowTop) {
    float cy = static_cast<float>(rowTop) + static_cast<float>(ROW_HEIGHT) / 2.0f;
    float xBack = static_cast<float>(LIST_LEFT) - 16.0f;
    float xTip = static_cast<float>(LIST_LEFT) - 6.0f;
    u32 color = C2D_Color32(255, 220, 60, 255);
    C2D_DrawTriangle(xBack, cy - 6.0f, color, xBack, cy + 6.0f, color, xTip, cy, color, 0.0f);
}

// A small up- or down-pointing triangle, centered at (centerX, centerY) -
// the bottom-screen list's "there's more to scroll this way" indicator (see
// its own call sites: shown only when scrollOffset actually allows moving
// further in that direction, hidden otherwise).
void DrawScrollArrow(float centerX, float centerY, bool pointingDown) {
    float halfW = 7.0f;
    float halfH = 5.0f;
    u32 color = C2D_Color32(255, 255, 255, 255); // white, distinct from the cursor triangle's yellow
    if (pointingDown) {
        C2D_DrawTriangle(centerX - halfW, centerY - halfH, color, centerX + halfW, centerY - halfH, color, centerX,
                          centerY + halfH, color, 0.0f);
    } else {
        C2D_DrawTriangle(centerX - halfW, centerY + halfH, color, centerX + halfW, centerY + halfH, color, centerX,
                          centerY - halfH, color, 0.0f);
    }
}

// Line 1 (label, SERVER_LABEL_SCALE) + line 2 (the actual address once
// known, bigger - SERVER_ADDRESS_SCALE) for the dedicated server screen -
// see main()'s own comment on why this is a whole separate screen (X to
// enter, B to leave) rather than an always-on background service.
// Draws `text` (may contain explicit '\n' breaks, same convention as
// CtrUi::DrawText/DrawTextWrapped) horizontally centered, one line at a
// time - not CtrUi::DrawTextWrapped(), which only wraps automatically at a
// fixed pixel width and always left-aligns every resulting line at the same
// X, so it can't center lines of differing width the way a short manually-
// broken error message (e.g. HttpServerStatusLines()'s own "Could not
// determine this\nconsole's IP address." below) needs.
void DrawTextCenteredMultiline(float centerX, float y, float scale, u32 color, const std::string &text) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        float w = 0.0f, h = 0.0f;
        CtrUi::MeasureText(scale, line.c_str(), &w, &h);
        CtrUi::DrawText(centerX - w / 2.0f, y, scale, color, line.c_str());
        y += h;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

std::pair<std::string, std::string> HttpServerStatusLines() {
    switch (MiiHttpServer::GetState()) {
        case MiiHttpServer::State::Ready:
            return {"Server running at", "http://" + MiiHttpServer::GetLocalIpString() + ":" +
                                              std::to_string(MiiHttpServer::GetPort())};
        case MiiHttpServer::State::Failed:
            return {"Server failed to start", MiiHttpServer::GetError()};
        case MiiHttpServer::State::Starting:
        default:
            return {"Starting server at", "..."};
    }
}

// Per-tab list state (which Mii list, plus where the cursor/scroll sit in
// it) - kept separate per tab so switching tabs (L/R) preserves each tab's
// own scroll position and focus instead of resetting it, same as the Wii U
// build's own TabState. Indexed by static_cast<int>(MiiDetailPanel::Tab).
struct TabState {
    std::vector<Ver3MiiDecoded> miis;
    int focusedIndex = -1;
    int scrollOffset = 0;
};
constexpr int TAB_COUNT = 3;

// Flattens a Mii list into the raw, concatenated CFLStoreData blob format
// MiiHttpServer's /data/*.dat endpoints serve (96 bytes per Mii, no
// encoding/separators) - same shape as the Wii U build's own
// BuildRawStoreDataBlob().
std::vector<uint8_t> BuildRawStoreDataBlob(const std::vector<Ver3MiiDecoded> &miis) {
    std::vector<uint8_t> out;
    out.reserve(miis.size() * VER3_STORE_DATA_SIZE);
    for (const Ver3MiiDecoded &mii : miis) out.insert(out.end(), mii.storeData.begin(), mii.storeData.end());
    return out;
}

// The header line above the list itself, telling the user which tab (L/R to
// switch) they're looking at, how many Miis are in it, and - for the
// Friends tab - whose friends they are (the current console user's own Mii
// name, via CtrAccount - see main()'s own wiring). The big empty-state
// message drawn separately below (see this file's own "No friends found."/
// "Connecting..." handling) is what actually communicates FRD's
// Connecting/Failed states - this header always reports the tab's current
// Mii count (0 while still connecting/failed, which is accurate) rather
// than duplicating that messaging here.
std::string TabHeaderText(MiiDetailPanel::Tab tab, const TabState &tabState, const std::string &currentUserMiiName) {
    std::string countSuffix = "(" + std::to_string(tabState.miis.size()) + " Miis) - L/R: Switch Tab";
    switch (tab) {
        case MiiDetailPanel::Tab::Library:
            return "Mii Library " + countSuffix;
        case MiiDetailPanel::Tab::RecentGames:
            return "Recent Miis from games " + countSuffix;
        case MiiDetailPanel::Tab::Friends:
        default: {
            // Degrades gracefully to no possessive prefix if the current
            // user's own Mii name isn't known yet (ACT still connecting, or
            // has no Mii set) - see CtrAccount::Poll() in main()'s own
            // per-frame handling.
            std::string namePrefix = currentUserMiiName.empty() ? "" : currentUserMiiName + "'s ";
            return namePrefix + "Friend's Miis " + countSuffix;
        }
    }
}

// Text for CtrFriends::UpdateGameModeDescription() (see that function's own
// comment) - shorter than TabHeaderText() above (that one's tuned for this
// app's own header line, not a friend list's status column), updated
// whenever the focused tab or its Mii count changes so a friend checking
// this console's profile sees roughly what's currently on screen.
std::string GameModeDescriptionText(MiiDetailPanel::Tab tab, size_t count) {
    switch (tab) {
        case MiiDetailPanel::Tab::Library:
            return "Checking Mii Library: " + std::to_string(count);
        case MiiDetailPanel::Tab::RecentGames:
            return "Checking Recent Miis: " + std::to_string(count);
        case MiiDetailPanel::Tab::Friends:
        default:
            return "Checking Friends Miis: " + std::to_string(count);
    }
}

// The small pencil icon drawn next to the creator name on the Mii details
// screen (romfs:/author_icon.png) - loaded once at startup like AnimatedBg's
// own texture, freed once at shutdown. Left invalid (no icon drawn, just the
// creator name text) if the file's missing/corrupt rather than failing
// startup over a cosmetic asset.
PngTexture::Texture g_authorIconTex;

// "No HOME" icon (romfs:/homeDisallowed.png) - loaded once at startup,
// shared by every screen that disables HOME (currently: the initial
// "Loading..." screen, and the local-server screen - see their own
// aptSetHomeAllowed()/aptCheckHomePressRejected() call sites) rather than
// loaded fresh each time, since both can happen more than once a session
// (well, Loading only once, but Server can be entered/left repeatedly).
PngTexture::Texture g_homeIconTex;

// Draws the "no HOME" icon overlay (if `animFrame` is currently playing -
// see kHomeIconTotalFrames's own comment for the animation shape) centered
// on the bottom screen, and advances `animFrame` by one - shared between
// every screen that can show it, each of which owns its own `animFrame`
// state (a rejected press on one screen doesn't affect an animation
// already in progress from a different one, though in practice only one of
// these screens is ever showing at a time anyway). Must be called from
// within an already-bound bottomTarget scene, once per frame, every frame
// (this is what actually advances the animation, not just renders it).
void DrawHomeIconOverlay(int &animFrame) {
    if (animFrame < 0 || !g_homeIconTex.valid) return;
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, HomeIconAlpha(animFrame));
    float iconW = static_cast<float>(g_homeIconTex.image.subtex->width);
    float iconH = static_cast<float>(g_homeIconTex.image.subtex->height);
    C2D_DrawImageAt(g_homeIconTex.image, (static_cast<float>(BOTTOM_W) - iconW) / 2.0f,
                     (static_cast<float>(BOTTOM_H) - iconH) / 2.0f, 0.0f, &tint, 1.0f, 1.0f);
    animFrame++;
    if (animFrame >= kHomeIconTotalFrames) animFrame = -1; // done - armed for the next press
}

// Diagnostic only, installed via std::set_terminate() at the very top of
// main() (right after CtrLog::Init(), so it can log at all): a "silent
// process exit, no crash screen, no dump" signature - which is exactly
// what's been reported scrolling to an uncached Mii with no internet - is
// the classic symptom of an uncaught C++ exception (most plausibly
// std::bad_alloc, e.g. from std::vector::push_back()/std::string
// concatenation under real memory pressure - this app is holding onto a
// lot by the time this could happen: dozens of cached portrait textures,
// CFL_DB data, AnimatedBg, SFX/BGM, both UI icons, citro3d's own command
// buffer) rather than a hardware fault, which Luma3DS's own exception
// dump would otherwise have caught and shown on-screen. This was tried
// once before, during an earlier (unrelated, now-reverted) investigation
// into backgrounding the portrait fetch, and never got a definitive
// answer either way before that work was reverted - worth trying again
// now, on this specific, still-unexplained crash.
void LogTerminate() {
    CtrLog::Printf("std::terminate() called (uncaught exception or fatal library error)");
    if (std::exception_ptr eptr = std::current_exception()) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception &e) {
            CtrLog::Printf("  active exception: %s", e.what());
        } catch (...) {
            CtrLog::Printf("  active exception: non-std::exception type");
        }
    } else {
        CtrLog::Printf("  no active exception (direct abort()/assert() call)");
    }
    std::abort();
}

// Diagnostic only - newlib mallinfo() (the same allocator malloc/new both
// go through), to see directly whether the app is running close to its
// actual heap ceiling by the time a crash like this happens, rather than
// inferring it indirectly from symptoms alone.
void LogHeapUsage(const char *label) {
    struct mallinfo info = mallinfo();
    CtrLog::Printf("heap[%s]: used=%u KB free=%u KB", label, info.uordblks / 1024, info.fordblks / 1024);
}

std::vector<uint8_t> ReadFileFully(const char *path) {
    std::vector<uint8_t> out;
    FILE *f = fopen(path, "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        out.resize(static_cast<size_t>(size));
        if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    fclose(f);
    return out;
}

} // namespace

int main() {
    // soc:u is needed for MiiHttpServer's own listening socket (BSD sockets
    // directly) - no longer needed by mii_image_fetch.cpp (now on httpc, a
    // separate IPC-based system service - see its own header comment) or by
    // CtrLog (its UDP side-channel was removed entirely - see ctr_log.cpp's
    // own comment), but still owned/brought up here, centrally, first.
    u32 *socBuffer = static_cast<u32 *>(memalign(0x1000, 0x100000));
    bool socOk = socBuffer && R_SUCCEEDED(socInit(socBuffer, 0x100000));
    CtrLog::Init();
    std::set_terminate(LogTerminate);
    CtrLog::Printf("mii-transfer (floor build) starting up (soc %s)", socOk ? "ok" : "FAILED");
    LogHeapUsage("startup");

    // New3DS-only clock speedup (804MHz + extra L2 cache, vs the 268MHz
    // shared with Old3DS) - toggled at runtime rather than via the CIA's own
    // exheader flags, so it's a no-op on Old3DS instead of needing a whole
    // separate build. Nothing in this app is actually CPU-bound enough to
    // need this (the real bottlenecks in this project's history have always
    // been memory/network/threading, never raw CPU throughput), but it's
    // free on New3DS and can only make the occasional synchronous PNG-
    // decode-and-upload frame (see MiiDetailPanel::Update()'s own comment)
    // a little shorter. Disabled again at shutdown, below - see that call's
    // own comment for why that's not strictly required either, just polite.
    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);
    if (isNew3DS) osSetSpeedupEnable(true);
    CtrLog::Printf("New3DS: %s", isNew3DS ? "yes (speedup enabled)" : "no");

    gfxInitDefault();
    // gfxSet3D(true) is deliberately *not* called here - it's what actually
    // lights the 3D LED, and the loading screen has no stereo content worth
    // that (it's drawn flat, single-eye - see below). Enabled later, right
    // before the main loop, once the real list/portrait UI is what's about
    // to show.
    romfsInit();
    CtrLog::Printf("gfx/romfs initialized");

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    CtrLog::Printf("citro3d/citro2d initialized");

    C3D_RenderTarget *topTargetLeft = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *topTargetRight = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    C3D_RenderTarget *bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    CtrLog::Printf("screen targets created");

    CtrUi::Init();
    CtrLog::Printf("CtrUi initialized");

    TabState tabs[TAB_COUNT];

    CtrMiiDb::LoadResult db = CtrMiiDb::Load();
    CtrLog::Printf("CFL_DB load: success=%d miis=%zu recentMiis=%zu error=%s", db.success, db.miis.size(),
                    db.recentMiis.size(), db.error.c_str());
    tabs[static_cast<int>(MiiDetailPanel::Tab::Library)].miis = std::move(db.miis);
    // Same file, no extra loading step - see CtrMiiDb::LoadResult::recentMiis's
    // own comment for what this section actually is (recently-picked Miis
    // from *other* titles, not necessarily this console's own Library).
    TabState &recentGamesTab = tabs[static_cast<int>(MiiDetailPanel::Tab::RecentGames)];
    recentGamesTab.miis = std::move(db.recentMiis);
    recentGamesTab.focusedIndex = recentGamesTab.miis.empty() ? -1 : 0;
    // Only the data - not MiiHttpServer::Start() itself, which doesn't run
    // until the user explicitly asks for it (X) - see main loop's own
    // comment on why. Safe to call before Start() per its own doc.
    MiiHttpServer::SetCflDbData(std::move(db.rawBytes));

    AnimatedBg::Load();
    std::vector<uint8_t> authorIconBytes = ReadFileFully("romfs:/author_icon.png");
    if (!authorIconBytes.empty()) g_authorIconTex = PngTexture::LoadFromMemory(authorIconBytes.data(), authorIconBytes.size());
    std::vector<uint8_t> homeIconBytes = ReadFileFully("romfs:/homeDisallowed.png");
    if (!homeIconBytes.empty()) g_homeIconTex = PngTexture::LoadFromMemory(homeIconBytes.data(), homeIconBytes.size());
    CtrLog::Printf("AnimatedBg loaded, author icon %s, home icon %s", g_authorIconTex.valid ? "loaded" : "MISSING",
                    g_homeIconTex.valid ? "loaded" : "MISSING");

    // Loads back/confirm/dpad/tab (short, stay resident all session) plus
    // bgm.wav (loaded fully into memory, ~5.8MB - freed automatically by
    // Sfx::Update() below once it finishes, since PlayBgm() never loops).
    bool sfxOk = Sfx::Init();
    CtrLog::Printf("Sfx::Init() %s", sfxOk ? "ok" : "FAILED");
    Sfx::PlayBgm();

    // MiiImageFetcher::Start() isn't for a worker queue this build actually
    // uses (that's the scrolling *list*'s thumbnails, not the portrait) -
    // it's what brings httpc up, before MiiDetailPanel's own (synchronous,
    // see its own comment) fetches use it.
    MiiImageFetcher::Start();
    CtrNetwork::BeginConnect();
    MiiDetailPanel::Init();

    // Scale the portrait cache to whatever APPLICATION memory is actually
    // free right now (every other subsystem above - AnimatedBg, the author/
    // home icons, Sfx/bgm, CFL_DB's own data - has already made its
    // allocations) rather than a single hardcoded constant. This is what
    // actually lets New3DS's extra FCRAM (meta/app.rsf's
    // SystemModeExt: 124MB, silently ignored on Old3DS and on the .3dsx
    // build - see that field's own comment) translate into a bigger cache,
    // with no New3DS-specific branch needed here: an Old3DS console or a
    // .3dsx run just reports less free memory and this naturally clamps
    // down near the old fixed value. 256KB is the per-portrait cost (see
    // MiiDetailPanel's own WINDOW_SIZE comment: each 210px portrait needs a
    // 256x256 POT texture, 256*256*4). kCacheHeadroomBytes is reserved for
    // allocations that still happen *after* this point (the bottom-screen
    // list's own thumbnails, the local HTTP server's buffers if X is ever
    // pressed, misc runtime allocations) - a safety margin, not itself
    // load-bearing math.
    constexpr u32 kBytesPerCachedPortrait = 256 * 1024;
    constexpr u32 kCacheHeadroomBytes = 4 * 1024 * 1024;
    constexpr int kMinCachedPortraits = 35; // this app's old fixed cap - never worse than before
    constexpr int kMaxCachedPortraits = 300; // this app's own addressable-Mii ceiling - see WINDOW_SIZE's comment
    u32 freeAppMem = osGetMemRegionFree(MEMREGION_APPLICATION);
    int cachedPortraitsCap = kMinCachedPortraits;
    if (freeAppMem > kCacheHeadroomBytes) {
        cachedPortraitsCap = static_cast<int>((freeAppMem - kCacheHeadroomBytes) / kBytesPerCachedPortrait);
        cachedPortraitsCap = std::max(cachedPortraitsCap, kMinCachedPortraits);
        cachedPortraitsCap = std::min(cachedPortraitsCap, kMaxCachedPortraits);
    }
    CtrLog::Printf("APPLICATION mem free=%lu KB -> portrait cache cap=%d",
                    static_cast<unsigned long>(freeAppMem / 1024), cachedPortraitsCap);
    MiiDetailPanel::SetMaxCachedPortraits(static_cast<size_t>(cachedPortraitsCap));
    CtrLog::Printf("MiiImageFetcher started, AC connect kicked off");

    // Fire-and-forget, same shape as CtrNetwork::BeginConnect() above -
    // frdInit() runs on its own background thread (see ctr_friends.h for
    // why: a stuck frd: service must never stall the render loop) and the
    // Friends tab just shows "connecting..." until CtrFriends::Poll()
    // reports an outcome (polled once per frame in the main loop below).
    CtrFriends::BeginInit();
    bool friendsLoaded = false;
    CtrLog::Printf("CtrFriends::BeginInit() kicked off");

    // Same shape, same reasoning (see ctr_account.h) - only used here for
    // the current console user's own Mii name, shown in the Friends tab's
    // header ("<name>'s Friend's Miis...").
    CtrAccount::BeginInit();
    bool accountLoaded = false;
    std::string currentUserMiiName;
    CtrLog::Printf("CtrAccount::BeginInit() kicked off");

    // MiiHttpServer::Start() is deliberately *not* called here yet - see
    // the call site further down, right before the main loop, for why.

    // The "Loading..." + initial-portrait-preload step (previously run
    // unconditionally right here) now only runs once the network gate
    // below actually confirms a connection - see RunInitialPreload() and
    // the NetworkGate handling just above the main loop.
    CtrLog::Printf("entering main loop");
    int frameCounter = 0;
    // The local HTTP server is opt-in, not always-on: it only runs while
    // the user is on the dedicated server screen (X to enter, B to leave),
    // during which the list/portrait screen and its own curl usage are
    // fully paused - see this loop's own handling below for why. Running
    // the server continuously in the background alongside the list's own
    // portrait prefetching crashed on real hardware (its accept-loop
    // thread doing real work concurrently with the main thread's own curl
    // fetches) - the same class of "two threads both doing real work"
    // failure this whole project kept running into, just from a different
    // second thread this time. This app appears to only tolerate one
    // thread actually doing anything at a time, so making the two features
    // mutually exclusive sidesteps it entirely instead of trying to time
    // them apart again.
    //
    // MiiDetails (A to enter, B to leave) doesn't have that problem at all
    // - it's just a different bottom-screen rendering of data already
    // sitting in `tabs`/`ComputeMiiDetails()`, no network activity of its
    // own - but it's still modeled as a mutually-exclusive screen (not,
    // say, an overlay on top of the list) for the same reason the server
    // screen is: one clear "what am I looking at right now" state instead
    // of combinable modes to reason about.
    //
    // NetworkGate is the startup state (not List): shown until the network
    // check just below resolves one way or the other, so the list/portrait
    // UI never comes up against a connection that was never going to work
    // (see ctr_network.h's own comment - this app's core feature needs real
    // internet, and letting the normal UI show anyway meant the portrait
    // fetch path hit real trouble a few frames in instead of failing
    // cleanly). Not a screen the user can navigate back to - once it
    // resolves to Connected it becomes List for the rest of the session.
    enum class Screen { List, MiiDetails, Server, NetworkGate };
    Screen screen = Screen::NetworkGate;
    MiiDetailPanel::Tab currentTab = MiiDetailPanel::Tab::Library;
    // What was last sent to CtrFriends::UpdateGameModeDescription() - only
    // re-sent when it actually changes (tab switch, a tab's Mii count
    // changing as FRD/ACT finish loading, or entering/leaving the Server/
    // MiiDetails screens - see the main loop's own unified description
    // block below), not every frame - this is an IPC call to the frd:
    // service, no reason to spam it 60 times a second for text that isn't
    // changing.
    std::string lastGameModeDescription;

    // "No HOME" icon animation state - one shared counter across every
    // screen (not per-screen), so an animation already playing when the
    // loading screen finishes or the server screen closes keeps running to
    // completion on whatever screen follows, instead of being cut off by
    // the transition. Triggering is still gated to only the screens that
    // actually disable HOME (via !aptIsHomeAllowed(), checked once per
    // frame below, not screen-scoped) - see that check's own comment for
    // why aptCheckHomePressRejected() alone isn't enough. RunInitialPreload()
    // captures this by reference (it's a `[&]` lambda) since it can also
    // start/continue the animation during its own loop, before the main
    // loop below even begins.
    int homeIconAnimFrame = -1;

    // See the Screen::NetworkGate comment above. IsWirelessSwitchOff()
    // answers immediately (no need to wait through BeginConnect()'s own
    // ~15s timeout just to learn the radio itself is off); otherwise the
    // main loop below polls CtrNetwork::Poll() every frame, bounded by its
    // own elapsed-time check here since Poll() itself never blocks.
    bool wirelessOff = CtrNetwork::IsWirelessSwitchOff();
    CtrLog::Printf("wireless switch: %s", wirelessOff ? "off" : "on (or undetermined)");
    u64 networkCheckStartMs = osGetTime();
    constexpr u64 kNetworkCheckTimeoutMs = 15000;
    // Distinguishes NetworkGate's two sub-states for the render block below:
    // still polling (show "Checking network connection...") vs a terminal
    // failure (show the actual error text + "Press START to exit" hint).
    bool networkCheckFailed = false;

    // The "Loading..." + initial-portrait-preload step this app always ran
    // unconditionally before the network gate existed - now runs exactly
    // once, the moment that gate resolves to Connected (see its call site
    // below). A local lambda, not a free function: it only makes sense
    // called from that one spot, with exactly these captures.
    auto RunInitialPreload = [&]() {
        TabState &libraryTab = tabs[static_cast<int>(MiiDetailPanel::Tab::Library)];
        TabState &recentTab = tabs[static_cast<int>(MiiDetailPanel::Tab::RecentGames)];
        TabState &friendsTab = tabs[static_cast<int>(MiiDetailPanel::Tab::Friends)];
        libraryTab.focusedIndex = libraryTab.miis.empty() ? -1 : 0;

        // Smart preloading: Library first (the default tab the user lands
        // on), but if it doesn't have enough Miis on its own to fill
        // INITIAL_PRELOAD_COUNT, fall through to Recent Miis (from
        // CFL_DB.dat's own embedded CFRA section - always available
        // synchronously by this point, same file Library itself came from)
        // and then Friends (FRD - may still be empty here if frdInit()
        // hasn't finished on its own background thread yet; if so it just
        // contributes nothing here, same as it always would have, and
        // those portraits fill in normally later once the user actually
        // visits that tab) - so a console with a small Mii Library still
        // gets a full cache's worth of portraits ready up-front instead of
        // stopping short.
        struct PreloadSource {
            MiiDetailPanel::Tab tab;
            std::vector<Ver3MiiDecoded> *miis;
        };
        PreloadSource sources[] = {
            {MiiDetailPanel::Tab::Library, &libraryTab.miis},
            {MiiDetailPanel::Tab::RecentGames, &recentTab.miis},
            {MiiDetailPanel::Tab::Friends, &friendsTab.miis},
        };
        bool anySource = false;
        for (const PreloadSource &src : sources) anySource = anySource || !src.miis->empty();
        if (!anySource) return;

        // The "Loading..." screen, redrawn once per preload iteration (not
        // a single static frame) so it can actually service input and show
        // a response to a HOME press - see aptSetHomeAllowed() below. This
        // trades a longer, incrementally-drawn startup pause for not
        // showing the list/portrait area gradually filling in one fetch
        // per rendered frame later - see MiiDetailPanel::Update()'s own
        // comment for why each fetch itself is still synchronous
        // (blocking, no background thread) regardless of pacing style.
        //
        // HOME disabled only for this specific window (not the network-
        // check/error screens before it, not the real app after it) - per
        // request: this is startup-only "don't interrupt the initial
        // load" protection, not a general lockout. libctru's own apt.h
        // doc comment on aptCheckHomePressRejected() is explicit that nothing
        // shows a "HOME disabled" indicator automatically - the app is
        // expected to poll it and draw its own (there's no system applet
        // involved in that specific indicator at all, despite how it might
        // look from the outside - ErrDisp/"erreula" is a different, much
        // heavier system applet for actual crash/error screens, unrelated
        // to this).
        CtrLog::Printf("drawing loading screen, HOME button disabled for its duration");
        aptSetHomeAllowed(false);

        int totalDone = 0;
        for (const PreloadSource &src : sources) {
            if (totalDone >= INITIAL_PRELOAD_COUNT) break;
            int countFromThisSource = std::min(static_cast<int>(src.miis->size()), INITIAL_PRELOAD_COUNT - totalDone);

            for (int i = 0; i < countFromThisSource && aptMainLoop(); i++) {
                hidScanInput();
                // !aptIsHomeAllowed() too, not just aptCheckHomePressRejected()
                // alone - see this check's other call site (the main loop
                // below) for why that matters.
                if (!aptIsHomeAllowed() && aptCheckHomePressRejected()) {
                    CtrLog::Printf("HOME press rejected during loading screen");
                    if (homeIconAnimFrame < 0) homeIconAnimFrame = 0; // ignored if the animation's already playing
                }

                C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
                CtrUi::BeginFrame();

                // Single-eye only (topTargetLeft) - gfxSet3D(true) isn't
                // called until this whole lambda returns (see its own call
                // site), so there's no stereo output to draw for the right
                // eye at all. "(n/N)" - a real progress count, not just a
                // spinner: each iteration is a genuinely slow blocking
                // fetch (see this lambda's own top comment), so showing
                // *something* moving forward here matters for perceived
                // responsiveness, not just actual speed.
                std::string loadingText = "Loading... (" + std::to_string(totalDone + 1) + "/" +
                                           std::to_string(INITIAL_PRELOAD_COUNT) + ")";
                float loadingW = 0.0f, loadingH = 0.0f;
                CtrUi::MeasureText(LOADING_SCALE, loadingText.c_str(), &loadingW, &loadingH);
                C2D_TargetClear(topTargetLeft, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
                C2D_SceneBegin(topTargetLeft);
                AnimatedBg::Draw(TOP_W, TOP_H, 0.0f);
                CtrUi::DrawText((TOP_W - loadingW) / 2.0f, (TOP_H - loadingH) / 2.0f, LOADING_SCALE,
                                 C2D_Color32(255, 255, 255, 255), loadingText.c_str());

                C2D_TargetClear(bottomTarget, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
                C2D_SceneBegin(bottomTarget);
                AnimatedBg::Draw(BOTTOM_W, BOTTOM_H, BOTTOM_WORLD_OFFSET_X);
                DrawHomeIconOverlay(homeIconAnimFrame);

                C3D_FrameEnd(0);

                MiiDetailPanel::Update(src.tab, *src.miis, 0);
                totalDone++;
            }
        }

        aptSetHomeAllowed(true);
        CtrLog::Printf("initial preload done (%d requested across sources), HOME button re-enabled", totalDone);
    };

    while (aptMainLoop()) {
        frameCounter++;

        // Only does anything while the BGM track is still playing - once it
        // finishes, this is what frees its decoded PCM (see Sfx::Update()'s
        // own comment).
        Sfx::Update();

        // One-shot: the moment frdInit() finishes (possibly several seconds
        // in, possibly never - see ctr_friends.h), pull the friend list and
        // Miis once, synchronously (LoadFriendMiis() is documented as a
        // purely local call - no network round-trip, safe to call directly
        // here rather than needing its own background thread). Checked
        // every frame, not gated on which screen/tab is active, so the
        // Friends tab is already populated by the time the user switches to
        // it rather than needing them to "wake it up" first.
        if (!friendsLoaded && CtrFriends::Poll() == CtrFriends::State::Connected) {
            TabState &friendsTab = tabs[static_cast<int>(MiiDetailPanel::Tab::Friends)];
            friendsTab.miis = CtrFriends::LoadFriendMiis();
            friendsTab.focusedIndex = friendsTab.miis.empty() ? -1 : 0;
            MiiHttpServer::SetFriendMiiData(BuildRawStoreDataBlob(friendsTab.miis));
            friendsLoaded = true;
            CtrLog::Printf("friends loaded: %zu Mii(s)", friendsTab.miis.size());
        }

        // Same one-shot idea as the friends load above - just needs the
        // current user's own Mii nickname for the Friends tab header, not
        // the whole console Mii list.
        if (!accountLoaded && CtrAccount::Poll() == CtrAccount::State::Connected) {
            int currentUserIndex = -1;
            std::vector<Ver3MiiDecoded> consoleMiis = CtrAccount::LoadConsoleMiis(&currentUserIndex);
            if (currentUserIndex >= 0) currentUserMiiName = consoleMiis[static_cast<size_t>(currentUserIndex)].nickname;
            accountLoaded = true;
            CtrLog::Printf("account loaded: current user Mii name \"%s\"", currentUserMiiName.c_str());
        }

        // NetworkGate resolution - see Screen::NetworkGate's own comment.
        // A still-Connecting Poll() here just means keep showing the
        // "Checking network connection..." frame below and try again next
        // frame, until either it resolves or kNetworkCheckTimeoutMs passes.
        if (screen == Screen::NetworkGate) {
            // Re-checked every frame while this screen is up (not just
            // once, before the loop) specifically so turning the wireless
            // switch back on while the "please enable wireless" error is
            // showing can retry automatically, below - the switch's own
            // physical state is cheap to query and can change at any time,
            // independent of whatever BeginConnect()'s own attempt is doing.
            bool nowWirelessOff = CtrNetwork::IsWirelessSwitchOff();
            if (networkCheckFailed && wirelessOff && !nowWirelessOff) {
                CtrLog::Printf("wireless switch back on - retrying connection");
                wirelessOff = false;
                networkCheckFailed = false;
                networkCheckStartMs = osGetTime();
                CtrNetwork::BeginConnect(); // safe to call again - see its own comment
            } else {
                wirelessOff = nowWirelessOff;
            }

            if (!networkCheckFailed) {
                CtrNetwork::State netState = wirelessOff ? CtrNetwork::State::Failed : CtrNetwork::Poll();
                bool timedOut = !wirelessOff && netState == CtrNetwork::State::Connecting &&
                                 (osGetTime() - networkCheckStartMs) >= kNetworkCheckTimeoutMs;
                if (netState == CtrNetwork::State::Connected) {
                    CtrLog::Printf("network check: connected");
                    RunInitialPreload();
                    // Only now, right before the real list/portrait UI is
                    // what's about to show - not during any of the screens
                    // before it (see gfxInitDefault()'s own comment on why) -
                    // since this is what actually lights the 3D LED.
                    gfxSet3D(true);
                    screen = Screen::List;
                } else if (netState == CtrNetwork::State::Failed || timedOut) {
                    CtrLog::Printf("network check: failed (wirelessOff=%d timedOut=%d) - showing blocking error screen",
                                    wirelessOff, timedOut);
                    networkCheckFailed = true; // stays on Screen::NetworkGate - the render block below shows the terminal error text
                }
            }
        }

        hidScanInput();
        u32 down = hidKeysDown();
        // down|repeatKeys (not repeatKeys alone): guarantees a single tap
        // registers immediately regardless of hidKeysDownRepeat()'s own
        // initial-delay timing, while still allowing held-key auto-repeat.
        u32 repeatKeys = hidKeysDownRepeat() | down;
        // START only exits the app from NetworkGate's own terminal error
        // state (no network - "Press START to exit" is the hint actually
        // shown there) - not from the normal list/portrait UI, Mii
        // details, the server screen, or even NetworkGate's own transient
        // "Checking network connection..." sub-state. Everywhere else,
        // HOME (system-level, suspends/closes via the Home Menu - already
        // the only way out of the Server/Loading screens specifically,
        // since those disable HOME themselves only for their own duration)
        // is how the app is meant to be closed, not a dedicated in-app
        // button - avoids an accidental START press quitting outright
        // during normal use, with no confirmation, unlike leaving any
        // other screen in this app (all of which need an explicit
        // B/Touch).
        if ((down & KEY_START) && screen == Screen::NetworkGate && networkCheckFailed) break;

        // "No HOME" icon trigger - unconditional, not screen-gated: relies
        // entirely on !aptIsHomeAllowed() to only ever fire on the screens
        // that actually disable it (currently just Server; the loading
        // screen has its own equivalent inside RunInitialPreload(), before
        // this loop even starts). aptCheckHomePressRejected() alone isn't
        // enough here - confirmed on real hardware that it can report a
        // rejected press even while HOME is *currently* allowed (most
        // likely a stale/queued event from a previous disallowed window
        // that hadn't been polled yet), which without this extra guard
        // showed the icon on a plain, legitimate HOME press. See
        // homeIconAnimFrame's own comment for why this is a single shared
        // counter rather than one per screen.
        if (!aptIsHomeAllowed() && aptCheckHomePressRejected()) {
            CtrLog::Printf("HOME press rejected");
            if (homeIconAnimFrame < 0) homeIconAnimFrame = 0; // ignored if the animation's already playing
        }

        if (screen == Screen::Server) {
            // KEY_TOUCH too, not just KEY_B - this screen has nothing else
            // touch-interactive on it, so a tap anywhere is unambiguous -
            // see the hint text drawn on it (B icon + "/Touch: Cancel").
            if (down & (KEY_B | KEY_TOUCH)) {
                CtrLog::Printf("leaving server mode");
                MiiHttpServer::Stop();
                gfxSet3D(true);
                aptSetHomeAllowed(true); // no need to keep HOME locked once we're leaving
                screen = Screen::List;
                Sfx::Play(Sfx::Sound::Back);
            }
        } else if (screen == Screen::MiiDetails) {
            // Same reasoning as the server screen above.
            if (down & (KEY_B | KEY_TOUCH)) {
                screen = Screen::List;
                Sfx::Play(Sfx::Sound::Back);
            }
        } else if (screen == Screen::NetworkGate) {
            // No input to handle here beyond the KEY_START check above
            // (which only actually does anything once networkCheckFailed
            // is true) - the gate itself resolves on its own each frame,
            // above.
        } else {
            // 3 tabs (Library/Friends/RecentGames), cycled with wraparound.
            // Tab/scroll/focus state lives in `tabs`, indexed by the tab
            // itself, so switching is instant and each tab's own scroll
            // position survives the round trip.
            if (down & KEY_R) {
                currentTab = static_cast<MiiDetailPanel::Tab>((static_cast<int>(currentTab) + 1) % TAB_COUNT);
                Sfx::Play(Sfx::Sound::Tab);
            } else if (down & KEY_L) {
                currentTab =
                    static_cast<MiiDetailPanel::Tab>((static_cast<int>(currentTab) + TAB_COUNT - 1) % TAB_COUNT);
                Sfx::Play(Sfx::Sound::Tab);
            }
            TabState &activeTab = tabs[static_cast<int>(currentTab)];

            if (!activeTab.miis.empty()) {
                int lastIndex = static_cast<int>(activeTab.miis.size()) - 1;
                int prevFocusedIndex = activeTab.focusedIndex;
                // Clamped, not wrapped: down at the last entry / up at the
                // first entry simply does nothing rather than jumping to
                // the other end.
                if (repeatKeys & KEY_UP) activeTab.focusedIndex = std::max(0, activeTab.focusedIndex - 1);
                if (repeatKeys & KEY_DOWN) activeTab.focusedIndex = std::min(lastIndex, activeTab.focusedIndex + 1);
                // Only when focus actually moved - not on a KEY_UP/KEY_DOWN
                // press that did nothing because focus was already at that
                // end of the list.
                if (activeTab.focusedIndex != prevFocusedIndex) Sfx::Play(Sfx::Sound::Dpad);
                if (activeTab.focusedIndex < activeTab.scrollOffset) activeTab.scrollOffset = activeTab.focusedIndex;
                if (activeTab.focusedIndex >= activeTab.scrollOffset + VISIBLE_ROWS)
                    activeTab.scrollOffset = activeTab.focusedIndex - VISIBLE_ROWS + 1;
                activeTab.scrollOffset = std::clamp(activeTab.scrollOffset, 0,
                                                     std::max(0, static_cast<int>(activeTab.miis.size()) - VISIBLE_ROWS));
            }

            // Touch: a fresh press only (down & KEY_TOUCH), not held/drag -
            // this app has no press-and-drag scrolling, just discrete taps.
            // See this file's own top comment for the overall behavior.
            if ((down & KEY_TOUCH) && !activeTab.miis.empty()) {
                touchPosition touch;
                hidTouchRead(&touch);
                float tx = static_cast<float>(touch.px);
                float ty = static_cast<float>(touch.py);

                bool canScrollUp = activeTab.scrollOffset > 0;
                bool canScrollDown = activeTab.scrollOffset + VISIBLE_ROWS < static_cast<int>(activeTab.miis.size());
                int lastIndex = static_cast<int>(activeTab.miis.size()) - 1;

                bool inTopArrow = tx >= SCROLL_ARROW_X - SCROLL_ARROW_HIT_HALF_W &&
                                   tx <= SCROLL_ARROW_X + SCROLL_ARROW_HIT_HALF_W &&
                                   ty >= SCROLL_ARROW_TOP_Y - SCROLL_ARROW_HIT_HALF_H &&
                                   ty <= SCROLL_ARROW_TOP_Y + SCROLL_ARROW_HIT_HALF_H;
                bool inBottomArrow = tx >= SCROLL_ARROW_X - SCROLL_ARROW_HIT_HALF_W &&
                                      tx <= SCROLL_ARROW_X + SCROLL_ARROW_HIT_HALF_W &&
                                      ty >= SCROLL_ARROW_BOTTOM_Y - SCROLL_ARROW_HIT_HALF_H &&
                                      ty <= SCROLL_ARROW_BOTTOM_Y + SCROLL_ARROW_HIT_HALF_H;

                if (canScrollUp && inTopArrow) {
                    // A full VISIBLE_ROWS page at once, not one row at a
                    // time like a single D-pad press - lands focus at the
                    // new page's own top row.
                    activeTab.scrollOffset = std::max(0, activeTab.scrollOffset - VISIBLE_ROWS);
                    activeTab.focusedIndex = std::min(activeTab.scrollOffset, lastIndex);
                    Sfx::Play(Sfx::Sound::Dpad);
                } else if (canScrollDown && inBottomArrow) {
                    activeTab.scrollOffset = std::min(std::max(0, static_cast<int>(activeTab.miis.size()) - VISIBLE_ROWS),
                                                        activeTab.scrollOffset + VISIBLE_ROWS);
                    activeTab.focusedIndex = std::min(activeTab.scrollOffset, lastIndex);
                    Sfx::Play(Sfx::Sound::Dpad);
                } else if (ty >= static_cast<float>(LIST_TOP)) {
                    // Row hit-test - same visible range the render code
                    // below computes (scrollOffset..endRow), full row width
                    // (not just the name text's own measured width) so the
                    // tap target is generous.
                    int endRow = std::min(static_cast<int>(activeTab.miis.size()), activeTab.scrollOffset + VISIBLE_ROWS);
                    for (int i = activeTab.scrollOffset; i < endRow; i++) {
                        float rowTop = static_cast<float>(LIST_TOP + (i - activeTab.scrollOffset) * ROW_HEIGHT);
                        if (ty >= rowTop && ty < rowTop + static_cast<float>(ROW_HEIGHT)) {
                            if (i == activeTab.focusedIndex) {
                                // Already focused - tapping it again is the
                                // same as KEY_A.
                                screen = Screen::MiiDetails;
                                Sfx::Play(Sfx::Sound::Confirm);
                            } else {
                                // Same as moving the D-pad cursor here -
                                // MiiDetailPanel::Update() below picks up
                                // the new focusedIndex this same frame,
                                // requesting/showing its portrait exactly
                                // like a D-pad move would.
                                activeTab.focusedIndex = i;
                                Sfx::Play(Sfx::Sound::Dpad);
                            }
                            break;
                        }
                    }
                }
            }
            // Every frame, not just on a focus change: MiiDetailPanel's own
            // prefetch window is paced to one fetch per call, so it needs
            // to keep being called even while focus sits still in order to
            // actually finish filling the window - see its own comment.
            // Skipped entirely once another screen is entered below
            // (nothing here starts a *new* fetch after that point - any
            // fetch this call itself performs is synchronous and already
            // finished by the time execution reaches the KEY_X/KEY_A
            // checks, so there's never one left "in flight" to cancel).
            MiiDetailPanel::Update(currentTab, activeTab.miis, activeTab.focusedIndex);

            if (down & KEY_X) {
                CtrLog::Printf("entering server mode, HOME button disabled while it's up");
                screen = Screen::Server;
                gfxSet3D(false);
                MiiHttpServer::Start(HTTP_SERVER_PORT);
                aptSetHomeAllowed(false);
                Sfx::Play(Sfx::Sound::Confirm);
            } else if ((down & KEY_A) && activeTab.focusedIndex >= 0) {
                screen = Screen::MiiDetails;
                Sfx::Play(Sfx::Sound::Confirm);
            }
        }

        // One place for every screen's game mode description, computed
        // fresh from `screen`/`currentTab` every frame rather than set once
        // on entry and separately reverted on exit - a screen transition
        // naturally "restores" the right text just by falling through to
        // whichever branch matches the new screen, no separate restore step
        // to keep in sync with the enter/leave logic above. Gated on
        // Connected, not just attempted every frame regardless:
        // UpdateGameModeDescription() is a harmless no-op before frdInit()
        // succeeds (see its own comment), but if lastGameModeDescription got
        // updated to reflect an attempt that was actually silently dropped,
        // the real first send once frdInit() *does* succeed would be
        // skipped too - the text wouldn't have "changed" from what's
        // already (wrongly) marked as last-sent.
        {
            TabState &descTab = tabs[static_cast<int>(currentTab)];
            std::string gameModeDescription;
            if (screen == Screen::Server) {
                gameModeDescription = "Hosting a local Mii server";
            } else if (screen == Screen::MiiDetails) {
                std::string name = (descTab.focusedIndex >= 0 && descTab.focusedIndex < static_cast<int>(descTab.miis.size()))
                                        ? descTab.miis[static_cast<size_t>(descTab.focusedIndex)].nickname
                                        : "";
                if (name.empty()) name = "(no name)";
                gameModeDescription = "Viewing details of the\n\"" + name + "\" Mii";
            } else if (screen == Screen::NetworkGate) {
                // Nothing meaningful to report yet - this state resolves
                // before the user ever reaches a real tab (see
                // Screen::NetworkGate's own comment), and FRD very likely
                // isn't Connected yet either at this point regardless.
                gameModeDescription = "Waiting for network connection";
            } else {
                gameModeDescription = GameModeDescriptionText(currentTab, descTab.miis.size());
            }

            if (CtrFriends::Poll() == CtrFriends::State::Connected && gameModeDescription != lastGameModeDescription) {
                CtrFriends::UpdateGameModeDescription(gameModeDescription);
                lastGameModeDescription = gameModeDescription;
            }
        }

        bool logThisFrame = frameCounter <= 5;
        // SYNCDRAW (not NONBLOCK): NONBLOCK was tried earlier under the
        // theory that it fixed a hang - it didn't (the real cause was
        // logging I/O overhead, since fixed - see ctr_log.cpp) - and left
        // unused, it let the loop spin as fast as the CPU allows whenever a
        // frame got skipped, with nothing pacing it to the display's actual
        // refresh rate. That meant a single physical button tap could get
        // sampled by hidScanInput() dozens of times in an instant, each one
        // counted as its own repeat pulse - the actual cause of D-pad
        // presses jumping straight to the end of the list instead of
        // moving one row. SYNCDRAW blocks until the GPU can actually
        // present the next frame, which throttles the whole loop
        // (including input polling) to a sane, consistent rate - the
        // standard citro3d usage pattern for exactly this reason.
        if (logThisFrame) CtrLog::Printf("frame %d: calling C3D_FrameBegin()", frameCounter);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        if (logThisFrame) CtrLog::Printf("frame %d: C3D_FrameBegin() returned", frameCounter);
        CtrUi::BeginFrame();

        if (screen == Screen::Server) {
            // Single-eye only - gfxSet3D(false) was called on entry, so
            // there's no right-eye target to draw into here.
            auto [serverLine1, serverLine2] = HttpServerStatusLines();
            float line1W = 0.0f, line1H = 0.0f;
            CtrUi::MeasureText(SERVER_LABEL_SCALE, serverLine1.c_str(), &line1W, &line1H);
            float centerY = static_cast<float>(TOP_H) / 2.0f;

            C2D_TargetClear(topTargetLeft, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(topTargetLeft);
            AnimatedBg::Draw(TOP_W, TOP_H, 0.0f);
            CtrUi::DrawText((static_cast<float>(TOP_W) - line1W) / 2.0f, centerY - line1H - 4.0f,
                             SERVER_LABEL_SCALE, C2D_Color32(255, 255, 255, 255), serverLine1.c_str());
            // serverLine2 can be a multi-line error (e.g. "Could not
            // determine this\nconsole's IP address.") - centered per line,
            // not just as one left-aligned block, see
            // DrawTextCenteredMultiline()'s own comment.
            DrawTextCenteredMultiline(static_cast<float>(TOP_W) / 2.0f, centerY + 4.0f, SERVER_ADDRESS_SCALE,
                                       C2D_Color32(255, 255, 255, 255), serverLine2);
            if (logThisFrame) CtrLog::Printf("frame %d: server-mode top screen drawn", frameCounter);

            C2D_TargetClear(bottomTarget, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(bottomTarget);
            AnimatedBg::Draw(BOTTOM_W, BOTTOM_H, BOTTOM_WORLD_OFFSET_X);
            float hintW = 0.0f, hintH = 0.0f;
            // "\xEE\x80\x81" is U+E001, the 3DS shared system font's own B
            // button icon glyph (Private Use Area codepoint, confirmed
            // byte-for-byte against a user-supplied reference dump - not
            // guessed) - embedded as an explicit UTF-8 escape rather than a
            // literal character since editors/chat pipelines have
            // repeatedly mangled/stripped this glyph when pasted as-is.
            const char *hintText = "\xEE\x80\x81/Touch: Cancel";
            CtrUi::MeasureText(BOTTOM_HINT_SCALE, hintText, &hintW, &hintH);
            CtrUi::DrawText((static_cast<float>(BOTTOM_W) - hintW) / 2.0f, (static_cast<float>(BOTTOM_H) - hintH) / 2.0f,
                             BOTTOM_HINT_SCALE, C2D_Color32(200, 200, 200, 255), hintText);
        } else if (screen == Screen::MiiDetails) {
            // Top screen: the same live stereo portrait as the normal list
            // view, unchanged - focus doesn't move while viewing details,
            // and the focused Mii's portrait is already cached from just
            // having been on it in the list, so there's nothing new to
            // fetch or cancel here (unlike entering server mode).
            float depthPx = osGet3DSliderState() * MAX_3D_DEPTH_PX;
            C2D_TargetClear(topTargetLeft, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(topTargetLeft);
            AnimatedBg::Draw(TOP_W, TOP_H, 0.0f);
            MiiDetailPanel::Draw(depthPx / 2.0f);

            C2D_TargetClear(topTargetRight, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(topTargetRight);
            AnimatedBg::Draw(TOP_W, TOP_H, 0.0f);
            MiiDetailPanel::Draw(-depthPx / 2.0f);

            C2D_TargetClear(bottomTarget, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(bottomTarget);
            AnimatedBg::Draw(BOTTOM_W, BOTTOM_H, BOTTOM_WORLD_OFFSET_X);

            TabState &activeTab = tabs[static_cast<int>(currentTab)];
            const Ver3MiiDecoded &mii = activeTab.miis[static_cast<size_t>(activeTab.focusedIndex)];
            MiiDetails details = ComputeMiiDetails(mii);

            float y = DETAIL_TOP;
            float w = 0.0f, h = 0.0f;

            CtrUi::MeasureText(DETAIL_NAME_SCALE, mii.nickname.c_str(), &w, &h);
            CtrUi::DrawText(DETAIL_LEFT, y, DETAIL_NAME_SCALE, C2D_Color32(255, 255, 255, 255),
                             mii.nickname.c_str());
            y += h + DETAIL_LINE_GAP;

            // Author row: the small pencil icon (loaded once at startup -
            // see g_authorIconTex's own comment), sized to match the
            // creator-name text's own line height, then the name itself.
            CtrUi::MeasureText(DETAIL_AUTHOR_SCALE, mii.creatorName.c_str(), &w, &h);
            float authorRowX = DETAIL_LEFT;
            if (g_authorIconTex.valid) {
                float iconScale = h / static_cast<float>(g_authorIconTex.image.subtex->height);
                float iconW = static_cast<float>(g_authorIconTex.image.subtex->width) * iconScale;
                C2D_DrawImageAt(g_authorIconTex.image, authorRowX, y, 0.0f, nullptr, iconScale, iconScale);
                authorRowX += iconW + 5.0f;
            }
            CtrUi::DrawText(authorRowX, y, DETAIL_AUTHOR_SCALE, Gray(), mii.creatorName.c_str());
            y += h + DETAIL_SECTION_GAP;

            // Label (gray) + value (white) on one row, in the same order as
            // the Wii U sibling build's own detail screen - see
            // ComputeMiiDetails()'s own comment (ver3_mii.h) for what each
            // field actually means and how it's derived from the Mii's own
            // CreateID. There is no MAC address field: Ver3StoreData/
            // CreateID only ever stores a checksum of 3 MAC bytes for
            // Wii(RVL)-origin Miis specifically, not a recoverable address,
            // and CTR/NTR (3DS/Wii U)-made Miis don't use that scheme at
            // all - createIdHex is a CreateID, not a MAC, and is labeled
            // accordingly below.
            auto drawInfoLine = [&](const char *label, const std::string &value) {
                std::string labelText = std::string(label) + ": ";
                float labelW = 0.0f, labelH = 0.0f;
                CtrUi::MeasureText(DETAIL_LINE_SCALE, labelText.c_str(), &labelW, &labelH);
                CtrUi::DrawText(DETAIL_LEFT, y, DETAIL_LINE_SCALE, Gray(), labelText.c_str());
                CtrUi::DrawText(DETAIL_LEFT + labelW, y, DETAIL_LINE_SCALE, C2D_Color32(255, 255, 255, 255),
                                 value.c_str());
                y += labelH + DETAIL_LINE_GAP;
            };
            drawInfoLine("Gender", details.gender);
            drawInfoLine("Birthday", details.birthday);
            drawInfoLine("Favorite", details.isFavorite ? "Yes" : "No");
            drawInfoLine("Created", details.createdDate);
            drawInfoLine("Made on", details.madeOn);
            drawInfoLine("Create ID", details.createIdHex);
            drawInfoLine("Copying enabled", details.copyingEnabled ? "Yes" : "No");
            drawInfoLine("Sharing allowed", details.sharingAllowed ? "Yes" : "No");
            drawInfoLine("Special Mii", details.isSpecial ? "Yes" : "No");

            float hintW = 0.0f, hintH = 0.0f;
            // "\xEE\x80\x81" is U+E001, the 3DS shared system font's own B
            // button icon glyph (Private Use Area codepoint, confirmed
            // byte-for-byte against a user-supplied reference dump - not
            // guessed) - embedded as an explicit UTF-8 escape rather than a
            // literal character since editors/chat pipelines have
            // repeatedly mangled/stripped this glyph when pasted as-is.
            const char *hintText = "\xEE\x80\x81/Touch: Cancel";
            CtrUi::MeasureText(BOTTOM_HINT_SCALE, hintText, &hintW, &hintH);
            CtrUi::DrawText((static_cast<float>(BOTTOM_W) - hintW) / 2.0f, static_cast<float>(BOTTOM_H) - hintH - 4.0f,
                             BOTTOM_HINT_SCALE, C2D_Color32(200, 200, 200, 255), hintText);
        } else if (screen == Screen::NetworkGate) {
            // Single-eye only (topTargetLeft) - gfxSet3D(true) isn't called
            // until this gate resolves to Connected (see its own comment),
            // so there's no stereo output to draw for the right eye at all,
            // same reasoning as the "Loading..." screen this state precedes.
            C2D_TargetClear(topTargetLeft, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(topTargetLeft);
            AnimatedBg::Draw(TOP_W, TOP_H, 0.0f);

            if (!networkCheckFailed) {
                float w = 0.0f, h = 0.0f;
                CtrUi::MeasureText(LOADING_SCALE, "Checking network connection...", &w, &h);
                CtrUi::DrawText((static_cast<float>(TOP_W) - w) / 2.0f, (static_cast<float>(TOP_H) - h) / 2.0f,
                                 LOADING_SCALE, C2D_Color32(255, 255, 255, 255), "Checking network connection...");
            } else {
                // wirelessOff vs "switch is on but no internet" - see
                // CtrNetwork::IsWirelessSwitchOff()'s own comment for how
                // these are told apart.
                const char *message = wirelessOff
                    ? "Please enable wireless\ncommunications to use\nthis app."
                    : "Please make sure to have\nset up an internet connection\nbefore using this app.";
                // Approximate vertical centering for a 3-line block at
                // NETWORK_ERROR_SCALE (system font glyph height is 30px at
                // scale 1.0) - DrawTextCenteredMultiline's own y is the top
                // of the first line, not the block's overall center.
                float blockH = 3.0f * 30.0f * NETWORK_ERROR_SCALE;
                DrawTextCenteredMultiline(static_cast<float>(TOP_W) / 2.0f, (static_cast<float>(TOP_H) - blockH) / 2.0f,
                                           NETWORK_ERROR_SCALE, C2D_Color32(255, 255, 255, 255), message);
            }

            C2D_TargetClear(bottomTarget, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(bottomTarget);
            AnimatedBg::Draw(BOTTOM_W, BOTTOM_H, BOTTOM_WORLD_OFFSET_X);
            if (networkCheckFailed) {
                float hintW = 0.0f, hintH = 0.0f;
                const char *hintText = "Press START to exit";
                CtrUi::MeasureText(BOTTOM_HINT_SCALE, hintText, &hintW, &hintH);
                CtrUi::DrawText((static_cast<float>(BOTTOM_W) - hintW) / 2.0f, (static_cast<float>(BOTTOM_H) - hintH) / 2.0f,
                                 BOTTOM_HINT_SCALE, C2D_Color32(200, 200, 200, 255), hintText);
            }
        } else {
            // Stereoscopic: the portrait is drawn twice, once per eye,
            // shifted in opposite directions by an amount driven by the
            // console's own 3D slider (0 = off, matching the depth math
            // naturally collapsing to eyeOffsetPx=0 - no separate "3D
            // disabled" branch needed). AnimatedBg is drawn identically
            // (zero offset) to both eyes so it stays flat - only the
            // portrait gets depth.
            float depthPx = osGet3DSliderState() * MAX_3D_DEPTH_PX;
            float versionW = 0.0f, versionH = 0.0f;
            CtrUi::MeasureText(TOP_VERSION_SCALE, APP_VERSION_TEXT, &versionW, &versionH);
            float versionX = static_cast<float>(TOP_W) - versionW - TOP_VERSION_MARGIN_X;

            C2D_TargetClear(topTargetLeft, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(topTargetLeft);
            AnimatedBg::Draw(TOP_W, TOP_H, 0.0f);
            MiiDetailPanel::Draw(depthPx / 2.0f);
            CtrUi::DrawText(TOP_HINT_X, TOP_HINT_Y, TOP_HINT_SCALE, C2D_Color32(255, 255, 255, 255),
                             "\xEE\x80\x82: Host local Mii server  \xEE\x81\xB3: Return to HOME Menu");
            CtrUi::DrawText(versionX, TOP_VERSION_Y, TOP_VERSION_SCALE, C2D_Color32(255, 255, 255, 255), APP_VERSION_TEXT);
            if (MiiDetailPanel::LastFetchFailed()) {
                CtrUi::DrawText(TOP_HINT_X, TOP_FETCH_ERROR_Y, TOP_FETCH_ERROR_SCALE, C2D_Color32(255, 70, 70, 255),
                                 "Could not download Mii image - check your internet connection.");
            }

            C2D_TargetClear(topTargetRight, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(topTargetRight);
            AnimatedBg::Draw(TOP_W, TOP_H, 0.0f);
            MiiDetailPanel::Draw(-depthPx / 2.0f);
            CtrUi::DrawText(TOP_HINT_X, TOP_HINT_Y, TOP_HINT_SCALE, C2D_Color32(255, 255, 255, 255),
                             "\xEE\x80\x82: Host local Mii server  \xEE\x81\xB3: Return to HOME Menu");
            CtrUi::DrawText(versionX, TOP_VERSION_Y, TOP_VERSION_SCALE, C2D_Color32(255, 255, 255, 255), APP_VERSION_TEXT);
            if (MiiDetailPanel::LastFetchFailed()) {
                CtrUi::DrawText(TOP_HINT_X, TOP_FETCH_ERROR_Y, TOP_FETCH_ERROR_SCALE, C2D_Color32(255, 70, 70, 255),
                                 "Could not download Mii image - check your internet connection.");
            }
            if (logThisFrame) CtrLog::Printf("frame %d: top screen (both eyes) drawn", frameCounter);

            C2D_TargetClear(bottomTarget, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(bottomTarget);
            AnimatedBg::Draw(BOTTOM_W, BOTTOM_H, BOTTOM_WORLD_OFFSET_X);
            if (logThisFrame) CtrLog::Printf("frame %d: AnimatedBg::Draw(bottom) returned", frameCounter);

            TabState &activeTab = tabs[static_cast<int>(currentTab)];
            CtrUi::DrawTextWrapped(static_cast<float>(LIST_LEFT), TAB_HEADER_Y, TAB_HEADER_SCALE, Gray(),
                                    TAB_HEADER_WRAP_WIDTH, TabHeaderText(currentTab, activeTab, currentUserMiiName).c_str());

            if (activeTab.miis.empty()) {
                const char *emptyText = "No Miis found.";
                if (currentTab == MiiDetailPanel::Tab::Friends) {
                    switch (CtrFriends::Poll()) {
                        case CtrFriends::State::Connecting:
                            emptyText = "Connecting to friend service...";
                            break;
                        case CtrFriends::State::Failed:
                            emptyText = "Friend service unavailable.";
                            break;
                        case CtrFriends::State::Connected:
                        default:
                            emptyText = "No friends found.";
                            break;
                    }
                } else if (currentTab == MiiDetailPanel::Tab::RecentGames) {
                    // Always known synchronously (loaded from CFL_DB.dat
                    // alongside the Library tab, no FRD-style background
                    // connect step) - this section is just often actually
                    // empty on consoles that haven't used a Mii-picker
                    // feature (friend requests, NPC selection, etc.) in
                    // another title yet.
                    emptyText = "No recent Miis from other games found.";
                }
                CtrUi::DrawText(LIST_LEFT, LIST_TOP, NAME_SCALE, C2D_Color32(200, 200, 200, 255), emptyText);
            } else {
                int endRow = std::min(static_cast<int>(activeTab.miis.size()), activeTab.scrollOffset + VISIBLE_ROWS);
                for (int i = activeTab.scrollOffset; i < endRow; i++) {
                    int y = LIST_TOP + (i - activeTab.scrollOffset) * ROW_HEIGHT;
                    if (i == activeTab.focusedIndex) DrawCursorTriangle(y);
                    CtrUi::DrawText(static_cast<float>(LIST_LEFT), static_cast<float>(y), NAME_SCALE,
                                     C2D_Color32(255, 255, 255, 255), activeTab.miis[static_cast<size_t>(i)].nickname.c_str());
                }
                if (activeTab.scrollOffset > 0) DrawScrollArrow(SCROLL_ARROW_X, SCROLL_ARROW_TOP_Y, false);
                if (endRow < static_cast<int>(activeTab.miis.size()))
                    DrawScrollArrow(SCROLL_ARROW_X, SCROLL_ARROW_BOTTOM_Y, true);
            }
        }

        // "No HOME" icon overlay - drawn last, on top of whatever the
        // active screen's own bottom-screen content just rendered, and
        // unconditionally (every screen ends its own rendering with
        // bottomTarget already the active scene, so no need to re-select
        // it here). Deliberately *not* gated on `screen` the way triggering
        // itself is (see that check's own comment) - an animation already
        // in progress when the loading screen finishes or the server
        // screen closes keeps playing to completion on whatever screen
        // follows, rather than being cut off by the transition.
        DrawHomeIconOverlay(homeIconAnimFrame);
        if (logThisFrame) CtrLog::Printf("frame %d: list drawn, calling C3D_FrameEnd()", frameCounter);

        C3D_FrameEnd(0);
        if (logThisFrame) CtrLog::Printf("frame %d: C3D_FrameEnd() returned", frameCounter);
        // Diagnostic only, temporarily tightened from every 300 frames to
        // every 30 (still low volume - one line per ~0.5s, nowhere near
        // the per-draw-call volume that self-inflicted this app's own
        // worst hang way earlier in its history) - narrows an on-device
        // crash's actual timing down to within half a second instead of
        // five, and the extra state (screen/tab/focus/LastFetchFailed())
        // gives a snapshot of what the app was actually looking at right
        // up to the moment logging stops.
        if (frameCounter % 30 == 0) {
            CtrLog::Printf("frame %d done (screen=%d tab=%d focus=%d lastFetchFailed=%d)", frameCounter,
                            static_cast<int>(screen), static_cast<int>(currentTab),
                            tabs[static_cast<int>(currentTab)].focusedIndex, MiiDetailPanel::LastFetchFailed());
            LogHeapUsage("periodic");
        }
    }

    CtrLog::Printf("shutting down");

    // Not strictly required - the kernel resets clock/L2 cache state on its
    // own once this process actually exits - but restoring it explicitly
    // here means anything that runs between C3D_Fini() below and the actual
    // process teardown (or the Home Menu itself, immediately after) isn't
    // left running at this app's own requested clock speed.
    if (isNew3DS) osSetSpeedupEnable(false);

    // Same idea as the speedup restore above - defensive, in case the app
    // ends up exiting (aptMainLoop() itself returning false - a system-
    // triggered close, not a button press; KEY_START can't do this from
    // the server screen at all anymore, see its own comment) while
    // screen == Screen::Server, which would otherwise skip that screen's
    // own "leaving server mode" aptSetHomeAllowed(true) entirely.
    aptSetHomeAllowed(true);

    MiiDetailPanel::Shutdown();
    MiiImageFetcher::Stop();
    MiiHttpServer::Stop();
    CtrNetwork::Shutdown();
    CtrFriends::Shutdown();
    CtrAccount::Shutdown();
    AnimatedBg::Unload();
    PngTexture::Free(&g_authorIconTex);
    PngTexture::Free(&g_homeIconTex);
    Sfx::Shutdown();
    CtrUi::Shutdown();

    C2D_Fini();
    C3D_Fini();
    romfsExit();
    gfxExit();

    CtrLog::Printf("shutdown complete");
    CtrLog::Shutdown();
    if (socOk) socExit();
    if (socBuffer) free(socBuffer);
    return 0;
}
