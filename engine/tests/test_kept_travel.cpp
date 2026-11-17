// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/kept_travel.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
bool quiet = false;
void Check(bool ok, const char* what) {
    if (!ok) { if (!quiet) std::printf("FAIL: %s\n", what); ++failures; }
}

struct FakeClock {
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<FakeClock, duration>;
};

void Word(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
    std::memcpy(out.data() + at, &value, 4);
}

std::vector<std::uint8_t> Snapshot() {
    std::vector<std::uint8_t> out(0xa4 + 12 * 125, 0);
    Word(out, 0, BbEngine::KeptTravel::kFlagsMagic);
    out[4] = 1;
    Word(out, 8, 12);
    Word(out, 12, 0x14);
    Word(out, 16, 0xa4);
    for (std::uint32_t i = 0; i < 12; ++i) {
        Word(out, 0x14 + i * 12, 100 + i);
        Word(out, 0x18 + i * 12, i * 125);
        Word(out, 0x1c + i * 12, 125);
    }
    return out;
}
}

namespace {
using BbEngine::KeptTravel::WarpChoice;
using ChoiceFn = WarpChoice (*)(bool, std::int32_t, std::uint32_t, bool, std::int64_t, std::int64_t, bool);

struct ChoiceRow {
    bool client;
    std::int32_t script;
    std::uint32_t lamp;
    bool following;
    std::int64_t held_since, now;
    bool keep;
    WarpChoice want;
    const char* what;
};

constexpr std::int64_t kFresh = BbEngine::KeptTravel::kNotHeld;
constexpr std::int32_t kSnatcher = BbEngine::KeptTravel::kSnatcherEvent;
const ChoiceRow kChoiceTable[] = {
    {true, 13200310, 2602959, false, kFresh, 1000, true, WarpChoice::Hold, "the guest's own Mensis door warp waits for the host"},
    {true, 13200311, 3302959, false, 0, 14999, true, WarpChoice::Hold, "still waiting just under 15 s"},
    {true, 13200311, 3302959, true, kFresh, 1000, true, WarpChoice::Drop, "the guest's own Frontier door warp while following is dropped"},
    {true, 13200310, 2602959, true, 0, 20000, true, WarpChoice::Drop, "a held warp is dropped once the guest follows"},
    {true, 13200310, 2602959, false, 0, 15000, true, WarpChoice::Ask, "no host travel in 15 s: the host is asked"},
    {true, 13200310, 2602959, false, 0, 15000, false, WarpChoice::Solo, "no host travel in 15 s, keeping off: the guest goes alone"},
    {true, 3503, 2102961, false, 0, 15000, true, WarpChoice::Solo, "a held story warp to the Dream goes alone"},
    {true, kSnatcher, 2802958, false, kFresh, 0, true, WarpChoice::Stay, "the snatcher stays as it is"},
    {true, kSnatcher, 2802958, true, 0, 20000, false, WarpChoice::Stay, "the snatcher stays as it is while following"},
    {true, 7200, 2412951, false, kFresh, 0, true, WarpChoice::Ask, "a guest's lamp is an ask"},
    {true, 7200, 2412951, true, kFresh, 0, true, WarpChoice::Ask, "a guest's lamp is an ask while following too"},
    {true, 7200, 2412951, false, kFresh, 0, false, WarpChoice::Solo, "a guest's lamp with keeping off goes alone"},
    {true, 0, 2412951, false, kFresh, 0, true, WarpChoice::Ask, "no event: an ask as before"},
    {false, 13200310, 2602959, false, kFresh, 0, true, WarpChoice::Pass, "the host's door warp passes"},
    {false, 7200, 2412951, false, kFresh, 0, true, WarpChoice::Pass, "the host's lamp passes"},
    {false, kSnatcher, 2802958, false, kFresh, 0, true, WarpChoice::Pass, "the host's snatcher passes"},
};

int TableMisses(ChoiceFn fn, bool report) {
    int misses = 0;
    for (const ChoiceRow& row : kChoiceTable) {
        if (fn(row.client, row.script, row.lamp, row.following, row.held_since, row.now, row.keep) == row.want) continue;
        ++misses;
        if (report) Check(false, row.what);
    }
    return misses;
}

WarpChoice Real(bool c, std::int32_t s, std::uint32_t l, bool f, std::int64_t h, std::int64_t n, bool k) {
    return BbEngine::KeptTravel::GuestWarpChoice(c, s, l, f, h, n, k);
}
WarpChoice SwapHoldAsk(bool c, std::int32_t s, std::uint32_t l, bool f, std::int64_t h, std::int64_t n, bool k) {
    const WarpChoice real = Real(c, s, l, f, h, n, k);
    return real == WarpChoice::Hold ? WarpChoice::Ask : real == WarpChoice::Ask ? WarpChoice::Hold : real;
}
WarpChoice NoLimit(bool c, std::int32_t s, std::uint32_t l, bool f, std::int64_t, std::int64_t n, bool k) {
    return Real(c, s, l, f, kFresh, n, k);
}
WarpChoice NoDrop(bool c, std::int32_t s, std::uint32_t l, bool, std::int64_t h, std::int64_t n, bool k) {
    return Real(c, s, l, false, h, n, k);
}
WarpChoice HostHolds(bool, std::int32_t s, std::uint32_t l, bool f, std::int64_t h, std::int64_t n, bool k) {
    return Real(true, s, l, f, h, n, k);
}
WarpChoice SnatcherHeld(bool c, std::int32_t s, std::uint32_t l, bool f, std::int64_t h, std::int64_t n, bool k) {
    return Real(c, s == kSnatcher ? 13200310 : s, l, f, h, n, k);
}
WarpChoice KeepIgnored(bool c, std::int32_t s, std::uint32_t l, bool f, std::int64_t h, std::int64_t n, bool) {
    return Real(c, s, l, f, h, n, true);
}

struct DoorRun {
    int follows = 0, asks = 0, solos = 0, drops = 0;
    bool held_at_end = false;
};

DoorRun RunDoor(std::int64_t frame7_at, const std::vector<std::int64_t>& own_warps, bool keep) {
    BbEngine::KeptTravel::StoryHold hold;
    DoorRun run;
    bool following = false;
    for (std::int64_t now = 0; now <= 40000; now += 100) {
        for (std::int64_t at : own_warps) {
            if (at != now) continue;
            const WarpChoice choice = hold.Warp(true, 13200310, 2602959, following, now, keep);
            if (choice == WarpChoice::Drop) ++run.drops;
            else if (choice == WarpChoice::Ask) ++run.asks;
            else if (choice == WarpChoice::Solo) ++run.solos;
        }
        std::uint32_t lamp = 0;
        const WarpChoice choice = hold.Tick(true, following, now, keep, lamp);
        if (lamp && choice == WarpChoice::Ask) ++run.asks;
        else if (lamp && choice == WarpChoice::Solo) ++run.solos;
        else if (lamp && choice == WarpChoice::Drop) ++run.drops;
        if (now == frame7_at) {
            hold.Followed();
            following = true;
            ++run.follows;
        }
    }
    run.held_at_end = hold.Held();
    return run;
}

bool HeldThrough(bool give_up) {
    using namespace BbEngine::KeptTravel;
    Load<FakeClock> load;
    FakeClock::time_point now{};
    const std::int64_t rejoin_until = now.time_since_epoch().count() + 60000;
    load.Begin(now);
    bool held_always = true, old_rule_lapsed = false;
    Step step = Step::Wait;
    while (now.time_since_epoch() <= std::chrono::seconds(400)) {
        now += std::chrono::milliseconds(100);
        const auto since = now.time_since_epoch();
        const bool loading = give_up || since < std::chrono::seconds(90);
        const bool host_here = !give_up && since >= std::chrono::seconds(130);
        step = load.Tick(now, loading, !loading, true, host_here, 0);
        if (step == Step::Done || step == Step::GiveUp) break;
        const std::int64_t t = since.count();
        held_always = held_always && LeaveChecksHeld(rejoin_until, load.Active(), t);
        old_rule_lapsed = old_rule_lapsed || !LeaveChecksHeld(rejoin_until, false, t);
    }
    const std::int64_t end = now.time_since_epoch().count();
    return held_always && old_rule_lapsed && step == (give_up ? Step::GiveUp : Step::Done) && !LeaveChecksHeld(rejoin_until, load.Active(), end);
}
}

