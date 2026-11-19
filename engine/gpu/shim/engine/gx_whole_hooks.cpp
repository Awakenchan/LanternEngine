// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_whole_hooks.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <array>
#include <atomic>
#include <utility>

namespace BbEngine::GxWhole::Hooks {
namespace {
using u64 = std::uint64_t;

struct Owner {
    std::atomic<bool> on{false};
};
Owner g_owner;

thread_local std::array<u64, kFlushes> t_calls{};

template <Flush F> std::int64_t __attribute__((sysv_abi)) OnFlush(std::uint64_t, std::uint64_t*) noexcept {
    ++t_calls[static_cast<std::size_t>(F)];
    return 0;
}

template <std::size_t... I> constexpr std::array<Callback, kFlushes> MakeCallbacks(std::index_sequence<I...>) {
    return {OnFlush<static_cast<Flush>(I)>...};
}
constexpr std::array<Callback, kFlushes> kCallbacks = MakeCallbacks(std::make_index_sequence<kFlushes>{});
}

bool On() {
    return g_owner.on.load(std::memory_order_relaxed);
}

std::uint64_t ThreadCalls(Flush flush) {
    return t_calls[static_cast<std::size_t>(flush)];
}

bool InstallSites(std::uint8_t* image, std::size_t size) {
    std::array<Hook, kFlushes> hooks;
    for (std::size_t i = 0; i < kFlushes; ++i) {
        const Site& site = kSites[i];
        if (!Prepare(hooks[i], image, size, Offset(site), KindOf(site), Entry(site), kCallbacks[i])) {
            for (Hook& hook : hooks) Discard(hook);
            host_log("GX whole hooks off: GX's %s at %#zx is not 1.09's; nothing written", site.name, Offset(site));
            return false;
        }
    }
    if (const WriteResult result = Commit(std::span<Hook>(hooks)); result != WriteResult::Applied) {
        host_log("GX whole hooks off: GX's flushes could not be written (%s)", Describe(result));
        return false;
    }
    g_owner.on.store(true, std::memory_order_relaxed);
    return true;
}

bool Install(std::uint8_t* image, std::size_t size, Mode requested, Mode accepted) {
    const Verdict verdict = Decide(requested, accepted);
    if (verdict == Verdict::NeedsRecords) {
        host_log("GX whole hooks off: needs GX records (LANTERN_GX_RECORDS=1, 2 or 3 with LANTERN_GX_WHOLE=1)");
        return false;
    }
    if (verdict == Verdict::Off) {
        host_log("GX whole hooks: off (LANTERN_GX_WHOLE=1 with GX records hooks GX's draw flushes at start)");
        return false;
    }
    if (!InstallSites(image, size)) return false;
    host_log("GX whole hooks: on, %zu GX draw flushes hooked at start (LANTERN_GX_WHOLE=1); each still runs as the game's",
             kFlushes);
    return true;
}
}
