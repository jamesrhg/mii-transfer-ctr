#pragma once

// Brings up the 3DS's AC (access point / Wi-Fi) connection - the 3DS
// equivalent of the Wii U build's ACInitialize()/ACConnect() (see the Wii U
// build's mii_http_server.cpp). This turned out to be required on 3DS too,
// despite this app's earlier assumption that 3DS Wi-Fi association was
// entirely system-managed and needed no explicit app-side request.
//
// MiiHttpServer (needs a real assigned IP) and the portrait fetch
// (MiiImageFetcher, real internet access) both depend on this, so it's
// brought up once, centrally, here - MiiHttpServer waits on WaitUntilDone()
// from its own background thread, never from the caller's/main thread (see
// main.cpp's startup comment for why nothing network-related may block
// before the first frame renders); main.cpp itself uses the non-blocking
// Poll() below, once at startup, to show a real "no network" error instead
// of letting the list/portrait UI come up against a connection that was
// never going to work.
namespace CtrNetwork {

enum class State {
    Connecting, // BeginConnect()'s attempt is still in flight, or hasn't been started
    Connected,
    Failed,
};

// Kicks off the connection attempt and returns immediately - everything it
// does (acInit(), ACU_ConnectAsync(), etc) is fast/non-blocking, so no
// background thread is spawned for this call itself; the connection
// proceeds independently at the kernel level from here. Called once, early,
// from main() - safe to call even if it's never followed by a
// WaitUntilDone()/Poll() call from anyone (e.g. if both MiiHttpServer and
// the startup network check happen to be skipped before reaching that
// point). Also safe to call again later to retry (main.cpp does, once the
// wireless switch is confirmed back on after a switch-off failure) -
// resets the previous attempt's outcome first.
void BeginConnect();

// True if the physical wireless switch is off - checked via
// ACU_GetWifiStatus(), which 3dbrew documents as failing with a specific,
// distinct error code (0xE0A09D2E) in exactly that case, separate from
// "switch on but not connected to any access point" (which just reports a
// status of 0) or any other connection failure. Only meaningful after
// BeginConnect() (needs acInit() to have already succeeded) - returns false
// if acInit() itself failed, since then there's no way to tell either way
// and BeginConnect()'s own failure (surfaced via Poll()) already covers it.
// Doesn't need to wait for the connection attempt itself to finish - the
// switch's physical state is queryable immediately.
bool IsWirelessSwitchOff();

// Non-blocking - call once per frame (e.g. while showing a "Checking
// network connection..." screen) to learn how the attempt started by
// BeginConnect() is going, without ever stalling the caller. Bounded by the
// same internal timeout WaitUntilDone() has, but callers that want a
// visible bound of their own (to stop polling and show an error) should
// still track their own elapsed time - see main()'s own usage.
State Poll();

// Blocks the *calling* thread until the connection attempt started by
// BeginConnect() has finished, one way or the other (bounded by an internal
// ~15s timeout) - never call this from main()/the render thread. The actual
// wait happens lazily here, on whichever thread calls this first; safe to
// call from multiple threads and multiple times regardless - every caller
// gets the same answer once it's known. Returns true if connected.
bool WaitUntilDone();

// Tears down AC. Call once at app shutdown.
void Shutdown();

} // namespace CtrNetwork
