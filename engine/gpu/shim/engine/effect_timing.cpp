// SPDX-License-Identifier: GPL-3.0-or-later
#include "effect_timing.h"
#include "hook_entries.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace BbEngine::EffectTiming {
namespace {
#define GUEST_ABI __attribute__((sysv_abi))

constexpr std::uint64_t kAdvanceEffects = 0x1cbc980;
constexpr const auto& kAdvancePrologue = HookEntries::kEffectTimingAdvancePrologue;

std::uint64_t g_image = 0;
struct Caller {
    std::uint64_t calls = 0;
    double seconds = 0;
    std::unordered_set<std::uint64_t> chrs;
};
std::mutex g_mutex;
std::unordered_map<std::uint64_t, Caller> g_callers;
std::chrono::steady_clock::time_point g_since = std::chrono::steady_clock::now();

GUEST_ABI std::int64_t AdvanceEffects(std::uint64_t, std::uint64_t* saved) noexcept {
    float dt = 0;
    std::memcpy(&dt, SavedXmm(saved, 0), sizeof dt);
    const std::uint64_t chr = saved[5], ret = saved[6];
    const std::uint64_t site = ret > g_image ? ret - g_image + 0x400000 : 0;
    std::scoped_lock lock{g_mutex};
    auto& caller = g_callers[site];
    ++caller.calls;
    caller.seconds += dt;
    caller.chrs.insert(chr);
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - g_since).count();
    if (elapsed >= 10.0) {
        for (const auto& [at, c] : g_callers) {
            const double chrs = double(c.chrs.size());
            host_log("Effect timing: from 0x%llx, %.1f updates a second per character, %.2f s of effect "
                     "time a second per character (%zu characters, %.4f s a step)",
                     static_cast<unsigned long long>(at), double(c.calls) / chrs / elapsed,
                     c.seconds / chrs / elapsed, c.chrs.size(), c.calls ? c.seconds / double(c.calls) : 0.0);
        }
        g_callers.clear();
        g_since = now;
    }
    return 0;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("BB_EFFECT_TIMING");
    if (!on || on[0] != '1') return false;
    Hook hook;
    if (!Prepare(hook, image, size, kAdvanceEffects - 0x400000, SiteKind::Entry, kAdvancePrologue, AdvanceEffects)) {
        host_log("Effect timing off: the status effect update differs from 1.09");
        return false;
    }
    g_image = reinterpret_cast<std::uint64_t>(image);
    if (const WriteResult result = Commit(std::span{&hook, 1}); result != WriteResult::Applied) {
        host_log("Effect timing off: the status effect update could not be written (%s)", Describe(result));
        return false;
    }
    host_log("Effect timing: on (a report every 10 s)");
    return true;
}
}
