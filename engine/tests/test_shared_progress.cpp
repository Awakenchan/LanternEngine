// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/shared_progress.h"
#include "gpu/shim/engine/boss_rewards.h"
#include <chrono>
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

struct FakeClock {
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<FakeClock, duration>;
};
}

int main() {
    using namespace BbEngine::SharedProgress;
    using std::chrono::seconds;
    Ledger ledger;
    Presence<FakeClock> presence(seconds(10));
    FakeClock::time_point t{};

    std::set<std::uint32_t> host_save;
    for (std::uint32_t id = 0; id < 383; ++id) host_save.insert(12100000 + id);
    Check(ledger.For("staytorn").empty(), "the host's 383 flags from before the session reach nobody");

    presence.Seen("j3loo", t);
    ledger.Record(9401, presence.Here(t));
    Check(ledger.For("j3loo") == std::vector<std::uint32_t>{9401}, "a boss killed with j3loo there goes to j3loo");
    Check(ledger.For("staytorn").empty(), "a guest who had not joined yet does not get it");

    t += seconds(30);
    presence.Seen("staytorn", t);
    ledger.Record(9402, presence.Here(t));
    Check(ledger.For("staytorn") == std::vector<std::uint32_t>{9402}, "staytorn gets only what happened after joining");
    Check(ledger.For("j3loo") == std::vector<std::uint32_t>{9401}, "j3loo left more than 10 s ago and misses it");

    for (const std::uint32_t id : ledger.For("staytorn")) Check(!host_save.count(id), "no pre-join flag in staytorn's share");

    t += seconds(5);
    presence.Seen("j3loo", t);
    presence.Seen("staytorn", t);
    ledger.Record(9403, presence.Here(t));
    Check(ledger.For("j3loo") == (std::vector<std::uint32_t>{9401, 9403}), "a rejoin keeps the earlier share and adds the new one");

    presence.Seen("staytorn", t);
    ledger.Record(1000100, presence.Here(t));
    ledger.Record(1000100 | kClear, presence.Here(t));
    Check(ledger.For("staytorn").back() == (1000100 | kClear), "an NPC step's latest value wins");

    t += seconds(11);
    Check(presence.Here(t).empty(), "a hunter not seen for 10 s is no longer present");
    ledger.Record(9404, presence.Here(t));
    Check(ledger.For("staytorn").size() == 3 && ledger.For("j3loo").size() == 3, "an event with nobody there is kept from everyone");

    Check(BossRewards::IsBossLot(15000) && BossRewards::IsBossLot(80000300) && BossRewards::IsBossLot(3601800), "the first, last and a middle boss lot are known");
    Check(!BossRewards::IsBossLot(0) && !BossRewards::IsBossLot(14000) && !BossRewards::IsBossLot(2100900), "pickups and other lots are not boss lots");
    Check(BossRewards::IsTrophyEvent(12411800) && !BossRewards::IsTrophyEvent(12100000), "Gascoigne's event carries a trophy, an ending does not count");
    BossRewards::Given<FakeClock> given{std::chrono::minutes(10)};
    const FakeClock::time_point start{};
    Check(given.First(31000, start), "the first drop of a lot is given");
    Check(!given.First(31000, start + std::chrono::seconds(5)), "the host's frame after the guest's own event gives nothing");
    Check(given.First(21000, start + std::chrono::seconds(5)), "another boss's lot is its own");
    Check(given.First(31000, start + std::chrono::minutes(11)), "a later fight gives the lot again");

    Check(BossRewards::InsightWord(3) == -3 && BossRewards::InsightOf(-3) == 3, "a boss's Insight travels as a negative word in the boss drop frame");
    Check(BossRewards::InsightWord(0) == 0 && BossRewards::InsightWord(10) == 0 && BossRewards::InsightWord(-1) == 0, "only 1 to 9 Insight is sent");
    Check(BossRewards::InsightOf(31000) == 0 && BossRewards::InsightOf(0) == 0 && BossRewards::InsightOf(-10) == 0, "a lot is never read as Insight");
    Check(!BossRewards::IsBossLot(BossRewards::InsightWord(3)), "an older guest drops the word as an unknown lot");

    Check(BbEngine::SharedProgress::HostStepIsNewer(0b100, 0b010), "a guest behind the host's quest step takes it");
    Check(!BbEngine::SharedProgress::HostStepIsNewer(0b010, 0b100), "a guest ahead of the host keeps its quest step");
    Check(!BbEngine::SharedProgress::HostStepIsNewer(0b010, 0b010), "the same quest step writes nothing");
    Check(BbEngine::SharedProgress::HostStepIsNewer(0b001, 0), "a guest who never started takes the first step");
    Check(!BbEngine::SharedProgress::HostStepIsNewer(0, 0b001), "an empty block never wipes a guest's quest");

    if (failures) return 1;
    std::printf("shared-progress-test: ok\n");
    return 0;
}
