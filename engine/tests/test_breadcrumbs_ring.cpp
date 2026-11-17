// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/vk_breadcrumbs_ring.h"

using namespace Vulkan::Breadcrumbs;

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static void TestLocate() {
    DrawResources draw;
    DrawResource* a = draw.Add();
    *a = {.handle = 1, .gpu_base = 0x1000000, .gpu_size = 0x12c0000, .image = true};
    DrawResource* b = draw.Add();
    *b = {.handle = 2, .gpu_base = 0x4000000, .gpu_size = 0x100000};
    DrawResource* c = draw.Add();
    *c = {.handle = 3};

    Nearest n = Locate(draw, 0x1000010);
    CHECK(n.index == 0 && n.inside && n.distance == 0);

    n = Locate(draw, 0x1000000 + 0x12c0000 + 0xb0000);
    CHECK(n.index == 0 && !n.inside && n.past_end && n.distance == 0xb0000);

    n = Locate(draw, 0x3ff0000);
    CHECK(n.index == 1 && !n.inside && !n.past_end && n.distance == 0x10000);

    DrawResource* inner = draw.Add();
    *inner = {.handle = 4, .gpu_base = 0x1100000, .gpu_size = 0x1000};
    n = Locate(draw, 0x1100800);
    CHECK(n.index == 3 && n.inside);

    DrawResources empty;
    CHECK(Locate(empty, 0x1234).index == -1);
}

static void TestAddLimit() {
    DrawResources draw;
    for (uint32_t i = 0; i < MaxDrawResources + 3; ++i) {
        draw.Add();
    }
    CHECK(draw.count == MaxDrawResources);
    CHECK(draw.dropped == 3);
}

static DrawResources MakeDraw(uint32_t stream, uint64_t id, uint32_t items) {
    DrawResources d{.id = id, .stream = stream};
    for (uint32_t i = 0; i < items; ++i) {
        *d.Add() = {.handle = id, .guest = stream, .gpu_base = id, .gpu_size = id};
    }
    return d;
}

static bool Whole(const DrawResources& d, uint32_t stream, uint64_t id, uint32_t items) {
    bool whole = d.id == id && d.stream == stream && d.count == items && d.dropped == 0;
    for (uint32_t i = 0; i < items; ++i) {
        const DrawResource& r = d.items[i];
        whole = whole && r.handle == id && r.guest == stream && r.gpu_base == id && r.gpu_size == id;
    }
    return whole;
}

static void TestRing() {
    auto ring = std::make_unique<DrawRing<4>>();
    CHECK(!ring->Read(0, 0));
    for (uint64_t id = 1; id <= 6; ++id) {
        CHECK(ring->Publish(MakeDraw(0, id, 1)));
    }
    CHECK(ring->Recorded() == 6);
    CHECK(!ring->Read(0, 1));
    CHECK(!ring->Read(0, 2));
    const auto d5 = ring->Read(0, 5);
    CHECK(d5 && Whole(*d5, 0, 5, 1));
    CHECK(!ring->Read(1, 5));
    CHECK(ring->Read(0, 6));

    CHECK(ring->Publish(MakeDraw(2, 7, MaxDrawResources)));
    const auto d7 = ring->Read(2, 7);
    CHECK(d7 && Whole(*d7, 2, 7, MaxDrawResources));
    CHECK(!ring->Read(0, 3));

    CHECK(ring->Publish(MakeDraw(1, 8, 0)));
    const auto d8 = ring->Read(1, 8);
    CHECK(d8 && d8->count == 0 && d8->id == 8);

    DrawResources over{.id = 9};
    for (uint32_t i = 0; i < MaxDrawResources + 2; ++i) {
        over.Add();
    }
    CHECK(ring->Publish(over));
    const auto d9 = ring->Read(0, 9);
    CHECK(d9 && d9->count == MaxDrawResources && d9->dropped == 2);
}

