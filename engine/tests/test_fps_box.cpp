// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/fps_box.h"
#include <cstdint>
#include <cstdio>
#include <iterator>

namespace B = BbEngine::FpsBox;

namespace {
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    ++g_failed;
#ifndef LANTERN_FPS_BOX_MUTANTS
    std::printf("FAIL: %s\n", what);
#else
    (void)what;
#endif
}

void TestFirstFit() {
    B::Fit fit;
    Check(B::Width(fit, 1, 20, 200.0f, 8.0f, 0) == 216.0f, "the first text fits its width plus the padding on both sides");
    B::Fit odd;
    Check(B::Width(odd, 1, 20, 200.2f, 8.0f, 0) == 217.0f, "a fractional text width rounds up as ImGui lays it out");
}

void TestShrinksOnShorterText() {
    B::Fit fit;
    B::Width(fit, 1, 96, 900.0f, 8.0f, 0);
    Check(B::Width(fit, 1, 33, 300.0f, 8.0f, 500) == 916.0f, "the load screen's short line keeps the box at first");
    Check(B::Width(fit, 1, 33, 300.0f, 8.0f, 2000) == 916.0f, "and still keeps it just before the hold ends");
    Check(B::Width(fit, 1, 33, 300.0f, 8.0f, 2500) == 316.0f, "the short line held for two seconds shrinks the box");
    Check(B::Width(fit, 1, 96, 900.0f, 8.0f, 3000) == 916.0f, "the long line grows it back at once");
}

void TestAlternatingLength() {
    B::Fit fit;
    B::Width(fit, 1, 34, 310.0f, 8.0f, 0);
    bool steady = true;
    for (int i = 1; i <= 20; ++i) {
        const bool shorter = i % 2 == 1;
        steady &= B::Width(fit, 1, shorter ? 33 : 34, shorter ? 300.0f : 310.0f, 8.0f, std::int64_t(i) * 500) == 326.0f;
    }
    Check(steady, "GPU 99% and GPU 100% alternating every refresh keep one width");
    B::Fit up;
    B::Width(up, 1, 33, 300.0f, 8.0f, 0);
    Check(B::Width(up, 1, 34, 310.0f, 8.0f, 500) == 326.0f, "one character more grows the box at once");
}

void TestSteadyDigits() {
    B::Fit fit;
    B::Width(fit, 1, 33, 300.0f, 8.0f, 0);
    Check(B::Width(fit, 1, 33, 296.0f, 8.0f, 500) == 316.0f, "narrower digits at the same length keep the width");
    Check(B::Width(fit, 1, 33, 296.0f, 8.0f, 5000) == 316.0f, "and keep it however long they hold");
    Check(B::Width(fit, 1, 33, 303.0f, 8.0f, 5500) == 319.0f, "wider digits at the same length widen the box");
    Check(B::Width(fit, 1, 33, 298.0f, 8.0f, 6000) == 319.0f, "and it stays that wide while the length holds");
}

void TestKey() {
    B::Fit fit;
    B::Width(fit, 1, 33, 600.0f, 8.0f, 0);
    Check(B::Width(fit, 2, 33, 300.0f, 8.0f, 100) == 316.0f, "a smaller text scale fits again at once");
}

void TestLeft() {
    Check(B::Left(1900.0f, 316.0f, 1.0f) == 1584.0f, "top right ends at the anchor");
    Check(B::Left(960.0f, 316.0f, 0.5f) == 802.0f, "top centre is centred on the anchor");
    Check(B::Left(12.0f, 316.0f, 0.0f) == 12.0f, "top left starts at the anchor");
}

int RunSuite() {
    g_failed = 0;
    TestFirstFit();
    TestShrinksOnShorterText();
    TestAlternatingLength();
    TestSteadyDigits();
    TestKey();
    TestLeft();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_FPS_BOX_MUTANTS
    const char* names[] = {"", "never shrinks on shorter text", "shrinks without the hold", "ignores the scale key",
                           "drops the padding", "ignores the pivot", "follows every digit"};
    static_assert(std::size(names) == B::kMutants + 1);
    int survivors = 0;
    for (int m = 0; m <= B::kMutants; ++m) {
        B::g_mutant = m;
        const int found = RunSuite();
        if (m == 0) {
            if (found) {
                std::printf("fps box mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("fps box mutants: mutant %d (%s) survived\n", m, names[m]);
            ++survivors;
        } else {
            std::printf("fps box mutants: mutant %d (%s) caught by %d checks\n", m, names[m], found);
        }
    }
    if (survivors) return 1;
    std::printf("fps box mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("fps box: %d failures\n", g_failed);
        return 1;
    }
    std::printf("fps box: all tests passed\n");
    return 0;
#endif
}
