#include "animated_bg.h"

#include "png_texture.h"

#include <3ds.h>
#include <citro2d.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace AnimatedBg {

namespace {

// Matches the Wii U build's own PERIOD_MS (itself matching content/index.html's
// `animation: bgMove 20s linear infinite`).
constexpr u64 PERIOD_MS = 20000;

PngTexture::Texture g_tex;

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

void Load() {
    std::vector<uint8_t> bytes = ReadFileFully("romfs:/bg.png");
    if (bytes.empty()) return;
    g_tex = PngTexture::LoadFromMemory(bytes.data(), bytes.size());
}

void Draw(int screenW, int screenH, float worldOffsetX) {
    if (!g_tex.valid) return;
    int tileW = g_tex.image.subtex->width;
    int tileH = g_tex.image.subtex->height;
    if (tileW <= 0 || tileH <= 0) return;

    // A single 0..1 sawtooth every PERIOD_MS, via osGetTime()'s own
    // wraparound-free monotonic ms counter - inherently seamless (no
    // separate "start time" to track or reset, no jump at the loop point).
    // Shared across both screens' Draw() calls (both read the same clock),
    // which is what keeps their tiles moving in lockstep.
    float t = static_cast<float>(osGetTime() % PERIOD_MS) / static_cast<float>(PERIOD_MS);

    // CSS background-position: (0,0) -> (tileW,tileH) over one period - the
    // image's own origin shifts by a full tile, which (since it repeats) is
    // exactly what makes the loop seamless.
    float offsetY = t * static_cast<float>(tileH);
    float startY = offsetY - static_cast<float>(tileH);

    // World-space X offset, shifted into *this screen's* local coordinate
    // space by worldOffsetX (0 for the top screen, 40 for the bottom - see
    // this function's own header comment) and wrapped into [0, tileW) with
    // fmodf so it works the same regardless of how large worldOffsetX is.
    float worldOffsetTileX = t * static_cast<float>(tileW) - worldOffsetX;
    float localOffsetX = std::fmod(worldOffsetTileX, static_cast<float>(tileW));
    if (localOffsetX < 0.0f) localOffsetX += static_cast<float>(tileW);
    float startX = localOffsetX - static_cast<float>(tileW);

    for (float y = startY; y < static_cast<float>(screenH); y += static_cast<float>(tileH)) {
        for (float x = startX; x < static_cast<float>(screenW); x += static_cast<float>(tileW)) {
            C2D_DrawImageAt(g_tex.image, x, y, 0.0f);
        }
    }
}

void Unload() {
    PngTexture::Free(&g_tex);
}

} // namespace AnimatedBg
