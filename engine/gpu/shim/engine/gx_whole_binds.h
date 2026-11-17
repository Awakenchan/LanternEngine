// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx_whole.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace BbEngine::GxWholeBinds {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using GxWhole::Mode;

inline constexpr u32 kMaxStreams = 32;
inline constexpr u32 kSharpWords = 4;
inline constexpr u32 kReportFrames = 300;
inline constexpr u64 kSuspectCovered = 64;

#ifdef LANTERN_GX_BINDS_MUTANTS
enum class Mutant : u8 {
    None, SerialIgnored, GenerationIgnored, LastStreamIgnored, KeysIgnored, UnkeyedAllowed, NullStreamNeedsKey,
    PastEndHeld, IndexKeyIgnored, IndexSizeIgnored, SuspectMissed, StreamSlotKept, NotedKept, ResolveKeepsSerial,
    TakeKeepsSerial, IndexStreamSlotKept, SealsWithoutRecords, SealsIgnoreWhole, SealsIgnoreCompare, NoSealsHeld,
    UnsealedCountedPastEnd, VersionsIgnoreWhole, BindsByObjectForWhole, ObjectBindForAnyKey,
    SealsIgnoreBindsOff, VersionsIgnoreBindsOff, MemoGateIgnored, CpuWritesIgnored, InPlaceLost, SealsNotRevalidated,
    KeepKeepsSeals, SealsFoundEachHit, kCount
};
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

struct Stream {
    u64 base;
    u64 size;
    u64 key;
};

enum class Miss : u8 { Hit, First, Interleaved, Unkeyed, NotHeld, NotPersistent, Pipeline, Generation, Streams, Objects, StepRate, NoSeals, Unsealed,
                  Gated, Retired, CpuModified, kMisses };
inline constexpr u32 kMisses = static_cast<u32>(Miss::kMisses);
inline constexpr std::array<const char*, kMisses> kMissNames = {
    "hit", "first use", "another draw's streams in between", "a stream without a GX object", "a stream not inside its sealed GX buffer",
    "a range in the stream ring or a shadow", "other pipeline", "buffer cache changed", "other vertex streams", "other GX buffer versions", "other instance step rates",
    "no seal registry", "a stream in no sealed GX buffer", "buffer cache memos refused", "a sealed GX buffer retired since kept",
    "a range written by the CPU since kept"};

inline bool SealsForWhole(bool records, Mode whole, bool binds_off) {
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::SealsWithoutRecords)) return whole != Mode::Off && !binds_off;
    if (Mutated(Mutant::SealsIgnoreWhole)) return false;
    if (Mutated(Mutant::SealsIgnoreCompare)) return records && whole == Mode::Feed && !binds_off;
    if (Mutated(Mutant::SealsIgnoreBindsOff)) return records && whole != Mode::Off;
#endif
    return records && whole != Mode::Off && !binds_off;
}

inline bool VersionsWanted(bool compare, bool use, Mode whole, bool binds_off) {
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::VersionsIgnoreWhole)) return compare || use;
    if (Mutated(Mutant::VersionsIgnoreBindsOff)) return compare || use || whole != Mode::Off;
#endif
    return compare || use || (whole != Mode::Off && !binds_off);
}

inline bool BindsByObjectWanted(bool compare, bool use, Mode whole) {
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::BindsByObjectForWhole)) return compare || use || whole != Mode::Off;
#else
    (void)whole;
#endif
    return compare || use;
}

inline bool ObjectBind(u64 key, bool binds_by_object) {
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::ObjectBindForAnyKey)) return key != 0;
#endif
    return key != 0 && binds_by_object;
}

struct VertexKey {
    u64 pipeline;
    u64 bind_generation;
    u32 count;
    u32 step_rate[2];
    u32 pad;
    u32 sharps[kMaxStreams][kSharpWords];
    u64 keys[kMaxStreams];
};
static_assert(std::has_unique_object_representations_v<VertexKey>);

inline bool AllKeyed(std::span<const Stream> streams) {
    for (const Stream& s : streams) {
        bool needs = s.base != 0 && s.size != 0;
#ifdef LANTERN_GX_BINDS_MUTANTS
        if (Mutated(Mutant::NullStreamNeedsKey)) needs = true;
        if (Mutated(Mutant::UnkeyedAllowed)) needs = false;
#endif
        if (needs && !s.key) return false;
    }
    return true;
}

