// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/bbnet/thd/p2p_split.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace S = BbNet::P2pSplit;

int g_failed = 0;

void Expect(bool ok, const char* what) {
    (void)what;
    if (!ok) {
        ++g_failed;
#ifndef LANTERN_P2P_SPLIT_MUTANTS
        std::printf("FAIL: %s\n", what);
#endif
    }
}

void AddChunk(std::vector<std::uint8_t>& d, std::uint8_t kind, std::size_t payload) {
    d.push_back(kind);
    d.push_back(static_cast<std::uint8_t>(payload >> 8));
    d.push_back(static_cast<std::uint8_t>(payload));
    for (std::size_t i = 0; i < payload; ++i) d.push_back(static_cast<std::uint8_t>(i * 7 + kind));
}

std::vector<std::size_t> Ends(const std::vector<std::uint8_t>& d) {
    std::vector<std::size_t> ends;
    if (S::Wanted(true, d.data(), d.size(), true)) S::PieceEnds(d.data(), d.size(), ends);
    if (ends.empty()) ends.push_back(d.size());
    return ends;
}

bool CoversInOrder(const std::vector<std::size_t>& ends, std::size_t n) {
    std::size_t begin = 0;
    for (const std::size_t end : ends) {
        if (end <= begin) return false;
        begin = end;
    }
    return begin == n;
}

void Run() {
    std::vector<std::uint8_t> d;
    AddChunk(d, 1, 1196);
    Expect(d.size() == 1199 && Ends(d) == std::vector<std::size_t>{1199}, "1,199 bytes go whole");

    d.clear();
    AddChunk(d, 2, 1197);
    Expect(d.size() == 1200 && !S::Wanted(true, d.data(), d.size(), true), "1,200 bytes are not split");
    Expect(Ends(d) == std::vector<std::size_t>{1200}, "1,200 bytes go whole");

    d.clear();
    AddChunk(d, 0, 597);
    AddChunk(d, 3, 598);
    Expect(d.size() == 1201, "the 1,201-byte case is two chunks");
    Expect(Ends(d) == std::vector<std::size_t>{600, 1201}, "1,201 bytes leave as two pieces of whole chunks");

    d.clear();
    for (int i = 0; i < 10; ++i) AddChunk(d, static_cast<std::uint8_t>(i % 4), 297);
    Expect(Ends(d) == std::vector<std::size_t>{1200, 2400, 3000}, "pieces fill up to exactly 1,200 bytes");

    d.clear();
    AddChunk(d, 1, 1997);
    AddChunk(d, 2, 97);
    Expect(Ends(d) == std::vector<std::size_t>{2000, 2100}, "a chunk over 1,200 bytes goes alone, the next on its own");

    d.clear();
    AddChunk(d, 1, 97);
    AddChunk(d, 2, 1997);
    AddChunk(d, 3, 97);
    Expect(Ends(d) == std::vector<std::size_t>{100, 2100, 2200}, "a long chunk in the middle is a piece of its own");

    d.clear();
    AddChunk(d, 1, 1500);
    d[0] = 7;
    Expect(Ends(d) == std::vector<std::size_t>{1503}, "bytes that are not chunks (kind 7) go whole");

    d.clear();
    AddChunk(d, 1, 700);
    AddChunk(d, 1, 700);
    d[703] = 4;
    Expect(Ends(d) == std::vector<std::size_t>{1406}, "a kind of 4 is not a chunk: whole");

    d.clear();
    AddChunk(d, 1, 800);
    AddChunk(d, 2, 800);
    d.resize(d.size() - 10);
    Expect(Ends(d) == std::vector<std::size_t>{d.size()}, "a chunk running past the end: whole");

    d.clear();
    AddChunk(d, 1, 800);
    AddChunk(d, 2, 800);
    d.push_back(1);
    Expect(Ends(d) == std::vector<std::size_t>{d.size()}, "a cut header at the end: whole");

    d.clear();
    for (int i = 0; i < 4; ++i) AddChunk(d, 1, 997);
    Expect(!S::Wanted(false, d.data(), d.size(), true), "a socket that is not P2P never splits");
    Expect(!S::Wanted(true, nullptr, d.size(), true), "no bytes, no split");
    Expect(!S::Wanted(true, d.data(), d.size(), false), "a datagram that does not fit is not split");
    Expect(!S::Wanted(true, d.data(), d.size(), true, true), "a send to this PC (a room's loopback bridge) goes whole");
    Expect(S::Wanted(true, d.data(), d.size(), true, false), "a send to another PC splits");
    const std::uint8_t loop[4] = {127, 0, 0, 1}, lan[4] = {192, 168, 1, 7};
    std::uint32_t loop_addr = 0, lan_addr = 0;
    std::memcpy(&loop_addr, loop, 4);
    std::memcpy(&lan_addr, lan, 4);
    Expect(S::Loopback(loop_addr) && !S::Loopback(lan_addr), "127.x.x.x in network order is this PC");

    d.clear();
    std::size_t room = 65507 - 6;
    while (room >= 3 + 1) {
        const std::size_t payload = room - 3 >= 1100 ? 1100 : room - 3;
        AddChunk(d, static_cast<std::uint8_t>(d.size() % 4), payload);
        room -= 3 + payload;
    }
    const auto big = Ends(d);
    Expect(CoversInOrder(big, d.size()), "the largest datagram's pieces cover every byte once, in order");
    Expect(S::Largest(big) <= S::kPieceBytes, "every piece of the largest datagram is at most 1,200 bytes");
    Expect(S::Largest(std::vector<std::size_t>{600, 1201}) == 601, "the largest piece is measured from its own start");
}
}

int main() {
#ifdef LANTERN_P2P_SPLIT_MUTANTS
    for (int m = 0; m <= S::kMutants; ++m) {
        S::g_mutant = m;
        g_failed = 0;
        Run();
        if (m == 0 && g_failed) {
            std::printf("p2p split mutants: the unmutated run failed %d checks\n", g_failed);
            return 1;
        }
        if (m > 0 && !g_failed) {
            std::printf("p2p split mutants: mutant %d survived\n", m);
            return 1;
        }
    }
    std::printf("p2p split mutants: every mutant caught\n");
    return 0;
#else
    Run();
    if (g_failed) {
        std::printf("p2p-split-test: %d failed\n", g_failed);
        return 1;
    }
    std::printf("p2p-split-test: ok\n");
    return 0;
#endif
}
