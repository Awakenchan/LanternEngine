// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "lantern_frame_clock.h"
#include "lantern_switches.h"

namespace LanternPacingStats {

struct Summary {
    std::size_t count = 0;
    double mean = 0.0;
    double sd = 0.0;
    double min = 0.0;
    double max = 0.0;
};

inline Summary Summarize(const std::uint32_t* samples, std::size_t n) {
    Summary s;
    if (n == 0) {
        return s;
    }
    double sum = 0.0;
    std::uint32_t lo = samples[0], hi = samples[0];
    for (std::size_t i = 0; i < n; ++i) {
        sum += samples[i];
        lo = std::min(lo, samples[i]);
        hi = std::max(hi, samples[i]);
    }
    const double mean = sum / double(n);
    double sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = double(samples[i]) - mean;
        sq += d * d;
    }
    s.count = n;
    s.mean = mean;
    s.sd = std::sqrt(sq / double(n));
    s.min = lo;
    s.max = hi;
    return s;
}

inline double Percentile(std::uint32_t* samples, std::size_t n, double p) {
    if (n == 0) {
        return 0.0;
    }
    const double wanted = std::ceil(std::clamp(p, 0.0, 100.0) / 100.0 * double(n));
    const std::size_t rank = std::clamp<std::size_t>(std::size_t(wanted), 1, n) - 1;
    std::nth_element(samples, samples + rank, samples + n);
    return samples[rank];
}

template <std::size_t N>
class Ring {
public:
    void Push(std::uint32_t value) {
        const std::uint64_t at = written.load(std::memory_order_relaxed);
        slots[at % N].store(value, std::memory_order_relaxed);
        written.store(at + 1, std::memory_order_release);
    }

    std::size_t Drain(std::uint32_t* out) {
        const std::uint64_t end = written.load(std::memory_order_acquire);
        const std::uint64_t begin = std::max(read, end > N ? end - N : 0);
        for (std::uint64_t i = begin; i < end; ++i) {
            out[i - begin] = slots[i % N].load(std::memory_order_relaxed);
        }
        read = end;
        return std::size_t(end - begin);
    }

private:
    std::array<std::atomic<std::uint32_t>, N> slots{};
    std::atomic<std::uint64_t> written{0};
    std::uint64_t read = 0;
};

constexpr std::size_t kRing = 4096;

struct State {
    Ring<kRing> step, dt, dt_off, dt_grid, present, depth, pad, wait;
    std::atomic<std::int64_t> last_step{0}, last_present{0}, pending_pad{0};
};

inline State g_state;

inline bool Enabled() {
    return Lantern::Switches::Load().frame_stats;
}

inline std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline std::uint32_t Ns(std::int64_t ns) {
    return ns <= 0 ? 0u : ns >= std::int64_t(UINT32_MAX) ? UINT32_MAX : std::uint32_t(ns);
}

inline void Step(float game_dt, float average, std::int64_t vblank_ns = 0) {
    if (!Enabled()) {
        return;
    }
    const std::int64_t now = Now();
    const std::int64_t last = g_state.last_step.load(std::memory_order_relaxed);
    g_state.last_step.store(now, std::memory_order_relaxed);
    if (last) {
        g_state.step.Push(Ns(now - last));
    }
    if (game_dt > 0.0f && game_dt < 1.0f && average > 0.0f && average < 1.0f) {
        g_state.dt.Push(Ns(std::int64_t(double(game_dt) * 1e9)));
        g_state.dt_off.Push(Ns(std::int64_t(std::fabs(double(game_dt) - double(average)) * 1e9)));
        if (vblank_ns > 0) {
            g_state.dt_grid.Push(
                Ns(std::int64_t(LanternFrameClock::OffWholePeriods(double(game_dt) * 1e9, double(vblank_ns)))));
        }
    }
}

inline void Paced(std::int64_t waited_ns) {
    if (!Enabled()) {
        return;
    }
    g_state.wait.Push(Ns(waited_ns));
}

