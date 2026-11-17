// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <vector>

#include "gpu/shadps4/video_core/renderer_vulkan/rt_scene_rules.h"

namespace {
namespace R = Vulkan::RtRules;
int g_failed = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
#ifndef LANTERN_RT_RULES_MUTANTS
        std::printf("FAILED: %s\n", what);
#else
        (void)what;
#endif
    }
}

void TestCameraSlots() {
    std::mt19937 rng(7);
    std::vector<int> storage(64);
    for (int round = 0; round < 50; ++round) {
        R::CameraSlots map;
        std::vector<const void*> linear;
        bool same = true;
        const int distinct = 1 + round % 40;
        for (int i = 0; i < 400; ++i) {
            const void* camera = &storage[rng() % distinct];
            same = same && map.Index(camera) == R::LinearCameraIndex(linear, camera);
        }
        Expect(same, "the camera map gives the linear search's index for every draw");
        Expect(map.Size() == linear.size(), "the camera map holds as many cameras as the list");
        map.Clear();
        Expect(map.Size() == 0 && map.Index(&storage[3]) == 0, "a cleared map starts again at 0");
    }
    R::CameraSlots map;
    Expect(map.Index(&storage[0]) == 0 && map.Index(&storage[1]) == 1 && map.Index(&storage[0]) == 0 &&
               map.Index(&storage[2]) == 2 && map.Index(&storage[1]) == 1,
           "an older camera keeps its first index");
}

void TestBudget() {
    R::NewEntryBudget off;
    Expect(!off.On() && off.Allow(), "a budget without limits allows everything");
    for (int i = 0; i < 1000; ++i) {
        off.Spend(1'000'000);
    }
    Expect(off.Allow(), "a budget without limits never runs out");

    R::NewEntryBudget count{.max_entries = 3};
    int made = 0;
    while (count.Allow() && made < 100) {
        count.Spend(10);
        ++made;
    }
    Expect(made == 3, "a count budget allows exactly its count a frame");
    count.Reset();
    Expect(count.Allow() && count.made == 0, "a new frame refills the count");

    R::NewEntryBudget time{.max_ns = 1000};
    Expect(time.Allow(), "a time budget allows the first entry");
    time.Spend(600);
    Expect(time.Allow(), "a time budget allows entries while time is left");
    time.Spend(600);
    Expect(!time.Allow() && !time.TimeLeft() && time.CountLeft(), "a time budget stops once spent");
    time.Reset();
    Expect(time.Allow() && time.spent_ns == 0, "a new frame refills the time");

    R::NewEntryBudget both{.max_entries = 10, .max_ns = 100};
    both.Spend(150);
    Expect(!both.Allow(), "the time limit holds even with count left");
}

