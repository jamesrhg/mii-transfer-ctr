#include "ctr_network.h"

#include <3ds.h>

#include <condition_variable>
#include <mutex>

namespace CtrNetwork {

namespace {

constexpr s64 kConnectTimeoutNs = 15LL * 1000 * 1000 * 1000;

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_done = false;
bool g_connected = false;
bool g_waitInProgress = false;
bool g_acInitialized = false;
Handle g_connectEvent = 0;

} // namespace

void BeginConnect() {
    if (R_FAILED(acInit())) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_connected = false;
        g_done = true;
        return;
    }
    g_acInitialized = true;

    // All of this is fast and non-blocking - ACU_ConnectAsync() itself
    // returns immediately and signals g_connectEvent once the connection
    // attempt actually finishes, proceeding at the kernel level regardless
    // of whether anything ever waits on that event. No thread needed here:
    // an earlier version spawned one specifically to perform the *wait*
    // (svcWaitSynchronization, which can genuinely block for the rest of
    // kConnectTimeoutNs), but nothing in this app currently calls
    // WaitUntilDone() at all - that dedicated thread existed solely to
    // babysit a result nobody was consuming, one more concurrent thread
    // than necessary. See WaitUntilDone() below for where the actual wait
    // now happens instead: lazily, on whichever thread (if any) first asks
    // for the result.
    acuConfig config{};
    // AC_AP_TYPE_SLOT1|2|3 (not AC_AP_TYPE_ALL, which is wider than the u8
    // this function actually takes) - allow connecting via any of the
    // console's saved Wi-Fi slots.
    if (!R_SUCCEEDED(ACU_CreateDefaultConfig(&config)) ||
        !R_SUCCEEDED(ACU_SetAllowApType(&config, AC_AP_TYPE_SLOT1 | AC_AP_TYPE_SLOT2 | AC_AP_TYPE_SLOT3)) ||
        !R_SUCCEEDED(svcCreateEvent(&g_connectEvent, RESET_ONESHOT)) ||
        !R_SUCCEEDED(ACU_ConnectAsync(&config, g_connectEvent))) {
        if (g_connectEvent) {
            svcCloseHandle(g_connectEvent);
            g_connectEvent = 0;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_connected = false;
        g_done = true;
    }
}

bool IsWirelessSwitchOff() {
    if (!g_acInitialized) return false;
    u32 status = 0;
    // Result, not just success/fail: the specific error code is the whole
    // signal here (see this file's own header comment) - a *different*
    // failure (or plain success with status 0, meaning "on but not
    // connected to an access point yet") must not be misreported as the
    // switch being off.
    Result res = ACU_GetWifiStatus(&status);
    return static_cast<u32>(res) == 0xE0A09D2Eu;
}

State Poll() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_done) return g_connected ? State::Connected : State::Failed;
    if (!g_connectEvent) return State::Connecting; // BeginConnect() hasn't run/finished setting it up yet
    // Zero timeout - a real poll, never blocks. Interpretation matches
    // WaitUntilDone() below: the event firing at all (regardless of timeout
    // length) is what "connected" means for this API - see that function's
    // own comment.
    if (R_SUCCEEDED(svcWaitSynchronization(g_connectEvent, 0))) {
        svcCloseHandle(g_connectEvent);
        g_connectEvent = 0;
        g_connected = true;
        g_done = true;
        g_cv.notify_all();
        return State::Connected;
    }
    return State::Connecting;
}

bool WaitUntilDone() {
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_done) return g_connected;

    if (g_waitInProgress) {
        // Someone else is already performing the actual wait below - block
        // on the condition variable instead of also calling
        // svcWaitSynchronization() on the same RESET_ONESHOT event, which
        // only ever wakes *one* waiter (a second concurrent
        // svcWaitSynchronization() call on it would just block forever
        // once the first caller consumes the signal).
        g_cv.wait(lock, [] { return g_done; });
        return g_connected;
    }
    g_waitInProgress = true;
    Handle event = g_connectEvent;
    lock.unlock();

    bool connected = event && R_SUCCEEDED(svcWaitSynchronization(event, kConnectTimeoutNs));
    if (event) svcCloseHandle(event);

    lock.lock();
    g_connectEvent = 0;
    g_connected = connected;
    g_done = true;
    g_cv.notify_all();
    return g_connected;
}

void Shutdown() {
    if (g_acInitialized) {
        acExit();
        g_acInitialized = false;
    }
}

} // namespace CtrNetwork
