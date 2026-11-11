// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace BbParty {
inline constexpr int kMaxHunters = 5;
inline constexpr int kMaxCooperators = kMaxHunters - 1;
inline constexpr int kBossScaleCap = 2;

inline bool SlotOpen(int cooperators) { return cooperators < kMaxCooperators; }
inline bool Full(std::size_t guests_in_world) { return guests_in_world >= static_cast<std::size_t>(kMaxCooperators); }
inline std::int64_t BossCount(std::int64_t cooperators) { return std::min<std::int64_t>(cooperators, kBossScaleCap); }
inline constexpr int kDopingOne = 7500;
inline constexpr int kDopingTwo = 7501;
inline constexpr int kDopingMax = 7503;
inline int DopingRow(int cooperators, bool rows_present) {
    if (cooperators <= 0) return 0;
    if (cooperators == 1) return kDopingOne;
    if (cooperators == 2 || !rows_present) return kDopingTwo;
    return std::min(kDopingOne + cooperators - 1, kDopingMax);
}
}
