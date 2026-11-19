// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx_whole.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace BbEngine::GxWholeStageB {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace Gx = GxBindings;
using GxWhole::Field;
using GxWhole::Mode;
using GxWhole::Value;

inline constexpr u32 kMaxValues = GxWhole::kMaxValues;
inline constexpr u32 kEvidencePerDraw = GxWhole::kEvidencePerDraw;
inline constexpr u32 kSampleEvery = GxWhole::kSampleEvery;
inline constexpr u32 kPublishEvery = 256;
inline constexpr u32 kReportFrames = 300;
inline constexpr u32 kSuspectMinDraws = 256;

#ifdef LANTERN_GX_STAGEB_MUTANTS
enum class Mutant : u8 {
    None, TargetsUngrouped, ShadersGrouped, OverlayIgnoresMask, RestoreForward, CompareIgnoresMask, TakeWhenOff,
    FeedNeverSamples, CoverIgnoresOverwrite, CarryDropsBounds, SuspectMissed, SuspectOnTinyWindow, SuspectAfterTakeOff,
    OverlayKeepsUnread, CarryDropsShaderMask, IgnoresStageAOff, kCount
};
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

enum class Group : u8 { None, Targets, Render, Dynamic, Binds, kGroups };
inline constexpr u32 kGroups = static_cast<u32>(Group::kGroups);
inline constexpr std::array<const char*, kGroups> kGroupNames = {"not stage B", "targets", "render state", "dynamic state", "index bind"};

inline Group GroupOf(u32 field) {
    switch (field) {
    case GxWhole::CbColorTarget:
#ifdef LANTERN_GX_STAGEB_MUTANTS
        if (Mutated(Mutant::TargetsUngrouped)) return Group::None;
#endif
        return Group::Targets;
    case GxWhole::CbColorUnbound:
    case GxWhole::DbDepthTarget:
    case GxWhole::DbDepthView:
    case GxWhole::DbHtileBase:
    case GxWhole::DbDepthClear:
    case GxWhole::DbStencilClear:
        return Group::Targets;
    case GxWhole::CbTargetMask:
    case GxWhole::CbBlendControl:
        return Group::Render;
    case GxWhole::CbBlendColor:
    case GxWhole::DbDepthControl:
    case GxWhole::DbStencilControl:
    case GxWhole::DbStencilRefMask:
    case GxWhole::DbDepthBounds:
    case GxWhole::PaClClipCntl:
    case GxWhole::PaSuScModeCntl:
    case GxWhole::PaSuPolyOffset:
    case GxWhole::PaScModeCntl0:
    case GxWhole::PaClVport0:
    case GxWhole::PaScVportZ0:
    case GxWhole::PaClVteCntl:
    case GxWhole::PaScScreenScissor:
    case GxWhole::PaScVportScissor0:
    case GxWhole::VgtPrimitiveType:
        return Group::Dynamic;
    case GxWhole::VgtIndexType:
    case GxWhole::VgtIndexBase:
        return Group::Binds;
    case GxWhole::VsProgram:
#ifdef LANTERN_GX_STAGEB_MUTANTS
        if (Mutated(Mutant::ShadersGrouped)) return Group::Render;
#endif
        return Group::None;
    default:
        return Group::None;
    }
}

struct Carried {
    Gx::Render render;
    u32 topology;
    float depth_bounds[2];
    u32 index_bound;
    float depth_clear;
    u32 stencil_clear;
    u64 target_views[Gx::kTargetViews];
    u64 objects_epoch;
    u32 ps_bound;
    u32 cb_shader_mask;
};
static_assert(std::is_trivially_copyable_v<Carried>);
static_assert(sizeof(Carried) % alignof(u64) == 0);

inline Carried Carry(const Gx::State& state) {
    Carried carried{};
    carried.render = state.render;
    carried.topology = state.whole.topology;
    carried.depth_bounds[0] = state.whole.depth_bounds[0];
    carried.depth_bounds[1] = state.whole.depth_bounds[1];
#ifdef LANTERN_GX_STAGEB_MUTANTS
    if (Mutated(Mutant::CarryDropsBounds)) carried.depth_bounds[1] = 0.0f;
#endif
    carried.index_bound = state.whole.index_bound;
    carried.depth_clear = state.whole.depth_clear;
    carried.stencil_clear = state.whole.stencil_clear;
    std::memcpy(carried.target_views, state.target_views, sizeof carried.target_views);
    carried.objects_epoch = state.objects_epoch;
    carried.ps_bound = state.whole.ps_bound;
    carried.cb_shader_mask = state.whole.ps_registers[11];
#ifdef LANTERN_GX_STAGEB_MUTANTS
    if (Mutated(Mutant::CarryDropsShaderMask)) carried.ps_bound = 0;
#endif
    return carried;
}

