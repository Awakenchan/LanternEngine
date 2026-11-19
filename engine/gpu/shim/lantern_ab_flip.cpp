// SPDX-License-Identifier: GPL-3.0-or-later
#include "lantern_ab_flip.h"
#include <array>
#include <cstdio>
#include <mutex>
#include "engine/bottleneck_meter.h"
#include "lantern_switches.h"

namespace Lantern::AbFlip {
namespace {
constexpr std::size_t kFlipLine = 1024;

struct Entry {
    const char* name = "";
    std::atomic<int>* value = nullptr;
    int flipped = 0;
};

struct Registry {
    std::mutex mutex;
    std::array<Entry, kMaxSwitches> entries{};
    std::size_t used = 0;
    bool flipped = false;
    bool told_meter_off = false;
    std::atomic<Half> half{Half::None};
};
Registry g_registry;

long FlipAfter() {
    return Switches::Get().ab_flip;
}
}

Registered Register(const char* name, std::atomic<int>* value, int flipped) {
    if (FlipAfter() <= 0 || !value) return Registered::Off;
    std::scoped_lock lock(g_registry.mutex);
    if (Switches::Get().meter == 0) {
        if (!g_registry.told_meter_off) {
            g_registry.told_meter_off = true;
            Log(LogChannel::Always, "A/B flip: LANTERN_AB_FLIP is set but LANTERN_METER=0, so nothing flips");
        }
        return Registered::Off;
    }
    if (g_registry.flipped) return Registered::TooLate;
    if (g_registry.used == kMaxSwitches) return Registered::Full;
    g_registry.entries[g_registry.used++] = {name ? name : "?", value, flipped};
    return Registered::Added;
}

bool Unregister(const std::atomic<int>* value) {
    std::scoped_lock lock(g_registry.mutex);
    for (std::size_t i = 0; i < g_registry.used; ++i) {
        if (g_registry.entries[i].value != value) continue;
        g_registry.entries[i] = g_registry.entries[--g_registry.used];
        return true;
    }
    return false;
}

Half Current() {
    return g_registry.half.load(std::memory_order_acquire);
}

bool Advance(u64 since_first_frame_ns) {
    const Half half = HalfAt(FlipAfter(), since_first_frame_ns);
    char line[kFlipLine];
    {
        std::scoped_lock lock(g_registry.mutex);
        g_registry.half.store(half, std::memory_order_release);
        if (half != Half::After || g_registry.flipped) return false;
        g_registry.flipped = true;
        BbEngine::BottleneckMeter::Writer out{line, sizeof line};
        line[0] = '\0';
        out.Put("A/B flip: after %ld s, %zu switches flipped:", FlipAfter(), g_registry.used);
        for (std::size_t i = 0; i < g_registry.used; ++i) {
            const Entry& e = g_registry.entries[i];
            e.value->store(e.flipped, std::memory_order_release);
            out.Put(" %s=%d", e.name, e.flipped);
        }
    }
    LogLine(LogChannel::Always, line);
    return true;
}
}
