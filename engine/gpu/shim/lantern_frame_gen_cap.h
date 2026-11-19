// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace LanternFrameGenCap {

inline int DeliveredMultiplier(int asked, bool dynamic, float game_fps, float shown_fps, int previous) {
    asked = std::max(asked, 1);
    if (dynamic || game_fps <= 1.0f || shown_fps <= 0.0f) {
        return asked;
    }
    const float ratio = std::clamp(shown_fps / game_fps, 1.0f, float(asked));
    if (previous >= 1 && previous <= asked && std::abs(ratio - float(previous)) < 0.75f) {
        return previous;
    }
    return std::clamp(int(std::lround(ratio)), 1, asked);
}

constexpr std::uint32_t kLowestKnownRefreshHz = 30;
constexpr std::uint32_t kLowestGameShareFps = 20;

inline std::uint32_t RefreshShare(int delivered, std::uint32_t refresh) {
    if (delivered <= 1 || refresh < kLowestKnownRefreshHz) {
        return 0;
    }
    const std::uint32_t share = refresh / std::uint32_t(delivered);
    return share >= kLowestGameShareFps ? share : 0;
}

inline std::uint32_t Limit(std::uint32_t base, bool active, int delivered, std::uint32_t refresh, bool cap_to_refresh) {
    if (!active || delivered <= 1) {
        return base;
    }
    std::uint32_t limit = base / std::uint32_t(delivered);
    if (const std::uint32_t share = RefreshShare(delivered, refresh); share && cap_to_refresh) {
        limit = limit == 0 ? share : std::min(limit, share);
    }
    return limit == 0 ? base : limit;
}

inline std::uint32_t FixedRateLimit(std::uint32_t base, bool active, int delivered, std::uint32_t refresh) {
    const std::uint32_t share = active ? RefreshShare(delivered, refresh) : 0;
    if (!share) {
        return base;
    }
    return base == 0 ? share : std::min(base, share);
}

}
