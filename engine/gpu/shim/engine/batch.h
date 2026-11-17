// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>

#include "../lantern_steps.h"
#include "../lantern_switches.h"

namespace BbEngine::Batch {
enum Mode : std::uint8_t { Off, On, Compare };
static_assert(Off == std::uint8_t(Lantern::Mode::Off) && On == std::uint8_t(Lantern::Mode::On) &&
              Compare == std::uint8_t(Lantern::Mode::Compare));

inline Mode Switch(const char* name) {
    return Mode(Lantern::Steps::StartModeByName(name));
}

struct Step {
    std::string_view name;
    Mode mode = Off;
    std::atomic<std::uint64_t> checked{0}, differ{0};
    std::uint64_t shown_checked = 0, shown_differ = 0;
};

inline constexpr int kMaxSteps = 8;
inline Step g_steps[kMaxSteps];
inline std::atomic<int> g_count{0};
inline std::atomic<std::uint64_t> g_refused{0};
inline std::mutex g_registration;

inline Step* Register(std::string_view name, Mode mode) {
    const std::scoped_lock lock{g_registration};
    const int count = g_count.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) {
        if (g_steps[i].name == name) return &g_steps[i];
    }
    if (count >= kMaxSteps) {
        if (g_refused.fetch_add(1, std::memory_order_relaxed) == 0) {
            Lantern::Log(Lantern::LogChannel::Always, "Verify: no room for %.*s (%d steps); it is not counted",
                         int(name.size()), name.data(), kMaxSteps);
        }
        return nullptr;
    }
    g_steps[count].name = name;
    g_steps[count].mode = mode;
    g_count.store(count + 1, std::memory_order_release);
    return &g_steps[count];
}

inline void Note(Step* step, bool same) {
    step->checked.fetch_add(1, std::memory_order_relaxed);
    if (!same) step->differ.fetch_add(1, std::memory_order_relaxed);
}

inline void Report(const char* when) {
    const int count = g_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        Step& s = g_steps[i];
        if (s.mode == Off) continue;
        const std::uint64_t checked = s.checked.load(std::memory_order_relaxed);
        const std::uint64_t differ = s.differ.load(std::memory_order_relaxed);
        Lantern::Log(Lantern::LogChannel::Always, "Verify %.*s (%s): checked %llu, differ %llu; in all %llu, differ %llu",
                     int(s.name.size()), s.name.data(), when,
                     static_cast<unsigned long long>(checked - s.shown_checked),
                     static_cast<unsigned long long>(differ - s.shown_differ),
                     static_cast<unsigned long long>(checked), static_cast<unsigned long long>(differ));
        // written only by: the thread that calls Report (the flip thread, then the exit path once flips stop)
        s.shown_checked = checked;
        // written only by: the thread that calls Report (the flip thread, then the exit path once flips stop)
        s.shown_differ = differ;
    }
}
}
