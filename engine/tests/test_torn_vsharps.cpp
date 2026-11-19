// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/shim/engine/torn_vsharps.h"

namespace {
namespace T = BbEngine::TornVsharps;
using u64 = std::uint64_t;

struct Sharp {
    u64 base = 0;
    std::uint32_t stride = 0;
    std::uint32_t records = 0;
};
static_assert(sizeof(Sharp) == 16);

int g_failures = 0;
bool g_quiet = false;

void Expect(bool ok, const std::string& what) {
    if (ok) return;
    ++g_failures;
    if (!g_quiet) std::printf("FAILED: %s\n", what.c_str());
}

struct Case {
    std::vector<Sharp> got, table;
    std::vector<u64> keys;
};

Case Make() {
    Case c;
    c.table = {{0x1000, 16, 100}, {0x2000, 32, 50}, {0, 0, 0}, {0x4000, 12, 7}};
    c.got = c.table;
    c.got[1] = {0xdead0000, 32, 50};
    c.got[3] = {0x4000, 12, 9999};
    c.keys = {11, 22, 33, 44};
    return c;
}

int RunAll() {
    g_failures = 0;
    {
        Case c = Make();
        const std::vector<Sharp> before = c.got;
        std::vector<Sharp> noted;
        const auto n = T::Repair(true, c.got.data(), c.keys.data(), c.got.size(), [&](std::size_t i) { return c.table[i]; },
                                 [&](const Sharp& s) { noted.push_back(s); });
        Expect(n == 0, "intact: something was replaced");
        Expect(std::memcmp(before.data(), c.got.data(), before.size() * sizeof(Sharp)) == 0, "intact: the V#s changed");
        Expect(c.keys == std::vector<u64>({11, 22, 33, 44}), "intact: the keys changed");
        Expect(noted.empty(), "intact: a pending read was noted");
    }
    {
        Case c = Make();
        std::vector<Sharp> noted;
        const auto n = T::Repair(false, c.got.data(), c.keys.data(), c.got.size(), [&](std::size_t i) { return c.table[i]; },
                                 [&](const Sharp& s) { noted.push_back(s); });
        Expect(n == 2, "torn: " + std::to_string(n) + " V#s replaced, wanted 2");
        Expect(std::memcmp(c.table.data(), c.got.data(), c.table.size() * sizeof(Sharp)) == 0, "torn: the V#s are not the table's");
        Expect(c.keys == std::vector<u64>({11, 0, 33, 0}), "torn: the keys of the replaced V#s were not dropped, or others were");
        Expect(noted.size() == 2 && noted[0].base == 0x2000 && noted[1].base == 0x4000,
               "torn: the pending reads are not exactly the replaced table ranges");
    }
    {
        Case c = Make();
        c.got = c.table;
        std::vector<Sharp> noted;
        const auto n = T::Repair(false, c.got.data(), c.keys.data(), c.got.size(), [&](std::size_t i) { return c.table[i]; },
                                 [&](const Sharp& s) { noted.push_back(s); });
        Expect(n == 0 && noted.empty() && c.keys == std::vector<u64>({11, 22, 33, 44}),
               "torn but equal: an equal V# was replaced, noted or lost its key");
    }
    return g_failures;
}
}

int main(int argc, char** argv) {
#ifdef LANTERN_TORN_VSHARPS_MUTANTS
    if (argc > 1 && !std::strcmp(argv[1], "--mutants")) {
        g_quiet = true;
        if (RunAll()) {
            std::printf("torn-vsharps mutants: the unmutated run failed\n");
            return 1;
        }
        int survivors = 0;
        for (int m = 1; m < int(T::Mutant::kCount); ++m) {
            T::g_mutant = m;
            const int found = RunAll();
            std::printf("torn-vsharps mutants: mutant %d %s (%d checks)\n", m, found ? "caught" : "survived", found);
            survivors += found ? 0 : 1;
        }
        T::g_mutant = 0;
        if (survivors) return 1;
        std::printf("torn-vsharps mutants: every mutant caught (%d)\n", int(T::Mutant::kCount) - 1);
        return 0;
    }
    std::printf("torn-vsharps-test: the mutant build runs with --mutants\n");
    return 1;
#else
    (void)argc;
    (void)argv;
    const int failures = RunAll();
    std::printf("torn-vsharps-test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
#endif
}
