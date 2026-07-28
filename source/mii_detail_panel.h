#pragma once

#include <cstddef>
#include <vector>

#include "ver3_mii.h"

// Persistent top-screen Mii portrait panel. On 3DS, the top screen has no
// touch input and nothing else competing for it, so instead of a screen you
// open, this is a live panel that always shows whichever Mii is currently
// focused in the bottom-screen list: just its 210px-tall face render
// (fetched over HTTP, transparent background, no opaque rect behind it -
// see mii_detail_panel.cpp's own comment), centered horizontally - no name/
// creator/info text here (see main.cpp's own dedicated Mii-details screen
// for that), no blink-expression variant.
//
// Usage: Init() once at startup, then SetMaxCachedPortraits() if not using
// the default. Update(tab, miis, focusedIndex) every frame with which list
// is currently on screen, its full Mii list, and which index is currently
// focused (-1/empty if nothing's loaded yet) - this drives both what's
// displayed and a windowed prefetch (not just the single focused Mii - see
// mii_detail_panel.cpp's own comment on the window size/eviction policy
// and why each fetch is synchronous/blocking, paced to one per Update()
// call, rather than backgrounded onto its own thread). Draw() draws the
// panel on whatever render target/scene is currently bound (call it between
// C2D_SceneBegin(top) and the next C2D_SceneBegin/C3D_FrameEnd), showing an
// animated "..." placeholder while the focused Mii's portrait hasn't
// finished fetching yet. Shutdown() frees cached portraits, call once at
// app shutdown.
//
// Three tabs, one shared cache: `tab` tells this module which of the three
// Mii lists (Library/Friends/RecentGames) is currently active, so the
// *same* cache and prefetch window logic can serve all of them without one
// tab's scrolling evicting another tab's already-fetched portraits outright
// - see mii_detail_panel.cpp's own comment on the composite cache key and
// eviction-preference scheme (ported from the Wii U sibling build's own
// multi-tab image cache, composeImageKey()/eviction loop in its main.cpp).
// Only call Update() for whichever tab is actually on screen right now -
// same as the Wii U build, this module only ever prefetches one tab's worth
// of images at a time.
namespace MiiDetailPanel {

enum class Tab { Library = 0, Friends = 1, RecentGames = 2 };

// Sets how many distinct Miis' portraits are kept cached in total, shared
// across all three tabs (the prefetch window size). Call once at startup,
// before the first Update(). Defaults to WINDOW_SIZE if never called - see
// mii_detail_panel.cpp's own memory-budget comment for why.
void SetMaxCachedPortraits(size_t maxPortraits);

void Init();

void Update(Tab tab, const std::vector<Ver3MiiDecoded> &miis, int focusedIndex);

// eyeOffsetPx shifts only the portrait horizontally (positive = right) -
// pass 0.0f for a flat/non-stereo draw. See this header's own note above.
void Draw(float eyeOffsetPx = 0.0f);

// True if the most recently *attempted* portrait fetch failed (network
// error, or a downloaded file that failed to decode) and no fetch has
// succeeded since - not tied to any specific Mii, since with no network at
// all every fetch fails the same way regardless of which one it was for.
// main.cpp uses this to show a persistent warning that clears the moment
// some later fetch actually succeeds - see Update()'s own retry-backoff
// comment for why fetch attempts themselves are also throttled after a
// failure, not just reported.
bool LastFetchFailed();

void Shutdown();

} // namespace MiiDetailPanel
