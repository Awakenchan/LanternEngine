// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "engine_hooks.h"
#include "gx_whole.h"
#include "hook_entries.h"
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>

namespace BbEngine::GxWhole::Hooks {
inline constexpr std::size_t kJumpLength = 14;

enum class Verdict : std::uint8_t { Install, NeedsRecords, Off };
enum class Flush : std::uint8_t { OutputMerger, Index, Rasterizer, PostDraw, Targets, Vertices, kCount };
inline constexpr std::size_t kFlushes = static_cast<std::size_t>(Flush::kCount);

struct Site {
    Flush flush;
    const char* name;
    std::size_t offset;
    std::size_t function;
    std::size_t function_size;
    std::span<const std::uint8_t> entry;
};

inline constexpr Site kSites[] = {
    {Flush::OutputMerger, "output-merger flush", 0x26d2230, 0x26d2230, 0x558, HookEntries::kGxWholeOutputMergerEntry},
    {Flush::Index, "index flush", 0x26bba30, 0x26bba30, 0xdb, HookEntries::kGxWholeIndexEntry},
    {Flush::Rasterizer, "rasterizer flush", 0x26d3370, 0x26d3370, 0x5f6, HookEntries::kGxWholeRasterizerEntry},
    {Flush::PostDraw, "post-draw", 0x26cbd50, 0x26cbd50, 0xfb, HookEntries::kGxWholePostDrawEntry},
    {Flush::Targets, "target flush", 0x26d2790, 0x26d2790, 0x493, HookEntries::kGxWholeTargetsEntry},
    {Flush::Vertices, "vertex flush", 0x26cfcf0, 0x26cfcf0, 0x5d2, HookEntries::kGxWholeVerticesEntry},
};
static_assert(std::size(kSites) == kFlushes);

constexpr bool InOrder() {
    for (std::size_t i = 0; i < kFlushes; ++i)
        if (static_cast<std::size_t>(kSites[i].flush) != i) return false;
    return true;
}
constexpr bool InsideFunctions() {
    for (const Site& site : kSites)
        if (site.offset < site.function || site.offset + site.entry.size() > site.function + site.function_size ||
            site.entry.size() < kJumpLength)
            return false;
    return true;
}
static_assert(InOrder());
static_assert(InsideFunctions());

#ifdef LANTERN_GX_WHOLE_HOOKS_MUTANTS
enum class Mutant : std::uint8_t { None, CompareInstalls, RecordsIgnored, EntryShort, KindInside, OffsetPastEntry, kCount };
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

inline Verdict Decide(Mode requested, Mode accepted) {
#ifdef LANTERN_GX_WHOLE_HOOKS_MUTANTS
    if (Mutated(Mutant::CompareInstalls) && requested != Mode::Off && accepted == requested) return Verdict::Install;
    if (Mutated(Mutant::RecordsIgnored) && requested == Mode::Feed) return Verdict::Install;
#endif
    if (requested != Mode::Feed) return Verdict::Off;
    return accepted == Mode::Feed ? Verdict::Install : Verdict::NeedsRecords;
}

inline std::span<const std::uint8_t> Entry(const Site& site) {
#ifdef LANTERN_GX_WHOLE_HOOKS_MUTANTS
    if (Mutated(Mutant::EntryShort)) return site.entry.first(site.entry.size() - 1);
#endif
    return site.entry;
}

inline std::size_t Offset(const Site& site) {
#ifdef LANTERN_GX_WHOLE_HOOKS_MUTANTS
    if (Mutated(Mutant::OffsetPastEntry) && site.flush == Flush::PostDraw) return site.offset + 1;
#endif
    return site.offset;
}

inline SiteKind KindOf(const Site& site) {
#ifdef LANTERN_GX_WHOLE_HOOKS_MUTANTS
    if (Mutated(Mutant::KindInside) && site.flush == Flush::Rasterizer) return SiteKind::Inside;
#endif
    return Offset(site) == site.function ? SiteKind::Entry : SiteKind::Inside;
}
}
