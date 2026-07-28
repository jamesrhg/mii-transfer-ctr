#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal single-threaded HTTP/1.0 file server for the 3DS's local network -
// the direct 3DS port of the Wii U build's mii_http_server.h. Exists so a
// browser on the same network can pull raw files off the console - an
// index.html at `/` that browses this app's Mii sources in-page (via
// romfs:/mii-parser.js and romfs:/bg.png, served at `/mii-parser.js` and
// `/bg.png`), the raw Mii database at `/data/CFL_DB.dat` (renamed from the
// Wii U build's FFL_ODB.dat - covers both the Library tab and, client-side
// via mii-parser.js's own parseCflDbRecent(), the Recent Miis tab, which
// lives inside this same file rather than a separate endpoint - see that
// file's own header comment), and a raw, concatenated CFLStoreData blob
// (96 bytes per Mii, no encoding/separators) for the Friends tab at
// `/data/fp_db.dat`. Both /data/*.dat endpoints download as actual files
// (application/octet-stream + Content-Disposition: attachment), not inline
// text.
//
// Usage: Start() once at startup (returns immediately - see its own comment
// for why), poll GetState() once per frame to learn the outcome, Stop() at
// shutdown. Each connection is accepted and served to completion (no
// keep-alive) before the next one is accepted - fine for the handful of
// requests a single browser tab makes.
//
// 3DS port notes: uses libctru's soc:u BSD sockets instead of the Wii U
// build's socket_lib_init - but unlike that build, this module does *not*
// own socInit()/socExit() itself (an earlier version did): SOC is
// initialized once, centrally, in main.cpp, shared with CtrLog's UDP debug
// logger (which needs SOC up before anything else even starts - see
// ctr_log.h), since a second socInit() call in the same process doesn't
// work. This module also waits on CtrNetwork::WaitUntilDone() before doing
// anything network-related (gethostid() needs the AC connection that starts
// - see ctr_network.h for why that's needed at all on 3DS, unlike this
// build's original assumption that Wi-Fi association needed no app-side
// request). CFL_DB.dat has no simple on-disk path the way FFL_ODB.dat did
// (it lives inside a shared-extdata archive - see ctr_mii_db.h), so it's
// served from an in-memory blob via SetCflDbData(), the same shape as
// SetFriendMiiData() rather than re-read from disk per request.
namespace MiiHttpServer {

enum class State {
    Starting, // setup (socInit/bind/listen) still in progress on the background thread
    Ready,    // listening; GetLocalIpString()/GetPort() are valid
    Failed,   // setup failed - see GetError()
};

// Starts a background thread that does all of socInit()/socket()/bind()/
// listen() and then the accept loop - deliberately returns immediately
// rather than blocking on any of that: bind()/listen() are normally fast,
// but this still must never run on the caller's thread, because *nothing*
// in this app's startup sequence is allowed to block for long before the
// first frame renders (see main.cpp's own comment on why - the short
// version: an unresponsive app gets killed by the OS, and "nothing rendered
// yet, then the app just vanishes" is exactly what that looks like from the
// user's side). `port` is where it *starts* trying - if bind() fails (e.g.
// something else on this console already has that port), it retries on up
// to 6 consecutive ports (port, port+1, ... port+5) before giving up; see
// GetPort() for which one it actually ended up on.
void Start(uint16_t port);

// Stops the accept loop (if running) and closes the listening socket. Safe
// to call even if Start() wasn't, or failed. May block briefly waiting for
// the background thread's current setup step to notice and exit.
void Stop();

// Poll once per frame to learn how Start() turned out.
State GetState();

// Valid once GetState() == Failed - a short reason (e.g. "bind() failed on
// every port tried.").
std::string GetError();

// Replaces what's served at /data/CFL_DB.dat and /data/fp_db.dat
// respectively. Safe to call at any time, including from a different thread
// than Start()/Stop() - guarded internally by a mutex. An empty vector (the
// default before ever called) serves as a 404.
void SetCflDbData(std::vector<uint8_t> rawDbBytes);
void SetFriendMiiData(std::vector<uint8_t> rawStoreData);

// The console's assigned IPv4 address ("a.b.c.d") as of the last successful
// Start(), or an empty string if it couldn't be determined. Only valid once
// GetState() == Ready.
std::string GetLocalIpString();

// The port actually bound on the last successful Start() - see its comment
// for why this isn't always the `port` argument passed in. Only valid once
// GetState() == Ready.
uint16_t GetPort();

} // namespace MiiHttpServer
