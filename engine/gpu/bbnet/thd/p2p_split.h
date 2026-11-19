// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef LANTERN_P2P_SPLIT_MUTANTS
namespace BbNet::P2pSplit {
inline int g_mutant = 0;
inline constexpr int kMutants = 7;
}
#define LANTERN_P2P_SPLIT_PICK(n, wrong, right) (::BbNet::P2pSplit::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_P2P_SPLIT_PICK(n, wrong, right) (right)
#endif

namespace BbNet::P2pSplit {

inline constexpr std::size_t kPieceBytes = 1200;
inline constexpr std::size_t kOneFrameBytes = 1472;

inline bool Loopback(std::uint32_t addr_network_order) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&addr_network_order);
    return b[0] == 127;
}

inline bool Wanted(bool p2p, const void* bytes, std::uint64_t len, bool fits, bool loopback = false) {
    return LANTERN_P2P_SPLIT_PICK(1, true, p2p) && bytes && LANTERN_P2P_SPLIT_PICK(7, true, !loopback) &&
           LANTERN_P2P_SPLIT_PICK(2, len >= kPieceBytes, len > kPieceBytes) && fits;
}

inline void PieceEnds(const std::uint8_t* d, std::size_t n, std::vector<std::size_t>& ends) {
    ends.clear();
    std::size_t at = 0, start = 0;
    while (at < n) {
        if (n - at < 3 || d[at] > LANTERN_P2P_SPLIT_PICK(3, 4, 3)) {
            ends.clear();
            return;
        }
        const std::size_t chunk = 3 + ((static_cast<std::size_t>(d[at + 1]) << 8) | d[at + 2]);
        if (LANTERN_P2P_SPLIT_PICK(4, false, chunk > n - at)) {
            ends.clear();
            return;
        }
        if (LANTERN_P2P_SPLIT_PICK(5, true, at > start) &&
            at + chunk - start > LANTERN_P2P_SPLIT_PICK(6, kOneFrameBytes, kPieceBytes)) {
            ends.push_back(at);
            start = at;
        }
        at += chunk;
    }
    ends.push_back(n);
}

inline std::size_t Largest(const std::vector<std::size_t>& ends) {
    std::size_t largest = 0, begin = 0;
    for (const std::size_t end : ends) {
        largest = end - begin > largest ? end - begin : largest;
        begin = end;
    }
    return largest;
}

}
