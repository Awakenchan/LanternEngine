// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace BbEngine::MainJobsBarrier {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

namespace Layout {
inline constexpr u64 kSystem = 0x8;
inline constexpr u64 kDeque = 0x18;
inline constexpr u64 kDequeCount = 0x38;
inline constexpr u64 kIndex = 0xa0;
inline constexpr u64 kParticipantBytes = 0x530;
inline constexpr u64 kParticipants = 0x0;
inline constexpr u64 kCount = 0x8;
static_assert(kSystem < kDeque && kDeque < kDequeCount && kDequeCount < kIndex && kIndex < kParticipantBytes);
static_assert(kParticipants < kCount);
}

inline constexpr u32 kMostParticipants = 64;

enum class Check : std::uint8_t { Ok, NoSystem, NoParticipants, CountOutOfRange, NotInArray, NotOnBoundary, IndexDiffers };
enum class Route : std::uint8_t { Game, Ours };

struct Shape {
    u64 self = 0;
    u64 system = 0;
    u64 base = 0;
    u32 count = 0;
    u32 index = 0;
};

struct Outcome {
    bool swept = false;
    bool took = false;
};

#ifdef LANTERN_MAIN_JOBS_BARRIER_MUTANTS
enum class Mutant : int {
    None = 0,
    SweepWithOwnWork,
    StartIgnoresIndex,
    StartIn32Bits,
    KeepSweepingAfterSteal,
    StealFromItself,
    AcceptOffBoundary,
    AcceptIndexDiffers,
    RouteIgnoresCheck,
    LatchIgnoresRefusal,
    SkipGameStep,
    CountNotReloaded,
    ReadNullCaller,
    TakeNotExchanged,
    kCount
};
inline int g_mutant = 0;
inline bool Is(Mutant m) { return g_mutant == int(m); }
#define LANTERN_MJB_MUTANT(name) (::BbEngine::MainJobsBarrier::Is(::BbEngine::MainJobsBarrier::Mutant::name))
#else
#define LANTERN_MJB_MUTANT(name) false
#endif

constexpr Check CheckShape(const Shape& s) noexcept {
    if (!s.system) return Check::NoSystem;
    if (!s.base) return Check::NoParticipants;
    if (s.count == 0 || s.count > kMostParticipants) return Check::CountOutOfRange;
    if (s.self < s.base || s.self - s.base >= u64(s.count) * Layout::kParticipantBytes) return Check::NotInArray;
    if ((s.self - s.base) % Layout::kParticipantBytes != 0 && !LANTERN_MJB_MUTANT(AcceptOffBoundary)) return Check::NotOnBoundary;
    if (u64(s.index) != (s.self - s.base) / Layout::kParticipantBytes && !LANTERN_MJB_MUTANT(AcceptIndexDiffers))
        return Check::IndexDiffers;
    return Check::Ok;
}

constexpr Route Choose(bool on, Check check) noexcept {
    if (!on) return Route::Game;
    return check == Check::Ok || LANTERN_MJB_MUTANT(RouteIgnoresCheck) ? Route::Ours : Route::Game;
}

constexpr bool FrameLatch(bool step_on, bool refused) noexcept {
    return step_on && (!refused || LANTERN_MJB_MUTANT(LatchIgnoresRefusal));
}

constexpr u32 SweepStart(u32 index, u64 tsc, u32 count) noexcept {
    if (LANTERN_MJB_MUTANT(StartIgnoresIndex)) return u32(tsc % count);
    if (LANTERN_MJB_MUTANT(StartIn32Bits)) return u32(index + tsc) % count;
    return u32((u64(index) + tsc) % count);
}

struct CensusCounts {
    u64 total = 0;
    u64 covered = 0;
    u64 fell_back = 0;
    u64 used = 0;
};

struct alignas(64) Census {
    std::atomic<u64> total{0};
    std::atomic<u64> covered{0};
    std::atomic<u64> fell_back{0};
    std::atomic<u64> used{0};

    void Call(bool shape_ok, bool took) noexcept {
        total.fetch_add(1, std::memory_order_relaxed);
        if (shape_ok) covered.fetch_add(1, std::memory_order_relaxed);
        else fell_back.fetch_add(1, std::memory_order_relaxed);
        if (took) used.fetch_add(1, std::memory_order_relaxed);
    }
    CensusCounts Take() noexcept {
        CensusCounts c;
        c.used = TakeOne(used);
        c.fell_back = TakeOne(fell_back);
        c.covered = TakeOne(covered);
        c.total = TakeOne(total);
        return c;
    }

private:
    static u64 TakeOne(std::atomic<u64>& counter) noexcept {
        if (LANTERN_MJB_MUTANT(TakeNotExchanged)) {
            const u64 n = counter.load(std::memory_order_relaxed);
            for (int spin = 0; spin < 64; ++spin) __asm__ volatile("" ::: "memory");
            counter.store(0, std::memory_order_relaxed);
            return n;
        }
        return counter.exchange(0, std::memory_order_relaxed);
    }
};

template <class Ops> Shape ReadShape(const Ops& ops, u64 self) noexcept {
    Shape s;
    s.self = self;
    if (!self && !LANTERN_MJB_MUTANT(ReadNullCaller)) return s;
    s.system = ops.Load64(self + Layout::kSystem);
    if (!s.system) return s;
    s.base = ops.Load64(s.system + Layout::kParticipants);
    s.count = ops.Load32(s.system + Layout::kCount);
    s.index = ops.Load32(self + Layout::kIndex);
    return s;
}

template <class Ops> bool SweepOthers(Ops& ops, u64 self, u64 tsc) noexcept {
    u64 system = ops.Load64(self + Layout::kSystem);
    u32 count = ops.Load32(system + Layout::kCount);
    if (count == 0) return false;
    const u32 start = SweepStart(ops.Load32(self + Layout::kIndex), tsc, count);
    const u64 own = self + Layout::kDeque;
    bool took = false;
    for (u32 i = 0; i < count;) {
        const u64 at = ops.Load64(system + Layout::kParticipants) + u64((start + i) % count) * Layout::kParticipantBytes;
        if (at != self || LANTERN_MJB_MUTANT(StealFromItself)) {
            if (ops.Steal(at + Layout::kDeque, own) || ops.Load64(self + Layout::kDequeCount) != 0) {
                took = true;
                if (!LANTERN_MJB_MUTANT(KeepSweepingAfterSteal)) return true;
            }
            system = ops.Load64(self + Layout::kSystem);
        }
        ++i;
        if (!LANTERN_MJB_MUTANT(CountNotReloaded)) count = ops.Load32(system + Layout::kCount);
    }
    return took;
}

template <class Ops> Outcome RunStep(Ops& ops, u64 self) noexcept {
    if (!LANTERN_MJB_MUTANT(SkipGameStep)) ops.RunGameStep(self);
    if (ops.Load64(self + Layout::kDequeCount) != 0 && !LANTERN_MJB_MUTANT(SweepWithOwnWork)) return {};
    return {true, SweepOthers(ops, self, ops.Tsc())};
}
}
