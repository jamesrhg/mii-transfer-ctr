#pragma once

#include <citro2d.h>

// Shared text-drawing helper used by every screen (the bottom-screen Mii
// list, the top-screen detail panel, dialogs). Centralizes the one thing
// every one of them needs: the console's own system font (loaded via
// C2D_FontLoadSystem(), matching the console's configured region - the 3DS
// equivalent of the Wii U build's OSGetSharedData()-sourced system font, so
// there's still no bundled .ttf to worry about licensing/redistributing) and
// a single shared C2D_TextBuf.
//
// Unlike the Wii U build's SDL_ttf usage (which rendered each string to its
// own GPU texture, expensive enough that main.cpp only rebuilt row textures
// when the visible set actually changed), citro2d's C2D_Text is cheap - it's
// just glyph index/position records into the font's existing glyph atlas,
// not a texture of its own. So the idiomatic citro2d pattern (used here) is
// to just clear the shared buffer and re-parse *all* on-screen text fresh
// every frame via BeginFrame()+DrawText()/DrawTextWrapped(), rather than
// porting over the Wii U build's rebuild-only-on-change caching - simpler,
// and cheap enough at this app's text volume (a couple dozen short strings
// per frame) not to need that caching here.
namespace CtrUi {

// Loads the system font (for the console's own configured region) and
// creates the shared text buffer. Returns false (logged, non-fatal - text
// just won't render) on failure.
bool Init();

void Shutdown();

// Clears the shared text buffer for a new frame. Call once per frame,
// before any DrawText/DrawTextWrapped calls that frame (including inside a
// blocking modal's own inner loop, e.g. dialogs - "once per frame" there
// means once per its own loop iteration).
void BeginFrame();

// Parses and draws `text` (may contain explicit '\n' line breaks - there is
// no automatic word-wrapping; callers that need wrapping insert their own
// line breaks at compose time, since this app's dialog/status strings are
// few, short, and known ahead of time) at (x, y) - top-left corner of the
// block, not the baseline. `scale` is applied to both axes (system font
// glyph height is 30px at scale 1.0). Returns the drawn block's pixel
// width/height via outW/outH (may be null) - handy for right-align/centering
// math done by the caller before or after this call.
void DrawText(float x, float y, float scale, u32 color, const char *text, float *outW = nullptr,
              float *outH = nullptr);

// Same as DrawText, but only measures - draws nothing. Used to compute
// layout (e.g. a dialog box's size, or right-aligning the version string)
// before the real DrawText call.
void MeasureText(float scale, const char *text, float *outW, float *outH);

// Parses and draws `text` word-wrapped to `wrapWidth` pixels (citro2d's
// C2D_WordWrap flag) - for message text whose length isn't known/bounded
// ahead of time (dialog messages, which can embed a system error string), as
// opposed to DrawText's fixed short UI labels. There is no reliable way to
// measure the *wrapped* height in advance (C2D_Text::lines only counts
// explicit '\n's, not word-wrap breaks - a citro2d API limitation, not an
// oversight here), so callers that need to size a box around wrapped text
// should just use a fixed, generously-sized box rather than measuring first.
void DrawTextWrapped(float x, float y, float scale, u32 color, float wrapWidth, const char *text);

} // namespace CtrUi
