// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace BbEngine::VramEvict {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum class Verdict : std::uint8_t { Evict, KeepRecent, KeepTwin, KeepGpuWritten };
enum class Scan : std::uint8_t { Skip, CountOnly, Evict };

inline constexpr u32 kTwinReportCap = 10;
inline constexpr u32 kCountScanSeconds = 10;

struct Block {
    u32 now = 0;
    u32 last_use = 0;
    u32 idle_seconds = 0;
    bool has_twin = false;
    bool gpu_written = false;
};

#ifdef LANTERN_VRAM_EVICT_MUTANTS
enum class Mutant : std::uint8_t { None, IdleAtLimitKept, TwinIgnored, GpuWrittenIgnored, TwinLastBlockMissed, TwinLookupFirstOnly,
                                    CountOnlyEvicts, CountPastReportCap, CountEveryScan, TwinEvictCountsOnly, kCount };
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

template <typename HasTwinFn, typename GpuWrittenFn>
inline Verdict Decide(u32 now, u32 last_use, u32 idle_seconds, HasTwinFn&& has_twin, GpuWrittenFn&& gpu_written) {
    const u32 unused = now - last_use;
#ifdef LANTERN_VRAM_EVICT_MUTANTS
    if (Mutated(Mutant::IdleAtLimitKept) && unused <= idle_seconds) return Verdict::KeepRecent;
    if (Mutated(Mutant::TwinIgnored) && unused >= idle_seconds) return gpu_written() ? Verdict::KeepGpuWritten : Verdict::Evict;
    if (Mutated(Mutant::GpuWrittenIgnored) && unused >= idle_seconds) return has_twin() ? Verdict::KeepTwin : Verdict::Evict;
#endif
    if (unused < idle_seconds) return Verdict::KeepRecent;
    if (has_twin()) return Verdict::KeepTwin;
    if (gpu_written()) return Verdict::KeepGpuWritten;
    return Verdict::Evict;
}

inline Scan PlanScan(bool twins_on, bool twin_evict, u32 now, u32 last_count_scan, u32 count_reports) {
    if (!twins_on) return Scan::Evict;
#ifdef LANTERN_VRAM_EVICT_MUTANTS
    if (Mutated(Mutant::TwinEvictCountsOnly)) twin_evict = false;
    if (Mutated(Mutant::CountPastReportCap)) count_reports = 0;
    if (Mutated(Mutant::CountEveryScan)) last_count_scan = now - kCountScanSeconds;
#endif
    if (twin_evict) return Scan::Evict;
    if (count_reports >= kTwinReportCap) return Scan::Skip;
    if (now - last_count_scan < kCountScanSeconds) return Scan::Skip;
    return Scan::CountOnly;
}

inline bool MayEvict(Scan scan) {
#ifdef LANTERN_VRAM_EVICT_MUTANTS
    if (Mutated(Mutant::CountOnlyEvicts)) return scan != Scan::Skip;
#endif
    return scan == Scan::Evict;
}

inline Verdict Decide(const Block& block) {
    return Decide(block.now, block.last_use, block.idle_seconds, [&] { return block.has_twin; },
                  [&] { return block.gpu_written; });
}

inline void AddTwinBlocks(std::vector<u64>& blocks, u64 start, u64 size, u32 block_shift) {
    if (size == 0) return;
    const u64 first = start >> block_shift;
    u64 last = (start + size - 1) >> block_shift;
#ifdef LANTERN_VRAM_EVICT_MUTANTS
    if (Mutated(Mutant::TwinLastBlockMissed) && last > first) --last;
#endif
    for (u64 block = first; block <= last; ++block) blocks.push_back(block);
}

inline void SortTwinBlocks(std::vector<u64>& blocks) {
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
}

inline bool HasTwin(const std::vector<u64>& sorted_blocks, u64 block) {
#ifdef LANTERN_VRAM_EVICT_MUTANTS
    if (Mutated(Mutant::TwinLookupFirstOnly)) return !sorted_blocks.empty() && sorted_blocks.front() == block;
#endif
    return std::binary_search(sorted_blocks.begin(), sorted_blocks.end(), block);
}
}
