// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>

#include "gx_sealed.h"
#include "../lantern_switches.h"

namespace BbEngine::VertexCensus {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum Class : std::uint8_t { SealedGx, DynamicGx, PastEnd, UploadRing, Other, kClasses };
enum Path : std::uint8_t { StreamCopy, Arena, OtherPath, kPaths };
enum Hit : std::uint8_t { NoBuffer, InSealed, InDynamic, RunsPast };

inline constexpr const char* kClassNames[kClasses] = {"sealed GX", "dynamic GX", "past a GX buffer's end", "upload ring", "other"};
inline constexpr const char* kPathNames[kPaths] = {"stream copy", "arena", "other"};

enum Fate : std::uint8_t { FateHit, FateRefill, FateStreamCopied, FateRetired, FateDynamic, FateRing, FateOtherGx, FateNotGx, kFates };
inline constexpr u32 kCells = u32(kFates) + u32(GxSealed::kRetiredKinds);
inline constexpr const char* kFateNames[kFates] = {"sealed hit", "sealed refill", "stream-copied", "retired", "dynamic GX", "upload ring", "other GX", "not GX"};

inline u32 CellOf(Fate fate, GxSealed::Retired why) {
    return fate == FateRetired ? u32(kFates) + u32(why) : u32(fate);
}
inline bool Covered(u32 cell) {
    return cell == FateHit || cell == FateRefill;
}

inline constexpr u32 kRecords = u32(GxSealed::kHolders) + 2;
inline constexpr u32 kRecordPastEnd = GxSealed::kHolders, kRecordNone = GxSealed::kHolders + 1;
inline constexpr const char* kRecordNames[kRecords] = {"immutable", "default with data", "default without data", "dynamic",
                                                       "other GX", "past a GX buffer's end", "no GX holder"};

enum Gate : std::uint8_t { GateTried, GateModeOff, GateRegistryOff, GateCapture, GateShadows, GateChunkBuffers, GateBufferStats, GateHleObtain, kGates };
inline constexpr const char* kGateNames[kGates] = {"tried", "switch off", "no registry", "frame capture", "BB_SHADOWS",
                                                   "chunk buffers", "buffer stats", "HLE obtain"};

inline constexpr u32 StreamRecords = 0;
inline constexpr u32 StreamBytes = StreamRecords + kRecords;
inline constexpr u32 IndexRecords = StreamBytes + kRecords;
inline constexpr u32 IndexBytes = IndexRecords + kRecords;
inline constexpr u32 Gates = IndexBytes + kRecords;
inline constexpr u32 SealedDraws = Gates + kGates;
inline constexpr u32 UnsealedDraws = SealedDraws + 1;
inline constexpr u32 SealedHits = UnsealedDraws + 1;
inline constexpr u32 SealedMisses = SealedHits + 1;
inline constexpr u32 IndexChecked = SealedMisses + 1;
inline constexpr u32 IndexSealed = IndexChecked + 1;
inline constexpr u32 SealedPastEnd = IndexSealed + 1;
inline constexpr u32 kExtras = SealedPastEnd + 1;

struct Ring {
    u64 base = 0, bytes = 0;
    bool Holds(u64 address, u64 size) const {
        return bytes && address >= base && address - base < bytes && size <= bytes - (address - base);
    }
};

struct Window {
    u64 draws = 0, empty_draws = 0, sealed_draws = 0;
    u64 streams[kClasses]{}, stream_paths[kPaths]{}, ring_gx = 0;
    u64 indices[kClasses]{}, index_paths[kPaths]{};
    u64 lookups = 0, misses = 0;
    u64 fates[kCells]{}, index_fates[kCells]{}, blocked_any[kCells]{}, blocked_only[kCells]{};
    u64 fated_draws = 0, covered_draws = 0, mixed_draws = 0, index_uncovered = 0;
    u64 extra[kExtras]{};
    bool last_covered = false;
};

struct Memo {
    static constexpr u32 kSetBits = 14, kWays = 4, kFlushEvery = 256;
    struct Slot {
        u64 base = 0;
        u32 size = 0, stamp = 0;
    };
    struct alignas(64) Set {
        Slot ways[kWays];
    };
    static constexpr u32 kSealBits = 15, kSealWays = 4;
    struct Seal {
        u64 base = 0, layout = 0, seals = 0;
        u32 size = 0, id = 0, generation = 0, cell = 0;
    };
    Set sets[1u << kSetBits];
    Seal seals[1u << kSealBits];
    u32 seal_victim = 0;
    Window counts;
    u32 unflushed = 0;
};

class Buffers {
public:
    static constexpr u32 kPageShift = 16, kPages = 1u << 16, kMaxBump = 4096, kSteps = 8;

