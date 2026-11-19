// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace BbEngine::StreamPlan {
#ifdef LANTERN_STREAM_PLAN_MUTANTS
enum class Mutant : int { None = 0, AllOrNothing, SplitWhileCarried, SplitInCompare, BindUnsealed, KeepReadyRanges, HitIgnoresClean, HitIgnoresGeneration, kCount };
inline int g_mutant = 0;
inline bool Is(Mutant m) { return g_mutant == int(m); }
#define LANTERN_SP_MUTANT(name) (::BbEngine::StreamPlan::Is(::BbEngine::StreamPlan::Mutant::name))
#else
#define LANTERN_SP_MUTANT(name) false
#endif

enum class Mode : std::uint8_t { Off, On, Compare };

constexpr bool Split(Mode mode, std::uint32_t sealed_streams, std::uint32_t memory_streams, bool carried) {
    if (mode != Mode::On && !LANTERN_SP_MUTANT(SplitInCompare)) return false;
    if (carried && !LANTERN_SP_MUTANT(SplitWhileCarried)) return false;
    if (LANTERN_SP_MUTANT(AllOrNothing)) return sealed_streams == memory_streams && sealed_streams > 0;
    return sealed_streams > 0;
}

constexpr bool BoundSealed(bool split, bool sealed) {
    return split && (sealed || LANTERN_SP_MUTANT(BindUnsealed));
}

constexpr bool UseReady(bool ready, bool split) {
    return ready && (!split || LANTERN_SP_MUTANT(KeepReadyRanges));
}

constexpr bool Hit(bool slot_same, bool seal_valid, bool clean) {
    return slot_same && (seal_valid || LANTERN_SP_MUTANT(HitIgnoresGeneration)) && (clean || LANTERN_SP_MUTANT(HitIgnoresClean));
}
}
