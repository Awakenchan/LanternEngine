// SPDX-License-Identifier: GPL-3.0-or-later
#include "camera_timing.h"
#include "hook_entries.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>

#define GUEST_ABI __attribute__((sysv_abi))

namespace BbEngine::CameraTiming {
namespace {
enum class Take { Rate, Rates, Step, Swap, Restore };

constexpr std::size_t kFrameTime = 0x535a908;
constexpr float kGameStep = 1.0f / 30.0f;

constexpr const auto& kChase = HookEntries::kCameraTimingChase;
constexpr const auto& kFollow = HookEntries::kCameraTimingFollow;
constexpr const auto& kLockLoad = HookEntries::kCameraTimingLockLoad;
constexpr const auto& kLockDone = HookEntries::kCameraTimingLockDone;
constexpr const auto& kTurn = HookEntries::kCameraTimingTurn;
constexpr const auto& kPitchBack = HookEntries::kCameraTimingPitchBack;
constexpr const auto& kFocus = HookEntries::kCameraTimingFocus;
constexpr const auto& kPitch = HookEntries::kCameraTimingPitch;
constexpr const auto& kPitchLimit = HookEntries::kCameraTimingPitchLimit;

struct Site {
    std::size_t offset;
    std::span<const std::uint8_t> code;
    Take take;
    unsigned xmm;
    std::size_t field;
};

constexpr Site kSites[] = {
    {0x143ae7e, kChase, Take::Rate, 4, 0},
    {0x143baa0, kFollow, Take::Rates, 3, 0},
    {0x143c30f, kLockLoad, Take::Swap, 0, 0x240},
    {0x143c31d, kLockDone, Take::Restore, 0, 0x240},
    {0x143c78d, kTurn, Take::Rate, 5, 0},
    {0x143cef0, kPitchBack, Take::Step, 0, 0x1a8},
    {0x143e7ef, kFocus, Take::Rates, 1, 0},
    {0x143f97e, kPitch, Take::Step, 0, 0x23c},
    {0x143fa9f, kPitchLimit, Take::Step, 1, 0x23c},
};
constexpr std::size_t kCount = std::size(kSites);

std::uint8_t* g_image = nullptr;
float g_kept = 0.0f;

float ForFrame(float rate) {
    if (!(rate > 0.0f) || rate >= 1.0f) return rate;
    float frame;
    std::memcpy(&frame, g_image + kFrameTime, sizeof frame);
    if (!(frame > 0.0f) || frame >= kGameStep) return rate;
    return 1.0f - std::pow(1.0f - rate, frame / kGameStep);
}

template <std::size_t N> GUEST_ABI std::int64_t Apply(std::uint64_t, std::uint64_t* saved) noexcept {
    const Site& site = kSites[N];
    auto* xmm = reinterpret_cast<float*>(SavedXmm(saved, site.xmm));
    auto* field = reinterpret_cast<float*>(saved[InsideSaved::kR13] + site.field);
    switch (site.take) {
    case Take::Rate: xmm[0] = ForFrame(xmm[0]); break;
    case Take::Rates: xmm[0] = xmm[1] = xmm[2] = xmm[3] = ForFrame(xmm[0]); break;
    case Take::Step:
        if (*field > 0.0f) xmm[0] *= ForFrame(*field) / *field;
        break;
    case Take::Swap:
        g_kept = *field;
        *field = ForFrame(g_kept);
        break;
    case Take::Restore: *field = g_kept; break;
    }
    return 0;
}

template <std::size_t... N> constexpr std::array<Callback, kCount> Entries(std::index_sequence<N...>) {
    return {Apply<N>...};
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("LANTERN_CAMERA_TIMING");
    if (!on || on[0] != '1') return false;
    if (!image || size < kFrameTime + 4) return false;
    const auto entries = Entries(std::make_index_sequence<kCount>{});
    std::array<Hook, kCount> hooks;
    for (std::size_t i = 0; i < kCount; ++i) {
        if (!Prepare(hooks[i], image, size, kSites[i].offset, SiteKind::Inside, kSites[i].code, entries[i])) {
            for (std::size_t j = 0; j < i; ++j) Discard(hooks[j]);
            host_log("Camera timing off: the camera code differs from 1.09");
            return false;
        }
    }
    g_image = image;
    if (const WriteResult result = Commit(hooks); result != WriteResult::Applied) {
        host_log("Camera timing off: the camera code could not be written (%s)", Describe(result));
        return false;
    }
    host_log("Camera timing: on, the camera's smoothing follows the frame time");
    return true;
}
}
