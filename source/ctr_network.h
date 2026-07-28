#pragma once

// Brings up the 3DS's AC (access point / Wi-Fi) connection - the 3DS
// equivalent of the Wii U build's ACInitialize()/ACConnect() (see the Wii U
// build's mii_http_server.cpp). This turned out to be required on 3DS too,
// despite this app's earlier assumption that 3DS Wi-Fi association was
// entirely system-managed and needed no explicit app-side request: an
// active AC connection (ACU_ConnectAsync() here) must be brought up before
// FRD_Login() will succeed, which otherwise fails immediately with an "AC
// not connected" result - which is exactly what was happening (silently)
// before this module existed, the actual root cause of the Friends tab
// never being able to log in.
//
// Both MiiHttpServer (needs a real assigned IP) and CtrFriends (needs AC up
// before FRD_Login) depend on this, so it's brought up once, centrally, here
// - both of those wait on WaitUntilDone() from their own background threads,
// never from the caller's/main thread (see main.cpp's startup comment for
// why nothing network-related may block before the first frame renders).
namespace CtrNetwork {

// Kicks off the connection attempt and returns immediately - everything it
// does (acInit(), ACU_ConnectAsync(), etc) is fast/non-blocking, so no
// background thread is spawned for this call itself; the connection
// proceeds independently at the kernel level from here. Call once, early,
// from main() - safe to call even if it's never followed by a
// WaitUntilDone() call from anyone (e.g. if both MiiHttpServer and
// CtrFriends happen to be disabled/fail before reaching that point).
void BeginConnect();

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