void TestTiers() {
    Expect(R::PickTier(true, 400.0f) == R::Tier::Character && R::PickTier(true, -1.0f) == R::Tier::Character,
           "a character is first wherever it is");
    Expect(R::PickTier(false, 3.0f) == R::Tier::Near && R::PickTier(false, 80.0f) == R::Tier::Far &&
               R::PickTier(false, -1.0f) == R::Tier::Far,
           "near meshes come before far ones and unknown distance is far");
    const float eye[3] = {1, 2, 3}, at[3] = {4, 6, 3};
    Expect(std::abs(R::Distance(eye, at) - 5.0f) < 1e-5f, "distance is from the eye");

    R::NewEntryBudget budget{.max_entries = 8, .max_ns = 1000};
    int far = 0, near = 0, character = 0;
    for (int i = 0; i < 20; ++i) {
        if (budget.AllowTier(R::Tier::Far)) {
            budget.Spend(10);
            ++far;
        }
    }
    for (int i = 0; i < 20; ++i) {
        if (budget.AllowTier(R::Tier::Near)) {
            budget.Spend(10);
            ++near;
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (budget.AllowTier(R::Tier::Character)) {
            budget.Spend(10);
            ++character;
        }
    }
    Expect(far == 4, "far meshes get only the first half of the count");
    Expect(near == 4, "near meshes still get the half far ones left");
    Expect(character == 5, "characters never wait");
    R::NewEntryBudget time{.max_ns = 1000};
    time.Spend(600);
    Expect(!time.AllowTier(R::Tier::Far) && time.AllowTier(R::Tier::Near),
           "past half the time far meshes wait, near ones go on");
}

void TestBounds() {
    Expect(R::TriangleBound(300, 1, false) == 100 && R::TriangleBound(300, 4, false) == 400,
           "a list bounds a triangle per three indices and instance");
    Expect(R::TriangleBound(10, 2, true) == 16 && R::TriangleBound(2, 1, true) == 0,
           "a strip bounds index count minus two");
    Expect(R::FitsScene(100, 200, 100) && !R::FitsScene(100, 200, 101), "the scene limit is inclusive");
}

void TestRetry() {
    R::RetryGate gate;
    Expect(!gate.Blocked(5, 10), "an unknown key is not blocked");
    gate.Fail(5, 10, 30);
    Expect(gate.Blocked(5, 10) && gate.Blocked(5, 39) && !gate.Blocked(5, 40),
           "a failed key waits its frames, then may try again");
    Expect(!gate.Blocked(6, 11), "other keys are not blocked");
    gate.Prune(39);
    Expect(gate.Size() == 1, "prune keeps keys still waiting");
    gate.Prune(40);
    Expect(gate.Size() == 0, "prune drops keys whose wait is over");
}

void TestEvict() {
    const std::uint64_t limit = 800;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> aged;
    for (std::uint64_t i = 0; i < 100; ++i) {
        aged.emplace_back(900 + i, 8);
    }
    R::EvictLimits old_rule{};
    const auto below = R::EvictBefore(aged, 700, limit, 1000, old_rule);
    Expect(below.keep_from == 1000 - 240, "under 7/8 only unused entries go");
    const auto old_over = R::EvictBefore(aged, 750, limit, 1000, old_rule);
    Expect(old_over.keep_from == 900 + 19, "over 7/8 the old rule frees the oldest down to 3/4");
    R::EvictLimits hold{.hold = true};
    const auto held = R::EvictBefore(aged, 750, limit, 1000, hold);
    Expect(held.keep_from == 910, "the hold spares entries seen in its window");
    Expect(held.spared > 0, "the hold counts what it spared");
    const auto hard = R::EvictBefore(aged, 820, limit, 1000, hold);
    Expect(hard.keep_from == 900 + 40,"over the limit the hold frees down to 5/8 past its window");
    std::vector<std::pair<std::uint64_t, std::uint64_t>> recent;
    for (std::uint64_t i = 0; i < 100; ++i) {
        recent.emplace_back(999, 8);
    }
    Expect(R::EvictBefore(recent, 820, limit, 1000, hold).keep_from == 1000 - 240,
           "entries seen in the last frames are never freed");
}

void TestKeepPrevious() {
    Expect(R::KeepInScene(true, true, 0) && R::KeepInScene(true, true, R::KeepFrames),
           "a built mesh over budget stays in the scene for a few frames");
    Expect(!R::KeepInScene(true, true, R::KeepFrames + 1),
           "a mesh whose data is older than the limit leaves the scene");
    Expect(!R::KeepInScene(true, false, 0), "a mesh never built is never put in the scene");
    Expect(!R::KeepInScene(false, true, 0), "with the switch off an over-budget mesh leaves the scene");
    Expect(R::RefitInstead(true, true, true), "a built mesh whose rebuild hits the cap is refit");
    Expect(!R::RefitInstead(true, false, true), "a mesh never built cannot be refit");
    Expect(!R::RefitInstead(true, true, false) && !R::RefitInstead(false, true, true),
           "no refit without the cap or the switch");
    Expect(R::BuiltAfterUnrecorded(true, true), "an unrecorded refit keeps the old BLAS valid");
    Expect(!R::BuiltAfterUnrecorded(true, false), "an unrecorded rebuild leaves the mesh unbuilt");
    Expect(!R::BuiltAfterUnrecorded(false, true), "an unbuilt mesh stays unbuilt");

    struct Mesh {
        bool built = false;
    } mesh;
    int kept = 0, unsafe = 0;
    const bool over[] = {true, false, true, true, false, true};
    const bool recorded[] = {true, false, true, true, true, true};
    for (int f = 0; f < 6; ++f) {
        if (over[f]) {
            if (R::KeepInScene(true, mesh.built, 0)) {
                ++kept;
                unsafe += mesh.built ? 0 : 1;
            }
            continue;
        }
        const bool update = mesh.built;
        if (recorded[f]) {
            mesh.built = true;
        } else {
            mesh.built = R::BuiltAfterUnrecorded(mesh.built, update);
        }
    }
    Expect(unsafe == 0, "the scene never holds a BLAS that was not built");
    Expect(kept == 1, "over-budget frames keep the mesh only once it was built");
}

std::uint64_t PaletteHash(float lift, float turn) {
    const float bone[12] = {std::cos(turn), 0, std::sin(turn), 120.0f, 0, 1, 0, 4.0f + lift,
                            -std::sin(turn), 0, std::cos(turn), -80.0f};
    std::uint64_t h = 1469598103934665603ull;
    unsigned char bytes[sizeof(bone)];
    std::memcpy(bytes, bone, sizeof(bone));
    for (const unsigned char b : bytes) {
        h = (h ^ b) * 1099511628211ull;
    }
    return h;
}

struct RigidMesh {
    R::RigidBones rigid;
    std::uint64_t seen = ~0ull;
    bool built = false;
    bool pending = false;
    bool dynamic = false;
    int refits = 0;
    int moved = 0;

    void Draw(std::uint64_t frame, bool camera_pass, const R::Bones& now, bool room = true) {
        if (built && !dynamic) {
            const R::Rigid result = rigid.Check(seen != frame, camera_pass, now);
            moved += result == R::Rigid::Moved ? 1 : 0;
            pending = pending || result == R::Rigid::Moved;
            dynamic = result == R::Rigid::Written;
        }
        if (seen == frame) {
            return;
        }
        seen = frame;
        if ((!built || dynamic || pending) && room) {
            rigid.Captured(camera_pass, now);
            refits += built && pending ? 1 : 0;
            pending = false;
            built = true;
        }
    }
};

R::Bones Palette(std::uint64_t hash) {
    return {hash, true, false, false, 48};
}

void TestPalette() {
    Expect(R::PaletteBuffer(48, false) && R::PaletteBuffer(96, false) && R::PaletteBuffer(65536, false),
           "one bone, a small skeleton and a pool over 16 KiB read per vertex are palettes");
    Expect(!R::PaletteBuffer(416, true), "the model constants, read as constants, are no palette");
    Expect(!R::PaletteBuffer(864, false), "the scene constants are no palette");
    Expect(!R::PaletteBuffer(32, false), "less than a bone is no palette");
    Expect(R::PaletteHashed(R::PaletteHashLimit) && !R::PaletteHashed(R::PaletteHashLimit + 16),
           "palettes up to 16 KiB are hashed, larger pools are not");
}

void TestRigid() {
    RigidMesh door;
    std::uint64_t frame = 0;
    for (; frame < 30; ++frame) {
        door.Draw(frame, false, Palette(PaletteHash(0, 0)));
        door.Draw(frame, false, Palette(PaletteHash(0, 0)));
        door.Draw(frame, true, Palette(PaletteHash(0, 0)));
    }
    Expect(door.refits == 0 && door.moved == 0,
           "a still mesh drawn in both passes stays still: its palette holds no camera");
    for (int f = 1; f <= 10; ++f, ++frame) {
        door.Draw(frame, false, Palette(PaletteHash(0, 0.05f * float(f))));
        door.Draw(frame, true, Palette(PaletteHash(0, 0.05f * float(f))));
    }
    Expect(door.refits == 10, "a turning door is captured again once on every frame it turns");
    for (int f = 0; f < 10; ++f, ++frame) {
        door.Draw(frame, false, Palette(PaletteHash(0, 0.5f)));
        door.Draw(frame, true, Palette(PaletteHash(0, 0.5f)));
    }
    Expect(door.refits == 10, "a door that stopped is not captured again");

    RigidMesh mixed;
    for (std::uint64_t f = 0; f < 30; ++f) {
        mixed.Draw(f, false, Palette(PaletteHash(0, 0) ^ 0x51));
        mixed.Draw(f, false, Palette(PaletteHash(0, 0) ^ 0x77));
        mixed.Draw(f, true, Palette(PaletteHash(0, 0)));
    }
    Expect(mixed.refits == 0 && mixed.moved == 0,
           "draws of one mesh that read other palette sets never flag it: only the capturing draw compares");

    RigidMesh swapped;
    for (std::uint64_t f = 0; f < 30; ++f) {
        const bool camera_first = f >= 10;
        swapped.Draw(f, camera_first, Palette(camera_first ? 9 : 5));
        swapped.Draw(f, !camera_first, Palette(camera_first ? 5 : 9));
    }
    Expect(swapped.refits == 1, "a pass that captures for the first time refits once, then keeps its own palette");

    RigidMesh returned;
    returned.Draw(0, false, Palette(PaletteHash(0, 0)));
    returned.Draw(1, true, Palette(PaletteHash(0.25f, 0)));
    returned.Draw(1, false, Palette(PaletteHash(0.25f, 0)));
    returned.Draw(2, false, Palette(PaletteHash(0, 0)));
    returned.Draw(2, true, Palette(PaletteHash(0, 0)));
    Expect(returned.refits == 2,
           "a mesh that moves while the other pass captures and then moves back is captured again");

    RigidMesh pooled;
    for (std::uint64_t f = 0; f < 30; ++f) {
        pooled.Draw(f, false, {PaletteHash(0.01f * float(f), 0), true, false, true, 0});
    }
    Expect(pooled.refits == 0 && pooled.moved == 0,
           "a mesh on a shared pool stays put while an unrelated bone in the pool changes");

    RigidMesh lift;
    lift.Draw(0, false, Palette(PaletteHash(0, 0)));
    lift.Draw(1, false, Palette(PaletteHash(0.25f, 0)), false);
    Expect(lift.pending, "a lift that moved on a frame without room stays pending");
    lift.Draw(2, false, Palette(PaletteHash(0.25f, 0)));
    Expect(lift.refits == 1, "and is compared with the palette it was captured with, not last frame's");
    lift.Draw(3, false, Palette(PaletteHash(0.25f, 0)));
    Expect(lift.refits == 1 && !lift.pending, "once captured it is still again");

    RigidMesh written;
    written.Draw(0, false, Palette(PaletteHash(0, 0)));
    written.Draw(1, false, {0, true, true, false, 0});
    Expect(written.dynamic, "a GPU-written palette makes the mesh dynamic");

    R::RigidBones kept;
    kept.Captured(false, Palette(7));
    kept.Captured(false, {});
    kept.Captured(false, {3, true, false, true, 0});
    kept.Captured(false, {4, true, true, false, 0});
    Expect(kept.Check(true, false, Palette(7)) == R::Rigid::Still,
           "a capture without a palette, on a pool or GPU-written keeps the captured palette");
    Expect(kept.Check(true, false, {}) == R::Rigid::Unknown, "a draw without a palette is unknown, never moved");
    Expect(kept.Check(true, false, Palette(8)) == R::Rigid::Moved, "moved exactly when the palette differs");
}

struct MovingMesh {
    R::CapturedPose pose;
    std::uint8_t mask = 0;
    bool built = false;
    int refits = 0;

    void Draw(std::uint64_t frame, std::uint64_t key, bool camera_pass, std::uint8_t now_mask, R::Pose now,
              bool on = true) {
        const std::uint8_t last = mask;
        mask = now_mask;
        bool capture = !built;
        if (built) {
            capture = R::DynamicRefit(on, pose.Changed(camera_pass, now),
                                      R::CutOnly(last, 12) && R::CutOnly(now_mask, 12), frame, key) ==
                      R::Refit::Now;
        }
        if (capture) {
            built = true;
            ++refits;
            pose.Captured(camera_pass, now);
        }
    }
};

R::Pose Known(std::uint64_t hash) {
    return {hash, true};
}

void TestMovingRefit() {
    MovingMesh still;
    for (std::uint64_t f = 0; f < 40; ++f) still.Draw(f, 3, true, 2, Known(77));
    Expect(still.refits == 1, "a skinned mesh whose bone data never changes is built once and never refit");

    MovingMesh character;
    for (std::uint64_t f = 0; f < 40; ++f) character.Draw(f, 3, true, 2, Known(f));
    Expect(character.refits == 40, "a character whose bones change is refit every frame");

    MovingMesh foliage;
    for (std::uint64_t f = 0; f < 40; ++f) foliage.Draw(f, 1, true, 8, Known(f));
    Expect(foliage.refits == 1 + 10, "an animated cut-out seen only by bounce rays refits once in four frames");

    MovingMesh spread;
    int first = -1;
    for (std::uint64_t f = 0; f < 8; ++f) {
        const int before = spread.refits;
        spread.Draw(f, 2, true, 4, Known(f));
        if (f > 0 && spread.refits != before && first < 0) first = int(f);
    }
    Expect(first == 2, "the key spreads which frame a waiting cut-out refits on");

    MovingMesh lit;
    for (std::uint64_t f = 0; f < 5; ++f) lit.Draw(f, 1, true, 8, Known(f));
    const int waited = lit.refits;
    lit.Draw(5, 1, true, 2, Known(5));
    Expect(lit.refits == waited + 1, "a cut-out that sun or lamp rays can hit again refits at once");

    MovingMesh mixed;
    for (std::uint64_t f = 0; f < 8; ++f) mixed.Draw(f, 1, true, 8 | 1, Known(f));
    Expect(mixed.refits == 8, "a mesh also in a pass that sun or lamp rays hit refits every frame");

    MovingMesh unknown;
    for (std::uint64_t f = 0; f < 6; ++f) unknown.Draw(f, 3, true, 2, R::Pose{5, false});
    Expect(unknown.refits == 6, "bone data that could not be hashed counts as changed");

    MovingMesh off;
    for (std::uint64_t f = 0; f < 6; ++f) off.Draw(f, 1, true, 8, Known(9), false);
    Expect(off.refits == 6, "with the switch off every moving mesh refits every frame as before");

    R::CapturedPose passes;
    passes.Captured(false, Known(4));
    Expect(passes.Changed(true, Known(4)), "a capture in the shadow pass says nothing about the camera pass");
    Expect(!passes.Changed(false, Known(4)), "the same pass with the same data is unchanged");
    passes.Captured(true, Known(6));
    Expect(passes.Changed(false, Known(4)), "a capture in one pass makes the other pass refit before it is trusted again");
    passes.Captured(false, Known(4));
    passes.Captured(true, R::Pose{4, false});
    Expect(passes.Changed(true, Known(4)), "a capture whose data was not hashed leaves the pass unknown");

    Expect(R::CutOnly(4, 12) && R::CutOnly(8, 12) && R::CutOnly(12, 12), "cut-out masks alone are cut-only");
    Expect(!R::CutOnly(5, 12) && !R::CutOnly(2, 12) && !R::CutOnly(0, 12),
           "a mask with any other ray or no mask at all is not cut-only");
}

void TestHandOffOrder() {
    R::HandOff<int> ready;
    const float distances[] = {30.0f, -1.0f, 5.0f, 100.0f, 12.0f};
    for (int i = 0; i < 5; ++i) {
        ready.Add({.key = std::uint64_t(i), .distance = distances[i], .triangles = 10}, i);
    }
    ready.Add({.key = 9, .distance = 200.0f, .unbounded = true, .triangles = 10}, 9);
    std::vector<int> order;
    ready.Take({.entries = 10, .triangles = 1000}, [&](const R::ReadyMesh&, int v) { order.push_back(v); });
    Expect(order == std::vector<int>{9, 2, 4, 0, 3, 1},
           "the hand-off gives characters first, then nearest first, unknown distances last");
    Expect(ready.Size() == 0, "everything within the limits leaves the queue");
}

void TestHandOffBounds() {
    R::HandOff<int> ready;
    for (int i = 0; i < 10; ++i) {
        ready.Add({.key = std::uint64_t(i), .distance = float(i), .triangles = 100}, i);
    }
    std::vector<int> got;
    const auto take = [&](R::HandOffLimits limits) {
        got.clear();
        return ready.Take(limits, [&](const R::ReadyMesh&, int v) { got.push_back(v); });
    };
    Expect(take({.entries = 3, .triangles = 100000}) == 3 && got == std::vector<int>{0, 1, 2},
           "a frame takes at most its entry count, nearest first");
    Expect(ready.Size() == 7, "the rest wait in the queue, none lost");
    Expect(take({.entries = 10, .triangles = 250}) == 2 && got == std::vector<int>{3, 4},
           "a frame takes at most its triangle bound");
    Expect(take({.entries = 3, .triangles = 100000}) == 3 && got == std::vector<int>{5, 6, 7},
           "the next frame goes on where the last stopped");

    R::HandOff<int> skipped;
    skipped.Add({.key = 1, .distance = 1.0f, .triangles = 100}, 1);
    skipped.Add({.key = 2, .distance = 2.0f, .triangles = 300}, 2);
    skipped.Add({.key = 3, .distance = 3.0f, .triangles = 50}, 3);
    skipped.Add({.key = 4, .distance = 4.0f, .triangles = 50}, 4);
    std::vector<int> skipped_got;
    skipped.Take({.entries = 10, .triangles = 200}, [&](const R::ReadyMesh&, int v) { skipped_got.push_back(v); });
    Expect(skipped_got == std::vector<int>{1, 3, 4}, "a mesh over the frame's triangles left lets smaller ones by");
    skipped_got.clear();
    skipped.Take({.entries = 1, .triangles = 200}, [&](const R::ReadyMesh&, int v) { skipped_got.push_back(v); });
    Expect(skipped_got == std::vector<int>{2} && skipped.Size() == 0, "the skipped mesh lands first the next frame");

    R::HandOff<int> unknown;
    unknown.Add({.key = 1, .distance = -1.0f}, 1);
    unknown.Add({.key = 2, .distance = std::nanf("")}, 2);
    unknown.Add({.key = 3, .distance = -1.0f}, 3);
    unknown.Add({.key = 4, .distance = 3.0f}, 4);
    std::vector<int> unknown_got;
    unknown.Take({.entries = 10, .triangles = 100}, [&](const R::ReadyMesh&, int v) { unknown_got.push_back(v); });
    Expect(unknown_got == std::vector<int>{4, 1, 2, 3}, "unknown and NaN distances keep their order after the known");

    R::HandOff<int> big;
    big.Add({.key = 1, .distance = 1.0f, .triangles = 5000}, 1);
    big.Add({.key = 2, .distance = 2.0f, .triangles = 10}, 2);
    std::vector<int> big_got;
    big.Take({.entries = 4, .triangles = 100}, [&](const R::ReadyMesh&, int v) { big_got.push_back(v); });
    Expect(big_got == std::vector<int>{1}, "a mesh over the triangle bound alone still lands first, never starves");

    R::HandOff<int> mixed;
    for (int i = 0; i < 3; ++i) {
        mixed.Add({.key = std::uint64_t(i), .distance = 50.0f, .unbounded = true, .triangles = 1000}, i);
    }
    mixed.Add({.key = 7, .distance = 1.0f, .triangles = 10}, 7);
    mixed.Add({.key = 8, .distance = 2.0f, .triangles = 10}, 8);
    std::vector<int> mixed_got;
    const auto taken =
        mixed.Take({.entries = 1, .triangles = 100}, [&](const R::ReadyMesh&, int v) { mixed_got.push_back(v); });
    Expect(taken == 1 && mixed_got.size() == 4 && mixed_got.back() == 7 && mixed.Size() == 1,
           "characters and failures land outside the bounds and never wait");

    R::HandOff<int> drained;
    for (int i = 0; i < 6; ++i) {
        drained.Add({.key = std::uint64_t(i)}, i);
    }
    int retired = 0;
    drained.Drain([&](const R::ReadyMesh&, int) { ++retired; });
    Expect(retired == 6 && drained.Size() == 0, "a drain retires every waiting result");
}

void TestSetupKeys() {
    R::SetupKeys keys;
    Expect(!keys.Pending(5), "a new key is not pending");
    keys.Submitted(5, 640);
    Expect(keys.Pending(5) && keys.Size() == 1, "a submitted key is pending until it lands");
    keys.Submitted(6, 100);
    Expect(keys.Bytes() == 740, "pending meshes hold their scene bytes");
    keys.Landed(5);
    Expect(!keys.Pending(5) && keys.Size() == 1 && keys.Bytes() == 100,
           "a landed key can be sent again later and gives its bytes back");
    keys.Landed(5);
    keys.Landed(6);
    Expect(keys.Size() == 0 && keys.Bytes() == 0, "every landing gives back exactly what it held");

    Expect(R::FitsWithPending(100, 0, 1000, 900) && !R::FitsWithPending(100, 200, 1000, 900),
           "meshes in flight count against the scene limit");
    R::SetupKeys flood;
    std::uint64_t scene = 0;
    int sent = 0;
    for (int i = 0; i < 1000; ++i) {
        if (R::FitsWithPending(scene, flood.Bytes(), 320, 1)) {
            flood.Submitted(std::uint64_t(i), 1);
            ++sent;
        }
    }
    Expect(sent == 320 && flood.Bytes() == 320, "a flood of new meshes stops at the scene limit in flight");
}

void TestCopyBudget() {
    R::CopyBudget budget{.max_bytes = 1000, .max_in_flight = 4};
    Expect(budget.Allow(R::Tier::Near, 5000, 0) && budget.Allow(R::Tier::Far, 5000, 0),
           "the first copy of a frame always goes, so a big mesh never starves");
    Expect(!budget.Allow(R::Tier::Near, 10, 4), "nothing is sent with the worker full");
    budget.Spend(400);
    Expect(budget.Allow(R::Tier::Near, 200, 1), "a near mesh uses the whole copy budget");
    Expect(!budget.Allow(R::Tier::Near, 700, 1), "a near mesh stops at the copy budget");
    Expect(budget.Allow(R::Tier::Far, 50, 1) && !budget.Allow(R::Tier::Far, 200, 1),
           "a far mesh uses only the first half of the copy budget");
    Expect(budget.Allow(R::Tier::Character, 100000, 1), "a character never waits for the copy budget");
    Expect(budget.Allow(R::Tier::Character, 100000, 4), "a character never waits for a full worker");
    budget.Reset();
    Expect(budget.used == 0 && budget.Allow(R::Tier::Far, 400, 3), "a new frame refills the copy budget");
    R::CopyBudget defaults;
    Expect(defaults.max_bytes > 0 && defaults.max_in_flight > 0 && defaults.Allow(R::Tier::Far, 64, 0),
           "a default copy budget sends work");
}

void TestLand() {
    int inserts = 0;
    for (int bits = 0; bits < 8; ++bits) {
        const bool lost = bits & 1, taken = bits & 2, fits = bits & 4;
        const bool insert = R::Land(lost, taken, fits) == R::Landing::Insert;
        inserts += insert ? 1 : 0;
        Expect(insert == (!lost && !taken && fits),
               "a result lands only with the device alive, its key free and room in the scene");
    }
    Expect(inserts == 1, "exactly one case lands");
}

void TestSetupFlow() {
    struct Token {
        int id = 0;
    };
    std::mt19937 rng(11);
    std::map<int, int> state;
    std::map<std::uint64_t, int> scene;
    std::vector<std::pair<R::ReadyMesh, Token>> worker;
    R::HandOff<Token> ready;
    R::SetupKeys keys;
    std::map<std::uint64_t, int> in_flight;
    int next = 1;
    bool used_dead = false, twice = false, duplicate = false;
    const auto retire = [&](int id) {
        twice = twice || state[id] != 1;
        state[id] = 2;
    };
    for (int frame = 0; frame < 400; ++frame) {
        for (int d = 0; d < 20; ++d) {
            const std::uint64_t key = rng() % 60;
            if (scene.contains(key)) {
                used_dead = used_dead || state[scene[key]] != 3;
                continue;
            }
            if (keys.Pending(key)) {
                continue;
            }
            keys.Submitted(key, 64);
            duplicate = duplicate || in_flight[key]++ > 0;
            const int id = next++;
            state[id] = 1;
            worker.push_back({{.key = key, .distance = float(rng() % 100), .triangles = std::uint32_t(rng() % 300)},
                              {id}});
        }
        std::shuffle(worker.begin(), worker.end(), rng);
        const std::size_t finish = worker.size() / 2 + 1;
        for (std::size_t i = 0; i < finish && !worker.empty(); ++i) {
            ready.Add(worker.back().first, worker.back().second);
            worker.pop_back();
        }
        ready.Take({.entries = 8, .triangles = 1000}, [&](const R::ReadyMesh& mesh, Token token) {
            keys.Landed(mesh.key);
            --in_flight[mesh.key];
            const bool fits = rng() % 10 != 0;
            if (R::Land(false, scene.contains(mesh.key), fits) == R::Landing::Retire) {
                retire(token.id);
                return;
            }
            twice = twice || state[token.id] != 1;
            if (scene.contains(mesh.key)) {
                state[scene[mesh.key]] = 4;
            }
            state[token.id] = 3;
            scene[mesh.key] = token.id;
        });
        if (frame % 7 == 0) {
            std::erase_if(scene, [&](const auto& item) {
                if (rng() % 3 != 0) {
                    return false;
                }
                state[item.second] = 2;
                return true;
            });
        }
    }
    for (auto& item : worker) {
        ready.Add(item.first, item.second);
    }
    worker.clear();
    for (int frame = 0; frame < 1000 && ready.Size(); ++frame) {
        ready.Take({.entries = 8, .triangles = 1000}, [&](const R::ReadyMesh& mesh, Token token) {
            keys.Landed(mesh.key);
            if (R::Land(true, false, true) == R::Landing::Retire) {
                retire(token.id);
            }
        });
    }
    bool open = false, leaked = false;
    for (const auto& [id, s] : state) {
        open = open || s == 1;
        leaked = leaked || s == 4;
    }
    Expect(!duplicate, "a pending key is never sent to the worker twice");
    Expect(!used_dead, "a mesh in the scene is never a retired result");
    Expect(!twice, "each result lands or retires exactly once");
    Expect(!leaked, "a landing never replaces a mesh already in the scene");
    Expect(!open && keys.Size() == 0 && keys.Bytes() == 0 && ready.Size() == 0,
           "no result is lost: all landed or retired");
}

R::Box MakeBox(float x0, float y0, float z0, float x1, float y1, float z1) {
    return {{x0, y0, z0}, {x1, y1, z1}};
}

void TestBoxBits() {
    const std::vector<float> values{-1e30f, -3.5f, -1e-38f, -0.0f, 0.0f, 1e-38f, 0.25f, 2.5f, 1e30f};
    bool round = true, ordered = true;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float back = R::FromOrderedBits(R::OrderedBits(values[i]));
        round = round && std::memcmp(&back, &values[i], sizeof(float)) == 0;
        if (i > 0 && values[i - 1] < values[i]) {
            ordered = ordered && R::OrderedBits(values[i - 1]) < R::OrderedBits(values[i]);
        }
    }
    Expect(round, "a float survives the ordered encoding bit for bit");
    Expect(ordered, "the ordered encoding keeps the order of floats across the sign");
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> spread(-500.0f, 500.0f);
    std::uint32_t bits[6] = {R::EmptyLow, R::EmptyLow, R::EmptyLow, R::EmptyHigh, R::EmptyHigh, R::EmptyHigh};
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 300; ++i) {
        for (int a = 0; a < 3; ++a) {
            const float v = spread(rng);
            bits[a] = std::min(bits[a], R::OrderedBits(v));
            bits[3 + a] = std::max(bits[3 + a], R::OrderedBits(v));
            lo[a] = std::min(lo[a], v);
            hi[a] = std::max(hi[a], v);
        }
    }
    R::Box box;
    Expect(R::BoxFromBits(bits, box) && box.lo[0] == lo[0] && box.lo[1] == lo[1] && box.lo[2] == lo[2] &&
               box.hi[0] == hi[0] && box.hi[1] == hi[1] && box.hi[2] == hi[2],
           "atomic min and max on the encoding give the exact box of the points");
    const std::uint32_t empty[6] = {R::EmptyLow, R::EmptyLow, R::EmptyLow, R::EmptyHigh, R::EmptyHigh, R::EmptyHigh};
    Expect(!R::BoxFromBits(empty, box), "a slot no shader touched gives no box");
}

