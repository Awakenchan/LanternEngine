// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_switches.h"

#include "../gpu/shim/lantern_diag_c.h"
#include "../gpu/shim/lantern_shown_values.h"

namespace start {

namespace {

struct Row {
    std::string_view name;
    bool text;
    unsigned flags;
};

constexpr Row kRows[] = {
#define X(id, name, kind, def, mn, mx, live, step, flags) {name, std::string_view(#kind) == "Text", flags},
#include "../gpu/shim/lantern_switches.inc"
#undef X
};

constexpr std::string_view kInternal[] = {"LANTERN_EXTRA_VARIABLES", "LANTERN_PACKAGE_SWITCHES"};

const Row* Find(std::string_view name) {
    for (const Row& row : kRows) {
        if (row.name == name) return &row;
    }
    return nullptr;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool InNameList(std::string_view list, std::string_view name) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        if (Trim(list.substr(0, comma)) == name) return true;
        list.remove_prefix(comma == std::string_view::npos ? list.size() : comma + 1);
    }
    return false;
}

constexpr std::string_view kOnlinePrefixes[] = {"BB_THD_",       "BB_ROOM_",   "BB_ONLINE",
                                                "BB_NET_",       "BB_P2P_",    "BB_SHADNET_",
                                                "BB_REGION_PATCH_ONLINE", "BB_CLIENT_VERSION", "BB_GAME_MP_",
                                                "LANTERN_PEER_"};
constexpr std::string_view kLauncherRowsAllowed[] = {"BB_GUEST_IN_PLACE", "BB_BACKGROUND_FPS"};

bool OnlineName(std::string_view name) {
    for (std::string_view prefix : kOnlinePrefixes) {
        if (name.substr(0, prefix.size()) == prefix) return true;
    }
    return false;
}

bool LauncherRowAllowed(std::string_view name) {
    for (std::string_view allowed : kLauncherRowsAllowed) {
        if (name == allowed) return true;
    }
    return false;
}

std::string Refusal(std::string_view name, std::string_view value, std::string_view box_names) {
    if (!Lantern::Shown::UpperName(name)) return "not a switch name";
    for (std::string_view internal : kInternal) {
        if (name == internal) return "set by the launcher and bb-start";
    }
    const Row* row = Find(name);
    if (!LANTERN_MUTANT(3) && !row) return "not a switch this build reads";
    if (!LANTERN_MUTANT(2) && (Lantern::Shown::SecretName(name) || (row && (row->flags & LANTERN_SW_SECRET)))) {
        return "secret-like, set it in the launcher";
    }
    if (!LANTERN_MUTANT(4) && OnlineName(name)) return "online, room or server switch, set it in the launcher";
    if (!LANTERN_MUTANT(5) && row) {
        if (row->text && !(row->flags & LANTERN_SW_SHOW)) return "a path or text switch, set it in the launcher";
        if (!row->text && (row->flags & LANTERN_SW_LAUNCHER) && !LauncherRowAllowed(name)) return "set on the launcher's pages";
        if (!row->text && !Lantern::Shown::Plain(value, false)) return "the value is not a plain number or word";
    }
    if (!LANTERN_MUTANT(1) && InNameList(box_names, name)) return "the launcher's Extra variables set it";
    return {};
}

}

PackageSwitches ReadPackageSwitches(std::string_view text, std::string_view box_names) {
    PackageSwitches result;
    std::size_t number = 0;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = Trim(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        ++number;
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const std::size_t equals = line.find('=');
        const std::string_view name = Trim(line.substr(0, equals));
        const std::string label = Lantern::Shown::UpperName(name) ? std::string(name) : "line " + std::to_string(number);
        if (equals == std::string_view::npos || name.empty()) {
            result.refused.emplace_back("line " + std::to_string(number), "not NAME=value");
            continue;
        }
        const std::string_view value = Trim(line.substr(equals + 1));
        if (std::string reason = Refusal(name, value, box_names); !reason.empty()) {
            result.refused.emplace_back(label, std::move(reason));
            continue;
        }
        bool replaced = false;
        for (auto& [applied_name, applied_value] : result.applied) {
            if (applied_name == name) {
                applied_value = std::string(value);
                replaced = true;
            }
        }
        if (!replaced) result.applied.emplace_back(std::string(name), std::string(value));
    }
    return result;
}

std::string ShownPackageName(std::string_view name) {
    std::string shown;
    for (char c : name.substr(0, Lantern::Shown::kNameMax)) shown += Lantern::Shown::Printable(c);
    if (name.size() > Lantern::Shown::kNameMax) shown += "...";
    return shown;
}

std::string ShownPackageValue(std::string_view name, std::string_view value) {
    if (value.empty()) return "(empty)";
    const Row* row = Lantern::Shown::UpperName(name) ? Find(name) : nullptr;
    if (!row || row->text || (row->flags & LANTERN_SW_SECRET) || Lantern::Shown::SecretName(name)) return "<set>";
    return Lantern::Shown::Plain(value, false) ? std::string(value) : "<set>";
}

std::string DescribePackageSwitches(const PackageSwitches& result) {
    std::string text = "Package switches (" + std::string(kPackageSwitchesFile) + "): " + std::to_string(result.applied.size()) + " applied";
    for (std::size_t i = 0; i < result.applied.size(); ++i) {
        text += i ? " " : ": ";
        text += ShownPackageName(result.applied[i].first) + "=" + ShownPackageValue(result.applied[i].first, result.applied[i].second);
    }
    if (!result.refused.empty()) {
        text += "; refused " + std::to_string(result.refused.size()) + ":";
        for (const auto& [label, reason] : result.refused) text += " " + label + " (" + reason + ")";
    }
    return text;
}

}
