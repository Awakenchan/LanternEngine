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
bool quiet = false;

void Expect(bool ok, const char* what) {
    if (!ok) {
        if (!quiet) std::printf("FAIL: %s\n", what);
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

namespace RC = BbEngine::RingConstants;

constexpr std::uint64_t kThreshold = 16 * 1024;
constexpr RC::Ring kRing{0x1043400040, 0x1043400040 + (std::uint64_t(288) << 20)};

void TestDecide() {
    const std::uint64_t in_ring = kRing.base + 0x1000;
    const std::uint64_t outside = kRing.base - 0x10000;
    Expect(RC::Decide(RC::Off, false, true, kRing, in_ring, 256, kThreshold) == RC::Route::Untracked,
           "mode 0 without the census does nothing");
    Expect(RC::Decide(RC::Off, true, true, kRing, in_ring, 256, kThreshold) == RC::Route::Counted,
           "mode 0 with the census counts a ring binding as covered and copies it the old way");
    Expect(RC::Decide(RC::Off, true, true, kRing, outside, 256, kThreshold) == RC::Route::General,
           "mode 0 with the census counts a binding outside the ring as not covered");
    Expect(RC::Decide(RC::On, false, true, kRing, in_ring, 256, kThreshold) == RC::Route::Fast,
           "mode 1 copies a ring binding without checks");
    Expect(RC::Decide(RC::On, false, true, kRing, in_ring, kThreshold, kThreshold) == RC::Route::Fast,
           "a binding of exactly the stream threshold is still copied");
    Expect(RC::Decide(RC::On, false, true, kRing, in_ring, kThreshold + 1, kThreshold) == RC::Route::General,
           "a binding over the stream threshold takes the general path");
    Expect(RC::Decide(RC::On, false, true, kRing, kRing.end - 64, 128, kThreshold) == RC::Route::General,
           "a binding running past the ring's end takes the general path");
    Expect(RC::Decide(RC::On, false, true, RC::Ring{}, in_ring, 256, kThreshold) == RC::Route::General,
           "an unknown ring covers nothing");
    Expect(RC::Decide(RC::Compare, false, true, kRing, in_ring, 256, kThreshold) == RC::Route::Checked,
           "mode 2 checks every ring binding");
    for (const RC::Mode mode : {RC::Off, RC::On, RC::Compare}) {
        Expect(RC::Decide(mode, true, false, kRing, in_ring, 256, kThreshold) == RC::Route::Untracked,
               "a written or compute binding is never taken");
    }
}

void TestTally() {
    RC::Tally tally;
    const std::uint64_t in_ring = kRing.base + 0x2000;
    const RC::Route routes[] = {
        RC::Decide(RC::On, false, true, kRing, in_ring, 256, kThreshold),
        RC::Decide(RC::On, false, true, kRing, in_ring, 512, kThreshold),
        RC::Decide(RC::On, false, true, kRing, kRing.base - 0x10000, 256, kThreshold),
        RC::Decide(RC::On, false, true, kRing, in_ring, kThreshold * 2, kThreshold),
        RC::Decide(RC::On, false, false, kRing, in_ring, 256, kThreshold),
    };
    for (const RC::Route route : routes) {
        tally.Note(route);
        if (route == RC::Route::Fast) tally.Add(RC::Fast);
    }
    RC::Totals totals;
    RC::Publish(tally, totals, RC::On);
    RC::Counts last;
    RC::Counts w = RC::Take(totals, last);
    Expect(w.c[RC::Total] == 4, "every read-only binding is in the total");
    Expect(w.c[RC::Covered] == 2 && w.c[RC::Fast] == 2, "two ring bindings covered and copied without checks");
    Expect(w.c[RC::Covered] + (w.c[RC::Total] - w.c[RC::Covered]) == w.c[RC::Total] &&
               w.c[RC::Fast] + w.c[RC::RingFull] == w.c[RC::Covered],
           "fast plus ring full is the covered count, covered plus not covered is the total");
    w = RC::Take(totals, last);
    Expect(w.c[RC::Total] == 0, "a window counts only what came after the last one");
    std::uint32_t due = 0;
    for (std::uint32_t i = 0; i < RC::kPublishEvery * 3; ++i) due += tally.Due() ? (tally.unpublished = 0, 1u) : 0u;
    Expect(due == 3, "a tally asks to be published once every kPublishEvery uses");
}

void TestJudge() {
    Expect(RC::Judge(false, false, false, false) == 0, "an agreeing check has no reason");
    Expect(RC::Judge(false, false, false, true) == RC::ReasonBytes, "bytes differ");
    Expect(RC::Judge(true, false, false, false) == RC::ReasonClamped, "a clamped range differs");
    Expect(RC::Judge(false, true, false, false) == RC::ReasonGpuModified,
           "a GPU-modified range differs: its truth is on the GPU, not in the bytes both copies read");
    Expect(RC::Judge(false, false, true, false) == RC::ReasonQueuedWrite,
           "a queued GPU write over the bytes differs");
}

void TestFirstDiffering() {
    std::uint8_t a[32]{}, b[32]{};
    Expect(RC::FirstDiffering(a, b, 32) == 32, "equal bytes report the size");
    b[0] = 1;
    Expect(RC::FirstDiffering(a, b, 32) == 0, "a difference in the first byte");
    b[0] = 0;
    b[17] = 9;
    Expect(RC::FirstDiffering(a, b, 32) == 17, "a difference in the middle");
    Expect(RC::FirstDiffering(a, b, 17) == 17, "bytes past the size are not compared");
}

void TestQueuedWrites() {
    RC::QueuedWrites<4> queue;
    const auto reached_to = [](std::uint64_t done) { return [done](std::uint64_t packet) { return packet <= done; }; };
    std::uint32_t overwritten = 0;
    for (std::uint64_t i = 1; i <= 6; ++i) overwritten += queue.Push({1000 + i * 16, 16, i, RC::WriterDma}, reached_to(0));
    Expect(overwritten == 2 && queue.Size() == 4, "a full queue overwrites the oldest unreached write and says so");
    Expect(queue.ExactWriters(1000 + 16, 16, reached_to(0)) == 0, "an overwritten write is no longer seen");
    Expect(queue.ExactWriters(1000 + 3 * 16, 16, reached_to(0)) == 1u << RC::WriterDma, "a queued write is seen");
    Expect(queue.ExactWriters(1000 + 3 * 16, 16, reached_to(3)) == 0, "a write whose packet was reached is not counted");
    Expect(queue.ExactWriters(1000 + 3 * 16 + 16, 0, reached_to(0)) == 0, "an empty range overlaps nothing");
    queue.DropReached(reached_to(4));
    Expect(queue.Size() == 2, "reached writes leave the queue from the oldest");
    queue.DropReached(reached_to(9));
    Expect(queue.Size() == 0, "every reached write leaves");
    RC::QueuedWrites<4> full;
    for (std::uint64_t i = 1; i <= 4; ++i) full.Push({2000 + i * 16, 16, i, RC::WriterFence}, reached_to(0));
    Expect(!full.Push({2100, 16, 5, RC::WriterFence}, reached_to(2)), "pushing over reached writes loses nothing");
    Expect(full.Size() == 3, "a full queue drops its reached writes before taking a new one");
    Expect(full.ExactWriters(2000 + 3 * 16, 16, reached_to(2)) == 1u << RC::WriterFence &&
               full.ExactWriters(2100, 16, reached_to(2)) == 1u << RC::WriterFence,
           "the unreached writes and the new one are still seen");
}

void TestPlantedDifference() {
    std::uint8_t fast[64], general[64];
    for (int i = 0; i < 64; ++i) fast[i] = general[i] = static_cast<std::uint8_t>(i);
    general[40] = 0xee;
    const std::uint64_t at = RC::FirstDiffering(fast, general, 64);
    const std::uint8_t reasons = RC::Judge(false, false, false, at != 64);
    RC::Tally tally;
    tally.NoteCheck(reasons, false, 0);
    tally.NoteCheck(0, true, 0);
    tally.NoteCheck(RC::Judge(false, false, true, false), false, 1u << RC::WriterFence);
    RC::Totals totals;
    RC::Difference d{kRing.base + 64, 64, 64, 77, at, reasons, fast[at], general[at], 0};
    RC::RecordFirst(totals, d);
    RC::RecordFirst(totals, RC::Difference{1, 1, 1, 1, 0, RC::ReasonClamped, 0, 0, 0});
    RC::Publish(tally, totals, RC::Compare);
    RC::Counts last;
    const RC::Counts w = RC::Take(totals, last);
    Expect(w.c[RC::Checked] == 3 && w.c[RC::Differ] == 2, "a planted byte difference and a queued write are counted");
    Expect(w.c[RC::DifferBytes] == 1 && w.c[RC::DifferQueuedWrite] == 1 && w.c[RC::ExactFence] == 1,
           "each reason and the exact writer are counted");
    Expect(w.c[RC::CoarsePending] == 1, "a pending granule without an exact write is counted apart");
    RC::Difference first;
    Expect(RC::TakeFirst(totals, first), "the first difference is kept");
    Expect(first.packet == 77 && first.offset == 40 && first.fast_byte == 40 && first.general_byte == 0xee &&
               first.reasons == RC::ReasonBytes,
           "the first difference holds its packet, offset and both values");
    Expect(!RC::TakeFirst(totals, first), "the first difference is shown once");
    char line[512];
    RC::FormatDifference(line, sizeof line, d);
    Expect(std::strstr(line, "reasons bytes") && std::strstr(line, "byte 40") && std::strstr(line, "LANTERN_RING_CONSTANTS=0"),
           "the record names the reason, the byte and the switch that turns the step off");
}

void TestOtherWriters() {
    RC::Tally tally;
    RC::Totals totals;
    RC::Publish(tally, totals, RC::Compare);
    std::thread other([&] { RC::Publish(tally, totals, RC::Compare); });
    other.join();
    RC::Counts last;
    Expect(RC::Take(totals, last).c[RC::OtherWriters] == 1, "mode 2 counts a publish from a second thread");
    RC::Tally plain;
    RC::Publish(plain, totals, RC::On);
    std::thread again([&] { RC::Publish(plain, totals, RC::On); });
    again.join();
    Expect(plain.counts.c[RC::OtherWriters] == 0, "mode 1 does not check the writer");
}

void TestSuspect() {
    RC::Counts w;
    Expect(RC::Suspect(RC::Off, false, w) == nullptr, "nothing counted is not suspect");
    w.c[RC::Total] = 100;
    Expect(RC::Suspect(RC::Off, true, w) != nullptr, "census on, bindings counted, none in the ring");
    Expect(RC::Suspect(RC::On, false, w) != nullptr, "mode 1, bindings counted, none in the ring");
    w.c[RC::Covered] = 80;
    Expect(RC::Suspect(RC::Off, true, w) == nullptr, "census with covered bindings is fine");
    Expect(RC::Suspect(RC::On, false, w) != nullptr, "mode 1 with covered bindings but none copied");
    w.c[RC::Fast] = 80;
    Expect(RC::Suspect(RC::On, false, w) == nullptr, "mode 1 copying its covered bindings is fine");
    Expect(RC::Suspect(RC::Compare, false, w) != nullptr, "mode 2 with covered bindings and no checks");
    w.c[RC::Checked] = 80;
    Expect(RC::Suspect(RC::Compare, false, w) == nullptr, "mode 2 checking is fine");
    w.c[RC::Differ] = 80;
    Expect(RC::Suspect(RC::Compare, false, w) != nullptr, "every check differing is suspect");
}

void TestFormat() {
    char line[1024];
    RC::Counts w;
    RC::FrameInputs in;
    in.frames = 10;
    in.draws = 22340;
    in.stage_a_us = 89360;
    in.ring_known = true;
    RC::Format(line, sizeof line, RC::Off, false, w, in);
    Expect(std::strstr(line, "(0, off)") && std::strstr(line, "stage A 4.00 us/draw, 8.94 ms/frame, 2234 draws/frame") &&
               std::strstr(line, "not counted"),
           "mode 0 prints stage A and says the bindings are not counted");
    w.c[RC::Total] = 100;
    w.c[RC::Covered] = 80;
    w.c[RC::Fast] = 70;
    w.c[RC::RingFull] = 10;
    RC::Format(line, sizeof line, RC::On, false, w, in);
    Expect(std::strstr(line, "(1, no checks)") && std::strstr(line, "bindings 10/frame, in the upload ring 8/frame (80.0%)") &&
               std::strstr(line, "copied without checks 7/frame, ring full 10") && !std::strstr(line, "differ"),
           "mode 1 prints uses and copies without checks");
    w.c[RC::Checked] = 80;
    w.c[RC::Differ] = 3;
    w.c[RC::DifferGpuModified] = 3;
    w.c[RC::ExactShader] = 2;
    in.ring_known = false;
    RC::Format(line, sizeof line, RC::Compare, false, w, in);
    Expect(std::strstr(line, "(2, compare every use)") && std::strstr(line, "checked 80, differ 3") &&
               std::strstr(line, "GPU-modified 3") && std::strstr(line, "exact: shader 2") &&
               std::strstr(line, "upload ring not seen"),
           "mode 2 prints checks, differences by reason and exact writers");
    char small[24];
    const int n = RC::Format(small, sizeof small, RC::Compare, false, w, in);
    Expect(n > 0 && std::strlen(small) == sizeof small - 1, "a short buffer is filled and ended, never overrun");
}
}

int RunSuite() {
    using namespace BbEngine::RingConstants;
    failures = 0;
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

    TestDecide();
    TestTally();
    TestJudge();
    TestFirstDiffering();
    TestQueuedWrites();
    TestPlantedDifference();
    TestOtherWriters();
    TestSuspect();
    TestFormat();
    WrapRule();
    return failures;
}

int main(int argc, char** argv) {
#ifdef LANTERN_RING_MUTANTS
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Benchmark();
    using BbEngine::RingConstants::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        BbEngine::RingConstants::selected_mutant = static_cast<Mutant>(m);
        quiet = m != static_cast<int>(Mutant::None);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("ring-constants mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("ring-constants mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("ring-constants mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("ring-constants mutants: every mutant caught\n");
    return 0;
#else
    RunSuite();
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Benchmark();
    if (failures) {
        std::printf("ring-constants-test: %d failed\n", failures);
        return 1;
    }
    std::printf("ring-constants-test: ok\n");
    return 0;
#endif
}
