// SPDX-License-Identifier: GPL-3.0-or-later
#include "job_slots.h"
#include "hook_entries.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <span>

namespace BbEngine::JobSlots {
namespace {
#define GUEST_ABI __attribute__((sysv_abi))

constexpr std::size_t kJob = 0x1067f60;
constexpr const auto& kJobEntry = HookEntries::kJobSlotsJobEntry;
constexpr std::size_t kDestroy = 0x1067960;
constexpr const auto& kDestroyEntry = HookEntries::kJobSlotsDestroyEntry;
constexpr std::size_t kPoolGet = 0x1067f82;
constexpr std::uint8_t kPoolGetCall[] = {0xe8, 0xc9, 0xf3, 0xfe, 0xff};
constexpr std::size_t kTake = 0x1067fc5;
constexpr std::uint8_t kTakeAndKeep[] = {0x4c, 0x89, 0xe7, 0xe8, 0x53, 0xe9, 0xfe, 0xff,
                                         0x49, 0x89, 0x86, 0x20, 0x01, 0x00, 0x00};
constexpr std::size_t kGive = 0x1068075;
constexpr std::uint8_t kGiveAndClear[] = {0x49, 0x8b, 0xb6, 0x20, 0x01, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0xe8, 0x0c,
                                          0xe9, 0xfe, 0xff, 0x49, 0xc7, 0x86, 0x20, 0x01, 0x00, 0x00, 0x00};
constexpr std::size_t kRelease = 0x1056990;
constexpr std::uint8_t kReleaseEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x49, 0x89, 0xff};
constexpr std::size_t kPoolGetter = 0x1057350;
constexpr std::uint8_t kPoolGetterBody[] = {0x48, 0x8b, 0x05, 0x31, 0x14, 0x46, 0x04, 0xc3};
constexpr std::size_t kPoolSlot = 0x54b8788;
constexpr std::size_t kHeld = 0x120;

std::uint64_t g_pool_slot = 0;
std::uint64_t g_release = 0;
std::atomic<int> g_returned{0};

template <typename T> bool Read(std::uint64_t at, T* out) {
    return at && host_read_safe(reinterpret_cast<void*>(at), out, sizeof(T));
}

bool Matches(const std::uint8_t* image, std::size_t size, std::size_t at, std::span<const std::uint8_t> bytes) {
    return at <= size && bytes.size() <= size - at && std::memcmp(image + at, bytes.data(), bytes.size()) == 0;
}

GUEST_ABI std::int64_t ReturnHeld(std::uint64_t, std::uint64_t* saved) noexcept {
    const std::uint64_t job = saved[5];
    std::uint64_t held = 0, pool = 0;
    if (!Read(job + kHeld, &held) || !held || !Read(g_pool_slot, &pool) || !pool) return 0;
    Call(g_release, pool, held);
    *reinterpret_cast<std::uint64_t*>(job + kHeld) = 0;
    const int n = g_returned.fetch_add(1);
    if (n < 4 || (n & (n + 1)) == 0)
        host_log("Job slots: a Havok job left its thread-memory slot taken; returned to the pool (%d so far)", n + 1);
    return 0;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("LANTERN_JOB_SLOTS");
    if (on && on[0] == '0') {
        host_log("Job slots: off (LANTERN_JOB_SLOTS=0)");
        return false;
    }
    if (!Matches(image, size, kPoolGet, kPoolGetCall) || !Matches(image, size, kTake, kTakeAndKeep) ||
        !Matches(image, size, kGive, kGiveAndClear) || !Matches(image, size, kRelease, kReleaseEntry) ||
        !Matches(image, size, kPoolGetter, kPoolGetterBody)) {
        host_log("Job slots off: the Havok job's thread-memory pool differs from 1.09");
        return false;
    }
    Hook hooks[2];
    if (!Prepare(hooks[0], image, size, kJob, SiteKind::Entry, kJobEntry, ReturnHeld) ||
        !Prepare(hooks[1], image, size, kDestroy, SiteKind::Entry, kDestroyEntry, ReturnHeld)) {
        for (auto& h : hooks) Discard(h);
        host_log("Job slots off: the Havok job differs from 1.09");
        return false;
    }
    g_pool_slot = reinterpret_cast<std::uint64_t>(image + kPoolSlot);
    g_release = reinterpret_cast<std::uint64_t>(image + kRelease);
    if (const WriteResult result = Commit(hooks); result != WriteResult::Applied) {
        host_log("Job slots off: the Havok job could not be written (%s)", Describe(result));
        return false;
    }
    host_log("Job slots: a Havok job that retries or ends returns its thread-memory slot (cutscene freezes)");
    return true;
}
}
