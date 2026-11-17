// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx_whole.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace BbEngine::GxWholeOwedBlocks {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using GxWhole::Value;

inline constexpr u32 kBlockWords = 32;
inline constexpr u32 kSampleEvery = GxWhole::kSampleEvery;

#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
enum class Mutant : u8 {
    None, IgnoresMask, SkipsPreWrites, SkipsLastWord, WithholdsOnMaskedBits, UncoveredWithheld, EligibleAtReset,
    EligibleAfterTakeOff, KeepsSentBlocks, DropsOwed, KeepsLastCoverage, PreWriteAfterValues, NeverSamples, NoWrapClear,
    kCount
};
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

struct PreWrite {
    u32 reg;
    u32 value;
};

enum class Withheld : u8 { None, Blocks, Sample };

enum class Verdict : u8 { Withhold, OutsideRecord, MaskedBits, kCount };
inline constexpr std::array<const char*, static_cast<std::size_t>(Verdict::kCount)> kVerdictNames = {
    "withheld", "a word outside the record changed", "bits outside a value's mask changed"};

inline bool Eligible(bool feed, bool carried, bool reset, bool take_off) {
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
    if (Mutated(Mutant::EligibleAtReset)) reset = false;
    if (Mutated(Mutant::EligibleAfterTakeOff)) take_off = false;
#endif
    return feed && carried && !reset && !take_off;
}

inline bool SampleDraw(u64 judged_index) {
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
    if (Mutated(Mutant::NeverSamples)) return false;
#endif
    return judged_index % kSampleEvery == 0;
}

class Coverage {
public:
    explicit Coverage(u32 words) : words_(words), entries_(words) {}

    void PresetGeneration(u32 generation) {
        generation_ = generation;
    }

    void Build(std::span<const PreWrite> pre, std::span<const Value> values) {
        if (++generation_ == 0) {
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
            if (!Mutated(Mutant::NoWrapClear)) {
#endif
                for (Entry& e : entries_) e.stamp = 0;
                generation_ = 1;
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
            }
#endif
        }
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
        if (Mutated(Mutant::KeepsLastCoverage) && generation_ > 2) --generation_;
        if (Mutated(Mutant::SkipsPreWrites)) pre = {};
        if (Mutated(Mutant::PreWriteAfterValues)) {
            for (const Value& v : values) Put(v.reg, v.value, v.mask);
            for (const PreWrite& p : pre) Put(p.reg, p.value, ~0u);
            return;
        }
#endif
        for (const PreWrite& p : pre) Put(p.reg, p.value, ~0u);
        for (const Value& v : values) {
            u32 mask = v.mask;
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
            if (Mutated(Mutant::IgnoresMask)) mask = ~0u;
#endif
            Put(v.reg, v.value, mask);
        }
    }

    bool Covers(u32 word) const {
        return word < words_ && entries_[word].stamp == generation_;
    }

    u32 Apply(u32 word, u32 start) const {
        if (!Covers(word)) return start;
        const Entry& e = entries_[word];
        return (start & e.keep) | e.set;
    }

    Verdict Judge(std::span<const u32> live, std::span<const u32> sent, u32 block) const {
        const u32 first = block * kBlockWords;
        u32 last = first + kBlockWords;
        if (last > live.size()) last = static_cast<u32>(live.size());
        if (last > sent.size()) last = static_cast<u32>(sent.size());
        if (last > words_) last = words_;
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
        if (Mutated(Mutant::SkipsLastWord) && last > first) --last;
#endif
        Verdict verdict = Verdict::Withhold;
        for (u32 w = first; w < last; ++w) {
            const u32 changed = live[w] ^ sent[w];
            if (!changed) continue;
            const Entry& e = entries_[w];
            if (e.stamp != generation_) {
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
                if (Mutated(Mutant::UncoveredWithheld)) continue;
#endif
                return Verdict::OutsideRecord;
            }
            if ((changed & e.keep) == 0) continue;
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
            if (Mutated(Mutant::WithholdsOnMaskedBits)) continue;
#endif
            verdict = Verdict::MaskedBits;
        }
        return verdict;
    }

private:
    struct Entry {
        u32 stamp = 0;
        u32 keep = 0;
        u32 set = 0;
        u32 pad = 0;
    };

    void Put(u32 reg, u32 value, u32 mask) {
        if (reg >= words_) return;
        Entry& e = entries_[reg];
        if (e.stamp != generation_) {
            e.stamp = generation_;
            e.keep = ~0u;
            e.set = 0;
        }
        e.keep &= ~mask;
        e.set = (e.set & ~mask) | (value & mask);
    }

    u32 words_;
    std::vector<Entry> entries_;
    u32 generation_ = 0;
};

template <typename Bits>
void AfterPacket(Bits& dirty, std::span<const u32> withheld) {
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
    if (Mutated(Mutant::KeepsSentBlocks)) {
        for (u32 block : withheld) dirty.set(block);
        return;
    }
#endif
    dirty.reset();
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
    if (Mutated(Mutant::DropsOwed)) return;
#endif
    for (u32 block : withheld) dirty.set(block);
}

struct Counts {
    u64 packets = 0, carried = 0, eligible = 0, samples = 0, withheld_draws = 0, dirty_blocks = 0, withheld_blocks = 0;
    u64 sent[static_cast<std::size_t>(Verdict::kCount)]{};
    u64 flushes = 0, flushed_blocks = 0, owed_at_task = 0;
};
static_assert(std::is_trivially_copyable_v<Counts> && sizeof(Counts) % sizeof(u64) == 0);

inline std::size_t FormatLine(char* out, std::size_t size, bool feed, const Counts& c) {
    using ull = unsigned long long;
    if (!size) return 0;
    const double share = c.dirty_blocks ? 100.0 * double(c.withheld_blocks) / double(c.dirty_blocks) : 0.0;
    const int wrote = std::snprintf(
        out, size,
        "GX whole packets (LANTERN_GX_WHOLE=%d): %llu packets, %llu carried draws, %llu judged, %llu sent whole for the "
        "compare, %llu draws withheld blocks; %llu of %llu dirty blocks %s (%.1f%%), sent because %s %llu, because %s %llu; "
        "%llu owed flushes before tasks (%llu blocks)%s",
        feed ? 1 : 2, ull(c.packets), ull(c.carried), ull(c.eligible), ull(c.samples), ull(c.withheld_draws),
        ull(c.withheld_blocks), ull(c.dirty_blocks), feed ? "withheld" : "could be withheld (=2 sends them)", share,
        kVerdictNames[1], ull(c.sent[1]), kVerdictNames[2], ull(c.sent[2]), ull(c.flushes), ull(c.flushed_blocks),
        c.owed_at_task ? "; OWED BLOCKS REACHED A TASK" : "");
    if (wrote <= 0) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<std::size_t>(wrote) < size ? static_cast<std::size_t>(wrote) : size - 1;
}
}
