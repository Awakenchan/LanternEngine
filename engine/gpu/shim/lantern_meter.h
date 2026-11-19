// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include "lantern_switches.h"

namespace Lantern::Meter {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr std::size_t kLine = 64;
inline constexpr u64 kDrawPublishMask = 255;
inline constexpr std::size_t kDrawThreads = 31;
inline constexpr std::size_t kNameBytes = 16;
inline constexpr u32 kPlacePublishMask = 63;
inline constexpr std::size_t kPlaceThreads = 32;

struct alignas(kLine) DrawSlot {
    std::atomic<u64> draws{0};
    std::atomic<bool> used{false};
    char name[kNameBytes] = {};
};

struct DrawCounter {
    DrawSlot* slot = nullptr;
    bool shared = false;
    u64 count = 0;
    u64 published = 0;
};

struct alignas(kLine) PlaceSlot {
    std::atomic<u32> tid{0};
    std::atomic<u32> cpu{~0u};
};

struct PlaceCounter {
    PlaceSlot* slot = nullptr;
    u32 calls = 0;
};

struct State {
    std::array<DrawSlot, kDrawThreads + 1> draw_slots{};
    alignas(kLine) std::atomic<void (*)()> gpu_collect{nullptr};
    std::atomic<u64> gpu_busy_ns{0};
    std::atomic<bool> gpu_seen{false};
    alignas(kLine) std::atomic<u64> cap_wait_ns{0};
    std::atomic<u64> flip_hold_ns{0};
    std::atomic<u32> flip_cap{0};
    std::atomic<u32> game_cap{0};
    alignas(kLine) std::atomic<u64> barrier_lo{0};
    std::atomic<u64> main_samples{0};
    std::atomic<u64> main_stopped_ns{0};
    alignas(kLine) std::atomic<u64> frames{0};
    std::atomic<u64> first_flip_ns{0};
    std::atomic<u64> stall_ns{0};
    u64 last_flip_ns = 0;
    alignas(kLine) std::atomic<const std::atomic<u64>*> stage_b_busy_cycles{nullptr};
    std::atomic<const std::atomic<u64>*> stage_a_waited_cycles{nullptr};
    alignas(kLine) std::array<PlaceSlot, kPlaceThreads> place_slots{};
};

inline State g_state;

inline bool On() {
    return Switches::Get().meter != 0;
}

inline u64 SteadyNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

void PublishDraws(DrawCounter& counter);
inline thread_local DrawCounter t_draws;

inline void NoteDraw() {
    if (!On()) return;
    DrawCounter& c = t_draws;
    ++c.count;
    if (!c.slot || (c.count & kDrawPublishMask) == 0) PublishDraws(c);
}

inline void SetGpuCollect(void (*collect)()) {
    g_state.gpu_collect.store(collect, std::memory_order_release);
}

inline void PublishGpuUnknown() {
    g_state.gpu_seen.store(false, std::memory_order_release);
}

inline void PublishGpuBusy(u64 total_ns) {
    g_state.gpu_busy_ns.store(total_ns, std::memory_order_relaxed);
    g_state.gpu_seen.store(true, std::memory_order_release);
}

inline void NoteCapWait(u64 ns) {
    if (On() && ns) g_state.cap_wait_ns.fetch_add(ns, std::memory_order_relaxed);
}

inline void NoteFlipHold(u64 ns) {
    if (On() && ns) g_state.flip_hold_ns.fetch_add(ns, std::memory_order_relaxed);
}

inline void SetFlipCap(u32 fps) {
    g_state.flip_cap.store(fps, std::memory_order_relaxed);
}

inline void SetGameCap(u32 fps) {
    g_state.game_cap.store(fps, std::memory_order_relaxed);
}

inline void WatchPipe(const std::atomic<u64>* stage_b_busy_cycles, const std::atomic<u64>* stage_a_waited_cycles) {
    g_state.stage_b_busy_cycles.store(stage_b_busy_cycles, std::memory_order_release);
    g_state.stage_a_waited_cycles.store(stage_a_waited_cycles, std::memory_order_release);
}

inline constexpr u32 kStageASampleMask = 15;
inline constexpr long kMeasureMode = 2;
inline thread_local u32 t_stage_a_draws = 0;

inline bool SplitOn() {
    return Switches::Get().meter >= kMeasureMode;
}

inline bool SampleStageA() {
    return SplitOn() && (++t_stage_a_draws & kStageASampleMask) == 0;
}

void OnFlip(u64 now_ns);
void Detach();
bool NoteGuestImage(const std::uint8_t* image, std::size_t size);
void OnExit();

inline bool PlacementOn() {
    const char* list = Switches::Get().bb_sample_threads;
    return list && list[0];
}

void PublishPlace(PlaceCounter& counter);
inline thread_local PlaceCounter t_place;

inline void NotePlace() {
    if (!PlacementOn()) return;
    PlaceCounter& c = t_place;
    if ((c.calls++ & kPlacePublishMask) == 0) PublishPlace(c);
}

bool CpuOfThread(u32 tid, u32* cpu);
}
