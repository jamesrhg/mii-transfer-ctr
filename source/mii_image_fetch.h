#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Fetches Mii face-render images (PNG bytes) one at a time from
// http://mii-unsecure.ariankordi.net/miis/image.png, given each Mii's
// FFLStoreData/CFLStoreData (see ver3_mii.h).
//
// Usage: Start() once at startup (brings up httpc), FetchImageBlocking() as
// many times as needed (from any thread - each call opens/closes its own
// httpc context, see that function's own comment), Stop() at shutdown.
//
// 3DS port note: uses libctru's own httpc (system HTTP client), *not*
// libcurl+mbedtls (3ds-curl/3ds-mbedtls portlibs) the way the Wii U build
// and an earlier revision of this module did. httpc was ruled out
// previously specifically for HTTPS: it relies on the console's own system
// SSL module, whose TLS support is frozen at the console's original-era
// protocol/cipher set and can no longer complete a handshake with any
// modern, hardened HTTPS server - confirmed on-device and independently by
// libctru's own maintainer
// (https://github.com/devkitPro/libctru/issues/82#issuecomment-659619917).
// That blocker doesn't apply here anymore: this URL is plain http://, not
// https://, so there's no TLS handshake to fail in the first place -
// mii-unsecure.ariankordi.net (its own name says as much) serves plain HTTP
// directly rather than forcing a redirect to HTTPS, confirmed by hand
// against the real server. Switching back to httpc drops the
// libcurl+mbedtls dependency entirely and, unlike libcurl (which talks BSD
// sockets directly), doesn't need soc:u at all - httpc is its own separate
// 3DS system service reached over IPC, independent of mii_http_server.cpp's
// own soc:u-based listening socket.
//
// This module used to also own a background worker-queue
// (Start/SetWanted/PollCompleted) for the scrolling list's own thumbnails -
// removed since that feature was never actually wired up in this floor
// build (the list only ever shows plain nickname text, no per-row images)
// and the queue sat entirely dead. MiiDetailPanel now owns its own
// dedicated background worker for the one thing this app actually fetches
// images for (the focused Mii's portrait) - see its own comment for why
// that lives there instead of here.
namespace MiiImageFetcher {

// Result of a single completed image fetch.
struct Result {
    bool success = false;
    std::vector<uint8_t> pngBytes; // valid only if success
};

// Initializes httpc. Safe to call more than once (no-op after the first).
void Start();

// Tears down httpc. Safe to call even if Start() wasn't.
void Stop();

// Synchronously fetches one Mii's face-render PNG at `widthPx`, on the
// calling thread - safe to call from a background thread (each call opens
// its own httpc context; httpc itself needs no cross-call synchronization
// the way a shared curl handle would). Only valid to call between Start()
// and Stop().
// `expression`, if non-empty, is passed through as the "expression" query
// param (e.g. "blink") - empty means whatever the service defaults to.
// `cancel`, if non-null, is polled periodically during the transfer; setting
// it to true from another thread aborts the request promptly instead of
// waiting for it to finish or time out - the call still returns (with
// success=false) rather than blocking.
Result FetchImageBlocking(const std::array<uint8_t, 96> &storeData, int widthPx,
                           const std::string &expression = "", const std::atomic<bool> *cancel = nullptr);

} // namespace MiiImageFetcher
