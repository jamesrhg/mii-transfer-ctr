#include "ctr_thread.h"

#include "ctr_log.h"

namespace CtrThread {

namespace {

void Trampoline(void *arg) {
    auto *fn = static_cast<std::function<void()> *>(arg);
    (*fn)();
    delete fn;
}

// Child threads get strictly *lower* priority (a higher numeric value) than
// whichever thread is spawning them - the opposite of what an earlier
// version of this function did (prio-1, higher priority than the caller),
// which was copied from devkitPro's threads/thread-basic example without
// noticing that example's justification doesn't apply here: that example
// is about a *child* thread starving on a locked stdio resource the *main*
// thread holds, which higher child priority fixes. Every background thread
// in this app is the opposite shape - fire-and-forget network/curl work
// (blocking AC connect waits, CPU-heavy mbedtls TLS handshakes) that the
// *main* render thread should never be starved behind. Giving these
// threads higher priority let them preempt the render loop for however
// long their blocking/CPU-bound work took, which on real hardware looked
// exactly like a dead app (black screen, silently returned to Homebrew
// Launcher) rather than a clean hang or crash - there was nothing to catch,
// the render loop just never got scheduled. Every call site here spawns
// from the main thread, so this reads *that* thread's priority each time
// rather than hardcoding a value.
//
// 0x3F is the lowest priority userland apps are allowed to request
// (threadCreate() requires the range [0x18;0x3F], higher number = lower
// priority) - clamped here for the same reason the old code clamped
// against 0x18: if the caller's own priority is already at or near that
// floor, prio+1 would fall outside the valid range and threadCreate()
// would simply fail.
s32 LowerPriorityThanCaller() {
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s32 lower = prio + 1;
    return lower > 0x3F ? 0x3F : lower;
}

} // namespace

bool SpawnDetached(std::function<void()> fn, size_t stackSize) {
    auto *heapFn = new std::function<void()>(std::move(fn));
    Thread t = threadCreate(Trampoline, heapFn, stackSize, LowerPriorityThanCaller(), -2, true);
    if (!t) {
        // threadCreate() failing here used to fall back to running fn
        // *inline on the caller* as a "better than silently dropping it"
        // last resort - that was itself a real startup-hang bug (confirmed
        // on-device): CtrNetwork::BeginConnect() and CtrFriends::BeginLogin()
        // both call this from the main thread, and their fn does blocking
        // network IPC, so "inline on the caller" meant blocking the main
        // thread for up to a 15s timeout, indistinguishable from a genuine
        // hang. Dropping the task and reporting failure to the caller (who
        // can surface it as a normal Failed/retry state) is strictly safer
        // than silently re-introducing that exact hang, even though it
        // means the task just doesn't run this attempt.
        CtrLog::Printf("CtrThread::SpawnDetached: threadCreate() failed (out of thread slots?) - dropping task");
        delete heapFn;
        return false;
    }
    return true;
}

Thread SpawnJoinable(ThreadFunc entry, void *arg, size_t stackSize) {
    return threadCreate(entry, arg, stackSize, LowerPriorityThanCaller(), -2, false);
}

} // namespace CtrThread