    Buffers() : pages_(new std::atomic<u32>[kPages]) {
        for (u32 i = 0; i < kPages; ++i) pages_[i].store(0, std::memory_order_relaxed);
    }

    void Add(u64 base, u64 size, bool dynamic) {
        Add(base, size, dynamic, dynamic ? GxSealed::HolderDynamic : GxSealed::HolderOtherGx);
    }

    void Add(u64 base, u64 size, bool dynamic, GxSealed::Holder holder) {
        if (!size) return;
        std::scoped_lock lock(mutex_);
        if (Containing(base, 1)) ++overlapped_;
        const auto [at, fresh] = by_base_.try_emplace(base, Entry{size, dynamic, holder});
        if (!fresh) {
            BumpLocked(at->first, at->second.size);
            at->second = {size, dynamic, holder};
        }
        BumpLocked(base, size);
        ++made_[dynamic];
    }

    void Remove(u64 base) {
        std::scoped_lock lock(mutex_);
        const auto at = by_base_.find(base);
        if (at == by_base_.end()) {
            ++unknown_releases_;
            return;
        }
        BumpLocked(at->first, at->second.size);
        by_base_.erase(at);
        ++released_;
    }

    Hit Find(u64 address, u64 size, u32* record = nullptr) const {
        std::scoped_lock lock(mutex_);
        return FindLocked(address, size, record);
    }

    Hit Find(u64 address, u64 size, Memo& memo, u32* record = nullptr) const {
        const u64 hash = (address ^ (size << 40)) * 0x9E3779B97F4A7C15ull;
        Memo::Set& set = memo.sets[hash >> (64 - Memo::kSetBits)];
        const u32 stamp = (pages_[(address >> kPageShift) & (kPages - 1)].load(std::memory_order_acquire) +
                           global_.load(std::memory_order_acquire) * 0x9E3779B1u) << kStampBits;
        ++memo.counts.lookups;
        Memo::Slot* victim = nullptr;
        for (Memo::Slot& slot : set.ways) {
            if (slot.base == address && slot.size == u32(size) && address) {
                if ((slot.stamp & ~kStampMask) == stamp) {
                    if (record) *record = (slot.stamp & kStampMask) >> kHitBits;
                    return Hit(slot.stamp & kHitMask);
                }
                victim = &slot;
            } else if (!slot.base && !victim) {
                victim = &slot;
            }
        }
        if (!victim) victim = &set.ways[(hash >> 20) & (Memo::kWays - 1)];
        ++memo.counts.misses;
        u32 found = kRecordNone;
        const Hit hit = Find(address, size, &found);
        *victim = {address, u32(size), stamp | hit | (found << kHitBits)};
        if (record) *record = found;
        return hit;
    }

    struct Stats {
        u64 made = 0, made_dynamic = 0, released = 0, unknown_releases = 0, overlapped = 0, live = 0;
    };
    Stats Snapshot() const {
        std::scoped_lock lock(mutex_);
        return {made_[0] + made_[1], made_[1], released_, unknown_releases_, overlapped_, by_base_.size()};
    }

private:
    static constexpr u32 kHitBits = 2, kStampBits = 5;
    static constexpr u32 kHitMask = (1u << kHitBits) - 1, kStampMask = (1u << kStampBits) - 1;
    static_assert(kRecords <= (1u << (kStampBits - kHitBits)));

    struct Entry {
        u64 size;
        bool dynamic;
        GxSealed::Holder holder;
    };

    const Entry* Containing(u64 address, u64 size, Hit* hit = nullptr) const {
        auto at = by_base_.upper_bound(address);
        const Entry* partial = nullptr;
        for (u32 step = 0; step < kSteps && at != by_base_.begin(); ++step) {
            --at;
            const u64 offset = address - at->first;
            if (offset >= at->second.size) continue;
            if (size <= at->second.size - offset) {
                if (hit) *hit = at->second.dynamic ? InDynamic : InSealed;
                return &at->second;
            }
            if (!partial) partial = &at->second;
        }
        if (partial && hit) *hit = RunsPast;
        return partial;
    }