void TestFrustum() {
    const std::array<float, 12> view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const std::array<float, 4> proj{1.0f, -1.0f, 1.0f, -0.1f};
    Expect(!R::OutsideFrustum(MakeBox(-0.5f, -0.5f, 5, 0.5f, 0.5f, 6), view, proj), "a box ahead is inside");
    Expect(R::OutsideFrustum(MakeBox(10, -0.5f, 5, 11, 0.5f, 6), view, proj), "a box to the right is outside");
    Expect(R::OutsideFrustum(MakeBox(-11, -0.5f, 5, -10, 0.5f, 6), view, proj), "a box to the left is outside");
    Expect(R::OutsideFrustum(MakeBox(-0.5f, 10, 5, 0.5f, 11, 6), view, proj),
           "a box above is outside with a flipped y scale");
    Expect(R::OutsideFrustum(MakeBox(-20, -20, -6, 20, 20, -5), view, proj), "a wide wall behind is outside");
    Expect(!R::OutsideFrustum(MakeBox(4, -0.5f, 5, 8, 0.5f, 6), view, proj),
           "a box across the edge of the view is not outside");
    const std::array<float, 12> turned{0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0};
    Expect(!R::OutsideFrustum(MakeBox(5, -0.5f, -0.5f, 6, 0.5f, 0.5f), turned, proj) &&
               R::OutsideFrustum(MakeBox(-0.5f, -0.5f, 5, 0.5f, 0.5f, 6), turned, proj),
           "turning the camera brings one box in and puts the other out");
}

