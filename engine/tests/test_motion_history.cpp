// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <set>
#include <utility>
#include "gpu/shadps4/video_core/renderer_vulkan/motion_history.h"

using namespace Vulkan::Motion;

namespace {

[[maybe_unused]] void Basics() {
    const std::array<uint16_t, 5> indices{1000, 1007, 1002, 1007, UINT16_MAX};
    const auto range = IndexedRange(std::span(indices), -900, true);
    assert(range.first == 100 && range.count == 8);
    assert(IndexedRange(std::span(indices), -1001, true).count == 0);
    assert(IndexedRange(std::span(indices), 0, false).count == 64536);
    const std::array<uint32_t, 3> large{100000, 100003, UINT32_MAX};
    assert(IndexedRange(std::span(large), 7, true).count == 4);
    assert(IndexedRange(std::span(large), INT32_MAX, true).count == 0);
    const std::array<uint16_t, 1> restart{UINT16_MAX};
    assert(IndexedRange(std::span(restart), 0, true).count == 0);
    assert(IndexedRange(std::span<const uint16_t>{}, 0, false).count == 0);

    Draw draw{.shader = 1, .geometry = 2, .indices = 3, .topology = 4,
              .index_count = 5, .instances = 2, .first_instance = 100,
              .vertices = range};
    History history(32);
    history.NextFrame();
    const auto first = history.Prepare(draw);
    const auto duplicate = history.Prepare(draw);
    assert(first.store && !first.load && first.first_vertex == 100);
    assert(first.first_instance == 100 && first.instances == 2);
    assert(duplicate.store == first.store + 16);
    assert(history.Used() == 32);
    assert(!history.Prepare(draw).store && history.stats.exhausted == 1);
    history.NextFrame();
    const auto next = history.Prepare(draw);
    assert(next.load == first.store && next.store != first.store);
    assert(history.Prepare(draw).load == duplicate.store);
    history.NextFrame();
    history.NextFrame();
    assert(!history.Prepare(draw).load);

    history.NextFrame();
    auto changed = draw;
    changed.topology++;
    assert(!history.Prepare(changed).load);
    changed = draw;
    changed.first_instance++;
    assert(!history.Prepare(changed).load);
    history.NextFrame();
    changed = draw;
    changed.vertices.first++;
    assert(!history.Prepare(changed).load);

    History overflow(32);
    draw.vertices.first = 0;
    draw.vertices.count = INT32_MAX;
    draw.instances = UINT32_MAX;
    draw.first_instance = 0;
    assert(!overflow.Prepare(draw).store);
    assert(overflow.stats.exhausted == 1);
    draw.first_instance = 10;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 1);
    draw.first_instance = 0;
    draw.vertices.first = INT32_MAX;
    draw.vertices.count = 2;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 2);

    History gated(1024);
    const History::GateKey weapon{.shader = 7, .index_count = 3, .instances = 1};
    const auto frame = [&](uint64_t palette) {
        gated.NextFrame();
        return gated.Moving(weapon, palette);
    };
    assert(!gated.Moving(weapon, 1) && gated.stats.still == 1);
    assert(!frame(1));
    assert(frame(2) && frame(3));
    assert(!frame(3));
    gated.NextFrame();
    gated.NextFrame();
    assert(!gated.Moving(weapon, 4));
    assert(!gated.Moving(weapon, 5));

    History held(1024);
    held.SetHold(2);
    const auto step = [&](uint64_t palette) {
        held.NextFrame();
        return held.Moving(weapon, palette);
    };
    assert(!held.Moving(weapon, 1));
    assert(step(2));
    assert(step(2) && held.stats.held == 1);
    assert(step(3));
    assert(step(3) && step(3));
    assert(!step(3) && held.stats.held == 3);
    held.NextFrame();
    held.NextFrame();
    assert(!held.Moving(weapon, 4));
    assert(ClassifyBuffer(864) == BufferRole::Other && ClassifyBuffer(416) == BufferRole::Other);
    assert(ClassifyBuffer(96) == BufferRole::SmallSkeleton &&
           ClassifyBuffer(384) == BufferRole::SmallSkeleton);
    assert(ClassifyBuffer(656) == BufferRole::Skeleton && ClassifyBuffer(64) == BufferRole::Other);

    IndexRangeCache ranges;
    uint32_t scans = 0;
    const IndexRangeCache::Key mesh{.address = 0x1000, .count = 3, .index_size = 2};
    const auto scan = [&] {
        ++scans;
        return IndexRangeCache::Result{{4, 3}, 99};
    };
    assert(ranges.Get(mesh, 0, scan).topology == 99 && scans == 1);
    uint64_t rescanned = 0;
    for (uint64_t f = 1; f <= IndexRangeCache::Revalidate && !rescanned; ++f) {
        assert(ranges.Get(mesh, f, scan).range == (VertexRange{4, 3}));
        rescanned = scans == 2 ? f : 0;
    }
    assert(rescanned >= 1 && rescanned <= IndexRangeCache::Revalidate);
    for (uint64_t f = rescanned + 1; f < rescanned + IndexRangeCache::Revalidate; ++f) {
        ranges.Get(mesh, f, scan);
    }
    assert(scans == 2);
    ranges.Get(mesh, rescanned + IndexRangeCache::Revalidate, scan);
    assert(scans == 3);
    ranges.Trim(rescanned + IndexRangeCache::Revalidate + IndexRangeCache::Unused + 1);
    assert(ranges.Size() == 0);

    assert(JitterPhases(1920, 1920) == 8);
    assert(JitterPhases(1280, 1920) == 18);
    assert(JitterPhases(960, 1920) == 32);
    assert(JitterPhases(640, 1920) == 72);
    assert(JitterPhases(0, 1920) == 8);
}