template <uint32_t Size>
static void Hammer(uint32_t writers, uint64_t draws) {
    auto ring = std::make_unique<DrawRing<Size>>();
    std::array<std::atomic<uint64_t>, 4> latest{};
    std::atomic<uint32_t> running{writers};
    std::vector<std::thread> threads;
    for (uint32_t w = 0; w < writers; ++w) {
        threads.emplace_back([&, w] {
            for (uint64_t id = 1; id <= draws; ++id) {
                ring->Publish(MakeDraw(w, id, 1 + uint32_t(id % MaxDrawResources)));
                latest[w].store(id, std::memory_order_relaxed);
            }
            running.fetch_sub(1, std::memory_order_relaxed);
        });
    }
    uint64_t kept = 0;
    uint64_t torn = 0;
    for (uint32_t turn = 0; running.load(std::memory_order_relaxed) != 0; ++turn) {
        const uint32_t w = turn % writers;
        const uint64_t id = latest[w].load(std::memory_order_relaxed);
        if (const auto d = ring->Read(w, id)) {
            ++kept;
            torn += !Whole(*d, w, id, 1 + uint32_t(id % MaxDrawResources));
        }
    }
    for (std::thread& t : threads) {
        t.join();
    }
    std::printf("breadcrumbs ring: %u writer(s), %u slot(s): %llu copies kept, %llu torn\n", writers,
                Size, (unsigned long long)kept, (unsigned long long)torn);
    CHECK(torn == 0);
    for (uint32_t w = 0; w < writers; ++w) {
        const auto last = ring->Read(w, draws);
        CHECK(!last || Whole(*last, w, draws, 1 + uint32_t(draws % MaxDrawResources)));
    }
    if (writers == 1) {
        CHECK(ring->Read(0, draws).has_value());
    }
}

static void TestRingTornRead() {
    Hammer<1>(1, 200000);
    Hammer<2>(2, 100000);
    Hammer<8>(4, 50000);
}

static void TestHandleRanges() {
    HandleRanges ranges;
    ranges.Bind(10, 0x1000, 0x100);
    CHECK(ranges.Find(10).base == 0x1000 && ranges.Find(10).size == 0x100);
    ranges.Bind(10, 0x1100, 0x100);
    CHECK(ranges.Find(10).base == 0x1000 && ranges.Find(10).size == 0x200);
    ranges.Bind(10, 0x0f80, 0x100);
    CHECK(ranges.Find(10).base == 0x0f80 && ranges.Find(10).size == 0x280);
    ranges.Unbind(10, 0x1100);
    CHECK(ranges.Find(10).size == 0x280);
    ranges.Unbind(10, 0x0f80);
    CHECK(ranges.Find(10).size == 0);
    CHECK(ranges.Find(99).size == 0);
}

static void TestResolve() {
    HandleRanges ranges;
    ranges.Bind(30, 0x9000, 0x400);
    ranges.Bind(31, 0xa000, 0x800);
    DrawResources d;
    d.Add()->handle = 31;
    d.Add()->handle = 32;
    *d.Add() = {.handle = 30, .gpu_base = 1, .gpu_size = 1};
    ranges.Resolve(std::span{d.items.data(), d.count});
    CHECK(d.items[0].gpu_base == 0xa000 && d.items[0].gpu_size == 0x800);
    CHECK(d.items[1].gpu_base == 0 && d.items[1].gpu_size == 0);
    CHECK(d.items[2].gpu_base == 0x9000 && d.items[2].gpu_size == 0x400);
    ranges.Resolve({});
}

static void TestHandleReuse() {
    HandleRanges ranges;
    ranges.Bind(20, 0x100000, 0x1000);
    ranges.Bind(20, 0x7f00000000ull, 0x2000);
    CHECK(ranges.Find(20).base == 0x7f00000000ull && ranges.Find(20).size == 0x2000);
    ranges.Unbind(20, 0x7f00000000ull);
    CHECK(ranges.Find(20).size == 0);
    CHECK(ranges.Size() == 0);

    ranges.Bind(21, 0x5000, 0x1000);
    ranges.Bind(21, 0x3000, 0x1000);
    CHECK(ranges.Find(21).base == 0x3000 && ranges.Find(21).size == 0x1000);

    CHECK(HandleRanges::Merge({0x1000, 0x100}, {0x1100, 0x10}).size == 0x110);
    CHECK(HandleRanges::Merge({0x1000, 0x100}, {0x1101, 0x10}).base == 0x1101);
    CHECK(HandleRanges::Merge({0x1000, 0x100}, {0x1040, 0x10}).size == 0x100);
    CHECK(HandleRanges::Merge({0x1000, 0x100}, {0x0f00, 0x100}).base == 0x0f00);
    CHECK(HandleRanges::Merge({0x1000, 0x100}, {0x0e00, 0x100}).size == 0x100);
}

