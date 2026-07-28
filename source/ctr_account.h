#pragma once

#include <string>
#include <vector>

#include "ver3_mii.h"

// Thin wrapper around 3DS ACT (libctru's act.h) - the 3DS equivalent of the
// Wii U build's nn_account.h/nn::act wrapper. Reads the Mii attached to each
// local console account slot. Unlike the Wii U (12 system account slots),
// the 3DS typically only ever has a handful of account slots occupied (in
// practice usually just one, the console's single linked Nintendo Network
// ID) - this enumerates however many ACT itself reports rather than
// hardcoding a slot count.
//
// Non-blocking by design, same shape as ctr_friends.h: BeginInit() kicks off
// actInit(true) (forced "act:u", the user-level service - the default
// admin-service request typically has no Homebrew-Launcher access grant,
// and srv:GetServiceHandle *waits* for an unavailable port rather than
// failing fast per 3dbrew's own docs, the same class of hang confirmed
// on-device for frdInit() - see ctr_friends.h's own comment) on a
// background thread; Poll() (call once per frame) reports how it's going
// without ever stalling the caller.
namespace CtrAccount {

enum class State {
    Connecting, // actInit() in flight, or BeginInit() not called yet
    Connected,  // actInit() succeeded; console Mii(s) can be loaded
    Failed,     // actInit() failed outright (not hung) - non-fatal, retryable (see BeginInit())
};

// Starts a background thread that runs actInit(true) alone and reports the
// result. Returns immediately. Safe to call again after Poll() has reported
// Failed (retry).
void BeginInit();

// Call once per frame after BeginInit(). Non-blocking (a mutex-guarded read
// of whatever the background thread has reported so far).
State Poll();

// Valid once Poll() returns Failed - a short, human-readable reason. Empty
// if Poll() hasn't returned Failed (yet).
std::string GetInitError();

// Finalizes ACT. Safe to call even if actInit() never succeeded or
// BeginInit() was never called.
void Shutdown();

// Reads every occupied account slot's Mii (ACT_GetAccountInfo(...,
// INFO_TYPE_MII)) and decodes each into a Ver3MiiDecoded. Local-only, no
// network - only meaningful once Poll() has returned Connected; called
// once, not per frame, safe to call synchronously.
//
// If `outCurrentUserIndex` is non-null, it's filled with the index (within
// the returned vector) of the currently active account's own Mii, or -1 if
// it couldn't be determined.
std::vector<Ver3MiiDecoded> LoadConsoleMiis(int *outCurrentUserIndex = nullptr);

} // namespace CtrAccount