inline Gx::Whole WholeOf(const Carried& carried) {
    Gx::Whole whole{};
    whole.captured = 1;
    whole.topology = carried.topology;
    whole.depth_bounds[0] = carried.depth_bounds[0];
    whole.depth_bounds[1] = carried.depth_bounds[1];
    whole.index_bound = carried.index_bound;
    whole.depth_clear = carried.depth_clear;
    whole.stencil_clear = carried.stencil_clear;
    whole.ps_bound = carried.ps_bound;
    whole.ps_registers[11] = carried.cb_shader_mask;
    return whole;
}

inline u32 StageBValues(const Carried& carried, bool indexed, std::span<Value, kMaxValues> out) {
    const Gx::Whole whole = WholeOf(carried);
    GxWhole::Writer writer(out);
    GxWhole::PutOutputMerger(carried.render, whole, writer);
    GxWhole::PutRasterizer(carried.render, writer);
    GxWhole::PutTargets(carried.render, whole, writer);
    GxWhole::PutInputAssembly(carried.render, whole, indexed, writer);
    u32 kept = 0;
    for (u32 i = 0; i < writer.Count(); ++i) {
        if (GroupOf(out[i].field) != Group::None) out[kept++] = out[i];
    }
    return kept;
}

inline u32 KeepRead(const Carried& carried, std::span<Value, kMaxValues> values, u32 count) {
#ifdef LANTERN_GX_STAGEB_MUTANTS
    if (Mutated(Mutant::OverlayKeepsUnread)) return count;
#endif
    u32 kept = 0;
    for (u32 i = 0; i < count; ++i) {
        if (GxWhole::UnusedBy(values[i], carried.render) == GxWhole::Unused::Read) values[kept++] = values[i];
    }
    return kept;
}

inline u32 StageBOverlayValues(const Carried& carried, bool indexed, std::span<Value, kMaxValues> out) {
    return KeepRead(carried, out, StageBValues(carried, indexed, out));
}

enum class Cover : u8 { Carried, NotToken, NoPayload, NoWhole, Overwritten, NotDraw, kCovers };
inline constexpr u32 kCovers = static_cast<u32>(Cover::kCovers);
inline constexpr std::array<const char*, kCovers> kCoverNames = {"carried", "no GX record", "record without a payload",
                                                                 "record without its whole part", "overwritten before the copy",
                                                                 "not a plain draw packet"};

inline Cover CoverOf(bool token, bool payload, bool whole, bool intact) {
    if (!token) return Cover::NotToken;
    if (!payload) return Cover::NoPayload;
    if (!whole) return Cover::NoWhole;
#ifdef LANTERN_GX_STAGEB_MUTANTS
    if (Mutated(Mutant::CoverIgnoresOverwrite)) return Cover::Carried;
#endif
    return intact ? Cover::Carried : Cover::Overwritten;
}

inline bool TakeOffFollowing(bool take_off, bool stage_a_off) {
#ifdef LANTERN_GX_STAGEB_MUTANTS
    if (Mutated(Mutant::IgnoresStageAOff)) stage_a_off = false;
#endif
    return take_off || stage_a_off;
}

enum class Action : u8 { Register, Take, TakeAndCompare, Compare };

inline Action Decide(Mode mode, bool carried, bool take_off, u64 covered_index) {
    if (mode == Mode::Off || !carried) return Action::Register;
    if (mode == Mode::Compare) return Action::Compare;
    bool off = take_off;
#ifdef LANTERN_GX_STAGEB_MUTANTS
    if (Mutated(Mutant::TakeWhenOff)) off = false;
#endif
    if (off) return Action::Register;
    bool sample = covered_index % kSampleEvery == 0;
#ifdef LANTERN_GX_STAGEB_MUTANTS
    if (Mutated(Mutant::FeedNeverSamples)) sample = false;
#endif
    return sample ? Action::TakeAndCompare : Action::Take;
}

struct Saved {
    u32 reg;
    u32 old;
};

inline u32 Overlay(std::span<const Value> values, std::span<u32> regs, std::span<Saved, kMaxValues> saved) {
    u32 count = 0;
    for (const Value& value : values) {
        if (value.reg >= regs.size()) continue;
        u32 mask = value.mask;
#ifdef LANTERN_GX_STAGEB_MUTANTS
        if (Mutated(Mutant::OverlayIgnoresMask)) mask = ~0u;
#endif
        const u32 old = regs[value.reg];
        const u32 now = (old & ~mask) | (value.value & mask);
        if (now == old) continue;
        saved[count++] = {value.reg, old};
        regs[value.reg] = now;
    }
    return count;
}

inline void Restore(std::span<const Saved> saved, std::span<u32> regs) {
#ifdef LANTERN_GX_STAGEB_MUTANTS
    if (Mutated(Mutant::RestoreForward)) {
        for (const Saved& s : saved) regs[s.reg] = s.old;
        return;
    }
#endif
    for (std::size_t i = saved.size(); i-- > 0;) regs[saved[i].reg] = saved[i].old;
}

