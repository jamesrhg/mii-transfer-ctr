#pragma once

#include <string>
#include <vector>

#include "ver3_mii.h"

// Thin wrapper around 3DS FRD (libctru's frd.h) - the 3DS equivalent of the
// Wii U build's nn_friends.h/nn::fp wrapper. Reads the friend list and
// friend Mii data already synced to this console, offline.
//
// No FRD_Login() here at all, unlike an earlier revision of this module
// (which logged in first, matching the Wii U build's own nn::fp flow) - logging in
// is specifically what makes *this* console visible as online to friends
// and unlocks *live presence* data (FRD_GetFriendPresence's online/game-mode
// info); it requires an active AC connection first (see ctr_network.h) and
// fails under Parental Controls restrictions. Reading the friend list and
// each friend's Mii/profile (FRD_GetFriendKeyList/FRD_GetFriendMii/
// FRD_GetFriendProfile) has no such requirement: that data is the console's
// own locally-synced friend cache, kept up to date by the system in the
// background independent of any particular application. Skipping the login
// step entirely means this feature needs no network connection at all, and
// removes an entire class of failure (login rejected/banned account/AC not
// connected/15s timeout) that has nothing to do with what this app actually
// needs.
//
// Non-blocking by design: BeginInit() kicks off frdInit() on a background
// thread; Poll() (call once per frame) reports how it's going without ever
// stalling the caller. frdInit() was originally assumed to be local/fast
// enough to call synchronously (it's just one srvGetServiceHandle() call, no
// explicit network round-trip), but that assumption was wrong: confirmed
// on-device (UDP debug log showing execution stopped inside it, with BGM
// still playing - i.e. the main thread was stuck, not crashed) that
// frdInit() can block for a long time. Per 3dbrew's SRV:GetServiceHandle
// docs, GetServiceHandle *waits* for the target service's port to become
// available rather than failing fast unless the caller sets a "return
// immediately" flag - libctru's frdInit() doesn't set that flag, so a frd:
// service that isn't immediately ready blocks the caller indefinitely
// instead of erroring out. Backgrounding it means a stuck frd: service (the
// suspected cause: Homebrew Launcher's own exheader service-access list may
// not grant a frd: entry to loaded .3dsx content at all, in which case this
// wait truly never ends) leaves the Friends tab stuck "Connecting" forever
// instead of taking the whole app down with it.
namespace CtrFriends {

enum class State {
    Connecting, // frdInit() in flight, or BeginInit() not called yet
    Connected,  // frdInit() succeeded; friend data can be loaded
    Failed,     // frdInit() failed outright (not hung) - non-fatal, retryable (see BeginInit())
};

// Starts a background thread that runs frdInit() alone (no login - see this
// header's own comment for why that's not needed for what this module
// reads) and reports the result. Returns immediately. Safe to call again
// after Poll() has reported Failed (retry).
void BeginInit();

// Call once per frame after BeginInit(). Non-blocking (a mutex-guarded read
// of whatever the background thread has reported so far).
State Poll();

// Valid once Poll() returns Failed - a short, human-readable reason (via
// FRD_ResultToErrorCode(), the same support-code format the system's own
// error screens use). Empty if Poll() hasn't returned Failed (yet).
std::string GetInitError();

// Finalizes FRD. Safe to call even if frdInit() never succeeded or
// BeginInit() was never called. Note: if a BeginInit() is still in flight
// on its background thread, this does not (and cannot, since the thread is
// detached) wait for or cancel it - the same fire-and-forget risk the Wii U
// build's callback-based LoginAsync had.
void Shutdown();

// Fetches the local user's friends (FRD_GetFriendKeyList()) and their Mii
// data (FRD_GetFriendMii()) in one batched call. Only meaningful once
// Poll() has returned Connected; called once, not per frame. Purely local
// calls - safe to call synchronously.
std::vector<Ver3MiiDecoded> LoadFriendMiis();

// Sets this console's "game mode description" (FRD_UpdateGameModeDescription()
// - what a friend sees under this app's name on their own Friend List, e.g.
// "VS. Mode" for an actual game) to a UTF-8 string (real Mii nicknames,
// which can be outside ASCII, included - see this function's own .cpp
// comment for the UTF-8-to-UTF-16 decode), truncated to
// FRIEND_GAME_MODE_DESCRIPTION_LEN-1 UTF-16 code units. A no-op (not an
// error) if Poll() hasn't returned Connected yet - there's no valid frd:
// session to call it on. Purely local like LoadFriendMiis() (no
// FRD_Login() here - see this header's own top comment) - unconfirmed on
// real hardware whether this is actually visible to anyone without ever
// logging in, since nothing else in this app depends on the answer either
// way (worst case it's a harmless local no-op).
void UpdateGameModeDescription(const std::string &description);

} // namespace CtrFriends
