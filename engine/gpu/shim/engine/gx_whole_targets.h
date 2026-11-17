// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx_whole.h"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace BbEngine::GxWholeTargets {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace Gx = GxBindings;
using GxWhole::Mode;

inline constexpr u32 kColorSlots = 8;
inline constexpr u32 kSlots = Gx::kTargetViews;
inline constexpr u32 kDepthSlot = kSlots - 1;
inline constexpr u32 kReportFrames = 300;
inline constexpr u64 kSuspectCovered = 64;
static_assert(kSlots == kColorSlots + 1);

#ifdef LANTERN_GX_TARGETS_MUTANTS
enum class Mutant : u8 {
    None, ExtentsIgnored, SerialIgnored, DepthViewIgnored, EpochIgnored, HighSlotsKept, ValidUnmasked, GenerationIgnored,
    BeginIgnoresSerial, BeginWithoutHit, SuspectMissed, WordsIgnored, DepthWordsKeptInvalid, RebindKeepsMemo, TakeKeepsSerial,
    PrepareKeepsSerial, kCount
};
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

struct Identity {
    u64 views[kSlots];
    u64 epoch;
    u32 color_valid;
    u32 depth_valid;
    u32 target_mask;
    u32 depth_control;
    u32 color_words[kColorSlots][GxWhole::kColorWords];
    u32 depth_words[GxWhole::kDepthWords + 4];
};
static_assert(std::is_trivially_copyable_v<Identity>);
static_assert(sizeof(Gx::Render{}.targets[0]) == sizeof(Identity{}.color_words[0]));
static_assert(sizeof(Gx::Render{}.depth_target) == sizeof(Identity{}.depth_words));

inline Identity IdentityOf(const Gx::Render& render, std::span<const u64, kSlots> views, u64 epoch) {
    Identity id{};
    for (u32 i = 0; i < kSlots; ++i) id.views[i] = views[i];
    id.epoch = epoch;
    id.color_valid = render.color_valid;
    id.depth_valid = render.depth_valid;
    id.target_mask = render.blend[0];
    id.depth_control = render.depth_stencil[0] & GxWhole::kDepthControlMask;
    std::memcpy(id.color_words, render.targets, sizeof id.color_words);
    std::memcpy(id.depth_words, render.depth_target, sizeof id.depth_words);
    return id;
}

struct Key {
    u64 views[kSlots];
    u64 epoch;
    u64 registry_generation;
    u32 color_valid;
    u32 depth_valid;
    u32 mrt_mask;
    u32 target_mask;
    u32 color_control;
    u32 depth_control;
    u32 extents[kSlots];
    u32 color_words[kColorSlots][GxWhole::kColorWords];
    u32 depth_words[GxWhole::kDepthWords + 4];
    u32 pad;
};
static_assert(std::has_unique_object_representations_v<Key>);

inline Key MakeKey(const Identity& id, u32 mrt_mask, u32 color_control, std::span<const u32, kSlots> extents, u64 registry_generation) {
    Key key{};
    const u32 used = std::bit_width(mrt_mask & ((1u << kColorSlots) - 1));
    bool keep_high = false;
#ifdef LANTERN_GX_TARGETS_MUTANTS
    keep_high = Mutated(Mutant::HighSlotsKept);
#endif
    for (u32 i = 0; i < kColorSlots; ++i) {
        if (i < used || keep_high) {
            key.views[i] = id.views[i];
            key.extents[i] = extents[i];
            if ((id.color_valid >> i) & 1) std::memcpy(key.color_words[i], id.color_words[i], sizeof key.color_words[i]);
        }
    }
    bool keep_depth = id.depth_valid != 0;
#ifdef LANTERN_GX_TARGETS_MUTANTS
    if (Mutated(Mutant::DepthWordsKeptInvalid)) keep_depth = true;
#endif
    if (keep_depth) std::memcpy(key.depth_words, id.depth_words, sizeof key.depth_words);
    key.views[kDepthSlot] = id.views[kDepthSlot];
    key.extents[kDepthSlot] = extents[kDepthSlot];
    const u32 slot_mask = used >= 32 ? ~0u : (1u << used) - 1;
    key.color_valid = id.color_valid & slot_mask;
#ifdef LANTERN_GX_TARGETS_MUTANTS
    if (Mutated(Mutant::ValidUnmasked)) key.color_valid = id.color_valid;
#endif
    key.depth_valid = id.depth_valid;
    key.mrt_mask = mrt_mask;
    key.target_mask = id.target_mask;
    key.color_control = color_control;
    key.depth_control = id.depth_control;
    key.epoch = id.epoch;
    key.registry_generation = registry_generation;
    return key;
}

