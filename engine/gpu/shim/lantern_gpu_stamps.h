// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include "engine/bottleneck_meter.h"

#ifdef LANTERN_GPU_STAMPS_MUTANTS
namespace Lantern::GpuStamps {
inline int g_mutant = 0;
}
#define LANTERN_GPU_STAMPS_PICK(n, wrong, right) (::Lantern::GpuStamps::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_GPU_STAMPS_PICK(n, wrong, right) (right)
#endif

namespace Lantern::GpuStamps {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr int kMutants = 7;
inline constexpr u32 kSlots = 4096;
inline constexpr u32 kQueriesPerSlot = 2;
inline constexpr u32 kQueries = kSlots * kQueriesPerSlot;
inline constexpr std::size_t kOpen = 512;
inline constexpr u64 kLostNs = 2'000'000'000ull;

enum class State : u8 { Free, Begun, Ended, Lost };

struct Pending {
    u32 first = 0;
    u32 count = 0;
};

struct Result {
    bool available = false;
    u64 begin = 0;
    u64 end = 0;
};

struct Counts {
    u64 stamped = 0;
    u64 skipped = 0;
    u64 lost = 0;
    u64 collected = 0;
    u64 stale = 0;
    u64 reclaimed = 0;
};

class Slots {
public:
    std::optional<u32> Begin(u64 handle, u64 now_ns) {
        std::scoped_lock lock(mutex);
        while (next - oldest < kSlots && state[next % kSlots] == State::Lost) ++next;
        const u32 slot = u32(next % kSlots);
        const bool full = LANTERN_GPU_STAMPS_PICK(3, false, next - oldest >= kSlots || state[slot] != State::Free);
        if (full || open_used == kOpen) {
            ++counts.skipped;
            return std::nullopt;
        }
        state[slot] = State::Begun;
        begun_ns[slot] = now_ns;
        submit_tick[slot] = 0;
        open[open_used++] = {handle, slot};
        ++next;
        ++counts.stamped;
        return slot;
    }

    std::optional<u32> End(u64 handle, u64 tick) {
        std::scoped_lock lock(mutex);
        for (std::size_t i = 0; i < open_used; ++i) {
            if (open[i].handle != handle) continue;
            const u32 slot = open[i].slot;
            if (LANTERN_GPU_STAMPS_PICK(4, false, true)) open[i] = open[--open_used];
            submit_tick[slot] = tick;
            if (state[slot] == State::Lost) return std::nullopt;
            state[slot] = State::Ended;
            return slot;
        }
        return std::nullopt;
    }

    u32 Reclaim(u64 completed_tick) {
        std::scoped_lock lock(mutex);
        u32 freed = 0;
        for (std::size_t i = 0; i < lost_used;) {
            const u32 slot = lost[i];
            const u64 tick = submit_tick[slot];
            const bool done = tick && LANTERN_GPU_STAMPS_PICK(7, true, completed_tick >= tick);
            if (done && !Outstanding(slot)) {
                state[slot] = State::Free;
                lost[i] = lost[--lost_used];
                ++freed;
                ++counts.reclaimed;
            } else {
                ++i;
            }
        }
        return freed;
    }

    Pending Outstanding() {
        std::scoped_lock lock(mutex);
        return {u32(oldest % kSlots), u32(next - oldest)};
    }

    u32 LostNow() {
        std::scoped_lock lock(mutex);
        return u32(lost_used);
    }

    u32 Ready(Pending p, std::span<const Result> results, u64 now_ns, std::vector<BbEngine::BottleneckMeter::Span>& spans) {
        std::scoped_lock lock(mutex);
        u32 ready = 0;
        for (u32 i = 0; i < p.count && i < results.size(); ++i) {
            const u32 slot = (p.first + i) % kSlots;
            const Result& r = results[i];
            if (state[slot] == State::Lost) {
                ++ready;
                continue;
            }
            const bool fresh = LANTERN_GPU_STAMPS_PICK(
                5, true,
                LANTERN_GPU_STAMPS_PICK(6, r.end != last_end[slot] || r.begin != last_begin[slot],
                                        r.end != last_end[slot] && r.begin != last_begin[slot]));
            if (state[slot] == State::Ended && r.available && fresh) {
                spans.push_back({r.begin, r.end});
                last_begin[slot] = r.begin;
                last_end[slot] = r.end;
                ++counts.collected;
            } else if (now_ns - begun_ns[slot] > LANTERN_GPU_STAMPS_PICK(2, ~u64{0}, kLostNs)) {
                if (r.available && !fresh) ++counts.stale;
                ++counts.lost;
                if (state[slot] == State::Ended) RemoveOpen(slot);
                state[slot] = State::Lost;
                lost[lost_used++] = slot;
            } else {
                break;
            }
            ++ready;
        }
        return ready;
    }

    void Release(u32 count) {
        std::scoped_lock lock(mutex);
        for (u32 i = 0; i < count; ++i) {
            State& s = state[(oldest + i) % kSlots];
            if (s != State::Lost) s = State::Free;
        }
        oldest += LANTERN_GPU_STAMPS_PICK(1, u64{0}, u64{count});
    }

    Counts Read() {
        std::scoped_lock lock(mutex);
        return counts;
    }

    State StateOf(u32 slot) {
        std::scoped_lock lock(mutex);
        return state[slot % kSlots];
    }

private:
    struct Open {
        u64 handle = 0;
        u32 slot = 0;
    };

    bool Outstanding(u32 slot) const {
        const u64 from = oldest % kSlots;
        const u64 distance = (u64(slot) + kSlots - from) % kSlots;
        return distance < next - oldest;
    }

    void RemoveOpen(u32 slot) {
        for (std::size_t i = 0; i < open_used; ++i) {
            if (open[i].slot == slot) {
                open[i] = open[--open_used];
                return;
            }
        }
    }

    std::mutex mutex;
    std::array<State, kSlots> state{};
    std::array<u64, kSlots> begun_ns{};
    std::array<u64, kSlots> last_begin{};
    std::array<u64, kSlots> last_end{};
    std::array<u64, kSlots> submit_tick{};
    std::array<u32, kSlots> lost{};
    std::size_t lost_used = 0;
    std::array<Open, kOpen> open{};
    std::size_t open_used = 0;
    u64 next = 0;
    u64 oldest = 0;
    Counts counts;
};
}