void TestOffscreen() {
    const float eye[3] = {0, 0, 0};
    Expect(R::BoxDistance(MakeBox(-1, -1, -1, 1, 1, 1), eye) == 0.0f &&
               R::BoxDistance(MakeBox(3, -1, 4, 5, 1, 6), eye) == 5.0f,
           "the distance is to the nearest point of the box");
    Expect(R::ClassifyUndrawn(true, 10.0f, R::ShadowReachMeters) == R::Undrawn::Add &&
               R::ClassifyUndrawn(true, R::ShadowReachMeters, R::ShadowReachMeters) == R::Undrawn::Add,
           "an undrawn static outside the view within reach casts");
    Expect(R::ClassifyUndrawn(true, 50.0f, R::ShadowReachMeters) == R::Undrawn::Far &&
               R::ClassifyUndrawn(false, 50.0f, R::ShadowReachMeters) == R::Undrawn::Far &&
               R::ClassifyUndrawn(true, std::nanf(""), R::ShadowReachMeters) == R::Undrawn::Far,
           "beyond the reach or without a distance nothing is added");
    Expect(R::ClassifyUndrawn(false, 10.0f, R::ShadowReachMeters) == R::Undrawn::Gone,
           "an undrawn static inside the view is gone, not added");
    Expect(R::Hidden(10, 20) && !R::Hidden(25, 20), "a gone entry stays out until it is drawn again");
    Expect(!R::Hidden(20, 20), "an entry drawn later in the frame it was marked gone is back");
    Expect(!R::Hidden(1, 0), "a new entry is not gone");
    Expect(R::StaticCaster(true, false, false) && !R::StaticCaster(false, false, false) &&
               !R::StaticCaster(true, true, false) && !R::StaticCaster(true, false, true),
           "only built, still, single meshes are static casters");
    std::vector<R::Caster> casters{{0, 30.0f}, {1, 5.0f}, {2, 20.0f}, {3, 10.0f}};
    const std::size_t dropped = R::KeepNearest(casters, 2);
    Expect(dropped == 2 && casters.size() == 2 && casters[0].id == 1 && casters[1].id == 3,
           "the cap keeps the nearest casters");
    std::vector<R::Caster> few{{0, 3.0f}};
    Expect(R::KeepNearest(few, 8) == 0 && few.size() == 1, "under the cap every caster stays");
    Expect(R::SceneUnchanged(10, 10, 6, 6), "the same lists keep the scene");
    Expect(!R::SceneUnchanged(11, 10, 6, 6) && !R::SceneUnchanged(10, 10, 7, 6),
           "a drawn entry trading places with an added one rebuilds the scene");
}

