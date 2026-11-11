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

inline std::uint32_t Limit(std::uint32_t base, bool active, int delivered, std::uint32_t refresh, bool cap_to_refresh) {
    if (!active || delivered <= 1) {
        return base;
    }
    const std::uint32_t shown = std::uint32_t(delivered);
    std::uint32_t limit = base / shown;
    const std::uint32_t refresh_share = refresh / shown;
    if (refresh_share >= 20 && cap_to_refresh) {
        limit = limit == 0 ? refresh_share : std::min(limit, refresh_share);
    }
    return limit == 0 ? base : limit;
}

}
