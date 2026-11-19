// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "gpu/bbnet/room/room_counters.h"

namespace {
namespace N = BbRoom::NetCounts;
using Clock = std::chrono::steady_clock;
int g_failed = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
#ifndef LANTERN_NET_COUNTS_MUTANTS
        std::printf("FAILED: %s\n", what);
#else
        (void)what;
#endif
    }
}

void TestCounters() {
    N::Counters c;
    Expect(c.Take() == N::Snapshot{}, "fresh counters are all zero");
    c.Add(N::Kind::BundleSent, 600);
    c.Add(N::Kind::BundleSent, 400);
    c.Add(N::Kind::Reconnect);
    const N::Snapshot s = c.Take();
    Expect(s[N::Kind::BundleSent].count == 2, "every bundle is counted");
    Expect(s[N::Kind::BundleSent].bytes == 1000, "the bundle bytes add up");
    Expect(s[N::Kind::Reconnect].count == 1 && s[N::Kind::Reconnect].bytes == 0, "a reconnect counts without bytes");
    Expect(s[N::Kind::BundleLost].count == 0, "other categories stay zero");
    c.Backlog(10);
    c.Backlog(50000);
    c.Backlog(20);
    Expect(c.Take().backlog_peak == 50000, "the backlog peak keeps the highest value");
}

void TestLine() {
    Expect(N::Line(N::Snapshot{}) == "nothing sent or dropped", "an empty snapshot says nothing happened");
    N::Counters c;
    c.Add(N::Kind::BundleSent, 600);
    c.Add(N::Kind::BundleSent, 600);
    c.Add(N::Kind::BundleSent, 600);
    c.Add(N::Kind::BacklogRefused, 1200);
    c.Add(N::Kind::BridgeRecvError);
    c.Add(N::Kind::Reconnect);
    c.Backlog(131072);
    const std::string line = N::Line(c.Take());
    Expect(line == "bundle_sent=3/1800B backlog_refused=1/1200B bridge_recv_error=1 reconnects=1 backlog_peak=131072B",
           "the line lists nonzero categories with counts and bytes");
    Expect(line.find("bundle_lost") == std::string::npos, "a zero category is left out");
}

bool Clean(const std::string& line) {
    for (const char ch : line) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '=' || ch == '/' ||
                        ch == 'B' || ch == ' ' || ch == ':';
        if (!ok) return false;
    }
    return line.find('.') == std::string::npos;
}

void TestNoPersonalData() {
    std::mt19937_64 rng(11);
    bool clean = true;
    bool names_known = true;
    for (int round = 0; round < 200; ++round) {
        N::Counters c;
        for (std::size_t i = 0; i < N::kKinds; ++i) {
            const int times = static_cast<int>(rng() % 3);
            for (int t = 0; t < times; ++t) c.Add(static_cast<N::Kind>(i), rng() % 70000);
        }
        c.Backlog(rng() % 200000);
        N::Reporter r(std::chrono::seconds(30));
        const Clock::time_point t0{std::chrono::hours(1)};
        r.Tick(N::Snapshot{}, t0, true);
        const auto tick = r.Tick(c.Take(), t0 + std::chrono::seconds(30), true);
        const auto leave = r.Leave(c.Take());
        for (const auto* line : {&tick, &leave}) {
            if (!*line) continue;
            clean = clean && Clean(**line);
            std::string rest = (*line)->substr((*line)->find(':') + 2);
            for (std::size_t at = 0; at < rest.size();) {
                const std::size_t end = std::min(rest.find(' ', at), rest.size());
                const std::string word = rest.substr(at, rest.find('=', at) - at);
                bool known = word == "backlog_peak" || rest == "nothing sent or dropped";
                for (std::size_t i = 0; i < N::kKinds; ++i) known = known || word == N::Name(static_cast<N::Kind>(i));
                names_known = names_known && known;
                at = end + 1;
                if (rest == "nothing sent or dropped") break;
            }
        }
    }
    Expect(clean, "a line holds only counter names, digits and sizes: no names, addresses, room codes or keys");
    Expect(names_known, "every field in a line is a known counter");
}

