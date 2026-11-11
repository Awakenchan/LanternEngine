// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <vector>

#include "gpu/shim/engine/ring_constants.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

using BbEngine::RingConstants::Ring;

struct Block {
    std::uint64_t base;
    bool free = true;
    std::uint64_t release_packet = 0;
};

struct GxRingModel {
    std::vector<std::uint8_t> memory;
    std::vector<Block> blocks;
    std::uint64_t completed = 0;
    static constexpr std::uint64_t kBlock = 256;

    explicit GxRingModel(std::uint32_t count) : memory(count * kBlock) {
        for (std::uint32_t i = 0; i < count; ++i) blocks.push_back({i * kBlock});
    }
    void Reclaim() {
        for (Block& b : blocks) {
            if (!b.free && b.release_packet && b.release_packet <= completed) {
                b.free = true;
                b.release_packet = 0;
            }
        }
    }
    int Take() {
        for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t i = 0; i < blocks.size(); ++i) {
                if (blocks[i].free) {
                    blocks[i].free = false;
                    return static_cast<int>(i);
                }
            }
            Reclaim();
        }
        return -1;
    }
};

void WrapRule() {
    GxRingModel gx(4);
    struct Draw {
        std::uint64_t packet;
        int block;
        std::uint8_t value;
    };
    std::vector<Draw> stream;
    std::uint64_t packet = 0;
    std::uint64_t stage_a = 0;
    std::vector<std::uint8_t> copies;
    std::vector<std::uint8_t> expected;
    std::size_t next = 0;
    const auto stage_a_runs_to = [&](std::uint64_t until) {
        while (next < stream.size() && stream[next].packet <= until) {
            const Draw& d = stream[next++];
            if (d.block < 0) {
                gx.completed = d.packet;
                continue;
            }
            std::uint8_t copy;
            const Ring ring{reinterpret_cast<std::uint64_t>(gx.memory.data()),
                            reinterpret_cast<std::uint64_t>(gx.memory.data()) + gx.memory.size()};
            const std::uint64_t source = ring.base + gx.blocks[d.block].base;
            Expect(ring.Holds(source, 1), "the draw's constants are inside the ring");
            BbEngine::RingConstants::Copy(&copy, source, 1);
            copies.push_back(copy);
            expected.push_back(d.value);
        }
        stage_a = until;
    };
    std::mt19937 random(7);
    for (int round = 0; round < 2000; ++round) {
        int block = gx.Take();
        if (block < 0) {
            stage_a_runs_to(packet);
            block = gx.Take();
        }
        Expect(block >= 0, "a block comes back once stage A has passed its release");
        if (block < 0) return;
        const std::uint8_t value = static_cast<std::uint8_t>(random());
        gx.memory[gx.blocks[block].base] = value;
        stream.push_back({++packet, block, value});
        stream.push_back({++packet, -1, 0});
        gx.blocks[block].release_packet = packet;
        if (random() % 3 == 0) stage_a_runs_to(packet - random() % 4);
    }
    stage_a_runs_to(packet);
    Expect(copies.size() == 2000, "every draw was copied");
    Expect(copies == expected, "each copy holds the bytes written for its draw, across wraps");
    (void)stage_a;
}

volatile std::uint64_t g_sink = 0;

struct CheckModel {
    std::map<std::uint64_t, std::uint64_t> mappings;
    std::vector<std::uint64_t> gpu_pages;
    std::vector<std::uint64_t> pending[3];
    std::uint64_t base;

    explicit CheckModel(std::uint64_t ring_base, std::uint64_t ring_bytes) : base(ring_base) {
        std::mt19937_64 random(11);
        std::uint64_t at = ring_base - (std::uint64_t(1) << 30);
        for (int i = 0; i < 4000; ++i) {
            const std::uint64_t bytes = (random() % 64 + 1) << 16;
            mappings[at] = bytes;
            at += bytes;
        }
        mappings[ring_base] = ring_bytes;
        gpu_pages.assign((ring_bytes >> 12) / 64 + 1, 0);
        pending[0].assign(ring_bytes >> 12, 0);
        pending[1].assign(ring_bytes >> 16, 0);
        pending[2].assign(ring_bytes >> 20, 0);
    }
    std::uint64_t Clamp(std::uint64_t address, std::uint64_t size) const {
        auto it = mappings.upper_bound(address);
        --it;
        return std::min(size, it->first + it->second - address);
    }
    bool GpuModified(std::uint64_t address, std::uint64_t size) const {
        const std::uint64_t first = (address - base) >> 12, last = (address - base + size - 1) >> 12;
        for (std::uint64_t p = first; p <= last; ++p)
            if (gpu_pages[p >> 6] >> (p & 63) & 1) return true;
        return false;
    }
    bool PendingWrite(std::uint64_t address, std::uint64_t size) const {
        std::uint64_t newest = 0;
        const int shifts[3] = {12, 16, 20};
        for (int level = 0; level < 3; ++level) {
            const std::uint64_t first = (address - base) >> shifts[level], last = (address - base + size - 1) >> shifts[level];
            for (std::uint64_t g = first; g <= last; ++g) newest = std::max(newest, pending[level][g]);
        }
        return newest != 0;
    }
};