enum class Miss : u8 { Hit, First, Interleaved, Epoch, Generation, Views, Valid, Control, Extents, Words, MemoOff, kMisses };
inline constexpr u32 kMisses = static_cast<u32>(Miss::kMisses);
inline constexpr std::array<const char*, kMisses> kMissNames = {
    "hit", "first use", "another draw's targets in between", "GX object reused", "image registry changed",
    "other target views", "other valid targets", "other masks or controls", "other extents", "other target words",
    "texture binding memo off"};

struct Memo {
    bool valid = false;
    u64 serial = 0;
    Key key{};
};

inline Miss Classify(const Memo& memo, const Key& key, u64 serial) {
    if (!memo.valid) return Miss::First;
    bool check_serial = true, check_extents = true, check_depth = true, check_epoch = true, check_generation = true;
#ifdef LANTERN_GX_TARGETS_MUTANTS
    check_serial = !Mutated(Mutant::SerialIgnored);
    check_extents = !Mutated(Mutant::ExtentsIgnored);
    check_depth = !Mutated(Mutant::DepthViewIgnored);
    check_epoch = !Mutated(Mutant::EpochIgnored);
    check_generation = !Mutated(Mutant::GenerationIgnored);
#endif
    const Key& m = memo.key;
    if (check_serial && memo.serial != serial) return Miss::Interleaved;
    if (check_epoch && m.epoch != key.epoch) return Miss::Epoch;
    if (check_generation && m.registry_generation != key.registry_generation) return Miss::Generation;
    if (std::memcmp(m.views, key.views, sizeof(u64) * kColorSlots) != 0) return Miss::Views;
    if (check_depth && m.views[kDepthSlot] != key.views[kDepthSlot]) return Miss::Views;
    if (m.color_valid != key.color_valid || m.depth_valid != key.depth_valid) return Miss::Valid;
    if (m.mrt_mask != key.mrt_mask || m.target_mask != key.target_mask || m.color_control != key.color_control ||
        m.depth_control != key.depth_control)
        return Miss::Control;
    if (check_extents && std::memcmp(m.extents, key.extents, sizeof m.extents) != 0) return Miss::Extents;
    bool check_words = true;
#ifdef LANTERN_GX_TARGETS_MUTANTS
    check_words = !Mutated(Mutant::WordsIgnored);
#endif
    if (check_words && (std::memcmp(m.color_words, key.color_words, sizeof m.color_words) != 0 ||
                        std::memcmp(m.depth_words, key.depth_words, sizeof m.depth_words) != 0))
        return Miss::Words;
    return Miss::Hit;
}

inline void Remember(Memo& memo, const Key& key, u64 serial) {
    memo.valid = true;
    memo.key = key;
    memo.serial = serial;
}

inline bool BeginFast(bool targets_hit, u64 begin_serial, u64 previous_serial, bool begin_valid, bool scalars_same) {
    bool hit = targets_hit;
#ifdef LANTERN_GX_TARGETS_MUTANTS
    if (Mutated(Mutant::BeginWithoutHit)) hit = true;
#endif
    bool serial = begin_serial == previous_serial;
#ifdef LANTERN_GX_TARGETS_MUTANTS
    if (Mutated(Mutant::BeginIgnoresSerial)) serial = true;
#endif
    return hit && serial && begin_valid && scalars_same;
}

struct Chain {
    Memo memo;
    u64 serial = 0;
    u64 begin_serial = ~u64(0);
};

inline void NotePrepare(Chain& chain) {
#ifdef LANTERN_GX_TARGETS_MUTANTS
    if (Mutated(Mutant::PrepareKeepsSerial)) return;
#endif
    ++chain.serial;
}

inline Miss Look(const Chain& chain, const Key& key, bool memo_on) {
    return memo_on ? Classify(chain.memo, key, chain.serial) : Miss::MemoOff;
}