struct Extent {
    bool found;
    u32 id;
    u32 generation;
    u64 base;
    u64 bytes;
};

template <typename HoldsFn>
inline Miss HeldMiss(bool sealing_on, std::span<const Stream> streams, std::span<const Extent> extents, HoldsFn&& holds) {
    bool registry_checked = true;
#ifdef LANTERN_GX_BINDS_MUTANTS
    registry_checked = !Mutated(Mutant::NoSealsHeld);
#endif
    if (registry_checked && !sealing_on) return Miss::NoSeals;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const Stream& s = streams[i];
        if (s.base == 0 || s.size == 0) continue;
        if (i >= extents.size()) return Miss::Unsealed;
        const Extent& e = extents[i];
        Miss miss = !e.found ? Miss::Unsealed : holds(e.base, e.bytes, s.base, s.size) ? Miss::Hit : Miss::NotHeld;
#ifdef LANTERN_GX_BINDS_MUTANTS
        if (Mutated(Mutant::PastEndHeld) && e.found) miss = Miss::Hit;
        if (Mutated(Mutant::UnsealedCountedPastEnd) && miss == Miss::Unsealed) miss = Miss::NotHeld;
#endif
        if (miss != Miss::Hit) return miss;
    }
    return Miss::Hit;
}

inline Miss Classify(bool valid, u64 memo_serial, const VertexKey& memo, const VertexKey& now, u64 serial) {
    if (!valid) return Miss::First;
    bool serial_checked = true, generation_checked = true, keys_checked = true;
    u32 compared = now.count;
#ifdef LANTERN_GX_BINDS_MUTANTS
    serial_checked = !Mutated(Mutant::SerialIgnored);
    generation_checked = !Mutated(Mutant::GenerationIgnored);
    keys_checked = !Mutated(Mutant::KeysIgnored);
    if (Mutated(Mutant::LastStreamIgnored) && compared) --compared;
#endif
    if (serial_checked && memo_serial != serial) return Miss::Interleaved;
    if (memo.pipeline != now.pipeline || memo.count != now.count) return Miss::Pipeline;
    if (generation_checked && memo.bind_generation != now.bind_generation) return Miss::Generation;
    if (memo.step_rate[0] != now.step_rate[0] || memo.step_rate[1] != now.step_rate[1]) return Miss::StepRate;
    if (std::memcmp(memo.sharps, now.sharps, sizeof(u32) * kSharpWords * compared) != 0) return Miss::Streams;
    if (keys_checked && std::memcmp(memo.keys, now.keys, sizeof(u64) * compared) != 0) return Miss::Objects;
    return Miss::Hit;
}

struct IndexKey {
    u64 key;
    u64 address;
    u64 bytes;
    u64 bind_generation;
    u32 type;
    u32 pad;
};
static_assert(std::has_unique_object_representations_v<IndexKey>);

inline bool IndexHit(bool valid, const IndexKey& memo, const IndexKey& now) {
    if (!valid || !now.key) return false;
    bool size_checked = true, key_checked = true;
#ifdef LANTERN_GX_BINDS_MUTANTS
    size_checked = !Mutated(Mutant::IndexSizeIgnored);
    key_checked = !Mutated(Mutant::IndexKeyIgnored);
#endif
    return (!key_checked || memo.key == now.key) && memo.address == now.address && (!size_checked || memo.bytes == now.bytes) &&
           memo.bind_generation == now.bind_generation && memo.type == now.type;
}

struct Range {
    const void* buffer;
    u64 offset;
    u64 base;
    u64 size;
    bool persistent;
    bool in_place;
};

struct Seal {
    u32 id;
    u32 generation;
};

struct SealCache {
    std::array<Seal, kMaxStreams> seals{};
    u32 count = 0;
    bool checked = false;
};

inline void ForgetSeals(SealCache& cache) {
    cache.checked = false;
    cache.count = 0;
}