int g_failures = 0;
bool g_quiet = false;

void Expect(bool ok, const char* what) {
    if (ok) {
        return;
    }
    ++g_failures;
    if (!g_quiet) {
        std::printf("FAILED: %s\n", what);
    }
}

Draw Mesh(uint64_t shader, uint32_t count) {
    return Draw{.shader = shader, .geometry = 1, .index_count = count, .instances = 1,
                .vertices = {0, count}};
}

void StableOverflow() {
    History h(100);
    const auto frame = [&](std::initializer_list<uint64_t> order) {
        h.NextFrame();
        std::set<uint64_t> stored;
        for (const uint64_t id : order) {
            if (h.Prepare(Mesh(id, 30)).store) {
                stored.insert(id);
            }
        }
        return stored;
    };
    const std::set<uint64_t> first = frame({1, 2, 3, 4, 5});
    Expect(first == std::set<uint64_t>{1, 2, 3}, "overflow: the first frame does not store the first three");
    Expect(frame({5, 4, 3, 2, 1}) == first, "overflow: reversed order changed the stored set");
    Expect(frame({3, 5, 1, 4, 2}) == first, "overflow: shuffled order changed the stored set");
    Expect(frame({4, 5, 2, 3, 1}) == first, "overflow: a third order changed the stored set");
}

void CharactersFirst() {
    History h(100);
    const auto frame = [&] {
        h.NextFrame();
        const bool prop = h.Prepare(Mesh(1, 60), 0x100, false).store != 0;
        const bool character = h.Prepare(Mesh(2, 60), 0x200, true).store != 0;
        return std::pair{prop, character};
    };
    const auto first = frame();
    Expect(first.first && !first.second, "characters: the first frame is not first come");
    const auto second = frame();
    Expect(!second.first && second.second, "characters: the character did not take the space back");
    const auto third = frame();
    Expect(!third.first && third.second, "characters: the character did not keep the space");
    Expect(h.stats.exhausted_characters == 1, "characters: character skips not counted");
}

