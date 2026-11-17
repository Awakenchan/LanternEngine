// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace start {

inline constexpr std::string_view kPackageSwitchesFile = "engine-switches.txt";

struct PackageSwitches {
    std::vector<std::pair<std::string, std::string>> applied;
    std::vector<std::pair<std::string, std::string>> refused;
};

PackageSwitches ReadPackageSwitches(std::string_view text, std::string_view box_names);
std::string ShownPackageValue(std::string_view name, std::string_view value);
std::string ShownPackageName(std::string_view name);
std::string DescribePackageSwitches(const PackageSwitches& result);

}
