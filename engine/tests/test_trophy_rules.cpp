// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/trophy_rules.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {
namespace T = BbEngine::TrophyRules;

int g_failures = 0;
bool g_quiet = false;

void Check(bool ok, const char* what) {
    if (ok) return;
    ++g_failures;
    if (!g_quiet) std::printf("FAIL: %s\n", what);
}

constexpr std::uint32_t kPer = 1000;

struct FakeBlock {
    std::uint32_t kind = 1;
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(kPer / 8);
};

struct FakeFlags {
    std::int32_t mode = 0;
    std::map<std::uint32_t, FakeBlock> blocks;

    bool Per(std::uint32_t* per) const {
        *per = kPer;
        return true;
    }
    bool Mode(std::int32_t* m) const {
        *m = mode;
        return true;
    }
    std::uint64_t Block(std::uint32_t block, std::uint32_t* kind) const {
        const auto it = blocks.find(block);
        if (it == blocks.end()) return 0;
        *kind = it->second.kind;
        return std::uint64_t(block) + 1;
    }
    bool Byte(std::uint64_t data, std::uint32_t at, std::uint8_t* out) const {
        const auto it = blocks.find(std::uint32_t(data - 1));
        if (it == blocks.end() || at >= it->second.bytes.size()) return false;
        *out = it->second.bytes[at];
        return true;
    }
    void Set(std::uint32_t id, std::uint32_t kind) {
        FakeBlock& b = blocks[id / kPer];
        b.kind = kind;
        const std::uint32_t bit = id % kPer;
        b.bytes[bit / 8] |= std::uint8_t(0x80u >> (bit % 8));
    }
    void Own(std::uint32_t block) { blocks[block].kind = 1; }
};

FakeFlags HostWorldAsGuest() {
    FakeFlags f;
    f.mode = 1;
    for (std::uint32_t b : {6u, 7u, 8u}) f.Own(b);
    for (std::uint32_t id : {12701800u, 13201800u, 12100000u, 12100002u, 12100180u, 13200960u, 12801800u, 1000u, 9000u}) f.Set(id, 2);
    f.Set(6600, 1);
    return f;
}

FakeFlags OwnWorld() {
    FakeFlags f;
    f.mode = 0;
    for (std::uint32_t b : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 12100u, 12701u, 13201u}) f.Own(b);
    f.Set(12411800, 1);
    return f;
}

std::vector<int> Read(T::Sync& sync, const FakeFlags& f, std::uint64_t record) {
    std::vector<int> got;
    sync.Read(record, true, true, f.mode, [&](std::uint32_t id, bool* v) { return T::OwnFlag(f, id, v); },
              [&](int trophy) { got.push_back(trophy); });
    return got;
}

int RunAll() {
    g_failures = 0;
    {
        const FakeFlags guest = HostWorldAsGuest();
        bool v = true;
        Check(!T::OwnFlag(guest, 12701800, &v), "mode 1: a host boss flag in a world-copy block is refused");
        Check(!T::OwnFlag(guest, 12100000, &v), "mode 1: a host ending flag in the Dream copy is refused");
        Check(!T::OwnFlag(guest, 1000, &v) && !T::OwnFlag(guest, 9000, &v), "mode 1: blocks 1 and 9 are the copy");
        Check(T::OwnFlag(guest, 6600, &v) && v, "mode 1: block 6 stays the guest's own");
        Check(T::OwnFlag(guest, 7100, &v) && !v, "mode 1: block 7 stays the guest's own");
        Check(!T::OwnFlag(guest, 13501800, &v), "an unknown block is never read");

        FakeFlags kind2_own_world = OwnWorld();
        kind2_own_world.Set(12601800, 2);
        Check(!T::OwnFlag(kind2_own_world, 12601800, &v), "mode 0: an external buffer block is not the own pool");

        FakeFlags kept_own = guest;
        kept_own.blocks[12701].kind = 1;
        Check(!T::OwnFlag(kept_own, 12701800, &v), "mode 1: a map block is not own even if the kind looks own");
    }
    {
        T::Sync sync;
        const FakeFlags guest = HostWorldAsGuest();
        Check(Read(sync, guest, 0x1000).empty(), "mode 1 with the host's boss and ending flags reports nothing");
        Check(Read(sync, guest, 0x1000).empty(), "mode 1 again still reports nothing");
        const FakeFlags home = OwnWorld();
        const std::vector<int> got = Read(sync, home, 0x1000);
        Check(got == std::vector<int>{14}, "back in mode 0 on the same record, only the own trophy 14 is reported");
        Check(Read(sync, home, 0x1000).empty(), "the same record is not read twice");
    }
    {
        T::Sync sync;
        Check(Read(sync, HostWorldAsGuest(), 0x2000).empty(), "a guest in the host's world reports nothing");
        sync.Read(0, false, true, 0, [](std::uint32_t, bool*) { return false; }, [](int) {});
        FakeFlags home = OwnWorld();
        home.Set(6600, 1);
        const std::vector<int> got = Read(sync, home, 0x3000);
        Check(got == (std::vector<int>{1, 14}), "after the load home, the own ending and the own boss are reported");
    }
    {
        T::Sync sync;
        std::vector<int> got;
        const FakeFlags home = OwnWorld();
        sync.Read(0x4000, true, false, 0, [&](std::uint32_t id, bool* v) { return T::OwnFlag(home, id, v); },
                  [&](int t) { got.push_back(t); });
        Check(got.empty(), "an unreadable flag mode reads nothing");
    }
    {
        Check(T::AcceptRelayed(100000, 100000 + 59000), "a relayed trophy 59 s after the guest's fight is accepted");
        Check(T::AcceptRelayed(100000, 100000), "a relayed trophy during the fight is accepted");
        Check(!T::AcceptRelayed(100000, 100000 + 61000), "a relayed trophy 61 s after the fight is dropped");
        Check(!T::AcceptRelayed(0, 30000), "a guest that never saw a boss fight drops the relayed trophy");
    }
    {
        Check(!std::strcmp(T::RoleName(3), "host") && !std::strcmp(T::RoleName(6), "guest"), "host and guest roles");
        Check(!std::strcmp(T::RoleName(-1), "own") && !std::strcmp(T::RoleName(0), "own"), "no session is own");
    }
    return g_failures;
}
}

int main(int argc, char** argv) {
#ifdef LANTERN_TROPHY_RULES_MUTANTS
    if (argc > 1 && !std::strcmp(argv[1], "--mutants")) {
        g_quiet = true;
        if (RunAll()) {
            std::printf("trophy-rules mutants: the unmutated run failed\n");
            return 1;
        }
        int survivors = 0;
        for (int m = 1; m < int(T::Mutant::kCount); ++m) {
            T::g_mutant = m;
            const int found = RunAll();
            std::printf("trophy-rules mutants: mutant %d %s (%d checks)\n", m, found ? "caught" : "survived", found);
            survivors += found ? 0 : 1;
        }
        T::g_mutant = 0;
        if (survivors) return 1;
        std::printf("trophy-rules mutants: every mutant caught (%d)\n", int(T::Mutant::kCount) - 1);
        return 0;
    }
    std::printf("trophy-rules-test: the mutant build runs with --mutants\n");
    return 1;
#else
    (void)argc;
    (void)argv;
    const int failures = RunAll();
    std::printf("trophy-rules-test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
#endif
}
