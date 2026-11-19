// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_jobs_barrier_hook.h"
#include "main_jobs_barrier.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include "hook_entries.h"
#include "../lantern_steps.h"
#include <atomic>
#include <cstring>
#include <span>
#include <x86intrin.h>

namespace BbEngine::MainJobsBarrier {
namespace {
constexpr std::size_t kStep = 0x21b9980;
constexpr const auto& kStepEntry = HookEntries::kMainJobsBarrierStepEntry;
constexpr std::size_t kSteal = 0xe47d20;
constexpr std::uint8_t kStealEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
constexpr std::size_t kStepStealCall = 0x21b9a13;
constexpr std::uint8_t kStepStealCallBytes[] = {0xe8, 0x08, 0xe3, 0xc8, 0xfe};
constexpr std::size_t kBarrierStepCall = 0x21b8790;
constexpr std::uint8_t kBarrierStepCallBytes[] = {0x48, 0x8b, 0x3b, 0xe8, 0xe8, 0x11, 0x00, 0x00};
constexpr std::uint8_t kStealReturnMask = 0xff;
constexpr Lantern::StepId kId = Lantern::StepId::main_jobs_barrier;
constexpr std::uint8_t kReasonShape = 0;

struct alignas(Lantern::kCacheLine) Owner {
    std::atomic<bool> installed{false};
    std::atomic<bool> on{false};
    std::atomic<bool> refused{false};
    std::atomic<u64> steal{0};
    std::atomic<u64> game_step{0};
    std::atomic<int> menu_seen{0};
};
Owner g_owner;
alignas(Lantern::kCacheLine) std::atomic<int> g_switch{0};
alignas(Lantern::kCacheLine) Census g_census;

struct GuestOps {
    u64 steal;
    u64 game_step;
    u64 Load64(u64 at) const noexcept { return __atomic_load_n(reinterpret_cast<const u64*>(at), __ATOMIC_RELAXED); }
    u32 Load32(u64 at) const noexcept { return __atomic_load_n(reinterpret_cast<const u32*>(at), __ATOMIC_RELAXED); }
    bool Steal(u64 victim, u64 thief) const noexcept { return (Call(steal, victim, thief) & kStealReturnMask) != 0; }
    void RunGameStep(u64 self) const noexcept { Call(game_step, self); }
    u64 Tsc() const noexcept { return __rdtsc(); }
};

const char* Describe(Check check) noexcept {
    switch (check) {
    case Check::Ok: return "ok";
    case Check::NoSystem: return "no job system";
    case Check::NoParticipants: return "no participant array";
    case Check::CountOutOfRange: return "participant count out of range";
    case Check::NotInArray: return "caller not in the participant array";
    case Check::NotOnBoundary: return "caller not on a participant boundary";
    case Check::IndexDiffers: return "participant index differs from its place";
    }
    return "unknown";
}

void Refuse(const Shape& shape, Check check) noexcept {
    g_owner.on.store(false, std::memory_order_relaxed);
    if (g_owner.refused.exchange(true, std::memory_order_relaxed)) return;
    (void)Lantern::Steps::SetUnavailable(kId, "the job system's shape differs from 1.09");
    host_log("Main jobs barrier: off for this session and every job system, the game's step runs: %s (participant %#llx, system %#llx, array "
             "%#llx, count %u, index %u)",
             Describe(check), static_cast<unsigned long long>(shape.self), static_cast<unsigned long long>(shape.system),
             static_cast<unsigned long long>(shape.base), shape.count, shape.index);
}

void Count(Lantern::ThreadSlot* slot, bool shape_ok, bool took) noexcept {
    if (!slot) {
        g_census.Call(shape_ok, took);
        return;
    }
    Lantern::Steps::Covered(*slot, kId, shape_ok);
    if (!shape_ok) Lantern::Steps::FellBack(*slot, kId, kReasonShape);
    if (took) Lantern::Steps::Used(*slot, kId);
}

void Fold(const CensusCounts& c) noexcept {
#if LANTERN_DIAG_HOT
    Lantern::Steps::AddUnslotted(kId, c.total, c.covered, kReasonShape, c.fell_back, c.used);
#else
    (void)c;
#endif
}

std::int64_t __attribute__((sysv_abi)) OnStep(std::uint64_t, std::uint64_t* saved) noexcept {
    if (!g_owner.on.load(std::memory_order_acquire)) return 0;
    GuestOps ops{g_owner.steal.load(std::memory_order_relaxed), g_owner.game_step.load(std::memory_order_relaxed)};
    const u64 self = saved[Saved::kRdi];
    if (!self) return 0;
    const Shape shape = ReadShape(ops, self);
    const Check check = CheckShape(shape);
    Lantern::ThreadSlot* slot = Lantern::Steps::PeekSlot();
    if (Choose(true, check) == Route::Game) {
        Count(slot, false, false);
        Refuse(shape, check);
        return 0;
    }
    const Outcome outcome = RunStep(ops, self);
    Count(slot, true, outcome.took);
    saved[Saved::kResult] = 0;
    return 1;
}

bool Matches(const std::uint8_t* image, std::size_t size, std::size_t at, std::span<const std::uint8_t> bytes) noexcept {
    return at <= size && bytes.size() <= size - at && std::memcmp(image + at, bytes.data(), bytes.size()) == 0;
}
}

Installed Install(std::uint8_t* image, std::size_t size) noexcept {
    Lantern::Steps::StartOnce();
    if (Lantern::Steps::Get(kId) != Lantern::Mode::On) {
        (void)Lantern::Steps::SetUnavailable(kId, "not installed at start; LANTERN_MAIN_JOBS_BARRIER=1 installs it");
        return Installed::NotAsked;
    }
    if (!Matches(image, size, kSteal, kStealEntry) || !Matches(image, size, kStepStealCall, kStepStealCallBytes) ||
        !Matches(image, size, kBarrierStepCall, kBarrierStepCallBytes)) {
        (void)Lantern::Steps::SetUnavailable(kId, "GX's job barrier differs from 1.09");
        host_log("Main jobs barrier: not installed, GX's job barrier differs from 1.09");
        return Installed::Refused;
    }
    Hook hook;
    if (!Prepare(hook, image, size, kStep, SiteKind::Entry, kStepEntry, OnStep)) {
        (void)Lantern::Steps::SetUnavailable(kId, "GX's job barrier step differs from 1.09");
        host_log("Main jobs barrier: not installed, the barrier's step at %#zx differs from 1.09", kStep);
        return Installed::Refused;
    }
    std::uint8_t* game_step = OriginalStub(hook);
    if (!game_step) {
        Discard(hook);
        (void)Lantern::Steps::SetUnavailable(kId, "no memory for the hook");
        host_log("Main jobs barrier: not installed, no memory for the barrier step's stub");
        return Installed::Refused;
    }
    g_owner.steal.store(reinterpret_cast<u64>(image + kSteal), std::memory_order_relaxed);
    g_owner.game_step.store(reinterpret_cast<u64>(game_step), std::memory_order_relaxed);
    g_owner.on.store(true, std::memory_order_release);
    if (const WriteResult result = Commit(std::span<Hook>(&hook, 1)); result != WriteResult::Applied) {
        g_owner.on.store(false, std::memory_order_relaxed);
        if (result == WriteResult::NotApplied) FreeStub(game_step);
        (void)Lantern::Steps::SetUnavailable(kId, "the barrier step could not be written");
        host_log("Main jobs barrier: not installed, the barrier's step %s", BbEngine::Describe(result));
        return Installed::Refused;
    }
    g_owner.menu_seen.store(1, std::memory_order_relaxed);
    g_switch.store(1, std::memory_order_relaxed);
    g_owner.installed.store(true, std::memory_order_release);
    host_log("Main jobs barrier: on, the main thread takes the GX workers' queued jobs while it waits "
             "(LANTERN_MAIN_JOBS_BARRIER=1; the steps menu switches it at the next frame)");
    return Installed::Yes;
}

void Tick() noexcept {
    if (!g_owner.installed.load(std::memory_order_acquire)) return;
    Fold(g_census.Take());
    const int menu = Lantern::Steps::Get(kId) == Lantern::Mode::On ? 1 : 0;
    if (g_owner.menu_seen.exchange(menu, std::memory_order_relaxed) != menu) g_switch.store(menu, std::memory_order_relaxed);
    const bool step_on = g_switch.load(std::memory_order_relaxed) == 1;
    g_owner.on.store(FrameLatch(step_on, g_owner.refused.load(std::memory_order_relaxed)), std::memory_order_release);
}

bool LatchedOn() noexcept {
    return g_owner.on.load(std::memory_order_acquire);
}

std::atomic<int>& Switch() noexcept {
    return g_switch;
}
}
