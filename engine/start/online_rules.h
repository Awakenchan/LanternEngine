// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace OnlineRules {
inline constexpr const char* kPrefix = "Bloodborne.xml/";

struct Rule {
    const char* patch;
    const char* token;
    const char* title;
};

inline constexpr Rule kGameplay[] = {
    {"Lamp2Lamp (warp between lamps)", "patch.lamp2lamp", "Lamp2Lamp warps"},
    {"Lamp2Lamp: warp to bosses", "patch.lamp2lamp-bosses", "Lamp2Lamp warps to bosses"},
};

inline constexpr const char* kCheatLike[] = {
    "Player No Dead (Read note)", "Player Stealth (Read note)", "Player Silent (Read note)", "Enemy Control",
    "Restore Debug Menu (READ NOTES)",
};

inline bool CheatLike(const std::string& name) {
    return std::any_of(std::begin(kCheatLike), std::end(kCheatLike), [&](const char* c) { return name == c; });
}

inline std::string NameOf(const std::string& key) {
    const std::string prefix = kPrefix;
    return key.compare(0, prefix.size(), prefix) == 0 ? key.substr(prefix.size()) : std::string();
}

inline std::string Join(std::vector<std::string> parts) {
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    std::string out;
    for (const std::string& p : parts) {
        if (p.empty()) continue;
        out += (out.empty() ? "" : ",") + p;
    }
    return out.empty() ? "vanilla" : out;
}

inline std::vector<std::string> Tokens(const std::vector<std::string>& enabled, bool cheats) {
    std::vector<std::string> parts;
    for (const std::string& key : enabled) {
        const std::string name = NameOf(key);
        for (const Rule& r : kGameplay)
            if (name == r.patch) parts.push_back(r.token);
    }
    if (cheats) parts.push_back("patch.cheats");
    return parts;
}

inline std::string Ruleset(const std::vector<std::string>& enabled, bool cheats) { return Join(Tokens(enabled, cheats)); }

inline std::string Describe(const std::vector<std::string>& enabled, bool cheats) {
    std::string out;
    for (const std::string& key : enabled) {
        const std::string name = NameOf(key);
        for (const Rule& r : kGameplay)
            if (name == r.patch && out.find(r.title) == std::string::npos) out += (out.empty() ? "" : ", ") + std::string(r.title);
    }
    if (cheats) out += (out.empty() ? "" : ", ") + std::string("cheats");
    return out.empty() ? "vanilla" : out;
}

inline bool Valid(const std::string& rules) {
    if (rules.empty() || rules.size() > 256) return false;
    return std::all_of(rules.begin(), rules.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == ';' || ch == '=' ||
               ch == ',' || ch == '-';
    });
}
}
