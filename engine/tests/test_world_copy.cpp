// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/world_copy.h"
#include <cstdio>
#include <map>

namespace {
int failures = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}
}

int main() {
    using namespace BbEngine::WorldCopy;

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

    if (failures) return 1;
    std::printf("world-copy-test: ok\n");
    return 0;
}