void TestPersistentEvict() {
    const std::uint64_t limit = 800;
    const auto calm = R::EvictBefore({}, 100, limit, 1000, {});
    Expect(!R::Evicted(100, calm, true, true, 1000), "with the switch a static outlives the unused limit");
    Expect(R::Evicted(100, calm, false, true, 1000), "without the switch the same static ages out");
    Expect(R::Evicted(100, calm, true, false, 1000), "a moving entry still ages out with the switch");
    Expect(!R::Evicted(900, calm, false, false, 1000), "a recent entry stays");
    const auto later = R::EvictBefore({}, 100, limit, 5000, {});
    Expect(R::Evicted(2000, later, true, true, 5000) && !R::Evicted(2400, later, true, true, 5000),
           "a static unseen for longer than the long limit goes even with the switch");
    Expect(R::KeepStatics(true, false) && !R::KeepStatics(false, false), "the switch keeps statics");
    Expect(!R::KeepStatics(true, true) && R::Evicted(100, calm, R::KeepStatics(true, true), true, 1000),
           "after a stretch without a camera statics unseen for the unused limit are flushed");
    Expect(R::StaleReturn(1000, 700, 240, 1, 2), "a static back after the unused limit with other indices is rebuilt");
    Expect(!R::StaleReturn(1000, 700, 240, 1, 1), "the same indices keep the static");
    Expect(!R::StaleReturn(1000, 900, 240, 1, 2), "a static seen recently is not checked");
    Expect(R::RecentCaster(1000, 1000 - R::RecentFrames) && !R::RecentCaster(1000, 999 - R::RecentFrames),
           "only statics drawn in the last 600 frames cast from outside the view");
    std::vector<std::pair<std::uint64_t, std::uint64_t>> aged{{100, 8}};
    for (std::uint64_t i = 0; i < 100; ++i) {
        aged.emplace_back(900 + i, 8);
    }
    const auto pressed = R::EvictBefore(aged, 758, limit, 1000, {});
    Expect(R::Evicted(100, pressed, true, true, 1000) && R::Evicted(905, pressed, true, true, 1000),
           "under memory pressure statics go, oldest first");
    Expect(!R::Evicted(990, pressed, true, true, 1000), "and pressure frees only down to its target");
    Expect(pressed.keep_from == pressed.pressure_from, "the old plan is unchanged under pressure");
}