void PerObject() {
    History h(100);
    const auto frame = [&] {
        h.NextFrame();
        const bool a = h.Prepare(Mesh(11, 40), 0xa00).store != 0;
        const bool z = h.Prepare(Mesh(13, 40), 0xb00).store != 0;
        const bool b = h.Prepare(Mesh(12, 40), 0xa00).store != 0;
        return std::array{a, z, b};
    };
    const auto first = frame();
    Expect(first[0] && first[1] && !first[2], "objects: the first frame is not first come");
    const auto second = frame();
    Expect(second[0] && !second[1] && second[2], "objects: the object's second draw was not kept with it");
    const auto third = frame();
    Expect(third[0] && !third[1] && third[2], "objects: the whole object did not stay stored");
}

void StillReservesNothing() {
    History h(100);
    const History::GateKey prop{.shader = 31, .index_count = 60, .instances = 1};
    h.NextFrame();
    Expect(!h.Moving(prop, 1), "still: a first palette counted as moving");
    h.NextFrame();
    bool moved = h.Moving(prop, 2) && h.Prepare(Mesh(31, 60), 0x300).store != 0;
    const bool late = h.Prepare(Mesh(32, 60), 0x400).store != 0;
    Expect(moved && !late, "still: the moving prop did not take the space first");
    h.NextFrame();
    moved = h.Moving(prop, 2);
    const bool after = h.Prepare(Mesh(32, 60), 0x400).store != 0;
    Expect(!moved, "still: an unchanged palette counted as moving");
    Expect(after, "still: a still prop kept its reservation");
}

void RevalidationSpread() {
    IndexRangeCache c;
    constexpr uint32_t entries = 1000;
    uint32_t scans = 0;
    const auto scan = [&] {
        ++scans;
        return IndexRangeCache::Result{{0, 3}, 1};
    };
    const auto key = [](uint32_t i) {
        return IndexRangeCache::Key{.address = 0x10000 + uint64_t(i) * 64, .count = 3, .index_size = 2};
    };
    for (uint32_t i = 0; i < entries; ++i) {
        c.Get(key(i), 0, scan);
    }
    uint32_t most = 0, window = 0, next_window = 0;
    for (uint64_t f = 1; f <= 2 * IndexRangeCache::Revalidate; ++f) {
        const uint32_t before = scans;
        for (uint32_t i = 0; i < entries; ++i) {
            c.Get(key(i), f, scan);
        }
        most = std::max(most, scans - before);
        (f <= IndexRangeCache::Revalidate ? window : next_window) += scans - before;
    }
    Expect(window == entries && next_window == entries,
           "revalidation: an entry was not rescanned exactly once every Revalidate frames");
    Expect(most <= 3 * entries / IndexRangeCache::Revalidate,
           "revalidation: entries first seen in one frame are rescanned in one burst");
}

void CapacityNeverExceeded() {
    constexpr uint32_t capacity = 100;
    History h(capacity);
    bool within = true;
    uint32_t seed = 7;
    for (uint32_t f = 0; f < 50; ++f) {
        h.NextFrame();
        for (uint32_t d = 0; d < 12; ++d) {
            seed = seed * 1103515245u + 12345u;
            const uint32_t count = 5 + (seed >> 16) % 50;
            const uint32_t id = (seed >> 8) % 9;
            const auto a = h.Prepare(Mesh(100 + id, count), id < 3 ? 0x900 + id : 0, id % 2 == 0);
            const uint32_t half = 1 + ((f + 1) & 1) * capacity;
            if (a.store && (a.store < half || a.store + count > half + capacity)) {
                within = false;
            }
            within = within && h.Used() <= capacity;
        }
    }
    Expect(within, "capacity: a store went past the capacity");
}

void ExhaustedDoesNotLoad() {
    History h(50);
    h.NextFrame();
    h.Prepare(Mesh(1, 40));
    const bool skipped = !h.Prepare(Mesh(2, 40)).store;
    h.NextFrame();
    h.Prepare(Mesh(1, 40));
    Expect(skipped && !h.Prepare(Mesh(2, 40)).load, "exhausted: a skipped draw loaded history next frame");
}

