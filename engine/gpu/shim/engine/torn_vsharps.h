// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BbEngine::TornVsharps {
#ifdef LANTERN_TORN_VSHARPS_MUTANTS
enum class Mutant : int { None = 0, SkipRepair, KeepKeys, SkipNote, NoteEqual, kCount };
inline int g_mutant = 0;
inline bool Is(Mutant m) { return g_mutant == int(m); }
#define LANTERN_TV_MUTANT(name) (::BbEngine::TornVsharps::Is(::BbEngine::TornVsharps::Mutant::name))
#else
#define LANTERN_TV_MUTANT(name) false
#endif

template <class Buffer, class TableAt, class Note>
std::size_t Repair(bool intact, Buffer* got, std::uint64_t* keys, std::size_t count, TableAt&& table_at, Note&& note) {
    if (intact || LANTERN_TV_MUTANT(SkipRepair)) return 0;
    std::size_t replaced = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Buffer table = table_at(i);
        if (std::memcmp(&table, &got[i], sizeof(Buffer)) == 0) {
            if (LANTERN_TV_MUTANT(NoteEqual)) note(table);
            continue;
        }
        got[i] = table;
        if (!LANTERN_TV_MUTANT(KeepKeys)) keys[i] = 0;
        if (!LANTERN_TV_MUTANT(SkipNote)) note(table);
        ++replaced;
    }
    return replaced;
}
}