struct Bound {
    uint64_t handle = 0;
    int tag = 0;
};

static void TestFindUnbound() {
    std::multimap<uint64_t, Bound> map;
    map.emplace(0x1000, Bound{.handle = 7, .tag = 1});
    map.emplace(0x1000, Bound{.handle = 8, .tag = 2});
    map.emplace(0x2000, Bound{.handle = 9, .tag = 3});

    auto it = FindUnbound(map, 0x1000, 8);
    CHECK(it != map.end() && it->second.tag == 2);
    CHECK(FindUnbound(map, 0x1000, 9) == map.end());
    CHECK(FindUnbound(map, 0x3000, 0) == map.end());

    it = FindUnbound(map, 0x1000, 0);
    CHECK(it != map.end() && it->second.tag == 1);
    if (it != map.end()) {
        map.erase(it);
    }
    it = FindUnbound(map, 0x1000, 0);
    CHECK(it != map.end() && it->second.tag == 2);
    if (it != map.end()) {
        map.erase(it);
    }
    CHECK(FindUnbound(map, 0x1000, 0) == map.end());
    CHECK(map.size() == 1);

    map.emplace(0x4000, Bound{.handle = 0, .tag = 4});
    map.emplace(0x4000, Bound{.handle = 5, .tag = 5});
    it = FindUnbound(map, 0x4000, 0);
    CHECK(it != map.end() && it->second.tag == 4);
}

static void Bench() {
    constexpr uint32_t Handles = 30000;
    constexpr uint32_t Draws = 200000;
    constexpr uint32_t Resources = 12;
    HandleRanges ranges;
    for (uint32_t i = 0; i < Handles; ++i) {
        ranges.Bind(0x7f0000000000ull + i * 0x40ull, 0x100000000ull + i * 0x200000ull, 0x100000);
    }
    auto ring = std::make_unique<DrawRing<512>>();
    DrawResources d;
    uint64_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t draw = 0; draw < Draws; ++draw) {
        d = {.id = draw + 1ull, .image_descriptors = 8, .buffer_descriptors = 4};
        for (uint32_t i = 0; i < Resources; ++i) {
            const uint64_t handle = 0x7f0000000000ull + ((draw * 13 + i * 7919) % Handles) * 0x40ull;
            if (DrawResource* item = d.Add()) {
                *item = {.handle = handle,
                         .guest = handle,
                         .guest_size = 0x1000,
                         .format = 97,
                         .width = 1920,
                         .height = 1080,
                         .depth = 1,
                         .layers = 1,
                         .image = i < 8};
            }
        }
        ranges.Resolve(std::span{d.items.data(), d.count});
        sink += ring->Publish(d) + d.items[draw % Resources].gpu_base;
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                          .count() /
                      Draws;
    std::printf("breadcrumbs ring: %.0f ns per full-screen draw (%u resources, one lock, %u bound handles)%s\n",
                ns, Resources, Handles, sink == 1 ? " " : "");
    const auto last = ring->Read(0, Draws);
    CHECK(last && last->count == Resources && last->items[0].gpu_size == 0x100000);
}

int main(int argc, char** argv) {
    TestLocate();
    TestAddLimit();
    TestRing();
    TestRingTornRead();
    TestHandleRanges();
    TestHandleReuse();
    TestResolve();
    TestFindUnbound();
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
        Bench();
    }
    if (failures) {
        std::printf("breadcrumbs ring: %d failures\n", failures);
        return 1;
    }
    std::printf("breadcrumbs ring: all passed\n");
    return 0;
}