struct Counts {
    u64 draws = 0, covered = 0, taken = 0, checked = 0, skipped = 0, differ = 0;
    u64 values = 0, values_differ = 0, words_overlaid = 0, other_writers = 0;
    u64 by_cover[kCovers]{};
    u64 group_differ[kGroups]{};
    u64 field_differ[GxWhole::kFields]{};
    u64 unused_differ = 0;
    u64 unused_by[GxWhole::kUnusedClasses]{};
};
static_assert(std::is_trivially_copyable_v<Counts> && sizeof(Counts) % sizeof(u64) == 0);
inline constexpr std::size_t kCountWords = sizeof(Counts) / sizeof(u64);

struct Difference {
    u16 field;
    u32 reg;
    u32 record;
    u32 registers;
};

inline u32 CompareValues(std::span<const Value> values, std::span<const u32> regs, Counts& counts,
                         std::span<Difference, kEvidencePerDraw> evidence, const Gx::Render* render = nullptr,
                         GxWhole::UnusedFound* found = nullptr) {
    u32 differing = 0;
    for (const Value& value : values) {
        if (value.reg >= regs.size()) continue;
        u32 mask = value.mask;
#ifdef LANTERN_GX_STAGEB_MUTANTS
        if (Mutated(Mutant::CompareIgnoresMask)) mask = ~0u;
#endif
        ++counts.values;
        const u32 decoded = regs[value.reg];
        if (((decoded ^ value.value) & mask) == 0) continue;
        if (render) {
            const GxWhole::Unused unused = GxWhole::UnusedBy(value, *render);
            if (unused != GxWhole::Unused::Read) {
                const u32 u = static_cast<u32>(unused);
                ++counts.unused_differ;
                ++counts.unused_by[u];
                if (found && !((found->mask >> u) & 1)) {
                    found->mask |= 1u << u;
                    found->first[u] = {value.field, value.reg, value.value & mask, decoded & mask};
                }
                continue;
            }
        }
        ++counts.values_differ;
        ++counts.group_differ[static_cast<u32>(GroupOf(value.field))];
        ++counts.field_differ[value.field];
        if (differing < kEvidencePerDraw) evidence[differing] = {value.field, value.reg, value.value & mask, decoded & mask};
        ++differing;
    }
    return differing;
}

inline bool InstrumentSuspect(Mode mode, const Counts& c, bool take_off) {
#ifdef LANTERN_GX_STAGEB_MUTANTS
    if (Mutated(Mutant::SuspectMissed)) return false;
#endif
    u32 min_draws = kSuspectMinDraws;
#ifdef LANTERN_GX_STAGEB_MUTANTS
    if (Mutated(Mutant::SuspectOnTinyWindow)) min_draws = 1;
    if (Mutated(Mutant::SuspectAfterTakeOff)) take_off = false;
#endif
    if (c.draws >= min_draws && c.covered == 0) return true;
    if (mode == Mode::Off || take_off) return false;
    if (c.covered >= kSampleEvery && c.checked == 0) return true;
    return c.differ > 0 && c.differ == c.checked;
}

inline std::size_t FormatLine(char* out, std::size_t size, Mode mode, bool take_off, const Counts& c) {
    using ull = unsigned long long;
    GxWhole::LineBuilder line(out, size);
    line.Add("GX whole stage B (LANTERN_GX_WHOLE=%d%s): %llu draw packets, %llu covered by a carried record", GxWhole::ModeNumber(mode),
             take_off ? ", take turned off" : "", ull(c.draws), ull(c.covered));
    bool first = true;
    for (u32 r = 1; r < kCovers; ++r) {
        if (!c.by_cover[r]) continue;
        line.Add("%s%s %llu", first ? "; not covered: " : ", ", kCoverNames[r], ull(c.by_cover[r]));
        first = false;
    }
    if (mode != Mode::Off) {
        line.Add("; taken %llu (%llu register words overlaid); %llu checked, %llu skipped by the sample, %llu differ; %llu values compared, %llu differ",
                 ull(c.taken), ull(c.words_overlaid), ull(c.checked), ull(c.skipped), ull(c.differ), ull(c.values), ull(c.values_differ));
        if (c.other_writers) line.Add("; %llu calls from other writers", ull(c.other_writers));
        first = true;
        for (u32 g = 0; g < kGroups; ++g) {
            if (!c.group_differ[g]) continue;
            line.Add("%s%s %llu", first ? "; differ by group: " : ", ", kGroupNames[g], ull(c.group_differ[g]));
            first = false;
        }
        first = true;
        for (u32 f = 0; f < GxWhole::kFields; ++f) {
            if (!c.field_differ[f]) continue;
            line.Add("%s%s %llu", first ? "; differ by field: " : ", ", GxWhole::kFieldInfo[f].name, ull(c.field_differ[f]));
            first = false;
        }
        if (c.unused_differ) {
            line.Add("; %llu differences in values the draw does not read (no take-off)", ull(c.unused_differ));
            for (u32 u = 1; u < GxWhole::kUnusedClasses; ++u) {
                if (c.unused_by[u]) line.Add(", %s %llu", GxWhole::kUnusedNames[u], ull(c.unused_by[u]));
            }
        }
    }
    return line.Used();
}
}
