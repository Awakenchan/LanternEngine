// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#ifdef LANTERN_JOBS_WRONG
namespace BbEngine::JobLock {
inline int g_wrong = 0;
}
#define LANTERN_JOBS_PICK(n, wrong, right) (::BbEngine::JobLock::g_wrong == (n) ? (wrong) : (right))
#else
#define LANTERN_JOBS_PICK(n, wrong, right) (right)
#endif

namespace BbEngine::JobLock {

using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

inline constexpr u64 kOwnerMask = 0xffffffffffull;
inline constexpr u64 kSleepersMask = ~kOwnerMask;
inline constexpr u64 kOneSleeper = kOwnerMask + 1;
inline constexpr u64 kHandedOwner = kOwnerMask;
inline constexpr int kLockWrongCount = 6;

enum class Next : std::uint8_t { Take, Watch, CountIn };

struct Turn {
    Next next;
    u64 want;
};

constexpr bool OwnerMayTake(u64 word, u64 self) {
    const u64 owner = word & kOwnerMask;
    return owner == 0 || LANTERN_JOBS_PICK(2, false, owner == self);
}

constexpr Turn Decide(u64 word, u64 self, bool within_budget) {
    if (OwnerMayTake(word, self))
        return {Next::Take, LANTERN_JOBS_PICK(1, self & kOwnerMask, (word & kSleepersMask) | (self & kOwnerMask))};
    if (LANTERN_JOBS_PICK(3, false, within_budget)) return {Next::Watch, word};
    return {Next::CountIn, word + LANTERN_JOBS_PICK(6, u64(1), kOneSleeper)};
}

constexpr bool Taken(u64 word, u64 self) {
    return (word & kOwnerMask) == self;
}

constexpr u64 Deadline(u64 start, i32 budget) {
    return start + LANTERN_JOBS_PICK(5, u64(std::uint32_t(budget)), u64(i64(budget)));
}

constexpr bool WithinBudget(u64 now, u64 start, i32 budget) {
    return LANTERN_JOBS_PICK(4, now <= Deadline(start, budget), now < Deadline(start, budget));
}

constexpr bool HeldBy(u64 word, u64 self) {
    return (word & kOwnerMask) == self;
}

}
