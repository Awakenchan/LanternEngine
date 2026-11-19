// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if __has_include(<tsl/robin_map.h>)
#include <tsl/robin_map.h>
#define HAVE_ROBIN_MAP 1
#endif

#include "gpu/shim/engine/pipeline_memo.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

using namespace BbEngine::PipelineMemo;

void PreparedReasons() {
    PreparedTally tally;
    Expect(tally.Total() == 0 && tally.Missed() == 0, "empty tally");
    tally.Note(Used);
    tally.Note(Used);
    tally.Note(Flat);
    tally.Note(Checksum);
    tally.Note(Pending);
    tally.Note(NoPipeline);
    tally.Note(Prepared(200));
    Expect(tally.Total() == 7, "every draw counted once");
    Expect(tally.counts[Used] == 2 && tally.Missed() == 5, "used and missed split");
    Expect(tally.counts[Rejected] == 1, "an unknown reason lands in the last bucket");
    char line[256];
    tally.Format(line, sizeof(line));
    const std::string text = line;
    Expect(text.find("2 used, 5 not") == 0, "line starts with used and not");
    Expect(text.find("flat data 1") != std::string::npos, "flat reason printed");
    Expect(text.find("checksum 1") != std::string::npos, "checksum reason printed");
    Expect(text.find("unavailable") == std::string::npos, "zero reasons left out");
    tally.Clear();
    Expect(tally.Total() == 0, "cleared");
    char tiny[8];
    tally.Note(Flat);
    tally.Format(tiny, sizeof(tiny));
    Expect(std::strlen(tiny) < sizeof(tiny), "short buffer stays terminated");

    struct Case {
        bool ready, checksum, pipeline;
        Prepared reason;
    };
    const Case cases[] = {
        {true, true, true, Used},     {true, true, true, Flat},      {true, true, true, NoPipeline},
        {true, true, true, Program},  {true, false, true, Checksum}, {false, true, true, Pending},
        {true, true, false, Flat},
    };
    std::uint64_t old_used = 0;
    PreparedTally fixed;
    for (const Case& c : cases) {
        old_used += c.pipeline && c.ready && c.checksum;
        fixed.Note(c.reason);
    }
    Expect(old_used == 4,"the old count called flat, key and program misses used");
    Expect(fixed.counts[Used] == 1, "the new count only calls the prepared pipeline used");
}

struct Info {
    std::uint64_t base = 0;
    const std::uint32_t* ud = nullptr;
    std::size_t size = 0;
    std::vector<std::uint32_t> flat;
};

struct Machine {
    std::array<std::uint32_t, 64> memory{};
    std::array<std::array<std::uint32_t, 16>, 6> regs{};
    std::uint64_t refreshes = 0;

    void Refresh(Info& info) {
        ++refreshes;
        info.flat.assign(info.size + 4, 0);
        for (std::size_t i = 0; i < info.size; ++i) info.flat[i] = info.ud[i];
        for (std::size_t i = 0; i < 4; ++i) {
            info.flat[info.size + i] = memory[(info.ud[0] + i + info.base) & 63];
        }
    }
};

struct Draw {
    std::array<int, 2> stage_program;
    std::array<int, 2> stage_regs;
    std::array<std::uint64_t, 2> base;
    int prepared_checked;
    bool motion;
};

std::vector<std::vector<std::uint32_t>> RunDraws(const std::vector<Draw>& draws, bool fresh_on,
                                                 std::uint64_t& refreshes, std::uint32_t seed) {
    Machine m;
    std::mt19937 rng(seed);
    for (auto& r : m.regs) for (auto& w : r) w = rng();
    for (auto& w : m.memory) w = rng();
    std::array<Info, 4> programs;
    FreshFlats fresh;
    std::vector<std::vector<std::uint32_t>> out;
    const auto bind = [&](Info& info, int regs, std::uint64_t base) {
        info.base = base;
        info.ud = m.regs[regs].data();
        info.size = 8;
    };
    for (const Draw& d : draws) {
        for (int i = 0; i < 3; ++i) m.memory[rng() & 63] = rng();
        m.regs[rng() % 6][rng() & 15] = rng();
        fresh.Reset();
        for (int s = 0; s < d.prepared_checked; ++s) {
            Info& info = programs[d.stage_program[s]];
            bind(info, d.stage_regs[s], d.base[s]);
            fresh.Forget(&info);
            m.Refresh(info);
            fresh.Note(&info, info.base, info.ud, info.size);
        }
        const int passes = d.motion ? 2 : 1;
        for (int p = 0; p < passes; ++p) {
            for (int s = 0; s < 2; ++s) {
                Info& info = programs[d.stage_program[s]];
                bind(info, d.stage_regs[s], d.base[s]);
                if (!fresh_on || !fresh.Fresh(&info, info.base, info.ud, info.size)) {
                    fresh.Forget(&info);
                    m.Refresh(info);
                    fresh.Note(&info, info.base, info.ud, info.size);
                }
                out.push_back(info.flat);
            }
        }
    }
    refreshes = m.refreshes;
    return out;
}

