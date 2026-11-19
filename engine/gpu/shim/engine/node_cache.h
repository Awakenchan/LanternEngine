// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#include "job_lock.h"

namespace BbEngine::NodeCache {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr u64 kNodeSize = 0x18;
inline constexpr u64 kNodeAlign = 8;
inline constexpr u32 kCap = 1024;
inline constexpr int kCaches = 32;
inline constexpr int kFirstCacheWrong = 7;
inline constexpr int kCacheWrongCount = 7;

enum class Free : std::uint8_t { Keep, Pass, End };

constexpr Free OnFree(u64 p, u64 sentinel, u32 kept, bool keeping, bool held) {
    if (p != 0 && LANTERN_JOBS_PICK(7, false, p == sentinel)) return Free::End;
    if (p != 0 && keeping && LANTERN_JOBS_PICK(8, true, held) && LANTERN_JOBS_PICK(9, kept <= kCap, kept < kCap))
        return Free::Keep;
    return Free::Pass;
}

constexpr bool TakeOnAlloc(u64 size, u64 align, u32 kept, u64 top, bool held) {
    return LANTERN_JOBS_PICK(10, true, size == kNodeSize) && align <= kNodeAlign && LANTERN_JOBS_PICK(11, true, kept != 0) &&
           top != 0 && LANTERN_JOBS_PICK(12, true, held);
}

struct PoolFacts {
    u64 slot;
    u64 general;
    u64 general_vtable;
    u64 alloc;
    u64 free;
    u64 lock_vtable;
    u64 sentinel;
};

struct Expected {
    u64 general_vtable;
    u64 alloc;
    u64 free;
    u64 lock_vtable;
};

constexpr bool Adopt(const PoolFacts& f, const Expected& e) {
    return f.general != 0 && LANTERN_JOBS_PICK(13, true, f.slot == f.general) && f.general_vtable == e.general_vtable &&
           f.alloc == e.alloc && f.free == e.free && f.lock_vtable == e.lock_vtable && f.sentinel != 0;
}

}
