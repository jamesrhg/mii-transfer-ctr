#pragma once

#include <3ds.h>

#include <functional>

// Thin wrappers around libctru's threadCreate(), used everywhere this app
// spawns a background thread *instead of* plain std::thread. This matters:
// std::thread on this devkitARM/libctru target gets a small default stack
// (fine for trivial work, but this app's background threads do httpc/soc/ac
// IPC calls with their own internal buffers, which turned out to overflow
// it - the app would run for a few frames, then get killed once a
// background thread actually reached that code and blew its stack). There
// is no portable way to change std::thread's stack size through its own
// API, so threadCreate() (which takes an explicit stack size) is used
// directly instead, everywhere in this codebase.
namespace CtrThread {

constexpr size_t kDefaultStackSize = 64 * 1024;

// For threads that call httpc functions to fetch over HTTPS (TLS handshake/
// cipher setup inside http:C's own IPC handling, on top of this app's own
// PerformGet() which already has a 16KB local chunk buffer) - on-device
// evidence pointed at this exact category of bug recurring: the app was
// stable through multiple frames of real rendering as long as no thread was
// actively doing httpc network I/O, and became reproducibly unstable
// (crashing at a different point each identical run - the signature of
// stack-adjacent memory corruption, not a fixed logic bug) as soon as one
// was. 64KB was already known to be insufficient for less stack-hungry IPC
// work once before in this project (see kDefaultStackSize's own comment);
// HTTPS specifically is much more likely to need more.
constexpr size_t kNetworkStackSize = 256 * 1024;

// Spawns a detached thread running `fn` to completion, then discarding it -
// for fire-and-forget background work (network fetches, login attempts).
// Returns false (and drops fn entirely, without running it) if threadCreate()
// fails - see ctr_thread.cpp's own comment for why this must never silently
// fall back to running fn inline on the caller. Callers for whom "this
// background task just didn't start" needs to be reported (rather than
// silently retried/ignored) should check the return value.
bool SpawnDetached(std::function<void()> fn, size_t stackSize = kDefaultStackSize);

// Spawns a joinable thread running the plain function `entry(arg)` - for
// call sites that need to threadJoin()/threadFree() it later (e.g. to stop
// a long-running worker loop cleanly at shutdown). Returns nullptr on
// failure (out of thread slots).
Thread SpawnJoinable(ThreadFunc entry, void *arg, size_t stackSize = kDefaultStackSize);

} // namespace CtrThread