void FreshFlatsSkipOnlyRepeats() {
    FreshFlats fresh;
    int a = 0, b = 0;
    const std::uint32_t ud[4] = {};
    Expect(!fresh.Fresh(&a, 1, ud, 4), "nothing is fresh at first");
    fresh.Note(&a, 1, ud, 4);
    Expect(fresh.Fresh(&a, 1, ud, 4), "same info, base and user data");
    Expect(!fresh.Fresh(&a, 2, ud, 4), "another program base");
    Expect(!fresh.Fresh(&a, 1, ud + 1, 3), "other user data");
    Expect(!fresh.Fresh(&a, 1, ud, 3), "other user data size");
    Expect(!fresh.Fresh(&b, 1, ud, 4), "another info");
    fresh.Note(&a, 2, ud, 4);
    Expect(fresh.Fresh(&a, 2, ud, 4) && !fresh.Fresh(&a, 1, ud, 4), "a new note replaces the old");
    Expect(fresh.Count() == 1, "one entry per info");
    fresh.Forget(&a);
    Expect(!fresh.Fresh(&a, 2, ud, 4), "forgotten");
    fresh.Note(&a, 1, ud, 4);
    fresh.Reset();
    Expect(!fresh.Fresh(&a, 1, ud, 4), "a new draw starts empty");
    int many[FreshFlats::kEntries + 2];
    for (auto& m : many) fresh.Note(&m, 0, ud, 4);
    Expect(fresh.Count() == FreshFlats::kEntries, "bounded");
    Expect(!fresh.Fresh(&many[FreshFlats::kEntries + 1], 0, ud, 4), "an entry that did not fit is not fresh");

    std::mt19937 rng(7);
    std::uint64_t saved = 0, total = 0;
    for (int round = 0; round < 200; ++round) {
        std::vector<Draw> draws;
        for (int i = 0; i < 64; ++i) {
            Draw d{};
            d.stage_program = {int(rng() % 4), int(rng() % 4)};
            d.stage_regs = {int(rng() % 6), int(rng() % 6)};
            d.base = {rng() % 3, rng() % 3};
            d.prepared_checked = int(rng() % 3);
            d.motion = (rng() & 3) == 0;
            draws.push_back(d);
        }
        std::uint64_t with = 0, without = 0;
        const auto a_out = RunDraws(draws, true, with, round);
        const auto b_out = RunDraws(draws, false, without, round);
        Expect(a_out == b_out, "skipping a repeated refresh leaves every flat buffer the same");
        Expect(with <= without, "never more refreshes");
        saved += without - with;
        total += without;
    }
    Expect(saved > 0, "repeats were skipped");
    std::printf("fresh flats: %llu of %llu refreshes skipped in the model\n",
                static_cast<unsigned long long>(saved), static_cast<unsigned long long>(total));
}

