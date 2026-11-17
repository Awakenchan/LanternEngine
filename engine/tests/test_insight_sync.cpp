// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/bbnet/room/room_frames.h"
#include "gpu/shim/engine/insight_sync.h"
#include <cstdio>
#include <cstring>

namespace {
namespace I = BbEngine::InsightSync;
int g_failures = 0;
bool g_quiet = false;

void Check(bool ok, const char* what) {
    if (ok) return;
    if (!g_quiet) std::printf("FAIL: %s\n", what);
    ++g_failures;
}

void Frames() {
    const auto frame = I::Encode({456, I::kQuestPresence});
    Check(frame.size() == 4 && frame[0] == 29, "frame kind 29, four bytes");
    I::Frame out;
    Check(I::Decode(frame.data(), frame.size(), out) && out.insight == 456 && out.facts == I::kQuestPresence, "a frame reads back as sent");
    Check(!I::Decode(frame.data(), 3, out), "a short frame is refused");
    std::uint8_t longer[5] = {frame[0], frame[1], frame[2], frame[3], 0};
    Check(!I::Decode(longer, sizeof longer, out), "a longer frame is refused");
    std::uint8_t wrong[4] = {26, frame[1], frame[2], frame[3]};
    Check(!I::Decode(wrong, sizeof wrong, out) && !I::Decode(nullptr, 4, out), "another kind or no data is refused");
    Check(I::Clamp(1000) == 999 && I::Clamp(-5) == 0 && I::Clamp(99) == 99, "insight is clamped to 0-999");
    const auto big = I::Encode({60000, 0});
    Check(I::Decode(big.data(), big.size(), out) && out.insight == 999, "an absurd value is clamped on both ends");
    Check(BbRoom::Frames::kHostInsight == 29 && BbRoom::Frames::kVitals == 6 && BbRoom::Frames::kVitalsFixed == 36, "vitals keep their kind and size");
}

void Sending() {
    Check(!I::ShouldSend(false, 10, -1, 0, true), "only the host sends");
    Check(!I::ShouldSend(true, -1, 5, 99999, true), "no insight read, nothing sent");
    Check(I::ShouldSend(true, 10, -1, 0, false), "the first value goes out");
    Check(I::ShouldSend(true, 11, 10, 100, false), "a change goes out at once");
    Check(!I::ShouldSend(true, 10, 10, I::kHeartbeatMs - 1, false), "the same value waits for the heartbeat");
    Check(I::ShouldSend(true, 10, 10, I::kHeartbeatMs, false), "the heartbeat repeats it");
    Check(I::ShouldSend(true, 10, 10, 0, true), "a guest that asks gets it");
}

void Gates() {
    Check(I::ValueForGate(I::kRoleClient, 10, 45, 1000) == 45, "a guest in the host's world uses the host's insight");
    Check(I::ValueForGate(I::kRoleClient, 10, 45, I::kFreshMs + 1) == 10, "a stale host value falls back to the guest's own");
    Check(I::ValueForGate(I::kRoleClient, 10, -1, 0) == 10, "an unknown host value falls back to the guest's own");
    Check(I::ValueForGate(I::kRoleHost, 10, 45, 0) == 10, "the host uses its own");
    Check(I::ValueForGate(0, 10, 45, 0) == 10, "a hunter in its own world uses its own");
    Check(I::HostSendsQuest(I::kQuestPresence, 10, 0), "a fresh host that says so sends NPC quests");
    Check(!I::HostSendsQuest(0, 10, 0), "a host that doesn't say so sends none");
    Check(!I::HostSendsQuest(I::kQuestPresence, 10, I::kFreshMs + 1), "a stale host's word is not taken");

    Check(I::Passes(40, 40, I::kAtLeast) && !I::Passes(39, 40, I::kAtLeast), ">= 40 at 40 and 39");
    Check(I::Passes(14, 15, I::kBelow) && !I::Passes(15, 15, I::kBelow), "< 15 at 14 and 15");
    Check(I::Passes(18, 18, I::kAtMost) && !I::Passes(19, 18, I::kAtMost), "<= 18 at 18 and 19");
    Check(I::Passes(0, 0, I::kEqual) && !I::Passes(1, 0, I::kEqual), "== 0");
    Check(I::Passes(1, 0, 1) && !I::Passes(0, 0, 1), "!= 0");
    Check(I::Passes(11, 10, 2) && !I::Passes(10, 10, 2), "> 10");
    Check(I::Passes(0, 99, 6) && I::Passes(0, 99, 255), "an unknown compare passes, as in the game");

    const I::CathedralFlags at14 = I::CathedralModel(14), at15 = I::CathedralModel(15);
    Check(!at14.f12404002 && at15.f12404002, "12404002 from 15 insight");
    Check(!I::CathedralModel(39).f12404001 && I::CathedralModel(40).f12404001, "12404001 from 40 insight");
    Check(!I::CathedralModel(49).f12404003 && I::CathedralModel(50).f12404003, "12404003 from 50 insight");
    Check(I::CathedralAmygdalaShown(false, I::CathedralModel(40).f12404001) && !I::CathedralAmygdalaShown(false, I::CathedralModel(10).f12404001),
          "the Cathedral Ward's Amygdala follows the world owner's flag");
    const std::int32_t guest_sees = I::ValueForGate(I::kRoleClient, 10, 45, 0);
    Check(I::YahargulAmygdalaShown(false, guest_sees), "a 10-insight guest with a 45-insight host sees the host's Yahar'gul Amygdala");
    Check(!I::YahargulAmygdalaShown(false, I::ValueForGate(I::kRoleClient, 45, 10, 0)), "a 45-insight guest with a 10-insight host sees none, as the host");
    Check(I::YahargulAmygdalaShown(true, I::ValueForGate(I::kRoleClient, 0, 0, 0)), "the Blood Moon shows it to everyone");
    Check(I::Mask2410600Shown(I::ValueForGate(I::kRoleClient, 0, 20, 0)) && !I::Mask2410600Shown(I::ValueForGate(I::kRoleClient, 30, 19, 0)),
          "the 2410600 mask follows the host's 20");
}

void Bells() {
    Check(I::SkipBellRefund(true, 9183, 6, 2004, 8, 4680), "the bell's refund is left out while bells are free");
    Check(!I::SkipBellRefund(false, 9183, 6, 2004, 8, 4680), "a paid bell keeps the game's refund");
    Check(!I::SkipBellRefund(true, 9183, 7, 2003, 2, 9185), "the rest of the refund event runs");
    Check(!I::SkipBellRefund(true, 9183, 6, 2004, 8, 4681), "only the Insight effect");
    Check(!I::SkipBellRefund(true, 9350, 6, 2004, 8, 4680) && !I::SkipBellRefund(true, 9422, 6, 2004, 8, 4680),
          "boss and story Insight are never left out");
    Check(I::BellCycle(true, 10, false) == 10, "a free bell with nobody coming gives nothing");
    Check(I::BellCycle(true, 10, true) == 10, "a free bell that summoned costs nothing and gives nothing");
    Check(I::BellCycle(false, 10, false) == 10, "a paid bell with nobody coming is paid back");
    Check(I::BellCycle(false, 10, true) == 9, "a paid bell that summoned keeps its cost");
    std::int32_t insight = 3;
    for (int ring = 0; ring < 20; ++ring) insight = I::BellCycle(true, insight, ring % 2 == 0);
    Check(insight == 3, "twenty free rings, summons and sends home leave Insight where it was");
}

struct Seen {
    std::int32_t gate;
    bool quest;
};

Seen GuestSees(const I::HostValue& value, std::int64_t now, std::int32_t own) {
    I::Frame frame;
    std::int64_t age = -1;
    if (!value.Get(now, frame, age)) return {I::ValueForGate(I::kRoleClient, own, -1, -1), I::HostSendsQuest(0, -1, -1)};
    return {I::ValueForGate(I::kRoleClient, own, frame.insight, age), I::HostSendsQuest(frame.facts, frame.insight, age)};
}

void HostChange() {
    I::HostValue value;
    Check(GuestSees(value, 0, 10).gate == 10 && !GuestSees(value, 0, 10).quest, "nothing from a host yet: the guest's own Insight, no NPC quests");
    value.Take({45, I::kQuestPresence}, 1000);
    Check(GuestSees(value, 2000, 10).gate == 45 && GuestSees(value, 2000, 10).quest, "the first host's Insight and NPC quests");
    value.Forget();
    Check(GuestSees(value, 3000, 10).gate == 10 && !GuestSees(value, 3000, 10).quest,
          "after leaving or joining another room, the old host's value is gone within its 45 s");
    value.Take({5, 0}, 4000);
    Check(GuestSees(value, 5000, 10).gate == 5 && !GuestSees(value, 5000, 10).quest, "the new host's own value, and no NPC quests when it says it sends none");
}

int RunAll() {
    g_failures = 0;
    Frames();
    Sending();
    Gates();
    Bells();
    HostChange();
    return g_failures;
}
}

int main(int argc, char** argv) {
#ifdef LANTERN_INSIGHT_SYNC_MUTANTS
    if (argc > 1 && !std::strcmp(argv[1], "--mutants")) {
        g_quiet = true;
        if (RunAll()) {
            std::printf("insight-sync mutants: the unmutated run failed\n");
            return 1;
        }
        int survivors = 0;
        for (int m = 1; m < int(I::Mutant::kCount); ++m) {
            I::g_mutant = m;
            const int found = RunAll();
            std::printf("insight-sync mutants: mutant %d %s (%d checks)\n", m, found ? "caught" : "survived", found);
            survivors += found ? 0 : 1;
        }
        I::g_mutant = 0;
        if (survivors) return 1;
        std::printf("insight-sync mutants: every mutant caught (%d)\n", int(I::Mutant::kCount) - 1);
        return 0;
    }
    std::printf("insight-sync-test: the mutant build runs with --mutants\n");
    return 1;
#else
    (void)argc;
    (void)argv;
    const int failures = RunAll();
    std::printf("insight-sync-test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
#endif
}