    Hit FindLocked(u64 address, u64 size, u32* record) const {
        Hit hit = NoBuffer;
        const Entry* entry = Containing(address, size ? size : 1, &hit);
        if (record) *record = hit == RunsPast ? kRecordPastEnd : entry ? u32(entry->holder) : kRecordNone;
        return hit;
    }

    void BumpLocked(u64 base, u64 size) {
        const u64 first = base >> kPageShift, last = (base + size - 1) >> kPageShift;
        if (last - first >= kMaxBump) {
            global_.fetch_add(1, std::memory_order_release);
            return;
        }
        for (u64 page = first; page <= last; ++page) pages_[page & (kPages - 1)].fetch_add(1, std::memory_order_release);
    }

    mutable std::mutex mutex_;
    std::map<u64, Entry> by_base_;
    std::unique_ptr<std::atomic<u32>[]> pages_;
    std::atomic<u32> global_{0};
    u64 made_[2]{}, released_ = 0, unknown_releases_ = 0, overlapped_ = 0;
};

inline Class ClassOf(Hit hit, Ring ring, u64 address, u64 size) {
    if (hit == InSealed) return SealedGx;
    if (hit == InDynamic) return DynamicGx;
    if (hit == RunsPast) return PastEnd;
    return ring.Holds(address, size) ? UploadRing : Other;
}

inline Class Classify(const Buffers* buffers, Memo& memo, Ring ring, u64 address, u64 size, u32* record = nullptr) {
    if (record) *record = kRecordNone;
    return ClassOf(buffers ? buffers->Find(address, size, memo, record) : NoBuffer, ring, address, size);
}

inline void Record(Memo& memo, u32 records, u32 record, u64 size) {
    const u32 at = record < kRecords ? record : kRecordNone;
    ++memo.counts.extra[records + at];
    memo.counts.extra[records + kRecords + at] += size;
}

inline void Count(Memo& memo, u32 counter) {
    if (counter < kExtras) ++memo.counts.extra[counter];
}

inline u32 Sort(const GxSealed::Registry* sealed, Memo& memo, Class c, u64 address, u64 size, Path path, u64 layout) {
    if (c == DynamicGx) return FateDynamic;
    if (c == UploadRing) return FateRing;
    if (c == Other) return FateNotGx;
    if (!sealed) return FateOtherGx;
    const u64 hash = (address ^ (size << 40)) * 0x9E3779B97F4A7C15ull;
    Memo::Seal* set = &memo.seals[(hash >> (64 - Memo::kSealBits)) & ~u64(Memo::kSealWays - 1)];
    Memo::Seal* hit = nullptr;
    for (u32 way = 0; way < Memo::kSealWays && !hit; ++way) {
        if (set[way].base == address && set[way].size == u32(size)) hit = &set[way];
    }
    Memo::Seal& slot = hit ? *hit : set[memo.seal_victim++ & (Memo::kSealWays - 1)];
    const u64 seals = sealed->Sealed();
    if (slot.base == address && slot.size == u32(size)) {
        if (slot.cell <= FateStreamCopied && sealed->Valid({slot.id, slot.generation, 0, 0})) {
            if (path == StreamCopy) {
                slot.cell = FateStreamCopied;
            } else if (slot.cell != FateStreamCopied && slot.layout == layout) {
                slot.cell = FateHit;
            } else {
                slot.cell = FateRefill;
                slot.layout = layout;
            }
            return slot.cell;
        }
        if (slot.cell > FateStreamCopied && slot.seals == seals) return slot.cell;
    }
    GxSealed::Hit found;
    if (sealed->Find(address, size, &found) && GxSealed::Holds(found.base, found.size, address, size)) {
        const u32 cell = path == StreamCopy ? u32(FateStreamCopied) : u32(FateRefill);
        slot = {address, layout, seals, u32(size), found.id, found.generation, cell};
        return cell;
    }
    GxSealed::Retired why = GxSealed::ByCpuWrite;
    const u32 cell = c == SealedGx && sealed->RetiredAt(address, &why) ? CellOf(FateRetired, why) : u32(FateOtherGx);
    slot = {address, layout, seals, u32(size), 0, 0, cell};
    return cell;
}

struct Totals {
    std::atomic<u64> draws{0}, empty_draws{0}, sealed_draws{0};
    std::atomic<u64> streams[kClasses]{}, stream_paths[kPaths]{}, ring_gx{0};
    std::atomic<u64> indices[kClasses]{}, index_paths[kPaths]{};
    std::atomic<u64> lookups{0}, misses{0};
    std::atomic<u64> extra[kExtras]{};
};

inline Totals g_totals;
inline Buffers* g_buffers = nullptr;
inline const GxSealed::Registry* g_registry = nullptr;

struct CellTotals {
    std::atomic<u64> fates[kCells]{}, index_fates[kCells]{}, blocked_any[kCells]{}, blocked_only[kCells]{};
    std::atomic<u64> fated_draws{0}, covered_draws{0}, mixed_draws{0}, index_uncovered{0};
};
inline CellTotals g_cells;

inline bool On() {
    return Lantern::Switches::Get().frame_stats;
}

struct Draw {
    Window& counts;
    u32 streams = 0, sealed = 0, fated = 0, covered = 0;
    u64 blockers = 0;
    explicit Draw(Memo& memo) : counts(memo.counts) {}
    void Sorted(u32 cell) {
        ++fated;
        ++counts.fates[cell];
        if (Covered(cell)) ++covered;
        else blockers |= u64(1) << cell;
    }
    void Stream(Class c, Path p, bool in_ring) {
        ++streams;
        sealed += c == SealedGx;
        ++counts.streams[c];
        ++counts.stream_paths[p];
        counts.ring_gx += in_ring && (c == SealedGx || c == DynamicGx || c == PastEnd);
    }
    void Finish() {
        ++counts.draws;
        counts.empty_draws += !streams;
        counts.sealed_draws += streams && sealed == streams;
        counts.last_covered = fated && !blockers;
        if (!fated) return;
        ++counts.fated_draws;
        counts.covered_draws += !blockers;
        counts.mixed_draws += blockers && covered;
        const bool only = blockers && !(blockers & (blockers - 1));
        for (u32 cell = 0; cell < kCells; ++cell) {
            if (!((blockers >> cell) & 1)) continue;
            ++counts.blocked_any[cell];
            counts.blocked_only[cell] += only;
        }
    }
};

inline void Index(Memo& memo, Class c, Path p) {
    ++memo.counts.indices[c];
    ++memo.counts.index_paths[p];
}

inline bool SealedCell(u32 cell) {
    return cell == FateHit || cell == FateRefill || cell == FateStreamCopied;
}

inline void IndexCoverage(Memo& memo, bool own_tried, u32 cell) {
    if (!own_tried) return;
    ++memo.counts.extra[IndexChecked];
    memo.counts.extra[IndexSealed] += SealedCell(cell);
}

inline void IndexFate(Memo& memo, u32 cell) {
    ++memo.counts.index_fates[cell];
    memo.counts.index_uncovered += memo.counts.last_covered && !Covered(cell);
    memo.counts.last_covered = false;
}

inline void Publish(Memo& memo) {
    const auto add = [](std::atomic<u64>& counter, u64& by) {
        if (!by) return;
        counter.fetch_add(by, std::memory_order_relaxed);
        by = 0;
    };
    Window& w = memo.counts;
    add(g_totals.draws, w.draws);
    add(g_totals.empty_draws, w.empty_draws);
    add(g_totals.sealed_draws, w.sealed_draws);
    for (int c = 0; c < kClasses; ++c) {
        add(g_totals.streams[c], w.streams[c]);
        add(g_totals.indices[c], w.indices[c]);
    }
    for (int p = 0; p < kPaths; ++p) {
        add(g_totals.stream_paths[p], w.stream_paths[p]);
        add(g_totals.index_paths[p], w.index_paths[p]);
    }
    add(g_totals.ring_gx, w.ring_gx);
    add(g_totals.lookups, w.lookups);
    add(g_totals.misses, w.misses);
    for (u32 c = 0; c < kCells; ++c) {
        add(g_cells.fates[c], w.fates[c]);
        add(g_cells.index_fates[c], w.index_fates[c]);
        add(g_cells.blocked_any[c], w.blocked_any[c]);
        add(g_cells.blocked_only[c], w.blocked_only[c]);
    }
    add(g_cells.fated_draws, w.fated_draws);
    add(g_cells.covered_draws, w.covered_draws);
    add(g_cells.mixed_draws, w.mixed_draws);
    add(g_cells.index_uncovered, w.index_uncovered);
    for (u32 e = 0; e < kExtras; ++e) add(g_totals.extra[e], w.extra[e]);
    memo.unflushed = 0;
}

inline void Flush(Memo& memo) {
    if (++memo.unflushed >= Memo::kFlushEvery) Publish(memo);
}

inline Window Read() {
    Window w;
    const auto load = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    w.draws = load(g_totals.draws);
    w.empty_draws = load(g_totals.empty_draws);
    w.sealed_draws = load(g_totals.sealed_draws);
    for (int c = 0; c < kClasses; ++c) {
        w.streams[c] = load(g_totals.streams[c]);
        w.indices[c] = load(g_totals.indices[c]);
    }
    for (int p = 0; p < kPaths; ++p) {
        w.stream_paths[p] = load(g_totals.stream_paths[p]);
        w.index_paths[p] = load(g_totals.index_paths[p]);
    }
    w.ring_gx = load(g_totals.ring_gx);
    w.lookups = load(g_totals.lookups);
    w.misses = load(g_totals.misses);
    for (u32 c = 0; c < kCells; ++c) {
        w.fates[c] = load(g_cells.fates[c]);
        w.index_fates[c] = load(g_cells.index_fates[c]);
        w.blocked_any[c] = load(g_cells.blocked_any[c]);
        w.blocked_only[c] = load(g_cells.blocked_only[c]);
    }
    w.fated_draws = load(g_cells.fated_draws);
    w.covered_draws = load(g_cells.covered_draws);
    w.mixed_draws = load(g_cells.mixed_draws);
    w.index_uncovered = load(g_cells.index_uncovered);
    for (u32 e = 0; e < kExtras; ++e) w.extra[e] = load(g_totals.extra[e]);
    return w;
}

inline Window Take(Window& last) {
    const Window now = Read();
    Window d;
    d.draws = now.draws - last.draws;
    d.empty_draws = now.empty_draws - last.empty_draws;
    d.sealed_draws = now.sealed_draws - last.sealed_draws;
    for (int c = 0; c < kClasses; ++c) {
        d.streams[c] = now.streams[c] - last.streams[c];
        d.indices[c] = now.indices[c] - last.indices[c];
    }
    for (int p = 0; p < kPaths; ++p) {
        d.stream_paths[p] = now.stream_paths[p] - last.stream_paths[p];
        d.index_paths[p] = now.index_paths[p] - last.index_paths[p];
    }
    d.ring_gx = now.ring_gx - last.ring_gx;
    d.lookups = now.lookups - last.lookups;
    d.misses = now.misses - last.misses;
    for (u32 c = 0; c < kCells; ++c) {
        d.fates[c] = now.fates[c] - last.fates[c];
        d.index_fates[c] = now.index_fates[c] - last.index_fates[c];
        d.blocked_any[c] = now.blocked_any[c] - last.blocked_any[c];
        d.blocked_only[c] = now.blocked_only[c] - last.blocked_only[c];
    }
    d.fated_draws = now.fated_draws - last.fated_draws;
    d.covered_draws = now.covered_draws - last.covered_draws;
    d.mixed_draws = now.mixed_draws - last.mixed_draws;
    d.index_uncovered = now.index_uncovered - last.index_uncovered;
    for (u32 e = 0; e < kExtras; ++e) d.extra[e] = now.extra[e] - last.extra[e];
    last = now;
    return d;
}

inline int Format(char* out, std::size_t room, const char* when, const Window& w, const Buffers::Stats* gx) {
    std::size_t used = 0;
    const auto put = [&](const char* format, auto... args) {
        if (used >= room) return;
        const int n = std::snprintf(out + used, room - used, format, args...);
        if (n > 0) used += static_cast<std::size_t>(n);
    };
    const auto pct = [](u64 part, u64 all) { return all ? 100.0 * double(part) / double(all) : 0.0; };
    u64 streams = 0, indices = 0;
    for (int c = 0; c < kClasses; ++c) {
        streams += w.streams[c];
        indices += w.indices[c];
    }
    const u64 with_streams = w.draws - w.empty_draws;
    put("Vertex data (%s): %llu draws (%llu without streams); streams %llu:", when, static_cast<unsigned long long>(w.draws),
        static_cast<unsigned long long>(w.empty_draws), static_cast<unsigned long long>(streams));
    for (int c = 0; c < kClasses; ++c) put("%s %.1f%% %s", c ? "," : "", pct(w.streams[c], streams), kClassNames[c]);
    put(" (%.1f%% are GX buffers inside the upload ring); bound by", pct(w.ring_gx, streams));
    for (int p = 0; p < kPaths; ++p) put("%s %s %.1f%%", p ? "," : "", kPathNames[p], pct(w.stream_paths[p], streams));
    put("; draws fully in sealed GX: %.1f%%; index %llu:", pct(w.sealed_draws, with_streams), static_cast<unsigned long long>(indices));
    for (int c = 0; c < kClasses; ++c) put("%s %.1f%% %s", c ? "," : "", pct(w.indices[c], indices), kClassNames[c]);
    put("%s", "; bound by");
    for (int p = 0; p < kPaths; ++p) put("%s %s %.1f%%", p ? "," : "", kPathNames[p], pct(w.index_paths[p], indices));
    put("; memo misses %.2f%% of %llu lookups", pct(w.misses, w.lookups), static_cast<unsigned long long>(w.lookups));
    if (gx) {
        put("; GX buffers so far %llu made (%llu dynamic, %llu inside another), %llu released (%llu not seen made), %llu live",
            static_cast<unsigned long long>(gx->made), static_cast<unsigned long long>(gx->made_dynamic),
            static_cast<unsigned long long>(gx->overlapped), static_cast<unsigned long long>(gx->released),
            static_cast<unsigned long long>(gx->unknown_releases), static_cast<unsigned long long>(gx->live));
    } else {
        put("%s", "; GX buffers not watched");
    }
    return static_cast<int>(used < room ? used : room - 1);
}

inline void CellName(char* out, std::size_t room, u32 cell) {
    if (cell < kFates) std::snprintf(out, room, "%s", kFateNames[cell]);
    else std::snprintf(out, room, "retired by %s", GxSealed::kRetiredNames[cell - kFates]);
}

struct RetiredCounts {
    u64 by[GxSealed::kRetiredKinds]{};
    u64 sealed = 0;
    u32 live = 0;
};

inline RetiredCounts Retirements(const GxSealed::Registry* registry) {
    RetiredCounts r;
    if (!registry) return r;
    for (int why = 0; why < GxSealed::kRetiredKinds; ++why) r.by[why] = registry->RetiredCount(GxSealed::Retired(why));
    r.sealed = registry->Sealed();
    r.live = registry->Live();
    return r;
}

inline int FormatObjects(char* out, std::size_t room, const char* when, const Window& w, const RetiredCounts* registry) {
    std::size_t used = 0;
    const auto put = [&](const char* format, auto... args) {
        if (used >= room) return;
        const int n = std::snprintf(out + used, room - used, format, args...);
        if (n > 0) used += static_cast<std::size_t>(n);
    };
    const auto pct = [](u64 part, u64 all) { return all ? 100.0 * double(part) / double(all) : 0.0; };
    char name[48];
    u64 streams = 0, indices = 0;
    for (u32 c = 0; c < kCells; ++c) {
        streams += w.fates[c];
        indices += w.index_fates[c];
    }
    put("Vertex data by GX object (%s): %llu draws, %.1f%% fully covered, %.1f%% partly; streams %llu:", when,
        static_cast<unsigned long long>(w.fated_draws), pct(w.covered_draws, w.fated_draws), pct(w.mixed_draws, w.fated_draws),
        static_cast<unsigned long long>(streams));
    for (u32 c = 0; c < kCells; ++c) {
        if (!w.fates[c]) continue;
        CellName(name, sizeof name, c);
        put(" %s %.1f%%;", name, pct(w.fates[c], streams));
    }
    put(" covered streams whose index is not %.1f%% of draws; index %llu:", pct(w.index_uncovered, w.fated_draws),
        static_cast<unsigned long long>(indices));
    for (u32 c = 0; c < kCells; ++c) {
        if (!w.index_fates[c]) continue;
        CellName(name, sizeof name, c);
        put(" %s %.1f%%;", name, pct(w.index_fates[c], indices));
    }
    put("%s", " draws blocked by (any, only):");
    for (u32 c = 0; c < kCells; ++c) {
        if (!w.blocked_any[c]) continue;
        CellName(name, sizeof name, c);
        put(" %s %.1f%% %.1f%%;", name, pct(w.blocked_any[c], w.fated_draws), pct(w.blocked_only[c], w.fated_draws));
    }
    if (registry) {
        u64 guest = 0, own = 0;
        for (int why = 0; why < GxSealed::kRetiredKinds; ++why) {
            if (why == GxSealed::ByOverlap) continue;
            (GxSealed::OwnSite(GxSealed::Retired(why)) ? own : guest) += registry->by[why];
        }
        put(" registry %llu sealed, %u live; retired %llu by overlapping seals, %llu at our own sites, %llu by the guest:",
            static_cast<unsigned long long>(registry->sealed), registry->live,
            static_cast<unsigned long long>(registry->by[GxSealed::ByOverlap]), static_cast<unsigned long long>(own),
            static_cast<unsigned long long>(guest));
        for (int why = 0; why < GxSealed::kRetiredKinds; ++why) {
            if (registry->by[why]) put(" %s %llu;", GxSealed::kRetiredNames[why], static_cast<unsigned long long>(registry->by[why]));
        }
    } else {
        put("%s", " no sealed registry");
    }
    return static_cast<int>(used < room ? used : room - 1);
}

inline int FormatOwn(char* out, std::size_t room, const char* when, const Window& w) {
    std::size_t used = 0;
    const auto put = [&](const char* format, auto... args) {
        if (used >= room) return;
        const int n = std::snprintf(out + used, room - used, format, args...);
        if (n > 0) used += static_cast<std::size_t>(n);
    };
    const auto pct = [](u64 part, u64 all) { return all ? 100.0 * double(part) / double(all) : 0.0; };
    const auto records = [&](const char* title, u32 counts, u32 bytes) {
        u64 n = 0, b = 0;
        for (u32 r = 0; r < kRecords; ++r) {
            n += w.extra[counts + r];
            b += w.extra[bytes + r];
        }
        put(" %s %llu:", title, static_cast<unsigned long long>(n));
        for (u32 r = 0; r < kRecords; ++r) {
            if (w.extra[counts + r])
                put(" %s %.1f%% (%.1f%% of bytes);", kRecordNames[r], pct(w.extra[counts + r], n), pct(w.extra[bytes + r], b));
        }
    };
    put("Own buffers (%s): by GX holder,", when);
    records("vertex records", StreamRecords, StreamBytes);
    records("index ranges", IndexRecords, IndexBytes);
    u64 gated = 0;
    for (u32 g = 0; g < kGates; ++g) gated += w.extra[Gates + g];
    put(" own-buffers gate per draw %llu:", static_cast<unsigned long long>(gated));
    for (u32 g = 0; g < kGates; ++g) {
        if (w.extra[Gates + g]) put(" %s %.1f%%;", kGateNames[g], pct(w.extra[Gates + g], gated));
    }
    const u64 tried = w.extra[SealedDraws] + w.extra[UnsealedDraws];
    put(" draws all sealed %llu of %llu tried (%.1f%%); sealed lookups %llu hits, %llu misses;",
        static_cast<unsigned long long>(w.extra[SealedDraws]), static_cast<unsigned long long>(tried), pct(w.extra[SealedDraws], tried),
        static_cast<unsigned long long>(w.extra[SealedHits]), static_cast<unsigned long long>(w.extra[SealedMisses]));
    put(" index ranges in a live sealed buffer %llu of %llu checked (%.1f%%)", static_cast<unsigned long long>(w.extra[IndexSealed]),
        static_cast<unsigned long long>(w.extra[IndexChecked]), pct(w.extra[IndexSealed], w.extra[IndexChecked]));
    put("; sealed lookups refused past the sealed end %llu", static_cast<unsigned long long>(w.extra[SealedPastEnd]));
    return static_cast<int>(used < room ? used : room - 1);
}

inline void Print(const char* when, const Window& w) {
    char line[1536];
    Buffers::Stats stats;
    const Buffers::Stats* gx = nullptr;
    if (g_buffers) {
        stats = g_buffers->Snapshot();
        gx = &stats;
    }
    Format(line, sizeof line, when, w, gx);
    std::printf("%s\n", line);
    char objects[2048];
    RetiredCounts retired;
    if (g_registry) retired = Retirements(g_registry);
    FormatObjects(objects, sizeof objects, when, w, g_registry ? &retired : nullptr);
    std::printf("%s\n", objects);
    char own[2048];
    FormatOwn(own, sizeof own, when, w);
    std::printf("%s\n", own);
}
}