void Benchmark() {
    const std::uint64_t ring_bytes = std::uint64_t(288) << 20;
    std::vector<std::uint8_t> ring_memory(ring_bytes);
    for (std::size_t i = 0; i < ring_memory.size(); i += 4096) ring_memory[i] = static_cast<std::uint8_t>(i >> 12);
    const Ring ring{reinterpret_cast<std::uint64_t>(ring_memory.data()),
                    reinterpret_cast<std::uint64_t>(ring_memory.data()) + ring_bytes};
    CheckModel checks(ring.base, ring_bytes);
    std::vector<std::uint8_t> out(std::uint64_t(64) << 20);
    std::mt19937_64 random(3);
    const std::uint64_t sizes[] = {64, 128, 256, 480, 1024};
    struct Copy {
        std::uint64_t address, size;
    };
    std::vector<Copy> copies(200000);
    for (Copy& c : copies) {
        c.size = sizes[random() % 5];
        c.address = ring.base + ((random() % (ring_bytes - 2048)) & ~std::uint64_t(63));
    }
    const auto run = [&](bool with_checks) {
        std::uint64_t cursor = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int rep = 0; rep < 5; ++rep) {
            for (const Copy& c : copies) {
                std::uint64_t size = c.size;
                if (with_checks) {
                    size = checks.Clamp(c.address, size);
                    if (checks.GpuModified(c.address, size) || checks.PendingWrite(c.address, size)) continue;
                } else if (!ring.Holds(c.address, size)) {
                    continue;
                }
                if (cursor + size > out.size()) cursor = 0;
                BbEngine::RingConstants::Copy(out.data() + cursor, c.address, size);
                cursor = (cursor + size + 255) & ~std::uint64_t(255);
            }
        }
        g_sink = g_sink + cursor + out[cursor / 2];
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() /
               (5.0 * copies.size());
    };
    run(true);
    run(false);
    double checked = 1e30, fast = 1e30;
    for (int i = 0; i < 3; ++i) {
        checked = std::min(checked, run(true));
        fast = std::min(fast, run(false));
    }
    std::printf("ring-constants bench: %.1f ns a copy with the checks (model), %.1f ns without, %.1f ns saved a copy\n",
                checked, fast, checked - fast);
}
}

int main(int argc, char** argv) {
    using namespace BbEngine::RingConstants;
    Expect(Parse(nullptr) == Off && Parse("") == Off && Parse("x") == Off, "off by default");
    Expect(Parse("0") == Off, "=0 off");
    Expect(Parse("1") == On, "=1 on");
    Expect(Parse("2") == Compare, "=2 compare");

    const Ring ring{0x1043400040, 0x1043400040 + (std::uint64_t(288) << 20)};
    Expect(ring.Known() && !Ring{}.Known(), "a ring is known once the pool is seen");
    Expect(ring.Holds(0x1043400040, 64), "the ring's start");
    Expect(ring.Holds(ring.end - 64, 64), "the last 64 bytes");
    Expect(!ring.Holds(ring.end - 64, 65), "a copy running past the ring's end takes the general path");
    Expect(!ring.Holds(ring.end, 16), "the byte after the ring");
    Expect(!ring.Holds(0x1043400000, 64), "before the ring");
    Expect(!ring.Holds(0x1043400040, 0), "an empty copy");
    Expect(!Ring{}.Holds(0x1043400040, 64), "an unknown ring holds nothing");
    Expect(!ring.Holds(ring.end - 64, ~std::uint64_t(0)), "no overflow on a huge size");

    Expect(Overlaps(100, 8, 104, 8) && Overlaps(104, 8, 100, 8), "overlapping writes");
    Expect(!Overlaps(100, 4, 104, 4) && !Overlaps(104, 4, 100, 4), "touching ranges do not overlap");
    Expect(!Overlaps(100, 0, 100, 8), "an empty write overlaps nothing");

    Window last;
    Note(Compared);
    Note(Compared);
    Note(BytesDiffer);
    Window w = Take(last);
    Expect(w.counts[Compared] == 2 && w.counts[BytesDiffer] == 1 && w.counts[GpuModified] == 0, "compare counts");
    w = Take(last);
    Expect(w.counts[Compared] == 0, "a window counts only what came after the last one");

    WrapRule();
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Benchmark();

    if (failures) {
        std::printf("ring-constants-test: %d failed\n", failures);
        return 1;
    }
    std::printf("ring-constants-test: ok\n");
    return 0;
}
