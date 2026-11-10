// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>

namespace BossRewards {
constexpr std::int32_t kLots[] = {
    15000,    15005,    21000,    31000,    2700990,  2700995,  3401800,  3401802,  3401850,
    3401852,  3501850,  3601800,  21002950, 25700000, 25700005, 50000001, 50000010, 50700000,
    50800000, 50800005, 51001900, 55100000, 80000000, 80000200, 80000300,
};

constexpr std::int32_t kTrophyEvents[] = {
    12201800, 12301700, 12301800, 12401800, 12411700, 12411800, 12421700, 12421800,
    12501800, 12601800, 12601850, 12701800, 12801800, 13201800, 13301800, 13401800,
    13401850, 13501800, 13501850, 13601800,
};

constexpr bool IsTrophyEvent(std::int32_t event) {
    return std::binary_search(std::begin(kTrophyEvents), std::end(kTrophyEvents), event);
}

constexpr bool IsBossLot(std::int32_t lot) {
    return std::binary_search(std::begin(kLots), std::end(kLots), lot);
}

constexpr std::int32_t kInsightEvent = 9350, kMostInsight = 9;

constexpr std::int32_t InsightWord(std::int32_t insight) {
    return insight >= 1 && insight <= kMostInsight ? -insight : 0;
}

constexpr std::int32_t InsightOf(std::int32_t word) {
    return word < 0 && word >= -kMostInsight ? -word : 0;
}

template <typename Clock> class Given {
public:
    explicit Given(typename Clock::duration keep) : keep_(keep) {}
    bool First(std::int32_t lot, typename Clock::time_point now) {
        std::erase_if(when_, [&](const auto& entry) { return now - entry.second > keep_; });
        return when_.emplace(lot, now).second;
    }

private:
    typename Clock::duration keep_;
    std::map<std::int32_t, typename Clock::time_point> when_;
};
}
