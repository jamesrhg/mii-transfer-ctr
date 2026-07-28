#include "ctr_friends.h"

#include "ctr_thread.h"

#include <3ds.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace CtrFriends {

namespace {

// Only ever set true by the background thread after a successful frdInit(),
// read by Shutdown()/LoadFriendMiis() on the caller's thread - plain
// std::atomic is enough since it's a one-shot flag, not a rendezvous.
std::atomic<bool> g_initialized{false};
bool g_initRequested = false;

// Written only by the background thread spawned in BeginInit(), read only
// by Poll()/GetInitError() on the caller's thread, always under g_mutex -
// this is the entire hand-off between the two, see BeginInit()'s own
// comment for why frdInit() must never run on the caller's thread.
std::mutex g_mutex;
bool g_initDone = false;
State g_initResult = State::Connecting;
std::string g_initError;

// Formats a libctru Result as a Nintendo-style "XXX-YYYY" support code via
// FRD_ResultToErrorCode() when possible (the same code format the system's
// own error screens show), falling back to the raw hex Result if that
// conversion itself fails.
std::string DescribeResult(Result res) {
    u32 errorCode = 0;
    if (R_SUCCEEDED(FRD_ResultToErrorCode(&errorCode, res))) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%03lu-%04lu", (errorCode / 10000) % 1000, errorCode % 10000);
        return buf;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(res));
    return buf;
}

} // namespace

void Shutdown() {
    if (!g_initialized) return;
    frdExit();
    g_initialized = false;
}

void BeginInit() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_initDone = false;
        g_initResult = State::Connecting;
        g_initError.clear();
    }
    g_initRequested = true;

    // frdInit() alone - see this file's header comment for why no
    // FRD_Login() step exists here, and for why this runs on a background
    // thread rather than the caller's (a stuck frd: service, per the
    // GetServiceHandle behavior documented there, would otherwise stall the
    // whole app's render loop indefinitely instead of just this thread).
    bool spawned = CtrThread::SpawnDetached([]() {
        Result frdRes = frdInit(true); // true = force frd:u (user service) - see ctr_friends.h.

        std::lock_guard<std::mutex> lock(g_mutex);
        if (R_SUCCEEDED(frdRes)) {
            g_initialized = true;
            g_initResult = State::Connected;
        } else {
            g_initResult = State::Failed;
            g_initError = "Could not start the friend service (error " + DescribeResult(frdRes) + ").";
        }
        g_initDone = true;
    });
    if (!spawned) {
        // Couldn't even start the background thread - report Failed right
        // here rather than leaving Poll() stuck reporting Connecting forever
        // (g_initDone would otherwise never get set).
        std::lock_guard<std::mutex> lock(g_mutex);
        g_initResult = State::Failed;
        g_initError = "Could not start a background thread for the friend service (the app may be low on thread slots).";
        g_initDone = true;
    }
}

State Poll() {
    if (!g_initRequested) return State::Connecting;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initDone ? g_initResult : State::Connecting;
}

std::string GetInitError() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initError;
}

std::vector<Ver3MiiDecoded> LoadFriendMiis() {
    std::vector<Ver3MiiDecoded> miis;
    if (!g_initialized) return miis;

    std::array<FriendKey, FRIEND_LIST_SIZE> friendKeys{};
    u32 count = 0;
    if (R_FAILED(FRD_GetFriendKeyList(friendKeys.data(), &count, 0, FRIEND_LIST_SIZE)) || count == 0) {
        return miis;
    }

    std::vector<FriendMii> miiList(count);
    if (R_FAILED(FRD_GetFriendMii(miiList.data(), friendKeys.data(), count))) {
        return miis;
    }

    miis.reserve(count);
    for (u32 i = 0; i < count; i++) {
        std::array<uint8_t, sizeof(FriendMii)> raw;
        std::memcpy(raw.data(), &miiList[i], raw.size());
        miis.push_back(DecodeVer3MiiFromStoreData(raw));
    }
    return miis;
}

void UpdateGameModeDescription(const std::string &description) {
    if (!g_initialized) return;

    // FriendGameModeDescription is UTF-16, NULL-terminated, at most
    // FRIEND_GAME_MODE_DESCRIPTION_LEN-1 code units. A real UTF-8 decode,
    // not a plain byte-widening copy (an earlier version of this function
    // did that, on the wrong assumption that every caller only ever passes
    // its own generated ASCII text - main.cpp's own MiiDetails description
    // also embeds the *focused Mii's own nickname*, which is real UTF-8 -
    // ver3_mii.cpp's char16ToUtf8() - and can be outside ASCII, e.g.
    // fullwidth Japanese-input Latin characters; byte-widening split each
    // multi-byte UTF-8 sequence into multiple garbage UTF-16 code units
    // instead of decoding it, confirmed on-device as visibly corrupted
    // text). Mii nicknames are themselves BMP-only (decoded from a plain
    // UTF-16 field, never a surrogate pair to begin with), but this decodes
    // the general case anyway - encoding a codepoint above the BMP as a
    // proper UTF-16 surrogate pair - since it costs no more code than
    // assuming BMP-only. Malformed sequences decode as U+FFFD rather than
    // desyncing the rest of the string.
    FriendGameModeDescription desc{};
    size_t outLen = 0;
    size_t maxOutLen = FRIEND_GAME_MODE_DESCRIPTION_LEN - 1;
    size_t i = 0;
    while (i < description.size() && outLen < maxOutLen) {
        unsigned char b0 = static_cast<unsigned char>(description[i]);
        u32 codepoint;
        int extraBytes;
        if (b0 < 0x80) {
            codepoint = b0;
            extraBytes = 0;
        } else if ((b0 & 0xE0) == 0xC0) {
            codepoint = b0 & 0x1F;
            extraBytes = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            codepoint = b0 & 0x0F;
            extraBytes = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            codepoint = b0 & 0x07;
            extraBytes = 3;
        } else {
            codepoint = 0xFFFD;
            extraBytes = 0;
        }
        i++;

        bool valid = true;
        for (int k = 0; k < extraBytes; k++) {
            if (i >= description.size() || (static_cast<unsigned char>(description[i]) & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (static_cast<unsigned char>(description[i]) & 0x3F);
            i++;
        }
        if (!valid) codepoint = 0xFFFD;

        if (codepoint <= 0xFFFF) {
            desc[outLen++] = static_cast<u16>(codepoint);
        } else if (outLen + 1 < maxOutLen) {
            codepoint -= 0x10000;
            desc[outLen++] = static_cast<u16>(0xD800 + (codepoint >> 10));
            desc[outLen++] = static_cast<u16>(0xDC00 + (codepoint & 0x3FF));
        }
    }
    desc[outLen] = 0;

    FRD_UpdateGameModeDescription(&desc);
}

} // namespace CtrFriends
