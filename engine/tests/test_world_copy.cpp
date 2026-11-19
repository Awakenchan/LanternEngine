// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/world_copy.h"
#include <cstdio>
#include <cstring>
#include <map>

namespace {
int failures = 0;
bool quiet = false;

void Check(bool ok, const char* what) {
    if (ok) return;
    if (!quiet) std::printf("FAIL: %s\n", what);
    ++failures;
}

struct FlagRange {
    std::uint32_t first, last, kind;
};
constexpr FlagRange kProgressFlags[] = {
#include "gpu/shim/engine/progress_flags.inc"
};

using namespace BbEngine::WorldCopy;

bool Bit(const Group& g, std::uint32_t group, std::uint32_t id) {
    const std::uint32_t at = id - group * 1000;
    return (g[at >> 3] & (0x80u >> (at & 7))) != 0;
}

void SetBit(Group& g, std::uint32_t group, std::uint32_t id, bool on) {
    const std::uint32_t at = id - group * 1000;
    const auto bit = std::uint8_t(0x80u >> (at & 7));
    g[at >> 3] = std::uint8_t(on ? g[at >> 3] | bit : g[at >> 3] & ~bit);
}

constexpr std::uint32_t kQuestGroup = 1, kNpcStep = 1671, kNextStep = 1672, kTalkGroup = 72400, kOwnTalkFlag = 72400460;

bool NpcShown(const Group& flags) {
    return Bit(flags, kQuestGroup, kNpcStep);
}

Split OldSplit(const Split& now) {
    Split old = now;
    Add(old.own, old.quest);
    old.quest = Group{};
    return old;
}

struct GuestRun {
    Group save{}, overlay{};
    int save_writes = 0;
};

struct Step {
    std::uint32_t id;
    bool on;
};

GuestRun PlayIn(std::uint32_t group, const Group& host_save, const Group& guest_save, const Group& vanilla_overlay, bool host_is_new,
                std::initializer_list<Step> steps) {
    const Split split = SplitGroup(group, true, kProgressFlags);
    const Split host_split = host_is_new ? split : OldSplit(split);
    GuestRun run;
    run.save = guest_save;
    run.overlay = vanilla_overlay;
    CopyMasked(run.overlay.data(), run.save.data(), split.own);
    const Group baseline = Masked(run.overlay.data(), split.own);
    const Group sent = Masked(host_save.data(), host_is_new ? HostSends(host_split.shared, host_split.quest) : host_split.shared);
    Image image;
    image[group] = sent;
    Receiver in;
    Image changed;
    for (const auto& frame : Encode(image, true)) in.Take(frame.data(), frame.size(), changed);
    WriteShared(run.overlay.data(), in.Value(group), GuestTakes(split.shared, split.quest, host_is_new));
    for (const Step& s : steps) SetBit(run.overlay, group, s.id, s.on);
    run.save_writes = WriteChanged(run.save.data(), baseline, Masked(run.overlay.data(), split.own), split.own);
    return run;
}

GuestRun PlayInHostWorld(const Group& host_save, const Group& guest_save, const Group& vanilla_overlay, bool host_is_new, bool step_in_host_world) {
    const GuestRun arrived = PlayIn(kQuestGroup, host_save, guest_save, vanilla_overlay, host_is_new, {});
    if (!step_in_host_world) return arrived;
    return PlayIn(kQuestGroup, host_save, guest_save, vanilla_overlay, host_is_new, {{kNextStep, true}, {kNpcStep, !Bit(arrived.overlay, kQuestGroup, kNpcStep)}});
}

void StateBlocks() {
    for (const std::uint32_t id : {1040u, 1059u, 1340u, 1343u, 1344u, 1359u, 1790u, 1809u})
        Check(Bit(SplitGroup(kQuestGroup, true, kProgressFlags).quest, kQuestGroup, id) && !Bit(SplitGroup(kQuestGroup, true, kProgressFlags).own, kQuestGroup, id),
              "the NPC state blocks 1040, 1340 and 1790 follow the host and never reach the save");
    Check(IsQuestFlag(72900001, kProgressFlags) && !CarryGroup(72900), "a chalice NPC flag is a quest flag, and its group is never in the world copy");
    for (const std::uint32_t id : {72500304u, 72500309u, 70002413u, 70002401u, 70000240u, 72410410u}) {
        const Split s = SplitGroup(id / 1000, true, kProgressFlags);
        Check(Bit(s.quest, id / 1000, id) && !Bit(s.own, id / 1000, id) && IsQuestFlag(id, kProgressFlags),
              "a state block or a flag an NPC event derives from a quest step follows the host");
    }
    const Split talk = SplitGroup(kTalkGroup, true, kProgressFlags);
    Check(Bit(talk.own, kTalkGroup, kOwnTalkFlag) && !Bit(talk.quest, kTalkGroup, kOwnTalkFlag) && !IsQuestFlag(kOwnTalkFlag, kProgressFlags),
          "a talk flag no event writes stays the hunter's own");
    Group host{}, guest{};
    SetBit(host, kQuestGroup, 1345, true);
    SetBit(guest, kQuestGroup, 1343, true);
    const GuestRun step = PlayIn(kQuestGroup, host, guest, Group{}, true, {{1343, false}, {1344, true}});
    Check(Bit(step.overlay, kQuestGroup, 1344) && !Bit(step.overlay, kQuestGroup, 1343), "the step 1343 to 1344 happens in the guest's copy");
    Check(std::memcmp(step.save.data(), guest.data(), kGroupBytes) == 0 && step.save_writes == 0, "and the guest's save is byte for byte as it was");
    const GuestRun seen = PlayIn(kQuestGroup, host, guest, Group{}, true, {});
    Check(Bit(seen.overlay, kQuestGroup, 1345) && !Bit(seen.overlay, kQuestGroup, 1343), "the guest sees the host's NPC 2400760 state, not its own");
    Group own_talk{};
    SetBit(own_talk, kTalkGroup, kOwnTalkFlag, true);
    const GuestRun talked = PlayIn(kTalkGroup, Group{}, own_talk, Group{}, true, {});
    Check(Bit(talked.overlay, kTalkGroup, kOwnTalkFlag), "the guest's own talk flag is in its copy");
    const GuestRun talks = PlayIn(kTalkGroup, Group{}, Group{}, Group{}, true, {{kOwnTalkFlag, true}});
    Check(talks.save_writes == 1 && Bit(talks.save, kTalkGroup, kOwnTalkFlag), "a talk in the host's world reaches the guest's save");
}

void OwnerCases() {
    const Split split = SplitGroup(kQuestGroup, true, kProgressFlags);
    Check(Bit(split.quest, kQuestGroup, kNpcStep) && !Bit(split.own, kQuestGroup, kNpcStep) && !Bit(split.shared, kQuestGroup, kNpcStep),
          "an NPC quest step is a quest bit: the host's world decides it, the guest's save keeps its own");
    for (const std::uint32_t id : {1650u, 1670u, 1680u, 1681u, 1689u, 1710u, 1720u, 1724u, 1729u})
        Check(Bit(split.quest, kQuestGroup, id), "every quest flag the m34 NPC events read follows the host");
    const Split talk = SplitGroup(73400, true, kProgressFlags);
    for (const std::uint32_t id : {73400403u, 73400512u, 73400513u}) Check(Bit(talk.quest, 73400, id), "the m34 talk flags the NPC events read follow the host");
    Check(Bit(SplitGroup(13401, true, kProgressFlags).shared, 13401, 13401800), "the m34 boss flag stays shared");
    const Split pickups = SplitGroup(52400, true, kProgressFlags);
    Check(IsZero(pickups.quest) && IsZero(pickups.shared) && !IsZero(pickups.own), "pickups stay each hunter's own");
    const Split gifts = SplitGroup(9, true, kProgressFlags);
    Check(Bit(gifts.own, 9, 9045) && !Bit(gifts.quest, 9, 9045) && Bit(gifts.shared, 9, 9801), "gift requests stay own, time of day stays shared");
    const Split plain = SplitGroup(kQuestGroup, false, kProgressFlags);
    Check(IsZero(plain.own) && IsZero(plain.quest) && Bit(plain.shared, kQuestGroup, kNpcStep), "without the personal world the host's world has it all");
    Check(Bit(HostSends(split.shared, split.quest), kQuestGroup, kNpcStep), "the host sends its NPC quest bits");
    Check(!Bit(GuestTakes(split.shared, split.quest, false), kQuestGroup, kNpcStep) && Bit(GuestTakes(split.shared, split.quest, true), kQuestGroup, kNpcStep),
          "a guest takes them only from a host that says it sends them");
    Check(IsQuestFlag(kNpcStep, kProgressFlags) && IsQuestFlag(73400403, kProgressFlags) && !IsQuestFlag(13401800, kProgressFlags) &&
              !IsQuestFlag(kOwnTalkFlag, kProgressFlags),
          "the host's progress never carries a quest step to a guest's save");

    Group host_on{}, guest_off{}, host_off{}, guest_on{};
    SetBit(host_on, kQuestGroup, kNpcStep, true);
    SetBit(guest_on, kQuestGroup, kNpcStep, true);
    for (const Group& vanilla : {host_on, guest_on, Group{}}) {
        const GuestRun a = PlayInHostWorld(host_on, guest_off, vanilla, true, false);
        Check(NpcShown(a.overlay) && NpcShown(host_on), "case 1: guest's quest off, host's on: the guest sees the NPC the host's world runs");
        Check(NpcShown(a.overlay) == NpcShown(host_on), "case 1: both run the NPC, so the guest's hits land on it");
        const GuestRun b = PlayInHostWorld(host_on, guest_off, vanilla, true, true);
        Check(std::memcmp(b.save.data(), guest_off.data(), kGroupBytes) == 0 && b.save_writes == 0,
              "case 2: quest steps in the host's world leave the guest's save byte for byte as it was");
        const GuestRun c = PlayInHostWorld(host_off, guest_on, vanilla, true, false);
        Check(!NpcShown(c.overlay) && !NpcShown(host_off), "case 3: host's quest off, guest's on: nothing shows for the guest");
        Check(std::memcmp(c.save.data(), guest_on.data(), kGroupBytes) == 0, "case 3: the guest's own quest stays in its save");
        const GuestRun d = PlayInHostWorld(host_off, guest_on, vanilla, true, true);
        Check(std::memcmp(d.save.data(), guest_on.data(), kGroupBytes) == 0, "case 3: and a step there never reaches its save");
    }
    const GuestRun old_host = PlayInHostWorld(host_on, guest_off, host_on, false, false);
    Check(NpcShown(old_host.overlay), "an older host sends no quest bits: the game's own copy of its world is left as it came");
    Check(std::memcmp(old_host.save.data(), guest_off.data(), kGroupBytes) == 0, "and the guest's save stays as it was");
    {
        Group overlay{};
        SetBit(overlay, kQuestGroup, kNextStep, true);
        const Group before = overlay;
        const Group sent = Masked(host_on.data(), HostSends(split.shared, split.quest));
        Check(WriteShared(overlay.data(), sent, OldSplit(split).shared) == 0 && std::memcmp(overlay.data(), before.data(), kGroupBytes) == 0,
              "an older guest masks the new quest bits: 0 NPC bytes change");
    }
}

void InsightFlags() {
    Check(!ShareGroup(12404) && !ShareGroup(12204) && !ShareGroup(12425), "the insight flags' groups stay unshared as groups");
    Check(ShareFlag(12404001) && ShareFlag(12404002) && ShareFlag(12404003), "the Cathedral Ward's insight flags follow the host");
    Check(ShareFlag(12204011) && ShareFlag(12204845) && ShareFlag(12425291), "the other world-owner insight flags follow the host");
    Check(!ShareFlag(12404000) && !ShareFlag(12404004) && !ShareFlag(12404800) && !ShareFlag(12204010), "the rest of those groups stays out");
    Check(!ShareFlag(12100320) && !ShareFlag(12105045), "the Dream's insight flags stay each hunter's own");
    Check(CarryGroup(12404) && CarryGroup(12204) && CarryGroup(12425) && !CarryGroup(12405) && !CarryGroup(12100) && !CarryGroup(6),
          "only the groups with an insight flag travel beside the shared ones");
    Check(CountBits(MaskOf(12404)) == 3, "group 12404 carries three bits");
    Image image;
    image[12404].fill(0xff);
    Receiver in;
    Image changed;
    for (const auto& frame : Encode(image, true)) in.Take(frame.data(), frame.size(), changed);
    Check(changed.count(12404) == 1, "a new guest takes the insight group");
    Group overlay{};
    WriteShared(overlay.data(), in.Value(12404), MaskOf(12404));
    Check(CountBits(overlay) == 3 && Bit(overlay, 12404, 12404001), "and writes only its insight bits");
    Check(!ShareGroup(12404), "an older guest's receiver drops the group");
}
}