void TestReporter() {
    N::Reporter r(std::chrono::seconds(30));
    const Clock::time_point t0{std::chrono::hours(2)};
    N::Snapshot quiet{};
    N::Snapshot busy{};
    busy.tally[static_cast<std::size_t>(N::Kind::BundleSent)] = {5, 3000};
    N::Snapshot busier = busy;
    busier.tally[static_cast<std::size_t>(N::Kind::BundleLost)] = {1, 600};

    Expect(!r.Leave(busy), "no summary before ever being in a room");
    Expect(!r.Tick(busy, t0, true), "entering the room starts the clock without a line");
    Expect(!r.Tick(busy, t0 + std::chrono::seconds(29), true), "no line before 30 s");
    const auto first = r.Tick(busy, t0 + std::chrono::seconds(30), true);
    Expect(first && *first == "net: bundle_sent=5/3000B", "a line at 30 s when counts changed");
    Expect(!r.Tick(busy, t0 + std::chrono::seconds(45), true), "no second line inside the same 30 s");
    Expect(!r.Tick(busy, t0 + std::chrono::seconds(60), true), "no line when nothing changed");
    const auto second = r.Tick(busier, t0 + std::chrono::seconds(90), true);
    Expect(second && *second == "net: bundle_sent=5/3000B bundle_lost=1/600B", "a line again once counts change");
    Expect(!r.Tick(busier, t0 + std::chrono::seconds(100), false), "no line while out of the room");
    Expect(!r.Tick(busier, t0 + std::chrono::seconds(200), true), "coming back restarts the clock");
    N::Snapshot later = busier;
    later.backlog_peak = 9000;
    Expect(!r.Tick(later, t0 + std::chrono::seconds(229), true), "no line before 30 s after coming back");
    Expect(r.Tick(later, t0 + std::chrono::seconds(230), true).has_value(), "a line 30 s after coming back");
    const auto bye = r.Leave(later);
    Expect(bye && *bye == "net on leaving the room: bundle_sent=5/3000B bundle_lost=1/600B backlog_peak=9000B",
           "leaving prints the summary");
    Expect(!r.Leave(later), "the summary prints once");

    N::Reporter calm(std::chrono::seconds(30));
    calm.Tick(quiet, t0, true);
    Expect(!calm.Tick(quiet, t0 + std::chrono::seconds(30), true), "a quiet room prints no line");
    const auto calm_bye = calm.Leave(quiet);
    Expect(calm_bye && *calm_bye == "net on leaving the room: nothing sent or dropped", "a quiet room still sums up");
}

struct Read {
    long n;
    N::RecvError error;
};

std::pair<int, int> DrainBridge(const std::vector<Read>& reads) {
    int forwarded = 0, errors_counted = 0;
    std::size_t at = 0;
    for (int errors = 0; at < reads.size();) {
        const Read r = reads[at++];
        if (r.n <= 0) {
            if (N::CountsAsRecvError(r.error)) ++errors_counted;
            if (N::AfterRecv(r.n, r.error, errors++) == N::RecvStep::Skip) continue;
            break;
        }
        errors = 0;
        ++forwarded;
    }
    return {forwarded, errors_counted};
}

void TestRecv() {
    using E = N::RecvError;
    using S = N::RecvStep;
    Expect(N::AfterRecv(5, E::None, 0) == S::Forward, "a datagram is forwarded");
    Expect(N::AfterRecv(-1, E::WouldBlock, 0) == S::Drained, "would-block ends the drain");
    Expect(N::AfterRecv(-1, E::WouldBlock, N::kMostRecvErrorsInRow) == S::Drained, "would-block is never an error");
    Expect(N::AfterRecv(-1, E::Transient, 0) == S::Skip, "a port-unreachable reset keeps reading");
    Expect(N::AfterRecv(-1, E::Transient, N::kMostRecvErrorsInRow - 1) == S::Skip, "errors under the cap keep reading");
    Expect(N::AfterRecv(-1, E::Transient, N::kMostRecvErrorsInRow) == S::Stop, "too many errors in a row stop this round");
    Expect(N::AfterRecv(-1, E::Fatal, 0) == S::Stop, "a fatal error stops this round");
    Expect(N::AfterRecv(0, E::None, 0) == S::Skip, "an empty datagram is skipped");
    Expect(N::AfterRecv(0, E::None, N::kMostRecvErrorsInRow) == S::Stop, "empty datagrams are capped too");
    Expect(N::CountsAsRecvError(E::Transient) && N::CountsAsRecvError(E::Fatal), "real errors are counted");
    Expect(!N::CountsAsRecvError(E::WouldBlock) && !N::CountsAsRecvError(E::None), "would-block is not counted");

    const auto mixed = DrainBridge({{40, E::None}, {-1, E::Transient}, {60, E::None}, {-1, E::WouldBlock}, {70, E::None}});
    Expect(mixed.first == 2 && mixed.second == 1, "a reset between datagrams loses none of them");
    std::vector<Read> storm(40, Read{-1, E::Transient});
    storm.push_back({10, E::None});
    const auto stormy = DrainBridge(storm);
    Expect(stormy.first == 0 && stormy.second == N::kMostRecvErrorsInRow + 1, "an error storm ends the round at the cap");
    const auto fatal = DrainBridge({{-1, E::Fatal}, {10, E::None}});
    Expect(fatal.first == 0 && fatal.second == 1, "a fatal error ends the round and is counted");
}

int RunSuite() {
    g_failed = 0;
    TestCounters();
    TestLine();
    TestNoPersonalData();
    TestReporter();
    TestRecv();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_NET_COUNTS_MUTANTS
    int survivors = 0;
    for (int m = 0; m <= N::kMutants; ++m) {
        N::g_mutant = m;
        const int found = RunSuite();
        if (m == 0 && found) {
            std::printf("net counts mutants: the unmutated run failed %d checks\n", found);
            return 1;
        }
        if (m && !found) {
            std::printf("net counts mutants: mutant %d survived\n", m);
            ++survivors;
        }
    }
    if (survivors) return 1;
    std::printf("net counts mutants: every mutant caught (%d)\n", N::kMutants);
    return 0;
#else
    if (RunSuite()) {
        std::printf("net counts: %d checks failed\n", g_failed);
        return 1;
    }
    std::printf("net counts: all tests passed\n");
    return 0;
#endif
}
