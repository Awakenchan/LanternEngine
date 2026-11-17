// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <vector>

#include "gpu/shim/engine/vram_evict.h"

namespace {
using BbEngine::VramEvict::Block;
using BbEngine::VramEvict::Decide;
using BbEngine::VramEvict::Verdict;
using u64 = std::uint64_t;

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr std::uint32_t kCleanupSeconds = 180;
constexpr std::uint32_t kTightSeconds = 10;
constexpr std::uint32_t kBlockShift = 16;
constexpr u64 kBlockSize = u64(1) << kBlockShift;
constexpr u64 kTrackerPage = 4096;

void TestDecide() {
    const std::uint32_t now = 10000;
    Expect(Decide({now, now - kCleanupSeconds + 1, kCleanupSeconds, false, false}) == Verdict::KeepRecent,
           "a block used one second inside the idle time stays");
    Expect(Decide({now, now - kCleanupSeconds, kCleanupSeconds, false, false}) == Verdict::Evict,
           "a block idle exactly the idle time with no GPU data goes");
    Expect(Decide({now, now - kTightSeconds - 50, kTightSeconds, false, false}) == Verdict::Evict,
           "a block idle past the tight-VRAM time goes");
    Expect(Decide({now, now - kCleanupSeconds, kCleanupSeconds, true, false}) == Verdict::KeepTwin,
           "an idle block holding a twin stays");
    Expect(Decide({now, now - kCleanupSeconds, kCleanupSeconds, true, true}) == Verdict::KeepTwin,
           "an idle block holding a twin and GPU data counts as kept for the twin");
    Expect(Decide({now, now - kCleanupSeconds, kCleanupSeconds, false, true}) == Verdict::KeepGpuWritten,
           "an idle block with GPU-written data stays");
    Expect(Decide({now, now, kCleanupSeconds, true, true}) == Verdict::KeepRecent,
           "a recent block is kept as recent whatever else it holds");
    Expect(Decide({now, 0, kCleanupSeconds, false, false}) == Verdict::Evict,
           "a block never used since start goes");
    int lookups = 0;
    const auto count = [&] {
        ++lookups;
        return true;
    };
    Expect(Decide(now, now - 1, kCleanupSeconds, count, count) == Verdict::KeepRecent && lookups == 0,
           "a recent block asks for no twin or GPU-write lookup");
}

void TestTwinBlocks() {
    using BbEngine::VramEvict::AddTwinBlocks;
    using BbEngine::VramEvict::HasTwin;
    using BbEngine::VramEvict::SortTwinBlocks;

    std::vector<u64> blocks;
    AddTwinBlocks(blocks, 7 * kBlockSize + 3 * kTrackerPage + 16, 64, kBlockShift);
    AddTwinBlocks(blocks, 2 * kBlockSize + 8, 32, kBlockShift);
    AddTwinBlocks(blocks, 7 * kBlockSize + 5 * kTrackerPage, 8, kBlockShift);
    AddTwinBlocks(blocks, 4 * kBlockSize - 16, 32, kBlockShift);
    AddTwinBlocks(blocks, 9 * kBlockSize, 0, kBlockShift);
    SortTwinBlocks(blocks);
    Expect(blocks == std::vector<u64>{2, 3, 4, 7}, "twins give their blocks once each, a twin over a block edge both");
    Expect(HasTwin(blocks, 2), "the first twin block is found");
    Expect(HasTwin(blocks, 4), "the block a twin runs into is found");
    Expect(HasTwin(blocks, 7), "the last twin block is found");
    Expect(!HasTwin(blocks, 5), "a block between twins has none");
    Expect(!HasTwin(blocks, 9), "an empty twin marks no block");
    Expect(!HasTwin({}, 2), "no twins: no block holds one");

    std::vector<u64> edge;
    AddTwinBlocks(edge, kBlockSize - 1, 2, kBlockShift);
    Expect(edge == std::vector<u64>{0, 1}, "a two-byte twin over a block edge marks both blocks");
}

void TestScan() {
    using BbEngine::VramEvict::AddTwinBlocks;
    using BbEngine::VramEvict::HasTwin;
    using BbEngine::VramEvict::SortTwinBlocks;

    const std::uint32_t now = 5000;
    std::vector<u64> twin_blocks;
    AddTwinBlocks(twin_blocks, 12 * kBlockSize + 100, 40, kBlockShift);
    SortTwinBlocks(twin_blocks);
    int evicted = 0, kept_twin = 0, kept_gpu = 0, kept_recent = 0;
    for (u64 block = 10; block < 16; ++block) {
        const Block facts{now, block == 15 ? now - 1 : now - 400, kCleanupSeconds, HasTwin(twin_blocks, block),
                          block == 11};
        switch (Decide(facts)) {
        case Verdict::Evict: ++evicted; break;
        case Verdict::KeepTwin: ++kept_twin; break;
        case Verdict::KeepGpuWritten: ++kept_gpu; break;
        case Verdict::KeepRecent: ++kept_recent; break;
        }
    }
    Expect(evicted == 3 && kept_twin == 1 && kept_gpu == 1 && kept_recent == 1,
           "a scan with twins on evicts the idle blocks without twins and keeps the twin block");
}

void TestPlanScan() {
    using BbEngine::VramEvict::kCountScanSeconds;
    using BbEngine::VramEvict::kTwinReportCap;
    using BbEngine::VramEvict::MayEvict;
    using BbEngine::VramEvict::PlanScan;
    using BbEngine::VramEvict::Scan;

    const std::uint32_t now = 7000;
    Expect(PlanScan(false, false, now, now, kTwinReportCap) == Scan::Evict, "twins off: the scan evicts as before");
    Expect(PlanScan(true, true, now, now, kTwinReportCap) == Scan::Evict,
           "twins on with LANTERN_TWIN_EVICT=1: the scan evicts every second, past the reports");
    Expect(PlanScan(true, false, now, now - kCountScanSeconds, 0) == Scan::CountOnly,
           "twins on with LANTERN_TWIN_EVICT=0: a count-only scan once its interval has passed");
    Expect(PlanScan(true, false, now, now - kCountScanSeconds + 1, 0) == Scan::Skip,
           "count only: no scan inside its interval");
    Expect(PlanScan(true, false, now, 0, kTwinReportCap - 1) == Scan::CountOnly,
           "count only: a scan while one report remains");
    Expect(PlanScan(true, false, now, 0, kTwinReportCap) == Scan::Skip,
           "count only: no scan once every report is used");
    Expect(MayEvict(Scan::Evict), "an evicting scan may evict");
    Expect(!MayEvict(Scan::CountOnly), "a count-only scan never evicts");
    Expect(!MayEvict(Scan::Skip), "a skipped scan never evicts");

    std::uint32_t last = 0;
    int count_scans = 0, evicting = 0;
    for (std::uint32_t second = 1000; second < 1060; ++second) {
        const Scan scan = PlanScan(true, false, second, last, 0);
        if (scan == Scan::CountOnly) {
            last = second;
            ++count_scans;
        }
        evicting += MayEvict(scan) ? 1 : 0;
    }
    Expect(count_scans == 6 && evicting == 0,
           "a minute of count-only seconds scans six times and evicts nothing");
}

int RunSuite() {
    failures = 0;
    TestDecide();
    TestTwinBlocks();
    TestScan();
    TestPlanScan();
    return failures;
}
}

int main() {
#ifdef LANTERN_VRAM_EVICT_MUTANTS
    using BbEngine::VramEvict::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        BbEngine::VramEvict::selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("vram evict mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("vram evict mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("vram evict mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("vram evict mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("vram evict: %d failures\n", failures);
        return 1;
    }
    std::printf("vram evict: all tests passed\n");
    return 0;
#endif
}
