// SPDX-License-Identifier: GPL-3.0-or-later
#include "render_guard.h"
#include "hook_entries.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <atomic>
#include <span>

namespace BbEngine::RenderGuard {
namespace {
#define GUEST_ABI __attribute__((sysv_abi))

constexpr std::uint64_t kBindTarget = 0x25c2100;
constexpr const auto& kBindPrologue = HookEntries::kRenderGuardBindPrologue;
constexpr std::uint64_t kSurfaceWidth = 0x259f870;
constexpr std::size_t kWidthSlot = 0x48, kSurface = 0x40;

std::uint64_t g_width = 0;
std::atomic<int> g_skipped{0};

template<typename T> bool Read(std::uint64_t at, T* out) {
    return at && host_read_safe(reinterpret_cast<void*>(at), out, sizeof(T));
}

bool Surfaceless(std::uint64_t target) {
    std::uint64_t vtable = 0, width = 0, surface = 0;
    if (!target || !Read(target, &vtable) || !Read(vtable + kWidthSlot, &width) || width != g_width) return false;
    return Read(target + kSurface, &surface) && !surface;
}

GUEST_ABI std::int64_t BindTarget(std::uint64_t, std::uint64_t* saved) noexcept {
    const std::uint64_t depth = saved[4], color = saved[3];
    if (!color || (!Surfaceless(depth) && !Surfaceless(color))) return 0;
    const int n = g_skipped.fetch_add(1);
    if (n < 4 || (n & (n + 1)) == 0)
        host_log("Render guard: a pass's %s target has no surface; pass skipped (%d so far)",
                 Surfaceless(color) ? "color" : "depth", n + 1);
    return 1;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    Hook hook;
    if (!Prepare(hook, image, size, kBindTarget - 0x400000, SiteKind::Entry, kBindPrologue, BindTarget)) {
        host_log("Render guard off: the render-target bind differs from 1.09");
        return false;
    }
    g_width = reinterpret_cast<std::uint64_t>(image) + (kSurfaceWidth - 0x400000);
    if (const WriteResult result = Commit(std::span{&hook, 1}); result != WriteResult::Applied) {
        host_log("Render guard off: the render-target bind could not be written (%s)", Describe(result));
        return false;
    }
    return true;
}
}