int RunAll() {
    failures = 0;

    Check(ShareGroup(12400) && ShareGroup(72410) && ShareGroup(52300), "doors, lamps and pickups are shared");
    Check(ShareGroup(0) && ShareGroup(1) && ShareGroup(2) && ShareGroup(9), "the world's global blocks are shared");
    Check(!ShareGroup(3) && !ShareGroup(4) && !ShareGroup(5), "death and load flags stay each hunter's");
    Check(!ShareGroup(6) && !ShareGroup(7) && !ShareGroup(8), "the personal blocks stay each hunter's");
    Check(!ShareGroup(12404) && !ShareGroup(12405) && !ShareGroup(12406), "a fight's own state stays out");
    Check(!ShareGroup(12100) && !ShareGroup(72101), "the Dream stays each hunter's");
    Check(!ShareGroup(12900) && !ShareGroup(72900), "dungeons have their own sharing");
    Check(!ShareGroup(60009), "the item ledger stays out");

    Check(!ShareFlag(21) && !ShareFlag(23), "ending flags are never copied");
    Check(!ShareFlag(2010) && ShareFlag(2410), "session flags stay, area gates go");
    Check(!ShareFlag(9022) && !ShareFlag(9180) && !ShareFlag(9450) && !ShareFlag(9550) && !ShareFlag(9901), "personal flags in block 9 stay");
    Check(ShareFlag(9801) && ShareFlag(9400), "time of day and world flags in block 9 go");
    Check(!ShareFlag(70009300) && ShareFlag(70009700), "shop stock is per hunter");
    Check(ShareFlag(12401800) && !ShareFlag(12404800), "boss deaths go, the fight's state stays");

    const Group mask = MaskOf(9);
    Check((mask[0] & 0x80) != 0, "mask keeps 9000");
    Check((mask[2] & (0x80 >> 4)) == 0, "mask drops 9020");

    {
        const Group bits = RangeBits(72400, 72400096, 72400115);
        Check(bits[12] == 0xff && bits[13] == 0xff && bits[14] == 0xf0 && bits[11] == 0, "a quest block's bits in its group");
        Check(IsZero(RangeBits(72400, 1000, 1019)) && IsZero(RangeBits(72400, 72401000, 72401019)), "a block of another group has no bits here");
        Check(RangeBits(72400, 72399990, 72400001)[0] == 0xc0, "a block that starts before the group");
        Group own{}, overlay{};
        own[12] = 0xa0;
        overlay[12] = 0x5f;
        overlay[20] = 0x33;
        Check(CopyMasked(overlay.data(), own.data(), bits) == 1 && overlay[12] == 0xa0 && overlay[20] == 0x33, "a guest's own step goes in the overlay, the host's flags beside it stay");
        overlay[13] = 0x80;
        own[20] = 0x11;
        Check(CopyMasked(own.data(), overlay.data(), bits) == 1 && own[13] == 0x80 && own[20] == 0x11, "a guest's own step goes back to its save, nothing else does");
        Check(CopyMasked(own.data(), overlay.data(), bits) == 0, "nothing new, nothing written");
        overlay[13] = 0;
        CopyMasked(own.data(), overlay.data(), bits);
        Check(own[13] == 0, "a quest step that clears a flag goes back too");
        Group shared{};
        shared.fill(0xff);
        Remove(shared, bits);
        Check(shared[12] == 0 && shared[14] == 0x0f && shared[11] == 0xff, "the host's copy leaves a guest's own bits out");
        Group both = bits;
        Group more{};
        more[0] = 0x01;
        Add(both, more);
        Check(both[0] == 0x01 && both[12] == 0xff, "own bits add up");
        Check(IsPickupGroup(52400) && IsPickupGroup(50000) && !IsPickupGroup(12400), "pickup flags are type 5");
        Check(IsNpcGroup(1) && IsNpcGroup(72400) && IsNpcGroup(73600) && !IsNpcGroup(0) && !IsNpcGroup(2) && !IsNpcGroup(12400) && !IsNpcGroup(52400),
              "NPC state 1000-1999 and the type 7 talk blocks are a hunter's own");
    }

    Group own{}, host{}, full{};
    full.fill(0xff);
    own[0] = 0x0f;
    host[0] = 0xf0;
    Check(WriteShared(own.data(), host, full) == 1 && own[0] == 0xf0, "a whole write takes the host's bits");
    Group partial{};
    partial[0] = 0xf0;
    Group target{};
    target[0] = 0x0f;
    WriteShared(target.data(), host, partial);
    Check(target[0] == 0xff, "bits out of the mask stay");

    Group guest{}, was{}, now{};
    guest[1] = 0x01;
    now[1] = 0x80;
    Check(WriteChanged(guest.data(), was, now, full) == 1 && guest[1] == 0x81, "a live change keeps the guest's own bits");
    was = now;
    now[1] = 0;
    WriteChanged(guest.data(), was, now, full);
    Check(guest[1] == 0x01, "a live clear only clears what the host cleared");

    Image groups;
    for (std::uint32_t i = 0; i < 130; ++i) {
        Group g{};
        g[0] = std::uint8_t(i + 1);
        groups[12400 + 0] = g;
        groups[52400 + i] = g;
    }
    const auto frames = Encode(groups, true);
    Check(frames.size() == 3, "130 groups and one more take 3 frames");
    Check(frames.front()[0] == kFirstPart && frames.back()[0] == kLastPart, "the whole image is marked");
    Receiver in;
    Image changed;
    for (const auto& f : frames) Check(in.Take(f.data(), f.size(), changed), "a frame reads");
    Check(in.Have() && in.Current().size() == groups.size() && changed.size() == groups.size(), "the whole image arrives");

    Image one;
    Group g{};
    g[3] = 9;
    one[12400] = g;
    const auto delta = Encode(one, false);
    Check(delta.size() == 1 && delta[0][0] == 0, "a delta is one plain frame");
    changed.clear();
    in.Take(delta[0].data(), delta[0].size(), changed);
    Check(changed.size() == 1 && in.Value(12400)[3] == 9, "a delta changes one group");
    changed.clear();
    in.Take(delta[0].data(), delta[0].size(), changed);
    Check(changed.empty(), "the same delta changes nothing");

    Image smaller;
    smaller[12400] = g;
    const auto again = Encode(smaller, true);
    changed.clear();
    in.Take(again[0].data(), again[0].size(), changed);
    Check(in.Current().size() == 1, "a newer whole image drops what it lacks");
    Check(changed.size() == groups.size() - 1 && IsZero(changed.begin()->second), "what it lacks reads as cleared");

    const Image none;
    const auto empty = Encode(none, true);
    Check(empty.size() == 1 && empty[0][0] == (kFirstPart | kLastPart), "an empty world is still a whole image");
    changed.clear();
    in.Take(empty[0].data(), empty[0].size(), changed);
    Check(in.Current().empty(), "an empty image clears");

    const std::uint8_t cut[3] = {0, 1, 2};
    Check(!in.Take(cut, sizeof cut, changed), "a cut frame is refused");
    Image bad;
    std::vector<std::uint8_t> sneaky(1 + kEntryBytes);
    const std::uint32_t personal = 6;
    std::memcpy(sneaky.data() + 1, &personal, 4);
    changed.clear();
    in.Take(sneaky.data(), sneaky.size(), changed);
    Check(changed.empty(), "a group that isn't shared is ignored");

    Check(IsGiftFlag(9040) && IsGiftFlag(9045) && IsGiftFlag(9077) && !IsGiftFlag(9039) && !IsGiftFlag(9080), "the gift request flags of common events 9040 and 9100");
    Check(ShareFlag(9045), "a gift flag sits in a shared block, so it has to be taken out as a hunter's own");
    {
        const Group gifts = RangeBits(9, kGiftFirst, kGiftLast);
        Check(CountBits(gifts) == 40 && (gifts[5] & 0x80) != 0 && (gifts[4] & 0x01) == 0, "40 gift bits from 9040");
    }
    {
        Group was{}, now{}, mask{};
        mask[0] = 0xf0;
        was[0] = 0x80;
        now[0] = 0xc8;
        const Group on = TurnedOn(was, now, mask);
        Check(on[0] == 0x40 && CountBits(on) == 1, "only a bit that was off, is on and is watched counts as turned on");
        std::uint8_t save[kGroupBytes] = {};
        Check(SetBits(save, on) == 1 && save[0] == 0x40 && SetBits(save, on) == 0, "a witnessed flag is written once");
    }
    {
        Witness witness;
        std::map<std::uint32_t, Group> masks;
        masks[12400].fill(0xff);
        Image first;
        first[12400][0] = 0x80;
        Check(!witness.Ready(), "a witness starts with no baseline");
        witness.Start(first);
        bool rebuilt = true;
        Check(witness.Step(first, masks, rebuilt).empty() && !rebuilt, "what was on at the baseline is not witnessed");
        Image door = first;
        door[12400][1] = 0x01;
        const Image fresh = witness.Step(door, masks, rebuilt);
        Check(fresh.size() == 1 && fresh.at(12400)[1] == 0x01 && fresh.at(12400)[0] == 0 && !rebuilt, "a door opened with the hunter there is witnessed");
        Check(witness.Step(door, masks, rebuilt).empty(), "and only once");
        Image image = door;
        image[12400].fill(0xff);
        Check(witness.Step(image, masks, rebuilt).empty() && rebuilt, "many flags at once are an image arriving, not play");
        Image other = image;
        other[52400][0] = 0x80;
        Check(witness.Step(other, masks, rebuilt).empty() && !rebuilt, "a group that is not watched is never witnessed");
        witness.Forget();
        Check(!witness.Ready(), "a new copy of the world needs a new baseline");
    }

    OwnerCases();
    StateBlocks();
    InsightFlags();
    return failures;
}

int main(int argc, char** argv) {
#ifdef LANTERN_WORLD_COPY_MUTANTS
    if (argc > 1 && !std::strcmp(argv[1], "--mutants")) {
        quiet = true;
        if (RunAll()) {
            std::printf("world-copy mutants: the unmutated run failed\n");
            return 1;
        }
        int survivors = 0;
        for (int m = 1; m < int(Mutant::kCount); ++m) {
            g_mutant = m;
            const int found = RunAll();
            std::printf("world-copy mutants: mutant %d %s (%d checks)\n", m, found ? "caught" : "survived", found);
            survivors += found ? 0 : 1;
        }
        g_mutant = 0;
        if (survivors) return 1;
        std::printf("world-copy mutants: every mutant caught (%d)\n", int(Mutant::kCount) - 1);
        return 0;
    }
    std::printf("world-copy-test: the mutant build runs with --mutants\n");
    return 1;
#else
    (void)argc;
    (void)argv;
    if (RunAll()) return 1;
    std::printf("world-copy-test: ok\n");
    return 0;
#endif
}
