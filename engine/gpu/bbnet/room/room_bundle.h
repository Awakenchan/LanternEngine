// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef LANTERN_ROOM_BUNDLE_MUTANTS
namespace BbRoom::Bundle {
inline int g_mutant = 0;
inline constexpr int kMutants = 7;
}
#define LANTERN_ROOM_BUNDLE_PICK(n, wrong, right) (::BbRoom::Bundle::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_ROOM_BUNDLE_PICK(n, wrong, right) (right)
#endif

namespace BbRoom::Bundle {

inline constexpr std::size_t kSlotBytes = 16384;

inline std::size_t Cap() { return LANTERN_ROOM_BUNDLE_PICK(1, std::size_t(65500), kSlotBytes); }

inline bool StartsNew(std::size_t held, std::size_t n) {
    return LANTERN_ROOM_BUNDLE_PICK(2, true, held != 0) &&
           LANTERN_ROOM_BUNDLE_PICK(3, held + 2 + n >= Cap(), held + LANTERN_ROOM_BUNDLE_PICK(4, 0, 2) + n > Cap());
}

inline bool Full(std::size_t held) { return LANTERN_ROOM_BUNDLE_PICK(5, held > Cap(), held >= Cap()); }

inline void Append(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t n) {
    out.push_back(static_cast<std::uint8_t>(LANTERN_ROOM_BUNDLE_PICK(6, n >> 8, n)));
    out.push_back(static_cast<std::uint8_t>(LANTERN_ROOM_BUNDLE_PICK(6, n, n >> 8)));
    out.insert(out.end(), data, data + n);
}

template <class Send>
void Add(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t n, Send&& send) {
    if (StartsNew(out.size(), n)) {
        send(out);
        out.clear();
    }
    Append(out, data, n);
    if (LANTERN_ROOM_BUNDLE_PICK(7, false, Full(out.size()))) {
        send(out);
        out.clear();
    }
}

}