int RunAll() {
    failures = 0;
    using namespace BbEngine::KeptTravel;
    using std::chrono::seconds;
    Check(SceneSeenFlag(12410000) == 12410000, "a run-once scene is seen when its own event flag is set");
    Check(SceneSeenFlag(12405263) == 12401000 && SceneSeenFlag(12200130) == 12204020 && SceneSeenFlag(12800431) == 12800434, "repeating scene events have a flag of their own");

    Check(CanKeep(2412951), "a lamp in Yharnam keeps the party");
    Check(!CanKeep(2102961), "the Hunter's Dream goes through each hunter's own Dream");
    Check(!CanKeep(2900100), "a chalice dungeon lamp is left to the summon");
    Check(!CanKeep(0), "no lamp, nothing to keep");

    auto good = Snapshot();
    Check(ValidFlags(good.data(), good.size()), "the size the game's serializer writes is accepted");
    Check(!ValidFlags(nullptr, 100), "no data");
    Check(!ValidFlags(good.data(), 16), "too short for its header");
    auto bad = good;
    bad[0] ^= 1;
    Check(!ValidFlags(bad.data(), bad.size()), "wrong magic");
    bad = good;
    bad[4] = 2;
    Check(!ValidFlags(bad.data(), bad.size()), "wrong version");
    bad = good;
    Word(bad, 8, 100000);
    Check(!ValidFlags(bad.data(), bad.size()), "an absurd block count");
    bad = good;
    Word(bad, 0x18 + 11 * 12, 12 * 125);
    Check(!ValidFlags(bad.data(), bad.size()), "a block past the end");
    bad = good;
    Word(bad, 12, 0xfffffff0u);
    Check(!ValidFlags(bad.data(), bad.size()), "a table past the end");
    Check(!ValidFlags(good.data(), good.size() - 1), "a cut frame");

    using KeptLoad = BbEngine::KeptTravel::Load<FakeClock>;
    FakeClock::time_point t{};
    auto Tick = [&](KeptLoad& load, bool loading, bool placed, bool session, bool here, int missing) {
        t += std::chrono::milliseconds(100);
        return load.Tick(t, loading, placed, session, here, missing);
    };

    auto Members = [&](KeptLoad& load, bool loading, bool placed, bool here, int missing, bool members_in) {
        t += std::chrono::milliseconds(100);
        return load.Tick(t, loading, placed, true, here, missing, members_in);
    };

    KeptLoad left;
    left.Begin(t);
    Check(Members(left, true, false, false, 0, false) == Step::Wait, "a member out of the game's session while loading is waited for");
    Members(left, false, true, false, 1, true);
    t += seconds(3);
    Check(Members(left, false, true, true, 1, true) == Step::Rebuild, "the bodies are rebuilt while everyone is in the session");
    Check(Members(left, false, true, true, 0, false) == Step::GiveUp, "a member whose game left the session ends the travel as lost, never Done");
    Check(left.MembersLeft() && !left.Active(), "and says that a member left");

    KeptLoad quick;
    quick.Begin(t);
    Members(quick, true, false, false, 0, true);
    Members(quick, false, true, false, 1, true);
    Check(Members(quick, false, true, true, 0, false) == Step::GiveUp && quick.MembersLeft(), "a member gone right after the load is lost at once");

    KeptLoad stayed;
    stayed.Begin(t);
    Members(stayed, true, false, false, 0, true);
    Members(stayed, false, true, false, 0, true);
    t += seconds(3);
    Check(Members(stayed, false, true, true, 0, true) == Step::Done && !stayed.MembersLeft(), "everyone in the session and here: Done as before");
    stayed.Begin(t);
    Check(!stayed.MembersLeft(), "a new travel starts with nobody gone");

    KeptLoad idle;
    Check(!idle.Active() && Tick(idle, true, true, true, true, 0) == Step::Wait, "nothing happens before Begin");

    KeptLoad smooth;
    smooth.Begin(t);
    Check(Tick(smooth, false, true, true, false, 0) == Step::Wait, "waits for the load to start");
    Check(Tick(smooth, true, false, true, false, 0) == Step::Wait, "loading");
    Check(!smooth.LoadOver(), "not over while loading");
    Check(Tick(smooth, false, false, true, false, 0) == Step::Wait, "no player yet");
    Check(Tick(smooth, false, true, true, false, 1) == Step::Wait, "placed");
    Check(smooth.LoadOver(), "over once placed");
    t += seconds(3);
    Check(Tick(smooth, false, true, true, true, 1) == Step::Rebuild, "bodies are built once the others are here");
    Check(Tick(smooth, false, true, true, true, 1) == Step::Wait, "not again at once");
    t += seconds(3);
    Check(Tick(smooth, false, true, true, true, 1) == Step::Rebuild, "again if it is still missing");
    Check(Tick(smooth, false, true, true, true, 0) == Step::Done, "done when everyone is back");
    Check(!smooth.Active(), "finished");

    KeptLoad alone;
    alone.Begin(t);
    Tick(alone, true, false, true, false, 0);
    Tick(alone, false, true, true, false, 1);
    t += seconds(3);
    Check(Tick(alone, false, true, true, false, 1) == Step::Wait, "waits for the others before building");
    t += seconds(20);
    Check(Tick(alone, false, true, true, false, 1) == Step::Rebuild, "builds after 20 s anyway");

    KeptLoad never;
    never.Begin(t);
    t += seconds(31);
    Check(Tick(never, false, true, true, false, 0) == Step::GiveUp, "the load never started");
    Check(!never.Active(), "off after giving up");

    KeptLoad slow;
    slow.Begin(t);
    t += seconds(200);
    Check(Tick(slow, true, false, true, false, 0) == Step::Wait, "a slow load is waited for");
    t += seconds(101);
    Check(Tick(slow, true, false, true, false, 0) == Step::GiveUp, "a stuck load is not");

    KeptLoad gone;
    gone.Begin(t);
    Tick(gone, true, false, true, false, 0);
    Check(Tick(gone, true, false, false, false, 0) == Step::GiveUp, "the session closed anyway: the old flow takes over");

    KeptLoad late;
    late.Begin(t);
    Tick(late, true, false, true, false, 0);
    Tick(late, false, true, true, false, 0);
    t += seconds(121);
    Check(Tick(late, false, true, true, false, 0) == Step::GiveUp, "the others never came");

    KeptLoad twice;
    twice.Begin(t);
    Tick(twice, true, false, true, false, 0);
    Tick(twice, false, true, true, false, 0);
    Check(twice.LoadOver(), "first load over");
    Tick(twice, true, false, true, false, 0);
    Check(!twice.LoadOver(), "a second load counts as loading");

    Check(CanKeepMap(0x18010000u) && CanKeepMap(0x1c000000u), "a world map keeps the party");
    Check(!CanKeepMap(0x15000000u) && !CanKeepMap(0x1d000000u) && !CanKeepMap(0), "the Dream, dungeons and no map don't");
    for (const std::int32_t type : {1, 2, 12, 13}) Check(PhantomType(type), "a phantom type");
    for (const std::int32_t type : {-1, 0, 3, 8, 11, 14}) Check(!PhantomType(type), "not a phantom type");
    Check(WrongTypeForOwnWorld(4, true, 1, -1), "a saved phantom type loading its own world");
    Check(WrongTypeForOwnWorld(0, true, 0, 1), "a saved pending phantom type at the first load");
    Check(!WrongTypeForOwnWorld(0, true, 1, 0), "the game's own pending type already puts it right");
    Check(!WrongTypeForOwnWorld(3, true, 1, -1), "a guest loading the host's world stays a phantom");
    Check(!WrongTypeForOwnWorld(4, false, 1, -1), "a guest placed in the host's world stays a phantom");
    Check(!WrongTypeForOwnWorld(0, true, 0, -1), "a normal hunter is left alone");
    Check(!WrongTypeForOwnWorld(0, true, 8, -1), "a grey hunter is left alone");
    Check(OwnWorldType(1) == 0 && OwnWorldType(-1) == 0 && OwnWorldType(0) == 0, "a bad home type becomes a normal hunter");
    Check(OwnWorldType(8) == 8, "a grey home type is kept");

    Check(HoldsHit(0x1000, 2, 0x138, 0x1000), "the first hit of a block");
    Check(HoldsHit(0x1000, 2, 0x138, 0x1138), "the last hit of a block");
    Check(!HoldsHit(0x1000, 2, 0x138, 0x1270), "one past the block's hits");
    Check(!HoldsHit(0x1000, 2, 0x138, 0xec8), "before the block's hits");
    Check(!HoldsHit(0x1000, 2, 0x138, 0x1001), "inside a hit, not at its start");
    Check(!HoldsHit(0x1000, 0, 0x138, 0x1000), "a block with no hits");

    Check(LampText(2402950) == 70002400 && LampText(2412951) == 70002411 && LampText(3302951) == 70003301, "a lamp's text is its map's, plus its number");
    Check(LampText(2102959) == 70002109 && LampMapText(2102959) == 70002100, "a point with no text of its own falls back to its map");
    Check(LampText(2100950) == LampMapText(2100950), "a low point is its map");
    Check(StoryWarp(12405263) && StoryWarp(13200310) && StoryWarp(kSnatcherEvent), "scripted story warps are the host's own");
    Check(!StoryWarp(7200) && !StoryWarp(12107000) && !StoryWarp(0), "lamp events stay behind the host's own choice");
    Check(HoldForHost(true, false, false), "a guest waits for a host still loading");
    Check(!HoldForHost(true, true, false), "the host is here");
    Check(!HoldForHost(true, false, true) && !HoldForHost(false, false, false), "the wait ends at its limit or with the link");

    Check(TableMisses(Real, true) == 0, "the guest warp table holds");
    Check(TableMisses(SwapHoldAsk, false) > 0, "mutant: hold and ask swapped is caught");
    Check(TableMisses(NoLimit, false) > 0, "mutant: no 15 s limit is caught");
    Check(TableMisses(NoDrop, false) > 0, "mutant: following ignored is caught");
    Check(TableMisses(HostHolds, false) > 0, "mutant: the host held too is caught");
    Check(TableMisses(SnatcherHeld, false) > 0, "mutant: the snatcher held is caught");
    Check(TableMisses(KeepIgnored, false) > 0, "mutant: keeping off ignored is caught");

    const DoorRun door = RunDoor(6100, {5900, 6800}, true);
    Check(door.follows == 1 && door.asks == 0 && door.solos == 0, "door: one follow, no ask, no solo leave");
    Check(door.drops == 1 && !door.held_at_end, "door: the late copy is dropped and nothing stays held");
    const DoorRun door_off = RunDoor(6100, {5900, 6800}, false);
    Check(door_off.follows == 1 && door_off.asks == 0 && door_off.solos == 0, "door with keeping off: still no solo leave");
    const DoorRun alone_door = RunDoor(-1, {5900, 6000}, true);
    Check(alone_door.asks == 1 && alone_door.solos == 0 && !alone_door.held_at_end, "the guest opened the door alone: one ask after 15 s");
    const DoorRun alone_off = RunDoor(-1, {5900}, false);
    Check(alone_off.solos == 1 && alone_off.asks == 0, "the guest opened the door alone, keeping off: one solo leave");
    const DoorRun late_host = RunDoor(25000, {5900}, true);
    Check(late_host.asks == 1 && late_host.follows == 1, "a host travel after the 15 s fallback still follows");

    Check(HeldThrough(false), "leave checks held through 90 s of loading and 40 s of waiting, until Done");
    Check(HeldThrough(true), "leave checks held through a stuck load until GiveUp");
    Check(LeaveChecksHeld(60000, false, 59999) && !LeaveChecksHeld(60000, false, 60000), "the old 60 s hold alone ends at 60 s");

    return failures;
}

int main(int argc, char** argv) {
#ifdef LANTERN_KEPT_TRAVEL_MUTANTS
    if (argc > 1 && !std::strcmp(argv[1], "--mutants")) {
        quiet = true;
        if (RunAll()) {
            std::printf("kept-travel mutants: the unmutated run failed\n");
            return 1;
        }
        int survivors = 0;
        for (int m = 1; m < int(BbEngine::KeptTravel::Mutant::kCount); ++m) {
            BbEngine::KeptTravel::g_mutant = m;
            const int found = RunAll();
            std::printf("kept-travel mutants: mutant %d %s (%d checks)\n", m, found ? "caught" : "survived", found);
            survivors += found ? 0 : 1;
        }
        BbEngine::KeptTravel::g_mutant = 0;
        if (survivors) return 1;
        std::printf("kept-travel mutants: every mutant caught (%d)\n", int(BbEngine::KeptTravel::Mutant::kCount) - 1);
        return 0;
    }
    std::printf("kept-travel-test: the mutant build runs with --mutants\n");
    return 1;
#else
    (void)argc;
    (void)argv;
    if (RunAll()) return 1;
    std::printf("kept-travel-test: ok\n");
    return 0;
#endif
}
