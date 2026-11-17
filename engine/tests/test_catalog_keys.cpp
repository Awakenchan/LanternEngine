// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/catalog_keys.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
namespace C = BbEngine::CatalogKeys;

int g_failures = 0;
bool g_quiet = false;

void Check(bool ok, const char* what) {
    if (ok) return;
    ++g_failures;
    if (!g_quiet) std::printf("FAIL: %s\n", what);
}

constexpr std::size_t kMotionOffset = 336;

struct Metas {
    std::map<std::uint64_t, C::StageMotion> stages;
    C::StageMotion operator()(std::uint64_t hash) const {
        const auto it = stages.find(hash);
        return it == stages.end() ? C::StageMotion{} : it->second;
    }
};

std::vector<unsigned char> Key(std::uint64_t vs, std::uint64_t fs, std::uint32_t motion, std::uint32_t compute = 0) {
    std::vector<unsigned char> key(C::kHeaderBytes + kMotionOffset + 64, 0);
    const std::uint32_t version = 6;
    std::memcpy(key.data(), &version, 4);
    std::memcpy(key.data() + 4, &compute, 4);
    std::memcpy(key.data() + C::kHeaderBytes + C::kVertexStage * 8, &vs, 8);
    std::memcpy(key.data() + C::kHeaderBytes + C::kFragmentStage * 8, &fs, 8);
    std::memcpy(key.data() + C::kHeaderBytes + kMotionOffset, &motion, 4);
    return key;
}

struct Named {
    std::string name;
    std::vector<unsigned char> key;
};

std::vector<std::string> Kept(const std::vector<Named>& keys, const Metas& metas) {
    std::vector<std::string> kept;
    for (const auto& k : keys) {
        if (!C::StaleKey(k.key.data(), k.key.size(), kMotionOffset, metas)) kept.push_back(k.name);
    }
    return kept;
}

int RunAll() {
    g_failures = 0;
    Metas metas;
    metas.stages[0x10] = {true, true, false};
    metas.stages[0x11] = {true, true, true};
    metas.stages[0x12] = {true, false, true};
    metas.stages[0x20] = {true, false, false};
    metas.stages[0x21] = {true, false, true};

    Check(!C::Stale(0, {true, true, false}, {true, false, false}), "plain key with plain shaders is kept");
    Check(!C::Stale(1, {true, true, true}, {true, false, true}), "motion key with motion shaders is kept");
    Check(C::Stale(0, {true, true, true}, {true, false, false}), "motion vertex shader in a key without motion is stale");
    Check(C::Stale(1, {true, true, false}, {true, false, true}), "fragment shader reading motion without a vertex shader writing it is stale");
    Check(C::Stale(0, {}, {true, false, true}), "fragment shader reading motion without a vertex shader is stale");
    Check(!C::Stale(1, {true, true, true}, {true, false, false}), "motion key whose fragment shader does not read motion is kept");
    Check(!C::Stale(0, {true, false, true}, {true, false, false}), "a non-vertex stage's motion flag does not count");

    const std::vector<Named> keys = {
        {"good-plain", Key(0x10, 0x20, 0)},
        {"stale-vertex", Key(0x11, 0x20, 0)},
        {"good-motion", Key(0x11, 0x21, 1)},
        {"stale-fragment", Key(0x10, 0x21, 1)},
        {"good-capture", Key(0x11, 0x20, 1)},
        {"good-other-stage", Key(0x12, 0x20, 0)},
        {"compute", Key(0x11, 0x20, 0, 1)},
    };
    const auto kept = Kept(keys, metas);
    const std::vector<std::string> expected = {"good-plain", "good-motion", "good-capture", "good-other-stage", "compute"};
    Check(kept == expected, "only the good keys are exported and seeded");

    std::vector<unsigned char> short_key(C::kHeaderBytes + 16, 0);
    Check(!C::StaleKey(short_key.data(), short_key.size(), kMotionOffset, metas), "a short key is left to the other checks");
    Check(!C::StaleKey(short_key.data(), 4, kMotionOffset, metas), "a key without its header is left to the other checks");
    return g_failures;
}
}

int main(int argc, char** argv) {
#ifdef LANTERN_CATALOG_KEYS_MUTANTS
    if (argc > 1 && !std::strcmp(argv[1], "--mutants")) {
        g_quiet = true;
        if (RunAll()) {
            std::printf("catalog-keys mutants: the unmutated run failed\n");
            return 1;
        }
        int survivors = 0;
        for (int m = 1; m < int(C::Mutant::kCount); ++m) {
            C::g_mutant = m;
            const int found = RunAll();
            std::printf("catalog-keys mutants: mutant %d %s (%d checks)\n", m, found ? "caught" : "survived", found);
            survivors += found ? 0 : 1;
        }
        C::g_mutant = 0;
        if (survivors) return 1;
        std::printf("catalog-keys mutants: every mutant caught (%d)\n", int(C::Mutant::kCount) - 1);
        return 0;
    }
    std::printf("catalog-keys-test: the mutant build runs with --mutants\n");
    return 1;
#else
    (void)argc;
    (void)argv;
    const int failures = RunAll();
    std::printf("catalog-keys-test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
#endif
}
