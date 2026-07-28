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

// App version, top-right corner of the top screen, same row as the X-to-
// host reminder above. A notch bigger than TOP_HINT_SCALE (system font
// glyph height is 30px at scale 1.0, so +0.05 is +1.5px) rather than
// matching it exactly - legible on its own at a glance, not just readable
// once you already know it's there.
constexpr float TOP_VERSION_SCALE = 0.6f;
constexpr float TOP_VERSION_MARGIN_X = 8.0f;
constexpr const char *APP_VERSION_TEXT = "v1.0.0";

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

// Deliberately less than MiiDetailPanel's own WINDOW_SIZE (25, see its own
// comment for the memory budget behind that number) - this is how many
// portraits to block-load up front, behind a single "Loading..." screen,
// before ever showing the real list/portrait UI; the remaining slots up to
// WINDOW_SIZE fill in gradually, WINDOW_STEP (6) at a time, as the user
// actually scrolls, rather than making the initial freeze longer than it
// needs to be for portraits that might not even get looked at right away.
constexpr int INITIAL_PRELOAD_COUNT = 20;

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
    CtrLog::Printf("mii-transfer (floor build) starting up (soc %s)", socOk ? "ok" : "FAILED");

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
    CtrLog::Printf("AnimatedBg loaded, author icon %s", g_authorIconTex.valid ? "loaded" : "MISSING");

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
    MiiDetailPanel::SetMaxCachedPortraits(30);
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

    TabState &libraryTab = tabs[static_cast<int>(MiiDetailPanel::Tab::Library)];
    libraryTab.focusedIndex = libraryTab.miis.empty() ? -1 : 0;

    if (!libraryTab.miis.empty()) {
        // One "Loading..." frame - just AnimatedBg (kept, per request) and
        // centered text, no list/portrait yet - then block to fetch the
        // first INITIAL_PRELOAD_COUNT portraits in a tight loop, all before
        // the real UI ever shows. This trades a longer single pause at
        // startup for not showing the list/portrait area gradually filling
        // in one fetch per rendered frame - see MiiDetailPanel::Update()'s
        // own comment for why each fetch itself is still synchronous
        // (blocking, no background thread) regardless of which of these
        // two pacing styles calls it.
        CtrLog::Printf("drawing loading screen");
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        CtrUi::BeginFrame();

        // Single-eye only (topTargetLeft) - gfxSet3D(true) hasn't been
        // called yet at this point (see its own comment further down), so
        // there's no stereo output to draw for the right eye at all.
        float loadingW = 0.0f, loadingH = 0.0f;
        CtrUi::MeasureText(LOADING_SCALE, "Loading...", &loadingW, &loadingH);
        C2D_TargetClear(topTargetLeft, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
        C2D_SceneBegin(topTargetLeft);
        AnimatedBg::Draw(TOP_W, TOP_H, 0.0f);
        CtrUi::DrawText((TOP_W - loadingW) / 2.0f, (TOP_H - loadingH) / 2.0f, LOADING_SCALE,
                         C2D_Color32(255, 255, 255, 255), "Loading...");

        C2D_TargetClear(bottomTarget, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
        C2D_SceneBegin(bottomTarget);
        AnimatedBg::Draw(BOTTOM_W, BOTTOM_H, BOTTOM_WORLD_OFFSET_X);

        C3D_FrameEnd(0);
        CtrLog::Printf("loading screen drawn, preloading first %d portraits", INITIAL_PRELOAD_COUNT);

        for (int i = 0; i < INITIAL_PRELOAD_COUNT; i++) {
            MiiDetailPanel::Update(MiiDetailPanel::Tab::Library, libraryTab.miis, 0);
        }
        CtrLog::Printf("initial preload done");
    }

    // Only now, right before the real list/portrait UI is what's about to
    // show - not during the loading screen above (see gfxInitDefault()'s
    // own comment on why) - since this is what actually lights the 3D LED.
    gfxSet3D(true);
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
    enum class Screen { List, MiiDetails, Server };
    Screen screen = Screen::List;
    MiiDetailPanel::Tab currentTab = MiiDetailPanel::Tab::Library;
    // What was last sent to CtrFriends::UpdateGameModeDescription() - only
    // re-sent when it actually changes (tab switch, a tab's Mii count
    // changing as FRD/ACT finish loading, or entering/leaving the Server/
    // MiiDetails screens - see the main loop's own unified description
    // block below), not every frame - this is an IPC call to the frd:
    // service, no reason to spam it 60 times a second for text that isn't
    // changing.
    std::string lastGameModeDescription;

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

        hidScanInput();
        u32 down = hidKeysDown();
        // down|repeatKeys (not repeatKeys alone): guarantees a single tap
        // registers immediately regardless of hidKeysDownRepeat()'s own
        // initial-delay timing, while still allowing held-key auto-repeat.
        u32 repeatKeys = hidKeysDownRepeat() | down;
        if (down & KEY_START) break;

        if (screen == Screen::Server) {
            // KEY_TOUCH too, not just KEY_B - this screen has nothing else
            // touch-interactive on it, so a tap anywhere is unambiguous -
            // see the hint text drawn on it ("Press B/Touch to cancel").
            if (down & (KEY_B | KEY_TOUCH)) {
                CtrLog::Printf("leaving server mode");
                MiiHttpServer::Stop();
                gfxSet3D(true);
                screen = Screen::List;
                Sfx::Play(Sfx::Sound::Back);
            }
        } else if (screen == Screen::MiiDetails) {
            // Same reasoning as the server screen above.
            if (down & (KEY_B | KEY_TOUCH)) {
                screen = Screen::List;
                Sfx::Play(Sfx::Sound::Back);
            }
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
                CtrLog::Printf("entering server mode");
                screen = Screen::Server;
                gfxSet3D(false);
                MiiHttpServer::Start(HTTP_SERVER_PORT);
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
            const char *hintText = "Press B/Touch to cancel";
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
            const char *hintText = "Press B/Touch to cancel";
            CtrUi::MeasureText(BOTTOM_HINT_SCALE, hintText, &hintW, &hintH);
            CtrUi::DrawText((static_cast<float>(BOTTOM_W) - hintW) / 2.0f, static_cast<float>(BOTTOM_H) - hintH - 4.0f,
                             BOTTOM_HINT_SCALE, C2D_Color32(200, 200, 200, 255), hintText);
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
                             "Press X to host Mii server");
            CtrUi::DrawText(versionX, TOP_HINT_Y, TOP_VERSION_SCALE, C2D_Color32(255, 255, 255, 255), APP_VERSION_TEXT);

            C2D_TargetClear(topTargetRight, C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
            C2D_SceneBegin(topTargetRight);
            AnimatedBg::Draw(TOP_W, TOP_H, 0.0f);
            MiiDetailPanel::Draw(-depthPx / 2.0f);
            CtrUi::DrawText(TOP_HINT_X, TOP_HINT_Y, TOP_HINT_SCALE, C2D_Color32(255, 255, 255, 255),
                             "Press X to host Mii server");
            CtrUi::DrawText(versionX, TOP_HINT_Y, TOP_VERSION_SCALE, C2D_Color32(255, 255, 255, 255), APP_VERSION_TEXT);
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
        if (logThisFrame) CtrLog::Printf("frame %d: list drawn, calling C3D_FrameEnd()", frameCounter);

        C3D_FrameEnd(0);
        if (logThisFrame) CtrLog::Printf("frame %d: C3D_FrameEnd() returned", frameCounter);
        if (frameCounter % 300 == 0) CtrLog::Printf("frame %d done", frameCounter);
    }

    CtrLog::Printf("shutting down");

    // Not strictly required - the kernel resets clock/L2 cache state on its
    // own once this process actually exits - but restoring it explicitly
    // here means anything that runs between C3D_Fini() below and the actual
    // process teardown (or the Home Menu itself, immediately after) isn't
    // left running at this app's own requested clock speed.
    if (isNew3DS) osSetSpeedupEnable(false);

    MiiDetailPanel::Shutdown();
    MiiImageFetcher::Stop();
    MiiHttpServer::Stop();
    CtrNetwork::Shutdown();
    CtrFriends::Shutdown();
    CtrAccount::Shutdown();
    AnimatedBg::Unload();
    PngTexture::Free(&g_authorIconTex);
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
