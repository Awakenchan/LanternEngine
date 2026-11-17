// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace BbEngine::RegWindow {
struct Copy {
    std::uint32_t dwords = 0;
    bool clamped = false;
};

constexpr Copy Clamp(std::uint32_t window_dwords, std::uint32_t first, std::uint32_t dwords) {
    if (first >= window_dwords) return {0, dwords != 0};
    const std::uint32_t room = window_dwords - first;
    return dwords <= room ? Copy{dwords, false} : Copy{room, true};
}
}