template <typename HoldsFn>
inline Miss SealOnce(bool sealing_on, std::span<const Stream> streams, std::span<const Extent> extents, HoldsFn&& holds, SealCache& cache) {
    const Miss miss = HeldMiss(sealing_on, streams, extents, holds);
    if (miss != Miss::Hit) return miss;
    cache.count = 0;
    for (std::size_t i = 0; i < streams.size() && i < extents.size(); ++i) {
        if (streams[i].base == 0 || streams[i].size == 0) continue;
        cache.seals[cache.count++] = {extents[i].id, extents[i].generation};
    }
    cache.checked = true;
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::SealsFoundEachHit)) cache.checked = false;
#endif
    return Miss::Hit;
}

template <typename ValidFn>
inline bool SealsValid(const SealCache& cache, ValidFn&& valid) {
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::SealsNotRevalidated)) return true;
#endif
    for (u32 i = 0; i < cache.count; ++i)
        if (!valid(cache.seals[i].id, cache.seals[i].generation)) return false;
    return true;
}

template <typename CleanFn>
inline bool RangesClean(std::span<const Range> ranges, CleanFn&& clean) {
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::CpuWritesIgnored)) return true;
#endif
    for (const Range& r : ranges) {
        bool in_place = r.in_place;
#ifdef LANTERN_GX_BINDS_MUTANTS
        if (Mutated(Mutant::InPlaceLost)) in_place = true;
#endif
        if (!clean(r.base, r.size, in_place)) return false;
    }
    return true;
}

template <typename SealFn, typename ValidFn, typename CleanFn>
inline Miss TakeMiss(bool memo_allowed, SealCache& cache, SealFn&& seal_once, ValidFn&& valid, std::span<const Range> ranges, CleanFn&& clean) {
    bool gated = !memo_allowed;
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::MemoGateIgnored)) gated = false;
#endif
    if (gated) return Miss::Gated;
    if (!cache.checked) {
        const Miss sealed = seal_once(cache);
        if (sealed != Miss::Hit) return sealed;
    } else if (!SealsValid(cache, valid)) {
        return Miss::Retired;
    }
    if (!RangesClean(ranges, clean)) return Miss::CpuModified;
    return Miss::Hit;
}

struct VertexChain {
    u64 serial = 0;
    u64 memo_serial = 0;
    bool valid = false;
    std::array<Range, kMaxStreams> noted{};
    std::array<Range, kMaxStreams> ranges{};
    u32 num_noted = 0;
    u32 num_ranges = 0;
    SealCache seals{};
};

inline void BeginResolve(VertexChain& chain) {
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (!Mutated(Mutant::ResolveKeepsSerial)) ++chain.serial;
    if (!Mutated(Mutant::NotedKept)) chain.num_noted = 0;
#else
    ++chain.serial;
    chain.num_noted = 0;
#endif
}

inline void Note(VertexChain& chain, const Range& range) {
    if (chain.num_noted < kMaxStreams) chain.noted[chain.num_noted++] = range;
}

inline bool AllPersistent(std::span<const Range> ranges) {
    for (const Range& r : ranges) {
        bool ok = r.persistent;
#ifdef LANTERN_GX_BINDS_MUTANTS
        if (Mutated(Mutant::StreamSlotKept)) ok = true;
#endif
        if (!ok) return false;
    }
    return true;
}

inline Miss MemoVerdict(bool keyed, std::span<const Range> noted) {
    if (!keyed) return Miss::Unkeyed;
    if (!AllPersistent(noted)) return Miss::NotPersistent;
    return Miss::Hit;
}

inline void Keep(VertexChain& chain) {
    chain.valid = true;
    chain.memo_serial = chain.serial;
    chain.ranges = chain.noted;
    chain.num_ranges = chain.num_noted;
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::KeepKeepsSeals)) return;
#endif
    ForgetSeals(chain.seals);
}

inline void Drop(VertexChain& chain) {
    chain.valid = false;
}

inline void Took(VertexChain& chain) {
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (!Mutated(Mutant::TakeKeepsSerial)) ++chain.serial;
#else
    ++chain.serial;
#endif
    chain.memo_serial = chain.serial;
}

inline Miss IndexVerdict(bool persistent) {
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::IndexStreamSlotKept)) return Miss::Hit;
#endif
    return persistent ? Miss::Hit : Miss::NotPersistent;
}

