// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>
#include <mutex>
#include "lantern_meter.h"
#include "lantern_meter_threads.h"

namespace Lantern::Meter {
namespace {
constexpr const char* kOtherThreads = "other threads";

struct Registry {
    std::mutex mutex;
    std::size_t draw_used = 0;
    std::size_t place_used = 0;
};
Registry g_registry;

}

void PublishDraws(DrawCounter& c) {
    if (!c.slot) {
        char name[kNameBytes] = {};
        if (!Threads::CurrentThreadName(name, sizeof name) || !name[0]) std::snprintf(name, sizeof name, "?");
        std::scoped_lock lock(g_registry.mutex);
        if (g_registry.draw_used < kDrawThreads) {
            DrawSlot& slot = g_state.draw_slots[g_registry.draw_used++];
            std::memcpy(slot.name, name, sizeof slot.name);
            slot.name[kNameBytes - 1] = '\0';
            slot.used.store(true, std::memory_order_release);
            c.slot = &slot;
        } else {
            DrawSlot& other = g_state.draw_slots[kDrawThreads];
            if (!other.used.load(std::memory_order_relaxed)) {
                std::snprintf(other.name, sizeof other.name, "%s", kOtherThreads);
                other.used.store(true, std::memory_order_release);
            }
            c.slot = &other;
            c.shared = true;
        }
    }
    if (c.shared) {
        c.slot->draws.fetch_add(c.count - c.published, std::memory_order_relaxed);
        c.published = c.count;
    } else {
        c.slot->draws.store(c.count, std::memory_order_relaxed);
    }
}

void PublishPlace(PlaceCounter& c) {
    if (!c.slot) {
        std::scoped_lock lock(g_registry.mutex);
        if (g_registry.place_used >= kPlaceThreads) {
            c.calls = 1;
            return;
        }
        PlaceSlot& slot = g_state.place_slots[g_registry.place_used++];
        slot.tid.store(Threads::CurrentTid(), std::memory_order_release);
        c.slot = &slot;
    }
    c.slot->cpu.store(Threads::CurrentCpu(), std::memory_order_relaxed);
}

bool CpuOfThread(u32 tid, u32* cpu) {
    for (const PlaceSlot& slot : g_state.place_slots) {
        const u32 id = slot.tid.load(std::memory_order_acquire);
        if (!id) return false;
        if (id == tid) {
            *cpu = slot.cpu.load(std::memory_order_relaxed);
            return *cpu != Threads::kUnknownCpu;
        }
    }
    return false;
}
}