void TestTickQueue() {
    R::TickQueue<int> queue;
    queue.Add(5, 1);
    queue.Add(5, 2);
    queue.Add(7, 3);
    queue.Add(9, 4);
    Expect(queue.TakeDone(4).empty() && queue.Size() == 4, "nothing is freed before the GPU reaches its tick");
    Expect(queue.TakeDone(5) == std::vector<int>{1, 2}, "the GPU at a tick frees what waited for it, in order");
    Expect(queue.Size() == 2 && queue.FirstTick() == 7, "what was freed leaves the queue, the rest waits");
    Expect(queue.TakeDone(100) == std::vector<int>{3, 4} && queue.Empty(), "a GPU far ahead frees the rest");
    Expect(queue.TakeDone(100).empty(), "a free runs once");

    std::mt19937 rng(11);
    R::TickQueue<int> random;
    std::vector<std::uint64_t> ticks;
    std::vector<int> freed;
    std::uint64_t tick = 1, gpu = 0;
    bool early = false;
    for (int step = 0; step < 2000; ++step) {
        if (rng() % 3 != 0) {
            ticks.push_back(tick);
            random.Add(tick, int(ticks.size() - 1));
        }
        tick += std::uint64_t(rng() % 2);
        gpu = std::min(tick, gpu + std::uint64_t(rng() % 3));
        for (int id : random.TakeDone(gpu)) {
            early = early || ticks[std::size_t(id)] > gpu;
            freed.push_back(id);
        }
    }
    for (int id : random.TakeDone(tick)) {
        freed.push_back(id);
    }
    bool ordered = freed.size() == ticks.size();
    for (std::size_t i = 0; ordered && i < freed.size(); ++i) {
        ordered = freed[i] == int(i);
    }
    Expect(!early, "random frees never run before the GPU is done with them");
    Expect(ordered && random.Empty(), "random frees each run once, in the order they were queued");
}