inline void PresentTaken(std::size_t queued) {
    if (!Enabled()) {
        return;
    }
    const std::int64_t now = Now();
    const std::int64_t last = g_state.last_present.load(std::memory_order_relaxed);
    g_state.last_present.store(now, std::memory_order_relaxed);
    if (last) {
        g_state.present.Push(Ns(now - last));
    }
    g_state.depth.Push(std::uint32_t(std::min<std::size_t>(queued, UINT32_MAX)));
}

inline void PadRead() {
    if (!Enabled() || g_state.pending_pad.load(std::memory_order_relaxed) != 0) {
        return;
    }
    std::int64_t expected = 0;
    g_state.pending_pad.compare_exchange_strong(expected, Now(), std::memory_order_relaxed);
}

inline void Presented() {
    if (!Enabled()) {
        return;
    }
    if (const std::int64_t read = g_state.pending_pad.exchange(0, std::memory_order_relaxed)) {
        g_state.pad.Push(Ns(Now() - read));
    }
}

inline std::size_t Line(char* out, std::size_t size, double seconds_since_start = -1.0) {
    static std::array<std::uint32_t, kRing> scratch;
    const auto ms = [](double ns) { return ns / 1e6; };
    std::size_t used = 0;
    const auto put = [&](const char* format, auto... args) {
        if (used < size) {
            const int n = std::snprintf(out + used, size - used, format, args...);
            if (n > 0) {
                used = std::min(size - 1, used + std::size_t(n));
            }
        }
    };
    if (seconds_since_start >= 0.0) {
        put("Frame pacing+ at %.1f s:", seconds_since_start);
    } else {
        put("%s", "Frame pacing+:");
    }
    const Summary step = Summarize(scratch.data(), g_state.step.Drain(scratch.data()));
    if (step.count) {
        put(" game steps %zu, mean %.3f ms, sd %.3f, min %.3f, max %.3f;", step.count, ms(step.mean), ms(step.sd),
            ms(step.min), ms(step.max));
    } else {
        put("%s", " game steps none seen;");
    }
    const Summary dt = Summarize(scratch.data(), g_state.dt.Drain(scratch.data()));
    const Summary off = Summarize(scratch.data(), g_state.dt_off.Drain(scratch.data()));
    if (dt.count) {
        put(" game dt mean %.3f ms, sd %.3f, |dt - average| mean %.3f ms, max %.3f;", ms(dt.mean), ms(dt.sd),
            ms(off.mean), ms(off.max));
    } else {
        put("%s", " game dt not readable;");
    }
    const Summary grid = Summarize(scratch.data(), g_state.dt_grid.Drain(scratch.data()));
    if (grid.count) {
        put(" dt off whole display periods mean %.3f ms, max %.3f;", ms(grid.mean), ms(grid.max));
    } else {
        put("%s", " flips not locked to the display;");
    }
    const Summary wait = Summarize(scratch.data(), g_state.wait.Drain(scratch.data()));
    if (wait.count) {
        put(" paced at frame start %zu, wait mean %.3f ms, max %.3f;", wait.count, ms(wait.mean), ms(wait.max));
    } else {
        put("%s", " not paced at frame start;");
    }
    const Summary present = Summarize(scratch.data(), g_state.present.Drain(scratch.data()));
    const Summary depth = Summarize(scratch.data(), g_state.depth.Drain(scratch.data()));
    put(" flips taken %zu, interval mean %.3f ms, sd %.3f, min %.3f, max %.3f, queue at take mean %.2f, max %.0f;",
        present.count, ms(present.mean), ms(present.sd), ms(present.min), ms(present.max), depth.mean, depth.max);
    const std::size_t pads = g_state.pad.Drain(scratch.data());
    const Summary pad = Summarize(scratch.data(), pads);
    put(" pad read to next present (not matched to its frame) mean %.2f ms, p95 %.2f ms, %zu presents",
        ms(pad.mean), ms(Percentile(scratch.data(), pads, 95.0)), pad.count);
    return used;
}

}
