// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: where the shader translation's time goes (BB_FRAME_STATS). A shader first seen in play
// is translated where the draw needs it (PipelineCache::CompileModule, the GPU thread): the
// "shader/pipeline compiles, N ms" of Frame stats. Each phase of TranslateProgram and the SPIR-V
// emission is timed (a clock read between phases) and printed every 5 s when shaders were
// translated:
//   Shader translation: 12 shaders, 41.3 ms (3.4 avg, longest 11.2): decode 0.4, translate 3.1,
//   SSA 6.0, resources 4.2, passes 5.1, structurize 7.9, late passes 6.3, SPIR-V 8.3 ms
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "lantern_switches.h"

namespace Shader::CompileStats {

enum Phase : int {
    Decode,
    Translate,
    Ssa,
    Resources,
    Passes,
    Structurize,
    LatePasses,
    Spirv,
    Count
};

inline bool Enabled() {
    return Lantern::Switches::Load().frame_stats;
}

inline std::array<std::atomic<unsigned long long>, Count> phase_ns{};
inline std::atomic<unsigned long long> shaders{0}, longest_ns{0};
inline std::atomic<long long> last_report_ns{0};
/// The shader being translated on this thread: its time so far.
inline thread_local unsigned long long current_ns = 0;

inline long long Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Times consecutive phases: Next(phase) books the time since the previous mark to `phase`.
class Timer {
public:
    Timer() : enabled{Enabled()}, mark{enabled ? Now() : 0} {}
    void Next(Phase phase) {
        if (!enabled) {
            return;
        }
        const long long now = Now();
        const auto ns = static_cast<unsigned long long>(now - mark);
        phase_ns[phase].fetch_add(ns, std::memory_order_relaxed);
        current_ns += ns;
        mark = now;
    }

private:
    bool enabled;
    long long mark;
};

/// A shader's translation began (TranslateProgram): prints the last window's line when 5 s
/// have passed since the previous one.
inline void Begin() {
    if (!Enabled()) {
        return;
    }
    current_ns = 0;
    const long long now = Now();
    long long last = last_report_ns.load(std::memory_order_relaxed);
    if (last == 0) {
        last_report_ns.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 5'000'000'000ll || !last_report_ns.compare_exchange_strong(last, now)) {
        return;
    }
    const unsigned long long count = shaders.exchange(0, std::memory_order_relaxed);
    std::array<double, Count> ms{};
    double total = 0;
    for (int i = 0; i < Count; ++i) {
        ms[i] = phase_ns[i].exchange(0, std::memory_order_relaxed) / 1e6;
        total += ms[i];
    }
    const double longest = longest_ns.exchange(0, std::memory_order_relaxed) / 1e6;
    if (count == 0) {
        return;
    }
    std::printf("Shader translation: %llu shaders, %.1f ms (%.1f avg, longest %.1f): decode %.1f, "
                "translate %.1f, SSA %.1f, resources %.1f, passes %.1f, structurize %.1f, late "
                "passes %.1f, SPIR-V %.1f ms\n",
                count, total, total / count, longest, ms[Decode], ms[Translate], ms[Ssa],
                ms[Resources], ms[Passes], ms[Structurize], ms[LatePasses], ms[Spirv]);
}

/// The SPIR-V of the shader is out: one shader counted with its total.
inline void End() {
    if (!Enabled()) {
        return;
    }
    shaders.fetch_add(1, std::memory_order_relaxed);
    unsigned long long longest = longest_ns.load(std::memory_order_relaxed);
    while (current_ns > longest &&
           !longest_ns.compare_exchange_weak(longest, current_ns, std::memory_order_relaxed)) {
    }
    current_ns = 0;
}

} // namespace Shader::CompileStats
