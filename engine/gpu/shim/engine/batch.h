// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace BbEngine::Batch {
enum Mode : std::uint8_t { Off, On, Compare };

inline bool Set(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] == '1';
}

inline Mode Resolve(const char* own, bool verify, bool batch) {
    if (own && own[0]) {
        if (own[0] == '2') return Compare;
        return own[0] == '1' ? On : Off;
    }
    if (verify) return Compare;
    if (batch) return On;
    return Off;
}

inline Mode Switch(const char* name) {
    return Resolve(std::getenv(name), Set("LANTERN_VERIFY"), Set("LANTERN_BATCH"));
}

struct Step {
    const char* name = nullptr;
    Mode mode = Off;
    std::atomic<std::uint64_t> checked{0}, differ{0};
    std::uint64_t shown_checked = 0, shown_differ = 0;
};

inline constexpr int kMaxSteps = 8;
inline Step g_steps[kMaxSteps];
inline std::atomic<int> g_count{0};

inline Step* Register(const char* name, Mode mode) {
    for (int i = 0; i < g_count.load(std::memory_order_acquire); ++i) {
        if (g_steps[i].name == name) return &g_steps[i];
    }
    const int at = g_count.load(std::memory_order_relaxed);
    if (at >= kMaxSteps) return nullptr;
    g_steps[at].name = name;
    g_steps[at].mode = mode;
    g_count.store(at + 1, std::memory_order_release);
    return &g_steps[at];
}

inline void Note(Step* step, bool same) {
    step->checked.store(step->checked.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (!same) step->differ.store(step->differ.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

inline void Report(const char* when) {
    const int count = g_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        Step& s = g_steps[i];
        if (s.mode == Off) continue;
        const std::uint64_t checked = s.checked.load(std::memory_order_relaxed);
        const std::uint64_t differ = s.differ.load(std::memory_order_relaxed);
        std::printf("Verify %s (%s): checked %llu, differ %llu; in all %llu, differ %llu\n", s.name, when,
                    static_cast<unsigned long long>(checked - s.shown_checked),
                    static_cast<unsigned long long>(differ - s.shown_differ),
                    static_cast<unsigned long long>(checked), static_cast<unsigned long long>(differ));
        s.shown_checked = checked;
        s.shown_differ = differ;
    }
}
}
