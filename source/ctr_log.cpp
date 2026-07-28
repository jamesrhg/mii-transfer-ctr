#include "ctr_log.h"

#include <sys/stat.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace CtrLog {

// Disabled outright, not just throttled: this app's whole hang-hunting
// history turned out to actually be self-inflicted by this logging code
// itself. Even batched (see Printf()'s own comment further down - a real,
// substantial improvement over per-call fflush()), it's still synchronous
// SD-card I/O sitting on the render loop's own thread, a real, physical-
// latency operation - never something worth shipping on by default. Flip
// back to true only for active, temporary debugging, never left on.
constexpr bool kLoggingEnabled = false;

namespace {
// Printf() is called from multiple threads concurrently by design (main.cpp
// as well as background threads like MiiHttpServer's SetupAndServe log
// their own progress) - g_file is a single shared stdio FILE*, and
// unsynchronized concurrent fputs()/fflush() on the same FILE* from
// multiple threads is undefined behavior (newlib's stdio has no built-in
// per-FILE locking on this target). This mutex serializes every Printf()
// call across all threads to rule that out.
std::mutex g_mutex;
// The file is opened *once*, here, and kept open for the process's whole
// life - every flush just fputs()+fflush()es into that same handle. An
// earlier version instead did a full fopen()/fclose() cycle on every single
// call (reasoning: durability against a hang on the very next line) - that
// turned out to be a real mistake, confirmed on-device: it introduced a
// *new* hang of its own (most likely FS handle/resource pressure from
// repeatedly opening and closing the same file in rapid succession - 3DS
// processes have a limited handle table, especially restrictive for a
// .3dsx run via Homebrew Launcher, which inherits HBL's own exheader rather
// than getting one of its own with capabilities the app controls). fflush()
// alone (no close/reopen) already provides the same "durable even if the
// very next line hangs" guarantee, without repeated open/close - see
// 3ds.h/newlib's sdmc: devoptab, which does push fflush()ed writes down to
// the SD card immediately rather than buffering them purely in process
// memory.
FILE *g_file = nullptr;
// Per-message sequence number, prefixed onto every line so a gap (from the
// batched-flush window not yet having reached the SD card at exit/crash
// time) is at least visibly a gap rather than indistinguishable from a hang.
std::atomic<uint32_t> g_sequence{0};
} // namespace

void Init() {
    if (!kLoggingEnabled) return;

    // fopen() can't create missing parent directories, only the file
    // itself - explicitly create every level (ignoring "already exists")
    // rather than assuming sdmc:/3ds/mii-transfer/ already exists just
    // because that's the documented install path; a previous version of
    // this code assumed that and silently produced no debug.log at all
    // when it turned out not to hold on-device.
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/mii-transfer", 0777);

    // "w" to truncate - debug.log always reflects only the most recent run,
    // not an ever-growing history - then kept open for every Printf() call
    // below, for the process's whole life (see the comment above).
    g_file = fopen("sdmc:/3ds/mii-transfer/debug.log", "w");
    if (g_file) {
        std::fputs("--- new run ---\n", g_file);
        std::fflush(g_file);
    }
}

void Shutdown() {
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

// Back to a plain fflush() on every single call - not batched. Batching
// (buffer N calls, flush together) was tried and genuinely helped the old
// 40+-calls/frame per-draw-call logging load, but it has a fatal flaw for
// diagnosing an actual crash specifically: Shutdown() never ran a final
// flush of whatever was still buffered, and worse, a real hard fault
// bypasses the normal shutdown path entirely regardless - so a crash
// between flush points silently loses every line since the last one, which
// is exactly what happened (a run logged nothing but the initial
// "--- new run ---" before dying). This stripped-down build's call volume
// is nowhere near what made per-call fflush() a bottleneck before, so
// there's no real tradeoff here right now - every line durable immediately
// is strictly better while call volume stays low.
void Printf(const char *fmt, ...) {
    if (!kLoggingEnabled) return;

    char buf[512];
    int prefixLen = std::snprintf(buf, sizeof(buf), "#%u ", static_cast<unsigned>(g_sequence.fetch_add(1)));
    if (prefixLen < 0) prefixLen = 0;
    if (static_cast<size_t>(prefixLen) >= sizeof(buf)) prefixLen = static_cast<int>(sizeof(buf)) - 1;

    va_list ap;
    va_start(ap, fmt);
    int msgLen = vsnprintf(buf + prefixLen, sizeof(buf) - static_cast<size_t>(prefixLen), fmt, ap);
    va_end(ap);
    if (msgLen <= 0) return;
    int len = prefixLen + msgLen;
    if (static_cast<size_t>(len) >= sizeof(buf) - 1) len = static_cast<int>(sizeof(buf) - 2);
    buf[len] = '\n';
    buf[len + 1] = '\0';

    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_file) {
        std::fputs(buf, g_file);
        std::fflush(g_file);
    }
}

} // namespace CtrLog