struct Counts {
    u64 resolves = 0, covered = 0, hits = 0, taken = 0, kept = 0, misses[kMisses]{};
    u64 compared = 0, differ = 0, differ_handles = 0, differ_offsets = 0, differ_layout = 0;
    u64 index_covered = 0, index_hits = 0, index_taken = 0, index_compared = 0, index_differ = 0, index_not_kept = 0, index_misses[kMisses]{};
    u64 vertex_cycles = 0, vertex_timed = 0, index_cycles = 0, index_timed = 0;
};
static_assert(std::is_trivially_copyable_v<Counts> && sizeof(Counts) % sizeof(u64) == 0);
inline constexpr std::size_t kCountWords = sizeof(Counts) / sizeof(u64);

inline double SkippedMicroseconds(u64 skipped, u64 cycles, u64 timed, double cycles_per_us) {
    if (!timed || cycles_per_us <= 0.0) return 0.0;
    return double(skipped) * (double(cycles) / double(timed)) / cycles_per_us;
}

inline bool InstrumentSuspect(Mode mode, const Counts& c) {
#ifdef LANTERN_GX_BINDS_MUTANTS
    if (Mutated(Mutant::SuspectMissed)) return false;
#endif
    if (c.resolves > 0 && c.covered == 0) return true;
    if (mode == Mode::Off) return false;
    if (c.misses[static_cast<u32>(Miss::NoSeals)] || c.index_misses[static_cast<u32>(Miss::NoSeals)]) return true;
    if (c.covered >= kSuspectCovered && c.hits == 0) return true;
    return c.differ > 0 && c.differ == c.compared;
}

inline std::size_t FormatSeals(char* out, std::size_t size, bool sealing, u32 live, u64 sealed, u64 notes) {
    using ull = unsigned long long;
    GxWhole::LineBuilder line(out, size);
    if (!sealing) {
        line.Add("GX whole binds: no seal registry, no draw's vertex or index binds can be memoized");
        return line.Used();
    }
    line.Add("GX whole binds: seal registry so far %u GX buffers sealed now, %llu sealed in all, %llu write and release notes", live, ull(sealed),
             ull(notes));
    return line.Used();
}

inline std::size_t FormatLine(char* out, std::size_t size, Mode mode, const Counts& c, double cycles_per_us) {
    using ull = unsigned long long;
    GxWhole::LineBuilder line(out, size);
    line.Add("GX whole binds (LANTERN_GX_WHOLE=%d): %llu vertex resolves, %llu covered, %llu memos kept; memo hits %llu (taken %llu)",
             GxWhole::ModeNumber(mode), ull(c.resolves), ull(c.covered), ull(c.kept), ull(c.hits), ull(c.taken));
    bool first = true;
    for (u32 r = 1; r < kMisses; ++r) {
        if (!c.misses[r]) continue;
        line.Add("%s%s %llu", first ? "; misses: " : ", ", kMissNames[r], ull(c.misses[r]));
        first = false;
    }
    line.Add("; compared %llu, differ %llu (handles %llu, offsets %llu, layout %llu); index covered %llu, hits %llu (taken %llu), compared %llu, differ %llu, not memoized (not held or not persistent) %llu",
             ull(c.compared), ull(c.differ), ull(c.differ_handles), ull(c.differ_offsets), ull(c.differ_layout), ull(c.index_covered),
             ull(c.index_hits), ull(c.index_taken), ull(c.index_compared), ull(c.index_differ), ull(c.index_not_kept));
    first = true;
    for (u32 r = 1; r < kMisses; ++r) {
        if (!c.index_misses[r]) continue;
        line.Add("%s%s %llu", first ? " (" : ", ", kMissNames[r], ull(c.index_misses[r]));
        first = false;
    }
    if (!first) line.Add(")");
    line.Add("; skipped (estimated from %llu timed vertex and %llu timed index resolves) %.1f us vertex, %.1f us index", ull(c.vertex_timed),
             ull(c.index_timed), SkippedMicroseconds(c.taken, c.vertex_cycles, c.vertex_timed, cycles_per_us),
             SkippedMicroseconds(c.index_taken, c.index_cycles, c.index_timed, cycles_per_us));
    return line.Used();
}
}
