#include "ctr_ui.h"

#include "ctr_log.h"

#include <3ds.h>

namespace CtrUi {

namespace {

constexpr size_t kMaxGlyphs = 8192;

C2D_Font g_font = nullptr;
C2D_TextBuf g_buf = nullptr;
bool g_cfguInited = false;

} // namespace

bool Init() {
    if (R_SUCCEEDED(cfguInit())) g_cfguInited = true;

    u8 region = CFG_REGION_USA;
    if (g_cfguInited) CFGU_SecureInfoGetRegion(&region);

    g_font = C2D_FontLoadSystem(static_cast<CFG_Region>(region));
    g_buf = C2D_TextBufNew(kMaxGlyphs);
    // g_font == nullptr is NOT a failure: C2D_FontLoadSystem() documents
    // that as its deliberate return value whenever the requested region
    // matches the console's own system region, at which point every caller
    // (DrawText()/MeasureText()/DrawTextWrapped() above) is meant to keep
    // passing nullptr straight through to C2D_TextFontParse() - which is
    // exactly what they already do. Only g_buf failing to allocate is a
    // real failure here.
    return g_buf != nullptr;
}

void Shutdown() {
    if (g_buf) {
        C2D_TextBufDelete(g_buf);
        g_buf = nullptr;
    }
    if (g_font) {
        C2D_FontFree(g_font);
        g_font = nullptr;
    }
    if (g_cfguInited) {
        cfguExit();
        g_cfguInited = false;
    }
}

void BeginFrame() {
    if (g_buf) C2D_TextBufClear(g_buf);
}

// Per-substep logging, gated by a call counter rather than unconditionally -
// this is called ~20-40 times per frame, so unconditional logging would
// flood the UDP/file log within a couple frames. Temporary instrumentation
// to find exactly *which* citro2d call inside here is where this app's
// still-unresolved render hang actually happens - every previous round of
// logging only bracketed the *outside* of this function, so all that's
// been established so far is "somewhere inside DrawText()", never which of
// its 4 citro2d calls specifically.
namespace {
int g_drawTextCallCount = 0;
}

void DrawText(float x, float y, float scale, u32 color, const char *text, float *outW, float *outH) {
    if (outW) *outW = 0.0f;
    if (outH) *outH = 0.0f;
    if (!g_buf || !text || !text[0]) return;

    int callIndex = g_drawTextCallCount++;
    bool logThis = callIndex < 60;

    if (logThis) CtrLog::Printf("CtrUi::DrawText[%d]: text=\"%s\" - calling C2D_TextFontParse", callIndex, text);
    C2D_Text t;
    C2D_TextFontParse(&t, g_font, g_buf, text);
    if (logThis) CtrLog::Printf("CtrUi::DrawText[%d]: C2D_TextFontParse returned - calling C2D_TextOptimize", callIndex);
    C2D_TextOptimize(&t);
    if (logThis) CtrLog::Printf("CtrUi::DrawText[%d]: C2D_TextOptimize returned - calling C2D_TextGetDimensions", callIndex);

    float w = 0.0f, h = 0.0f;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    if (logThis) {
        CtrLog::Printf("CtrUi::DrawText[%d]: C2D_TextGetDimensions returned w=%f h=%f - calling C2D_DrawText",
                        callIndex, w, h);
    }
    if (outW) *outW = w;
    if (outH) *outH = h;

    C2D_DrawText(&t, C2D_WithColor, x, y, 0.0f, scale, scale, color);
    if (logThis) CtrLog::Printf("CtrUi::DrawText[%d]: C2D_DrawText returned", callIndex);
}

void MeasureText(float scale, const char *text, float *outW, float *outH) {
    if (outW) *outW = 0.0f;
    if (outH) *outH = 0.0f;
    if (!g_buf || !text || !text[0]) return;

    C2D_Text t;
    C2D_TextFontParse(&t, g_font, g_buf, text);
    C2D_TextGetDimensions(&t, scale, scale, outW, outH);
}

void DrawTextWrapped(float x, float y, float scale, u32 color, float wrapWidth, const char *text) {
    if (!g_buf || !text || !text[0]) return;

    C2D_Text t;
    C2D_TextFontParse(&t, g_font, g_buf, text);
    C2D_TextOptimize(&t);

    C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap, x, y, 0.0f, scale, scale, color, wrapWidth);
}

} // namespace CtrUi