void CharacterTiers() {
    History h(100);
    const auto frame = [&](bool newcomer_first) {
        h.NextFrame();
        bool kept = false, newcomer = false;
        if (newcomer_first) {
            newcomer = h.Prepare(Mesh(2, 60), 0x220, true).store != 0;
        }
        kept = h.Prepare(Mesh(1, 60), 0x210, true).store != 0;
        if (!newcomer_first) {
            newcomer = h.Prepare(Mesh(2, 60), 0x220, true).store != 0;
        }
        return std::pair{kept, newcomer};
    };
    const auto first = frame(false);
    Expect(first.first && !first.second, "tiers: the first frame is not first come");
    const auto second = frame(true);
    Expect(second.first && !second.second, "tiers: a new character took a stored character's space");
}

void RejectedCharacterBlocksProps() {
    const auto run = [](bool with_character) {
        History h(100);
        h.NextFrame();
        h.Prepare(Mesh(1, 50), 0x100, false);
        const bool character = with_character && h.Prepare(Mesh(2, 60), 0x200, true).store != 0;
        h.NextFrame();
        const bool prop = h.Prepare(Mesh(3, 40), 0x300, false).store != 0;
        return std::pair{character, prop};
    };
    const auto blocked = run(true);
    Expect(!blocked.first && !blocked.second, "blocking: a new prop took the space a skipped character waits for");
    const auto control = run(false);
    Expect(control.second, "blocking: without the character the new prop is not stored");
}

void StillThenMoving() {
    History h(100);
    const History::GateKey prop{.shader = 41, .index_count = 30, .instances = 1};
    const auto frame = [&](uint64_t palette) {
        h.NextFrame();
        if (!h.Moving(prop, palette)) {
            return History::Allocation{};
        }
        return h.Prepare(Mesh(41, 30), 0x410);
    };
    frame(1);
    const auto moved = frame(2);
    const auto still = frame(2);
    const auto again = frame(3);
    const auto next = frame(4);
    Expect(moved.store && !still.store && !still.load, "gate: a still frame stored or loaded");
    Expect(again.store && !again.load, "gate: moving after a still frame loaded stale history");
    Expect(next.store && next.load == again.store, "gate: the next moving frame did not load the last one");
}

int RunAll() {
    g_failures = 0;
    StableOverflow();
    CharactersFirst();
    PerObject();
    StillReservesNothing();
    RevalidationSpread();
    CapacityNeverExceeded();
    ExhaustedDoesNotLoad();
    CharacterTiers();
    RejectedCharacterBlocksProps();
    StillThenMoving();
    return g_failures;
}

}

int main(int argc, char** argv) {
#ifdef LANTERN_MOTION_HISTORY_MUTANTS
    if (argc > 1 && !std::strcmp(argv[1], "--mutants")) {
        Basics();
        g_quiet = true;
        if (RunAll()) {
            std::printf("motion-history mutants: the unmutated run failed\n");
            return 1;
        }
        int survivors = 0;
        for (int m = 1; m < int(Mutant::kCount); ++m) {
            g_mutant = m;
            const int found = RunAll();
            std::printf("motion-history mutants: mutant %d %s (%d checks)\n", m, found ? "caught" : "survived",
                        found);
            survivors += found ? 0 : 1;
        }
        g_mutant = 0;
        if (survivors) {
            return 1;
        }
        std::printf("motion-history mutants: every mutant caught (%d)\n", int(Mutant::kCount) - 1);
        return 0;
    }
    std::printf("motion-history-test: the mutant build runs with --mutants\n");
    return 1;
#else
    (void)argc;
    (void)argv;
    Basics();
    const int failures = RunAll();
    std::printf("Motion history: %s (ranges, restart, offsets, matching, capacity, gating, index cache, "
                "jitter, stable overflow, characters first, per object, still props, revalidation spread; "
                "%d failures)\n",
                failures ? "FAILED" : "PASS", failures);
    return failures ? 1 : 0;
#endif
}
