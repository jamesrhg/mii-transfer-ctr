#include "png_texture.h"

#include <png.h>

#include <3ds.h>

#include <cstring>

namespace PngTexture {

namespace {

u32 NextPow2(u32 v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

// PICA200 GPU_RGBA8 texture data is fully byte-order-reversed from what
// libpng's simplified API (PNG_FORMAT_RGBA) hands back - RGBA -> ABGR, not
// just an R/B swap - confirmed against a real, shipped reference
// implementation (Steveice10/FBI's screen_load_texture_file(), which does
// this exact 4-byte reversal before uploading). Done in place.
void SwapToAbgr(uint8_t *pixels, size_t pixelCount) {
    for (size_t i = 0; i < pixelCount; i++) {
        uint8_t *p = pixels + i * 4;
        uint8_t r = p[0], g = p[1], b = p[2], a = p[3];
        p[0] = a;
        p[1] = b;
        p[2] = g;
        p[3] = r;
    }
}

// Writes `pixels` (linear, row-major RGBA8, row 0 = the PNG's top row -
// already byte-order-converted via SwapToAbgr - `width` x `height`) into
// `tex`'s own storage using the Morton/Z-order tiling PICA200 textures
// require within each 8x8 block - ported from Steveice10/FBI's
// screen_load_texture_untiled() (source/core/screen.c), a proven, shipped
// implementation. This works directly on `tex->data` (already CPU-writable
// linear memory for a non-VRAM texture) and needs no GX_DisplayTransfer: an
// earlier version of this file used GX_DisplayTransfer to do the tiling
// instead, but that transfer unit is fundamentally built for framebuffer
// post-processing (its "the framebuffer is sideways" hardware quirk - see
// 3ds/gpu/gx.h) and turned out to discolor images (reddish tint) when
// repurposed for plain memory->texture loading - this CPU-side approach
// sidesteps that hardware unit entirely.
//
// The source row is read top-down (plain y, no flip). An earlier version of
// this function flipped it (height-1-y), on the theory that PICA200
// textures need a V-flip the same as OpenGL - that theory was wrong for
// this codepath specifically: combined with the correct (non-rotated)
// Tex3DS_SubTexture top/bottom assignment in UploadToTexture() below (see
// its comment - libctru's tex3ds.h documents that top must be the *larger*
// v-coordinate for an unrotated draw), that manual flip produced a
// double-flip - net upside-down output, confirmed on-device. Removing the
// flip here (and leaving the already-correct top/bottom in
// UploadToTexture() alone) is what actually renders right-side-up.
void TileIntoTexture(C3D_Tex *tex, const uint8_t *pixels, u32 width, u32 height, u32 potWidth) {
    std::memset(tex->data, 0, tex->size);
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 dstPos = ((((y >> 3) * (potWidth >> 3) + (x >> 3)) << 6) +
                          ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) |
                           ((y & 4) << 3))) *
                         4;
            u32 srcPos = (y * width + x) * 4;
            std::memcpy(static_cast<uint8_t *>(tex->data) + dstPos, pixels + srcPos, 4);
        }
    }
    C3D_TexFlush(tex);
}

} // namespace

DecodedImage DecodeToPixels(const uint8_t *data, size_t size) {
    DecodedImage out;
    if (!data || size == 0) return out;

    png_image pngImage{};
    pngImage.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&pngImage, data, size)) return out;

    pngImage.format = PNG_FORMAT_RGBA;

    u32 srcW = pngImage.width;
    u32 srcH = pngImage.height;
    // citro3d textures top out at 1024x1024; nothing this app ever fetches
    // or bundles is anywhere near that, so anything bigger is bad data.
    if (srcW == 0 || srcH == 0 || srcW > 1024 || srcH > 1024) {
        png_image_free(&pngImage);
        return out;
    }

    out.pixels.resize(PNG_IMAGE_SIZE(pngImage));
    if (!png_image_finish_read(&pngImage, nullptr, out.pixels.data(), 0, nullptr)) {
        png_image_free(&pngImage);
        out.pixels.clear();
        return out;
    }
    png_image_free(&pngImage);

    SwapToAbgr(out.pixels.data(), static_cast<size_t>(srcW) * srcH);

    out.width = srcW;
    out.height = srcH;
    out.valid = true;
    return out;
}

Texture UploadToTexture(const DecodedImage &decoded) {
    Texture out;
    if (!decoded.valid) return out;

    u32 potW = NextPow2(decoded.width);
    u32 potH = NextPow2(decoded.height);
    // Matches FBI's own minimum (64px, not citro3d's bare 8px floor) -
    // kept the same as the proven reference rather than pushing the boundary.
    if (potW < 64) potW = 64;
    if (potH < 64) potH = 64;

    C3D_Tex *tex = new C3D_Tex();
    if (!C3D_TexInit(tex, static_cast<u16>(potW), static_cast<u16>(potH), GPU_RGBA8)) {
        delete tex;
        return out;
    }
    // GPU_NEAREST, not GPU_LINEAR: the subtexture's top/right edges sit
    // exactly at srcH/potH and srcW/potW - with linear filtering, a sample
    // taken right at that boundary blends 50/50 with the adjacent
    // zero-padding texel just outside the image (the POT texture is padded
    // up to potW x potH with transparent black, memset in TileIntoTexture),
    // which faded the image's topmost row(s) toward transparent - visible
    // on-device as the top of the face looking "cropped". Nearest sampling
    // never blends across that boundary.
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    TileIntoTexture(tex, decoded.pixels.data(), decoded.width, decoded.height, potW);

    Tex3DS_SubTexture *subtex = new Tex3DS_SubTexture();
    subtex->width = static_cast<u16>(decoded.width);
    subtex->height = static_cast<u16>(decoded.height);
    // Matches FBI's own screen_draw_texture() call exactly (the same
    // reference implementation TileIntoTexture() above is ported from):
    // top is always 1.0f, not srcH/potH - the PICA's V axis is inverted
    // relative to texture memory address (row 0 in memory, where this
    // image's top row lands since TileIntoTexture() doesn't flip, reads as
    // V=1.0, not V=0.0), so the image's top edge is always exactly at
    // V=1.0 regardless of how much POT padding is below it. bottom is
    // where the image's *own* last row ends, hence 1.0 - srcH/potH rather
    // than 0.0. Getting this wrong (top=srcH/potH, bottom=0.0, both valid
    // per libctru's tex3ds.h "top must exceed bottom to avoid rotation"
    // rule, so it superficially looked plausible and even avoided the
    // rotation bug) still worked for a *square* srcH==potH case by
    // coincidence but cropped the top edge for any image smaller than its
    // POT texture.
    subtex->left = 0.0f;
    subtex->top = 1.0f;
    subtex->right = static_cast<float>(decoded.width) / static_cast<float>(potW);
    subtex->bottom = 1.0f - static_cast<float>(decoded.height) / static_cast<float>(potH);

    out.image.tex = tex;
    out.image.subtex = subtex;
    out.valid = true;
    return out;
}

Texture LoadFromMemory(const uint8_t *data, size_t size) {
    DecodedImage decoded = DecodeToPixels(data, size);
    if (!decoded.valid) return Texture{};
    return UploadToTexture(decoded);
}

void Free(Texture *tex) {
    if (!tex || !tex->valid) return;
    if (tex->image.tex) {
        C3D_TexDelete(tex->image.tex);
        delete tex->image.tex;
    }
    delete tex->image.subtex;
    *tex = Texture{};
}

} // namespace PngTexture
