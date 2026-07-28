#pragma once

// A seamlessly-tiled background image (romfs:/bg.png) that scrolls
// diagonally, looping every 20s - matches the Wii U build's own animated_bg
// (which itself matched content/index.html's CSS `background-position`
// animation for the local-HTTP-server web viewer of the same file).
//
// Drawn "at all times" - on every screen (top detail panel, bottom list,
// dialogs) - on top of whatever solid clear color that screen already draws,
// under everything else. Since the 3DS has two independent screens, Draw()
// takes the screen dimensions so it can be called once per screen per frame.
namespace AnimatedBg {

// Loads romfs:/bg.png once. Safe to call even if it fails - Draw() is then a
// no-op (nothing drawn beyond whatever solid color the caller already
// cleared to).
void Load();

// Draws one frame of the tiled, scrolling background, sized to cover
// (screenW, screenH). Call once per frame per screen, right after clearing
// to a solid color and before anything else.
//
// worldOffsetX positions this screen within one shared, continuous pattern
// spanning both physical screens - not two independently-tiled copies. The
// top screen is 400px wide, the bottom 320px, and the bottom screen sits
// horizontally centered beneath the top one on real hardware, so passing
// worldOffsetX=0 for the top screen and worldOffsetX=(400-320)/2=40 for the
// bottom screen makes the tiles line up across the gap between them as if
// it were one continuous 400px-wide canvas.
void Draw(int screenW, int screenH, float worldOffsetX = 0.0f);

// Frees the loaded texture. Call once at shutdown.
void Unload();

} // namespace AnimatedBg