void GxRefreshMask() {
    enum State { Fresh, Filled, Stale };
    for (std::uint32_t unfilled = 0; unfilled < 4; ++unfilled) {
        for (std::uint32_t covered = 0; covered < 4; ++covered) {
            for (int state_missing = 0; state_missing < 2; ++state_missing) {
                std::array<State, 2> stages{};
                std::uint32_t refreshed = 0;
                for (std::uint32_t s = 0; s < 2; ++s) {
                    if (!((unfilled >> s) & 1)) {
                        stages[s] = Fresh;
                        continue;
                    }
                    stages[s] = Filled;
                    if (state_missing || !((covered >> s) & 1)) {
                        stages[s] = Fresh;
                        refreshed |= 1u << s;
                    }
                }
                if (state_missing) continue;
                std::array<State, 2> full = stages, skipped = stages;
                std::uint32_t full_count = 0, skipped_count = 0;
                for (std::uint32_t s = 0; s < 2; ++s) {
                    full[s] = Fresh;
                    ++full_count;
                    if ((StagesToRefresh(unfilled, refreshed) >> s) & 1) {
                        skipped[s] = Fresh;
                        ++skipped_count;
                    }
                }
                Expect(full == skipped, "a torn record leaves the same flat buffers with fewer refreshes");
                Expect(skipped[0] == Fresh && skipped[1] == Fresh, "every stage ends fresh");
                Expect(skipped_count <= full_count, "never more refreshes");
            }
        }
    }
    Expect(StagesToRefresh(0, 0) == 0, "nothing unfilled, nothing refreshed again");
    Expect(StagesToRefresh(3, 1) == 2, "the stage refreshed after a failed fill is not refreshed again");
}

