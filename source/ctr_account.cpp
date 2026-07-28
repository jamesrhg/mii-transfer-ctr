#include "ctr_account.h"

#include "ctr_thread.h"

#include <3ds.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace CtrAccount {

namespace {

// Generous upper bound on account slot numbers to probe - 3DS realistically
// only ever has a handful of local console accounts (usually just one, the
// console's single linked Nintendo Network ID), nowhere near the Wii U's 12
// fixed slots, but ACT doesn't expose "is this slot occupied" directly the
// way nn::act does, so slots are probed by trying INFO_TYPE_MII and
// treating failure as "not occupied", same idea as the Wii U version just
// without a fixed slot count to loop over.
constexpr u8 kMaxSlotsToProbe = 32;

std::atomic<bool> g_initialized{false};
bool g_initRequested = false;

std::mutex g_mutex;
bool g_initDone = false;
State g_initResult = State::Connecting;
std::string g_initError;

} // namespace

void Shutdown() {
    if (!g_initialized) return;
    actExit();
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

    bool spawned = CtrThread::SpawnDetached([]() {
        // true = force the user-level "act:u" service. The default (false)
        // asks for the admin service "act:a" first, which a Homebrew
        // Launcher app typically has no access grant for; srv:GetServiceHandle
        // then *blocks* waiting for a port that will never become available
        // (confirmed via 3dbrew's SRV:GetServiceHandle docs - it waits
        // rather than failing fast unless the caller sets a "return
        // immediately" flag, which actInit()/frdInit() don't) - the same
        // hang class confirmed on-device for frdInit() (see ctr_friends.h).
        // Runs on a background thread regardless, in case this console/CFW
        // combination doesn't have act:u instantly available either.
        Result actRes = actInit(true);

        std::lock_guard<std::mutex> lock(g_mutex);
        if (R_SUCCEEDED(actRes)) {
            g_initialized = true;
            g_initResult = State::Connected;
        } else {
            g_initResult = State::Failed;
            g_initError = "actInit failed.";
        }
        g_initDone = true;
    });
    if (!spawned) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_initResult = State::Failed;
        g_initError = "Could not start a background thread for the account service (the app may be low on thread slots).";
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

std::vector<Ver3MiiDecoded> LoadConsoleMiis(int *outCurrentUserIndex) {
    std::vector<Ver3MiiDecoded> miis;
    if (outCurrentUserIndex) *outCurrentUserIndex = -1;
    if (!g_initialized) return miis;

    u8 numAccounts = 0;
    ACT_GetCommonInfo(&numAccounts, sizeof(numAccounts), INFO_TYPE_COMMON_NUM_ACCOUNTS);

    u8 currentSlot = 0xFF; // sentinel that never matches a real slot below
    ACT_GetCommonInfo(&currentSlot, sizeof(currentSlot), INFO_TYPE_COMMON_CURRENT_ACCOUNT_SLOT);

    for (u8 slot = 0; slot < kMaxSlotsToProbe && miis.size() < numAccounts; slot++) {
        CFLStoreData storeData{};
        if (R_FAILED(ACT_GetAccountInfo(&storeData, sizeof(storeData), slot, INFO_TYPE_MII))) continue;

        std::array<uint8_t, sizeof(CFLStoreData)> raw;
        std::memcpy(raw.data(), &storeData, raw.size());

        if (outCurrentUserIndex && slot == currentSlot) *outCurrentUserIndex = static_cast<int>(miis.size());
        miis.push_back(DecodeVer3MiiFromStoreData(raw));
    }
    return miis;
}

} // namespace CtrAccount
