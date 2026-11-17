// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BbEngine::GxRecordHash {
using u64 = std::uint64_t;

#ifdef LANTERN_GX_RECORD_HASH_MUTANTS
enum class Mutant : std::uint8_t { None, TailDropped, LaneDropped, SeedIgnored, LaneAliased, kCount };
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

inline u64 Mix(u64 hash, u64 value) {
    hash = (hash ^ value) * 0x100000001B3ull;
    return hash ^ (hash >> 29);
}

inline u64 Word(const void* at, std::size_t index) {
    u64 value;
    std::memcpy(&value, static_cast<const unsigned char*>(at) + index * 8, sizeof value);
    return value;
}

inline u64 Block(u64 hash, const void* at, std::size_t words) {
    u64 seed = hash;
#ifdef LANTERN_GX_RECORD_HASH_MUTANTS
    if (Mutated(Mutant::SeedIgnored)) seed = 0xCBF29CE484222325ull;
#endif
    u64 a = seed, b = seed ^ 0x9E3779B97F4A7C15ull, c = seed ^ 0xC2B2AE3D27D4EB4Full, d = seed ^ 0x165667B19E3779F9ull;
    std::size_t third = 2;
#ifdef LANTERN_GX_RECORD_HASH_MUTANTS
    if (Mutated(Mutant::LaneAliased)) third = 1;
#endif
    std::size_t i = 0;
    for (; i + 4 <= words; i += 4) {
        a = Mix(a, Word(at, i));
        b = Mix(b, Word(at, i + 1));
        c = Mix(c, Word(at, i + third));
        d = Mix(d, Word(at, i + 3));
    }
    bool tail = true;
#ifdef LANTERN_GX_RECORD_HASH_MUTANTS
    tail = !Mutated(Mutant::TailDropped);
#endif
    for (; tail && i < words; ++i) a = Mix(a, Word(at, i));
    u64 out = Mix(Mix(a, words), b);
    out = Mix(out, c);
#ifdef LANTERN_GX_RECORD_HASH_MUTANTS
    if (Mutated(Mutant::LaneDropped)) return out;
#endif
    return Mix(out, d);
}
}