struct PaddedKey {
    std::uint8_t a;
    std::uint64_t b;
    std::uint16_t c;
    std::uint32_t d;
    bool operator==(const PaddedKey& o) const {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
};

struct BytesHash {
    std::size_t operator()(const PaddedKey& key) const noexcept {
        KeyBuilder k;
        k.Add(key);
        return k.Hash();
    }
};

void KeyHashKept() {
    std::mt19937_64 rng(3);
    std::vector<PaddedKey> keys;
    for (int i = 0; i < 2000; ++i) {
        PaddedKey key;
        std::memset(&key, 0, sizeof(key));
        key.a = std::uint8_t(rng());
        key.b = rng();
        key.c = std::uint16_t(rng());
        key.d = std::uint32_t(rng());
        keys.push_back(key);
    }
#ifdef HAVE_ROBIN_MAP
    tsl::robin_map<PaddedKey, int, BytesHash> map;
    for (int i = 0; i < 1000; ++i) map.emplace(keys[i], i);
    for (const auto& original : keys) {
        PaddedKey stored;
        std::memset(&stored, 0xAB, sizeof(stored));
        stored = original;
        const std::size_t kept = BytesHash{}(stored);
        const auto plain = map.find(stored);
        const auto prehashed = map.find(stored, kept);
        Expect(plain == prehashed, "a find with the kept hash finds the same entry");
    }
    std::printf("key hash: robin_map finds compared\n");
#else
    std::printf("key hash: robin_map not found, hash identity only\n");
#endif
    for (const auto& original : keys) {
        PaddedKey stored;
        std::memset(&stored, 0xAB, sizeof(stored));
        stored = original;
        Expect(BytesHash{}(stored) == BytesHash{}(stored), "hash is a function of the stored bytes");
    }
}

void KeyBuilderHash() {
    KeyBuilder a, b;
    a.Add(std::uint32_t(1));
    a.Add(std::uint32_t(2));
    b.Add(std::uint32_t(2));
    b.Add(std::uint32_t(1));
    Expect(a.Hash() != b.Hash(), "order matters");
    KeyBuilder c;
    c.Add(std::uint32_t(1));
    c.Add(std::uint32_t(2));
    Expect(a.Hash() == c.Hash(), "same words, same hash");
    KeyBuilder d;
    d.Add(std::uint32_t(1));
    Expect(d.Hash() != a.Hash(), "length matters");
    KeyBuilder e;
    for (std::uint32_t i = 0; i < KeyBuilder::kWords; ++i) e.Add(std::uint64_t(i));
    Expect(!e.Overflow(), "full but not over");
    e.Add(std::uint32_t(0));
    Expect(e.Overflow(), "overflow reported");
    e.Clear();
    Expect(!e.Overflow() && e.Count() == 0, "cleared");

    std::unordered_set<std::uint64_t> seen;
    std::mt19937 rng(11);
    std::array<std::uint32_t, 120> words{};
    for (auto& w : words) w = rng();
    for (int i = 0; i < 100000; ++i) {
        words[rng() % words.size()] ^= 1u << (rng() & 31);
        KeyBuilder k;
        for (const auto w : words) k.Add(w);
        seen.insert(k.Hash());
    }
    Expect(seen.size() > 99000, "single-bit changes give different hashes");
}

void MemoProbe() {
    Memo memo;
    int p1 = 0, p2 = 0;
    Expect(memo.Probe(true, 5) == nullptr, "unseen key misses");
    memo.Remember(5, &p1);
    Expect(memo.Probe(true, 5) == &p1, "remembered key hits");
    Expect(memo.Probe(false, 5) == nullptr, "an unkeyable draw misses");
    memo.Remember(6, nullptr);
    Expect(memo.Probe(true, 6) == nullptr, "a null pipeline is not remembered");
    Expect(memo.Compare(&p1, &p1), "same pipeline");
    Expect(!memo.Compare(&p1, &p2), "other pipeline");
    memo.Time(300, 1500);
    const Stats s = memo.TakeStats();
    Expect(s.probes == 4 && s.hits == 1, "probes and hits");
    Expect(s.misses[Unseen] == 2 && s.misses[Unkeyable] == 1, "misses by reason");
    Expect(s.compared == 2 && s.differ == 1, "compared and differ");
    Expect(!s.PassesGate(), "differences fail the gate");
    Expect(memo.Current().probes == 0, "stats restart after a report");
    char line[512];
    FormatStats(s, true, memo.Size(), line, sizeof(line));
    const std::string text = line;
    Expect(text.find("=2") != std::string::npos, "compare mode named");
    Expect(text.find("4 draws, 1 hits (25.0%)") != std::string::npos, "hit share printed");
    Expect(text.find("2 unseen, 1 unkeyable") != std::string::npos, "miss reasons printed");
    Expect(text.find("1 differ of 2 compared") != std::string::npos, "differences printed");
    Expect(text.find("key 0.300 us") != std::string::npos, "key time printed");

    Stats good;
    good.probes = 100;
    good.hits = 80;
    good.compared = 80;
    Expect(good.PassesGate(), "80% hits and 0 differences pass");
    good.hits = 79;
    Expect(!good.PassesGate(), "79% fails");
    good.hits = 90;
    good.compared = 0;
    Expect(!good.PassesGate(), "the gate needs a compare run");
}

void Bench() {
    std::mt19937 rng(5);
    std::array<std::uint32_t, 110> words{};
    for (auto& w : words) w = rng();
    KeyBuilder k;
    Memo memo;
    constexpr int kDraws = 400000;
    std::uint64_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kDraws; ++i) {
        words[i % 7] = std::uint32_t(i % 300);
        k.Clear();
        for (const auto w : words) k.Add(w);
        const std::uint64_t key = k.Hash();
        sink += reinterpret_cast<std::uintptr_t>(memo.Probe(true, key));
        memo.Remember(key, &sink);
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("bench: memo key of %zu words, probe and remember: %.3f us a draw (%llu)\n", words.size(),
                ns / kDraws / 1000.0, static_cast<unsigned long long>(sink & 1));
    FreshFlats fresh;
    int infos[2];
    const std::uint32_t ud[16] = {};
    const auto fresh_start = std::chrono::steady_clock::now();
    std::uint64_t hits = 0;
    for (int i = 0; i < kDraws; ++i) {
        fresh.Reset();
        for (int s = 0; s < 2; ++s) {
            fresh.Forget(&infos[s]);
            fresh.Note(&infos[s], 0x1000 + (i & 1), ud, 16);
        }
        for (int s = 0; s < 2; ++s) {
            hits += fresh.Fresh(&infos[s], 0x1000 + (i & 3), ud, 16);
        }
    }
    const double fresh_ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - fresh_start).count();
    std::printf("bench: fresh flat bookkeeping: %.5f us a draw (%llu)\n", fresh_ns / kDraws / 1000.0,
                static_cast<unsigned long long>(hits));
}
}

int main(int argc, char** argv) {
    PreparedReasons();
    FreshFlatsSkipOnlyRepeats();
    GxRefreshMask();
    KeyHashKept();
    KeyBuilderHash();
    MemoProbe();
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Bench();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("pipeline memo tests passed\n");
    return 0;
}
