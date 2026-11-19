// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>
#include <vector>

#include "gpu/shim/engine/texset_static.h"

namespace {
using namespace BbEngine::TexsetStatic;
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
#ifndef LANTERN_TEXSET_MUTANTS
        std::printf("FAIL: %s\n", what);
#else
        (void)what;
#endif
    }
}

ImageFacts Held() {
    ImageFacts image;
    image.held = true;
    return image;
}

void TestClassifyImage() {
    Expect(ClassifyImage(Held(), true) == Static, "a held settled texture is static");
    Expect(ClassifyImage(ImageFacts{}, false) == Static, "held is not required with GX textures off");
    Expect(ClassifyImage(ImageFacts{}, true) == NotHeld, "an image no GX texture holds is not static");
    ImageFacts null;
    null.null = true;
    Expect(ClassifyImage(null, true) == Static, "a null descriptor does not break a set");
    ImageFacts proxy = Held();
    proxy.proxy = true;
    proxy.target = true;
    Expect(ClassifyImage(proxy, true) == Proxy, "a scene proxy is named first");
    ImageFacts target = Held();
    target.target = true;
    target.moved = true;
    Expect(ClassifyImage(target, true) == Target, "a target comes before its layout change");
    ImageFacts moved = Held();
    moved.moved = true;
    Expect(ClassifyImage(moved, true) == Layout, "a transit that added a barrier is a layout change");
    ImageFacts moved_unheld;
    moved_unheld.moved = true;
    Expect(ClassifyImage(moved_unheld, true) == Layout, "a layout change comes before not held");
}

void TestRank() {
    Expect(Rank(Static, Static) == Static, "static stays static");
    Expect(Rank(Static, Layout) == Layout, "the first reason is taken");
    Expect(Rank(Proxy, Layout) == Proxy, "a later lesser reason does not replace a greater one");
    Expect(Rank(Layout, Proxy) == Proxy, "a later greater reason replaces a lesser one");
    Expect(Rank(Target, Static) == Target, "a later static image keeps the reason");
}

void TestClassifySet() {
    ImageFacts null;
    null.null = true;
    ImageFacts proxy = Held();
    proxy.proxy = true;
    ImageFacts moved = Held();
    moved.moved = true;
    const std::vector<ImageFacts> clean{Held(), null, Held()};
    const SetCount a = ClassifySet(clean, true);
    Expect(a.reason == Static, "a held settled set with a null slot is static");
    Expect(a.checks == 2, "null slots run no image check");
    Expect(a.transits == 2, "every non-proxy image runs a transit");
    const std::vector<ImageFacts> mixed{proxy, moved, Held()};
    const SetCount b = ClassifySet(mixed, true);
    Expect(b.reason == Proxy, "the greatest reason of the set is kept");
    Expect(b.checks == 3, "proxies are checked");
    Expect(b.transits == 2, "proxies take no transit");
    const std::vector<ImageFacts> reversed{moved, proxy};
    Expect(ClassifySet(reversed, true).reason == Proxy, "the order of images does not change the reason");
    const std::vector<ImageFacts> unheld{ImageFacts{}, ImageFacts{}};
    Expect(ClassifySet(unheld, false).reason == Static, "GX textures off: an unheld set is static");
    Expect(ClassifySet(unheld, true).reason == NotHeld, "GX textures on: an unheld set is not");
    Expect(ClassifySet({}, true).reason == Static && ClassifySet({}, true).checks == 0, "an empty set is static and checks nothing");
}

void TestTally() {
    Tally tally;
    Add(tally, SetCount{Static, 3, 3});
    Add(tally, SetCount{Layout, 2, 2});
    Add(tally, SetCount{Proxy, 4, 1});
    Expect(tally.hits == 3, "every hit is counted");
    Expect(tally.reasons[Static] == 1 && tally.reasons[Layout] == 1 && tally.reasons[Proxy] == 1, "hits by reason");
    Expect(tally.checks == 9 && tally.transits == 6, "checks and transits of all hits");
    Expect(tally.static_checks == 3 && tally.static_transits == 3, "checks and transits of static hits only");
    Expect(!Suspect(tally), "static hits are not suspect");
    Tally none;
    Expect(!Suspect(none), "no hits are not suspect");
    Add(none, SetCount{NotHeld, 1, 1});
    Expect(Suspect(none), "hits with none static are suspect");
}

void TestFormat() {
    Tally tally;
    tally.held_required = true;
    for (int i = 0; i < 4; ++i) Add(tally, SetCount{Static, 2, 2});
    Add(tally, SetCount{Layout, 1, 1});
    Add(tally, SetCount{Layout, 1, 1});
    char line[640];
    Format(line, sizeof line, tally, 2);
    Expect(std::strstr(line, "held by GX textures") != nullptr, "the line says held was required");
    Expect(std::strstr(line, "2.0 of 3.0 a frame over 2 frames") != nullptr, "static and all hits a frame");
    Expect(std::strstr(line, "static hits run 4.0 image checks and 4.0 transits") != nullptr, "static work a frame");
    Expect(std::strstr(line, "layout change 1.0;") != nullptr, "reasons a frame");
    Expect(std::strstr(line, "INSTRUMENT SUSPECT") == nullptr, "no suspect line with static hits");
    Tally suspect;
    Add(suspect, SetCount{NotHeld, 1, 1});
    Format(line, sizeof line, suspect, 1);
    Expect(std::strstr(line, "INSTRUMENT SUSPECT") != nullptr, "the suspect line is printed");
    Expect(std::strstr(line, "GX textures off") != nullptr, "the line says held was not required");
    char small[16];
    const int used = Format(small, sizeof small, tally, 2);
    Expect(used == static_cast<int>(sizeof small) - 1 && std::strlen(small) == sizeof small - 1, "a short buffer is filled and ended");
}

int RunSuite() {
    failures = 0;
    TestClassifyImage();
    TestRank();
    TestClassifySet();
    TestTally();
    TestFormat();
    return failures;
}
}

int main() {
#ifdef LANTERN_TEXSET_MUTANTS
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("texset static mutants: FAIL: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("texset static mutants: FAIL: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("texset static mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("texset static mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("texset static: FAIL: %d failures\n", failures);
        return 1;
    }
    std::printf("texset static: all tests passed\n");
    return 0;
#endif
}