inline void Took(Chain& chain, const Key& key) {
#ifdef LANTERN_GX_TARGETS_MUTANTS
    if (!Mutated(Mutant::TakeKeepsSerial)) ++chain.serial;
#else
    ++chain.serial;
#endif
    Remember(chain.memo, key, chain.serial);
}

inline void Found(Chain& chain, const Key& key) {
    Remember(chain.memo, key, chain.serial);
}

inline void Rebound(Chain& chain) {
#ifdef LANTERN_GX_TARGETS_MUTANTS
    if (Mutated(Mutant::RebindKeepsMemo)) return;
#endif
    chain.memo.valid = false;
}

inline void BeginConfirmed(Chain& chain) {
    chain.begin_serial = chain.serial;
}

inline bool BeginFastNow(const Chain& chain, bool targets_hit, bool begin_valid, bool scalars_same) {
    return BeginFast(targets_hit, chain.begin_serial, chain.serial - 1, begin_valid, scalars_same);
}

struct Counts {
    u64 prepares = 0, covered = 0, hits = 0, taken = 0;
    u64 misses[kMisses]{};
    u64 compared = 0, differ = 0, slot_id_differ[kSlots]{}, slot_view_differ[kSlots]{};
    u64 begin_fast = 0, begin_compared = 0, begin_differ = 0;
    u64 find_cycles = 0, finds_timed = 0, signature_cycles = 0, signatures_timed = 0;
};
static_assert(std::is_trivially_copyable_v<Counts> && sizeof(Counts) % sizeof(u64) == 0);
inline constexpr std::size_t kCountWords = sizeof(Counts) / sizeof(u64);

inline double SkippedMicroseconds(u64 skipped, u64 cycles, u64 timed, double cycles_per_us) {
    if (!timed || cycles_per_us <= 0.0) return 0.0;
    return double(skipped) * (double(cycles) / double(timed)) / cycles_per_us;
}

inline bool InstrumentSuspect(Mode mode, const Counts& c) {
#ifdef LANTERN_GX_TARGETS_MUTANTS
    if (Mutated(Mutant::SuspectMissed)) return false;
#endif
    if (c.prepares > 0 && c.covered == 0) return true;
    if (mode == Mode::Off) return false;
    if (c.covered >= kSuspectCovered && c.hits == 0) return true;
    return c.differ > 0 && c.differ == c.compared;
}

inline std::size_t FormatLine(char* out, std::size_t size, Mode mode, const Counts& c, double cycles_per_us) {
    using ull = unsigned long long;
    GxWhole::LineBuilder line(out, size);
    line.Add("GX whole targets (LANTERN_GX_WHOLE=%d): %llu target preparations, %llu covered; memo hits %llu (taken %llu)", GxWhole::ModeNumber(mode),
             ull(c.prepares), ull(c.covered), ull(c.hits), ull(c.taken));
    bool first = true;
    for (u32 r = 1; r < kMisses; ++r) {
        if (!c.misses[r]) continue;
        line.Add("%s%s %llu", first ? "; misses: " : ", ", kMissNames[r], ull(c.misses[r]));
        first = false;
    }
    line.Add("; begin rendering from the memo without a signature %llu; compared %llu, differ %llu; begin compared %llu, differ %llu",
             ull(c.begin_fast), ull(c.compared), ull(c.differ), ull(c.begin_compared), ull(c.begin_differ));
    first = true;
    for (u32 s = 0; s < kSlots; ++s) {
        if (!c.slot_id_differ[s] && !c.slot_view_differ[s]) continue;
        if (s == kDepthSlot) {
            line.Add("%sdepth image %llu view %llu", first ? "; differ by slot: " : ", ", ull(c.slot_id_differ[s]), ull(c.slot_view_differ[s]));
        } else {
            line.Add("%scolor %u image %llu view %llu", first ? "; differ by slot: " : ", ", s, ull(c.slot_id_differ[s]), ull(c.slot_view_differ[s]));
        }
        first = false;
    }
    const double target_us = SkippedMicroseconds(c.taken, c.find_cycles, c.finds_timed, cycles_per_us);
    const double begin_us = SkippedMicroseconds(c.begin_fast, c.signature_cycles, c.signatures_timed, cycles_per_us);
    line.Add("; skipped (estimated from %llu timed lookups and %llu timed signatures) %.1f us targets, %.1f us signatures",
             ull(c.finds_timed), ull(c.signatures_timed), target_us, begin_us);
    return line.Used();
}
}
