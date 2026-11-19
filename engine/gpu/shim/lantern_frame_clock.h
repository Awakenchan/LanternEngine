// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace LanternFrameClock {

struct Grid {
    std::int64_t anchor = 0;
    std::int64_t period = 0;
    std::int64_t vblank = 0;
};

inline std::int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

inline std::int64_t CeilDiv(std::int64_t a, std::int64_t b) {
    return -FloorDiv(-a, b);
}

constexpr std::int64_t kPhaseShiftPerFrameDivisor = 8;
constexpr std::int64_t kSlotJitterDivisor = 256;

inline std::int64_t SlotAtOrBefore(std::int64_t t, const Grid& grid) {
    const std::int64_t jitter = grid.period / kSlotJitterDivisor;
    return grid.anchor + FloorDiv(t + jitter - grid.anchor, grid.period) * grid.period;
}

inline std::int64_t NextStart(std::int64_t now, std::int64_t previous, const Grid& grid) {
    if (grid.period <= 0 || !previous) {
        return now;
    }
    const std::int64_t due = previous + grid.period;
    if (now >= due) {
        return now;
    }
    if (grid.vblank <= 0) {
        return due;
    }
    const std::int64_t shift = grid.period / kPhaseShiftPerFrameDivisor;
    return std::max(now, std::max(SlotAtOrBefore(due, grid), due - shift));
}

inline std::int64_t Step(std::int64_t start, std::int64_t previous, const Grid& grid, std::int64_t& carry) {
    if (!previous || start <= previous) {
        carry = 0;
        return grid.period > 0 ? grid.period : 0;
    }
    const std::int64_t interval = start - previous;
    if (grid.vblank <= 0) {
        carry = 0;
        return interval;
    }
    const std::int64_t owed = interval + carry;
    const std::int64_t refreshes = std::max<std::int64_t>(1, (owed + grid.vblank / 2) / grid.vblank);
    const std::int64_t step = refreshes * grid.vblank;
    carry = std::clamp(owed - step, -grid.vblank, grid.vblank);
    return step;
}

inline std::int64_t StepMicros(std::int64_t step_ns, float game_target_seconds) {
    const std::int64_t target = std::int64_t(double(game_target_seconds) * 1e6);
    return std::max<std::int64_t>((step_ns + 500) / 1000, target);
}

constexpr double kTargetMatchTolerance = 0.02;
constexpr std::int64_t kSpinBeforeDeadlineNs = 2000000;

inline bool PacesTarget(float game_target_seconds, float rate) {
    if (!(game_target_seconds > 0.0f) || !(rate > 0.0f) || !std::isfinite(game_target_seconds)) {
        return false;
    }
    return std::fabs(double(game_target_seconds) * double(rate) - 1.0) < kTargetMatchTolerance;
}

inline double OffWholePeriods(double dt_ns, double vblank_ns) {
    if (vblank_ns <= 0.0 || dt_ns <= 0.0) {
        return 0.0;
    }
    const double whole = std::max(1.0, std::round(dt_ns / vblank_ns)) * vblank_ns;
    return std::fabs(dt_ns - whole);
}

struct Shared {
    std::mutex mutex;
    Grid grid;
    std::atomic<bool> published{false};
    std::atomic<void (*)(std::int64_t)> waiter{nullptr};
};

inline Shared g_shared;

inline void Publish(std::int64_t anchor, std::int64_t period, std::int64_t vblank) {
    std::scoped_lock lock{g_shared.mutex};
    g_shared.grid = {anchor, period, vblank};
    g_shared.published.store(period > 0, std::memory_order_release);
}

inline void Clear() {
    std::scoped_lock lock{g_shared.mutex};
    g_shared.grid = {};
    g_shared.published.store(false, std::memory_order_release);
}

inline bool Current(Grid& out) {
    if (!g_shared.published.load(std::memory_order_acquire)) {
        return false;
    }
    std::scoped_lock lock{g_shared.mutex};
    out = g_shared.grid;
    return out.period > 0;
}

inline std::int64_t LockedVblank() {
    Grid grid;
    return Current(grid) ? grid.vblank : 0;
}

inline void SetWaiter(void (*wait)(std::int64_t)) {
    g_shared.waiter.store(wait, std::memory_order_release);
}

inline void WaitUntil(std::int64_t deadline) {
    if (auto* wait = g_shared.waiter.load(std::memory_order_acquire)) {
        wait(deadline);
        return;
    }
    const std::int64_t left = deadline - SteadyNs();
    if (left > kSpinBeforeDeadlineNs) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(left - kSpinBeforeDeadlineNs));
    }
    while (SteadyNs() < deadline) {
        std::this_thread::yield();
    }
}

inline bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("LANTERN_FRAME_START_PACING");
        return !(v && v[0] == '0');
    }();
    return on;
}

inline std::atomic<bool>& Live() {
    static std::atomic<bool> on{Enabled()};
    return on;
}

}
