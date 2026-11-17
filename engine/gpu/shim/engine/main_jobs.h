// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "engine_hooks.h"
#include "job_lock.h"

namespace BbEngine::MainJobs {

inline constexpr std::size_t kLock = 0x20368c0;
inline constexpr std::size_t kTryLock = 0x20369b0;
inline constexpr std::size_t kUnlock = 0x2036a40;
inline constexpr std::size_t kProcessOne = 0x2032370;
inline constexpr std::size_t kAppend = 0x2032780;
inline constexpr std::size_t kEnqueueUnwoken = 0x20322e0;
inline constexpr std::size_t kGeneralAlloc = 0x1fc5f30;
inline constexpr std::size_t kGeneralFree = 0x1fc5fc0;
inline constexpr std::size_t kSelfStub = 0x2bbfe78;
inline constexpr std::size_t kWaitSemaStub = 0x2bbec78;
inline constexpr std::size_t kSignalSemaStub = 0x2bbec88;
inline constexpr std::size_t kSelfGot = 0x53e5370;
inline constexpr std::size_t kWaitSemaGot = 0x53e4a70;
inline constexpr std::size_t kSignalSemaGot = 0x53e4a78;
inline constexpr std::size_t kStackGuardGot = 0x53e3be0;
inline constexpr std::size_t kPanic = 0x20b55b0;
inline constexpr std::size_t kPanicFile = 0x493b017;
inline constexpr std::size_t kPanicText = 0x493b0cb;
inline constexpr std::size_t kMutexVtables[] = {0x53a19b0, 0x53aed80, 0x53b1ab0};
inline constexpr std::size_t kQueueLockVtable = 0x53a19b0;
inline constexpr std::size_t kGeneralAllocatorSlot = 0x5540420;
inline constexpr std::size_t kGeneralVtable = 0x53aefa0;
inline constexpr std::size_t kAllocatorSlots = 28;
inline constexpr std::size_t kAllocSlot = 0x58 / 8;
inline constexpr std::size_t kFreeSlot = 0x70 / 8;

inline constexpr std::size_t kMutexBudget = 0x8;
inline constexpr std::size_t kMutexSema = 0x10;
inline constexpr std::size_t kMutexWord = 0x18;
inline constexpr std::size_t kMutexLockSlot = 0x18;
inline constexpr std::size_t kMutexUnlockSlot = 0x28;
inline constexpr std::size_t kPoolSentinel = 0x18;
inline constexpr std::size_t kPoolAllocator = 0x28;
inline constexpr std::size_t kPoolQueueLock = 0x30;

struct Game {
    std::uint64_t self = 0;
    std::uint64_t wait_sema = 0;
    std::uint64_t panic = 0;
    std::uint64_t panic_file = 0;
    std::uint64_t panic_text = 0;
    std::array<std::uint64_t, std::size(kMutexVtables)> mutex_vtables{};
    std::uint64_t general_slot = 0;
    std::uint64_t general_vtable = 0;
    std::uint64_t general_alloc = 0;
    std::uint64_t general_free = 0;
    std::uint64_t queue_lock_vtable = 0;
};

Game GameAt(const std::uint8_t* image);

enum class Body : std::uint8_t { Lock, TryLock, Unlock, ProcessOne, Append, GeneralAlloc, GeneralFree };
[[nodiscard]] bool BodyIs109(const std::uint8_t* image, std::size_t size, Body body);

inline constexpr int kSwitchWrongCount = 1;

template <typename Switch> void FollowStep(Switch& value, bool step, bool& step_was) {
    if (LANTERN_JOBS_PICK(14, true, step != step_was)) value.store(int(step), std::memory_order_relaxed);
    step_was = step;
}

[[nodiscard]] bool Bind(const Game& game);
void SetActive(bool lock, bool cache);
std::atomic<int>* LockSwitch();
std::atomic<int>* CacheSwitch();
Callback LockCallback();
Callback PopCallback();

enum Counter : std::uint8_t {
    LockFirst,
    LockAgain,
    LockSpun,
    LockSlept,
    LockPassed,
    AllocHit,
    AllocPassed,
    FreeKept,
    FreePassed,
    NotHeld,
    PoolsAdopted,
    PoolsEnded,
    PoolsRefused,
    NodesGivenBack,
    BrokenLists,
    kCounters
};
struct Totals {
    std::array<std::uint64_t, kCounters> n{};
    std::uint32_t live = 0;
    std::uint64_t kept_now = 0;
};
Totals Read();
void FlushThread();

bool Install(std::uint8_t* image, std::size_t size);
void Tick();
}
