#pragma once

#include <citro2d.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Runtime PNG-bytes -> citro2d C2D_Image loader, used by everywhere this app
// needs to turn a downloaded or bundled PNG into something citro2d can draw
// (the animated background, the author-name pencil icon, and - most often -
// every Mii face render fetched over HTTP). There is no SDL2_image on 3DS
// and citro2d itself has no PNG decoder (it only understands its own
// pre-baked .t3x texture atlases), so this exists to fill that gap: it wraps
// libpng's "simplified API" for the actual decode, then does the two things
// citro2d/citro3d need that a plain decoded RGBA8 buffer doesn't yet have -
// see png_texture.cpp for why both steps are necessary (and why they're done
// via a CPU-side pass rather than a GX display transfer - the transfer unit
// turned out to mis-tile/discolor images when repurposed for plain
// memory->texture loading, see TileIntoTexture()'s comment):
//
//   1. Reverses the byte order of every pixel, RGBA -> ABGR - the PICA200
//      GPU's "RGBA8" texture format is fully byte-order-reversed from what
//      libpng (or any other PNG decoder) hands back, confirmed against a
//      real, shipped reference implementation (Steveice10/FBI).
//   2. Converts the image from a plain linear (row-major) pixel buffer into
//      the GPU's tiled (Morton/Z-order, within 8x8 blocks) texture layout -
//      C3D_TexLoadImage alone does *not* do this (it is a plain memcpy).
//
// Every loaded image becomes its own C3D_Tex sized to the next power-of-two
// >= the PNG's own dimensions (as citro3d textures require), with a
// Tex3DS_SubTexture cropped to the PNG's real size - built by hand here since
// this never goes through the offline tex3ds tool.
//
// Split into two steps (DecodeToPixels/UploadToTexture), not just one
// LoadFromMemory() call, specifically so a background thread can do the
// expensive libpng decode + byte-swap (pure CPU, no GPU/citro3d state
// touched at all) while the actual C3D_Tex allocation and GPU-tiled upload -
// the only citro3d-touching part - stays on the main thread, where every
// other citro3d/citro2d call in this app already happens. See
// mii_detail_panel.cpp's own background-worker comment for why that split
// matters here specifically.
namespace PngTexture {

// One loaded image. Both `image.tex` and `image.subtex` are heap-allocated
// and owned by this Texture (freed together by Free() below) - plain
// C2D_Image is just two pointers, so a Texture can be copied/stored in a
// cache/container freely; only call Free() once per successful
// UploadToTexture()/LoadFromMemory().
struct Texture {
    C2D_Image image{};
    bool valid = false;
};

// A fully-decoded, byte-swapped (ABGR, ready for PICA200's own RGBA8
// texture format), still-linear (untiled) RGBA8 pixel buffer plus its real
// (non-power-of-two) dimensions - no GPU/citro3d state touched at all, safe
// to produce on any thread.
struct DecodedImage {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid = false;
};

// Decodes `data` (PNG file bytes, `size` long) via libpng's simplified API
// and byte-swaps it to PICA200's own pixel format. Returns a DecodedImage
// with valid=false on any failure (corrupt data, allocation failure,
// oversized image) - never crashes on bad input, since fetched images can
// fail/truncate. Safe to call from any thread.
DecodedImage DecodeToPixels(const uint8_t *data, size_t size);

// Allocates a new C3D_Tex sized to the next power-of-two >= decoded's own
// dimensions, tiles `decoded`'s pixels into it, and wraps the result in a
// Tex3DS_SubTexture cropped to the real (non-padded) size. Touches
// citro3d/GPU texture-memory allocation - call only from the same thread
// every other citro3d call in this app runs on (the main thread). Returns
// an invalid Texture if `decoded` itself isn't valid or allocation fails.
Texture UploadToTexture(const DecodedImage &decoded);

// Convenience wrapper (DecodeToPixels() + UploadToTexture()) for the
// existing single-threaded call sites (AnimatedBg, the author-name icon)
// that don't need the two steps split across threads.
Texture LoadFromMemory(const uint8_t *data, size_t size);

// Frees the C3D_Tex owned by `tex` and resets it to an invalid/empty state.
// Safe to call on an already-invalid (or already-freed) Texture.
void Free(Texture *tex);

} // namespace PngTexture
