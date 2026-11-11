// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace GpuCacheStamp {
enum class Action { Keep, Restamp, Recompile };

inline std::vector<std::string> Accepted(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return c == '\r' || c == ' ' || c == '\t'; }),
                   line.end());
        if (line.size() == 16 && std::all_of(line.begin(), line.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
            out.push_back("gpu " + line);
    }
    return out;
}

inline Action Decide(const std::string& stamped, const std::string& identity, const std::string& legacy,
                     const std::vector<std::string>& accepted) {
    if (identity.empty() || stamped == identity) return Action::Keep;
    if (!stamped.empty() && !legacy.empty() && stamped == legacy) return Action::Restamp;
    if (!stamped.empty() && std::find(accepted.begin(), accepted.end(), stamped) != accepted.end()) return Action::Restamp;
    return Action::Recompile;
}

inline std::string RecompileNotice(const std::string& game) {
    return "GPU cache " + game +
           ": the engine's shader compiler changed with this update; recompiling the saved shaders once before the "
           "game starts (a few minutes, the window opens after it)";
}
}
