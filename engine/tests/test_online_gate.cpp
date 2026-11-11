// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "gpu/shim/engine/online_gate.h"
#include "gpu/shim/engine/seamless_party.h"
#include "start/online_rules.h"

namespace {
int failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

const char* Gate(const std::map<std::string, std::string>& env) {
    return BbOnlineGate::ConfiguredBy([&](const char* key) -> const char* {
        const auto it = env.find(key);
        return it == env.end() ? nullptr : it->second.c_str();
    });
}

constexpr int kOff = 0;
constexpr int kInfinite = -1;
}

int main() {
    Check(!Gate({}), "offline: cheats allowed");
    Check(!Gate({{"BB_ONLINE", "0"}}), "BB_ONLINE=0 alone: cheats allowed");
    Check(!Gate({{"BB_NET_BACKEND", ""}}), "empty backend: cheats allowed");
    Check(Gate({{"BB_ONLINE", "1"}}), "BB_ONLINE=1 refuses");
    Check(Gate({{"BB_ONLINE", "0"}, {"BB_NET_BACKEND", "thd"}}), "BB_ONLINE=0 with thd refuses");
    Check(Gate({{"BB_ONLINE", "0"}, {"BB_NET_BACKEND", "room"}}), "BB_ONLINE=0 with room refuses");
    Check(Gate({{"BB_NET_BACKEND", "other"}}), "any backend refuses");
    Check(Gate({{"BB_ONLINE", "0"}, {"BB_ROOM_SERVER", "wss://x"}}), "room server alone refuses");
    Check(Gate({{"BB_ROOM_NAME", "r"}}), "room name alone refuses");
    Check(Gate({{"BB_ONLINE", "0"}, {"BB_THD_SERVER", "https://x"}}), "thd server alone refuses");
    Check(Gate({{"BB_THD_TOKEN", "t"}}), "thd token alone refuses");
    Check(Gate({{"BB_ONLINE", "0"}, {"BB_SHADNET_SERVER", "h:31313"}}), "shadNet server alone refuses");
    Check(Gate({{"BB_SHADNET_NPID", "n"}}), "shadNet id alone refuses");

    const std::vector<std::map<std::string, std::string>> online = {
        {{"BB_ONLINE", "1"}, {"BB_NET_BACKEND", "thd"}},
        {{"BB_ONLINE", "1"}, {"BB_NET_BACKEND", "room"}},
        {{"BB_ONLINE", "1"}, {"BB_SHADNET_SERVER", "h"}},
        {{"BB_ONLINE", "0"}, {"BB_NET_BACKEND", "thd"}},
        {{"BB_ONLINE", "0"}, {"BB_NET_BACKEND", "room"}},
        {{"BB_ONLINE", "0"}, {"BB_SHADNET_SERVER", "h"}},
    };
    bool none = true;
    for (const auto& env : online)
        for (const int setting : {kOff, kInfinite, 1, 5, 99}) none = none && BbOnlineGate::Allowed(setting, kOff, Gate(env)) == kOff;
    Check(none, "every cheat setting is held off with each backend");
    Check(BbOnlineGate::Allowed(5, kOff, nullptr) == 5 && BbOnlineGate::Allowed(kInfinite, kOff, nullptr) == kInfinite,
          "offline settings pass through");

    Check(!BbOnlineGate::LiveReason().load(), "no live backend at first");
    BbOnlineGate::MarkLive("seamless co-op");
    BbOnlineGate::MarkLive("The Hunter's Dream");
    Check(BbOnlineGate::LiveReason().load() && std::string(BbOnlineGate::LiveReason().load()) == "seamless co-op",
          "a live backend sticks, first reason kept");
    Check(BbOnlineGate::Reason() != nullptr, "a live backend refuses cheats without any variable");

    using OnlineRules::Ruleset;
    const std::string l2l = "Bloodborne.xml/Lamp2Lamp (warp between lamps)";
    const std::string bosses = "Bloodborne.xml/Lamp2Lamp: warp to bosses";
    Check(Ruleset({}, false) == "vanilla", "nothing on: vanilla");
    Check(Ruleset({"Bloodborne.xml/Skip Intro", "Bloodborne.xml/60 FPS++", "Bloodborne.xml/Disable DoF"}, false) == "vanilla",
          "graphics, frame rate and intro: vanilla");
    Check(Ruleset({l2l}, false) == "patch.lamp2lamp", "warps: patch.lamp2lamp");
    Check(Ruleset({bosses, l2l}, false) == "patch.lamp2lamp,patch.lamp2lamp-bosses", "warps and bosses, sorted, comma");
    Check(Ruleset({l2l, l2l}, false) == "patch.lamp2lamp", "duplicates once");
    Check(Ruleset({"Bloodborne.xml/No Rally Decay", "Bloodborne.xml/DS1-like physics", "Bloodborne.xml/Disable Rally (HP Regain)"},
                  false) == "vanilla",
          "rally and physics patches stay vanilla, as bbhost 0.2.31 marks none of its patches as gameplay");
    Check(Ruleset({"Bloodborne.xml/Player No Dead (Read note)", "Bloodborne.xml/Enemy Control"}, false) == "vanilla",
          "cheat-like patches are skipped online, no token");
    Check(Ruleset({}, true) == "patch.cheats", "cheats safety net");
    Check(Ruleset({l2l}, true) == "patch.cheats,patch.lamp2lamp", "cheats and warps");
    Check(Ruleset({"Other.xml/Lamp2Lamp (warp between lamps)"}, false) == "vanilla", "other files do not match");
    Check(OnlineRules::Describe({l2l, bosses}, false) == "Lamp2Lamp warps, Lamp2Lamp warps to bosses", "launcher words");
    Check(OnlineRules::Describe({}, false) == "vanilla", "launcher words vanilla");
    Check(OnlineRules::Valid("patch.lamp2lamp,patch.cheats") && OnlineRules::Valid("vanilla"), "valid rules");
    Check(!OnlineRules::Valid("Patch.X") && !OnlineRules::Valid("") && !OnlineRules::Valid("a b"), "invalid rules");
    Check(OnlineRules::CheatLike("Player Stealth (Read note)") && !OnlineRules::CheatLike("Restore Debug Camera"),
          "cheat-like list");

    for (int c = 0; c <= 5; ++c) {
        char what[64];
        std::snprintf(what, sizeof what, "slot open at %d cooperators: %s", c, c < 4 ? "yes" : "no");
        Check(BbParty::SlotOpen(c) == (c < 4), what);
    }
    Check(!BbParty::Full(3) && BbParty::Full(4) && BbParty::Full(5), "summoning stops at 4 guests");
    Check(BbParty::BossCount(0) == 0 && BbParty::BossCount(2) == 2 && BbParty::BossCount(4) == 2, "bosses scale as for 2 at most");
    Check(BbParty::kMaxHunters == 5, "5 hunters");
    Check(BbParty::DopingRow(0, true) == 0 && BbParty::DopingRow(-1, true) == 0, "no cooperators: no doping");
    Check(BbParty::DopingRow(1, true) == 7500 && BbParty::DopingRow(2, true) == 7501, "1 and 2 cooperators: the game's rows");
    Check(BbParty::DopingRow(3, true) == 7502 && BbParty::DopingRow(4, true) == 7503, "3 and 4 cooperators: the game's unused rows");
    Check(BbParty::DopingRow(5, true) == 7503 && BbParty::DopingRow(9, true) == 7503, "more than 4: capped at the 4 row");
    Check(BbParty::DopingRow(3, false) == 7501 && BbParty::DopingRow(4, false) == 7501 && BbParty::DopingRow(1, false) == 7500,
          "rows missing: the game's own 2 row");

    std::printf(failures ? "online-gate-test: %d failed\n" : "online-gate-test: ok\n", failures);
    return failures ? 1 : 0;
}