int RunSuite() {
    g_failed = 0;
    TestBoxBits();
    TestFrustum();
    TestOffscreen();
    TestPersistentEvict();
    TestCameraSlots();
    TestBudget();
    TestTiers();
    TestBounds();
    TestRetry();
    TestEvict();
    TestKeepPrevious();
    TestPalette();
    TestRigid();
    TestMovingRefit();
    TestHandOffOrder();
    TestHandOffBounds();
    TestSetupKeys();
    TestCopyBudget();
    TestLand();
    TestSetupFlow();
    TestTickQueue();
    return g_failed;
}
}

int main(int argc, char** argv) {
#ifdef LANTERN_RT_RULES_MUTANTS
    (void)argc;
    (void)argv;
    int survivors = 0;
    for (int m = 0; m <= R::kMutants; ++m) {
        R::g_mutant = m;
        const int found = RunSuite();
        if (m == 0 && found) {
            std::printf("rt scene rules mutants: the unmutated run failed %d checks\n", found);
            return 1;
        }
        if (m && !found) {
            std::printf("rt scene rules mutants: mutant %d survived\n", m);
            ++survivors;
        }
    }
    if (survivors) {
        return 1;
    }
    std::printf("rt scene rules mutants: every mutant caught\n");
    return 0;
#else
    (void)argc;
    (void)argv;
    if (RunSuite()) {
        std::printf("rt scene rules: %d checks failed\n", g_failed);
        return 1;
    }
    std::printf("rt scene rules: all tests passed\n");
    return 0;
#endif
}
