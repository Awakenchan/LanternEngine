// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

#include "gpu/shim/engine/gc_marks.h"

namespace {
using namespace BbEngine::GcMarks;

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr u64 MiB(u64 n) {
    return n << 20;
}

Marks OldStartMarks(s64 budget) {
    const s64 min_spacing_expected = budget - s64(1ull << 30);
    const s64 min_spacing_critical = budget - s64(512ull << 20);
    const s64 mem_threshold = std::min<s64>(budget, s64(8ull << 30));
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    Marks m;
    m.pressure = static_cast<u64>(std::max<u64>(std::min(budget - min_vacancy_expected, min_spacing_expected),
                                                u64(1536) << 20));
    m.critical = static_cast<u64>(std::max<u64>(std::min(budget - min_vacancy_critical, min_spacing_critical),
                                                u64(3) << 30));
    if (budget <= s64(3ull << 30)) {
        m.pressure = static_cast<u64>(budget / 100 * 75);
        m.critical = static_cast<u64>(budget / 100 * 88);
    }
    m.trigger = static_cast<u64>((budget - mem_threshold) / 2);
    return m;
}

Marks OldPassMarks(Marks current, const Marks& start, bool integrated, u64 forced, u64 percent, bool new_second,
                   bool low_vram, u64 live) {
    if (integrated || forced) {
        const u64 budget = forced ? forced : live;
        if (budget != 0) {
            current.trigger = budget / 10 * 7;
            current.pressure = budget / 100 * 85;
            current.critical = budget / 100 * 95;
        }
    } else if (percent != 0 && new_second) {
        if (live != 0) {
            const bool small = low_vram || live <= (u64(3) << 30);
            current.pressure = small ? live / 100 * percent : std::max(start.pressure, live / 100 * percent);
            current.critical =
                small ? live / 100 * (percent + 10) : std::max({start.critical, live / 100 * (percent + 10), current.pressure});
        }
    }
    return current;
}

Inputs CardInputs(u64 start_budget, u64 used, u64 image_bytes, u64 percent) {
    Inputs in;
    in.start = StartMarks(static_cast<s64>(start_budget));
    in.current = in.start;
    in.start_budget = start_budget;
    in.used = used;
    in.image_bytes = image_bytes;
    in.percent = percent;
    in.new_second = true;
    return in;
}

void TestStartMarks() {
    bool same = true;
    for (u64 mib = 256; mib <= 24 * 1024; mib += 97) same &= StartMarks(s64(MiB(mib))) == OldStartMarks(s64(MiB(mib)));
    Expect(same, "the start marks are shadPS4's from 256 MiB to 24 GiB");
    const Marks six = StartMarks(s64(MiB(4721)));
    Expect(six.pressure >> 20 == 1888 && six.critical >> 20 == 3776 && six.trigger == 0,
           "a 6 GB card's start budget gives the 40% and 80% marks and no trigger");
    const Marks small = StartMarks(s64(MiB(2560)));
    Expect(small.pressure == MiB(2560) / 100 * 75 && small.critical == MiB(2560) / 100 * 88,
           "a start budget of 3 GiB or less keeps the 75% and 88% marks");
}

void TestDerivation() {
    const auto live = [] { return MiB(5300); };
    const auto never = [] {
        failures += 1000;
        return u64(0);
    };

    const Inputs six = CardInputs(MiB(4721), MiB(2749), MiB(800), 0);
    const Result floored = Derive(six, never);
    Expect(floored.hysteresis, "the old marks come with hysteresis");
    Expect(floored.marks.pressure >= MiB(1949) + MiB(512), "6 GB, budget off: the pressure mark sits over the non-image VRAM plus an image budget");
    Expect(floored.marks.pressure == MiB(1949) + MiB(4721) / 4, "6 GB, budget off: the image budget is a quarter of the start budget");
    Expect(floored.marks.critical > floored.marks.pressure, "6 GB, budget off: the critical mark stays over the pressure mark");
    Expect(floored.marks.critical == six.start.critical, "6 GB, budget off: the critical mark keeps shadPS4's when it is higher");
    Expect(floored.marks.trigger == 0, "6 GB, budget off: the trigger stays");

    Inputs lifted = six;
    lifted.used = MiB(3300);
    const Result both = Derive(lifted, never);
    Expect(both.marks.pressure == MiB(2500) + MiB(4721) / 4 && both.marks.critical == both.marks.pressure + MiB(512),
           "non-image VRAM near the critical mark lifts both marks, critical 512 MiB over pressure");

    Inputs heavy = six;
    heavy.used = MiB(3800);
    const Result raised = Derive(heavy, never);
    Expect(raised.marks.pressure == MiB(4721) - MiB(1024) && raised.marks.critical == MiB(4721) - MiB(512),
           "non-image VRAM past the critical mark lifts both marks only up to 1 GiB and 512 MiB under the start budget");

    Inputs full = six;
    full.used = MiB(7000);
    full.image_bytes = MiB(500);
    const Result capped = Derive(full, never);
    Expect(capped.marks.pressure < MiB(4721) && capped.marks.critical < MiB(4721) && capped.marks.critical > capped.marks.pressure,
           "non-image VRAM over the whole budget never lifts a mark past the budget");

    const Result budget = Derive(CardInputs(MiB(4721), MiB(2749), MiB(800), 80), live);
    Expect(!budget.hysteresis, "the budget rule has no hysteresis");
    Expect(budget.marks.pressure >> 20 == 4240 && budget.marks.critical >> 20 == 4770,
           "6 GB, budget on: 80% and 90% of the live budget");

    const Inputs big = CardInputs(MiB(14336), MiB(6000), MiB(3500), 0);
    const Result big_old = Derive(big, never);
    Expect(big_old.marks == big.start, "16 GB, budget off: the marks are shadPS4's");

    const Inputs tiny = CardInputs(MiB(1536), MiB(1400), MiB(300), 0);
    const Result tiny_old = Derive(tiny, never);
    Expect(ImageBudget(MiB(1536)) == MiB(512), "the image budget is at least 512 MiB");
    Expect(tiny_old.marks.pressure == tiny.start.pressure && tiny_old.marks.critical == tiny.start.critical,
           "a 2 GB card, budget off: the floor stops at shadPS4's marks when they are over the budget ceilings");

    Inputs integrated = CardInputs(MiB(4000), MiB(9000), MiB(500), 0);
    integrated.integrated = true;
    const Result igpu = Derive(integrated, [] { return MiB(10000); });
    Expect(igpu.marks.trigger == MiB(10000) / 10 * 7 && igpu.marks.pressure == MiB(10000) / 100 * 85 &&
               igpu.marks.critical == MiB(10000) / 100 * 95 && !igpu.hysteresis,
           "an integrated GPU keeps the 70/85/95% live budget rule and no floor");
    Inputs forced = CardInputs(MiB(4721), MiB(2749), MiB(800), 0);
    forced.forced_budget = MiB(6000);
    const Result forced_marks = Derive(forced, never);
    Expect(forced_marks.marks.pressure == MiB(6000) / 100 * 85 && !forced_marks.hysteresis,
           "BB_GC_BUDGET_MB keeps its fixed rule with the percent off");

    Inputs low = CardInputs(MiB(7000), MiB(5000), MiB(2000), 80);
    low.low_vram = true;
    const Result amd = Derive(low, [] { return MiB(7000); });
    Expect(amd.marks.pressure == MiB(7000) / 100 * 80 && amd.marks.critical == MiB(7000) / 100 * 90,
           "a low-VRAM AMD card keeps the small-budget marks");
    const Result small = Derive(CardInputs(MiB(2800), MiB(2000), MiB(900), 80), [] { return MiB(2800); });
    Expect(small.marks.pressure == MiB(2800) / 100 * 80, "a budget of 3 GiB or less keeps the small-budget marks");
}

void TestDefaultUnchanged() {
    bool same = true, no_hysteresis = true, same_mode = true;
    for (u64 card : std::initializer_list<u64>{4096, 6144, 8192, 12288, 16384, 24576}) {
        const u64 start_budget = MiB(card) / 8 * 7;
        for (u64 live_mib : std::initializer_list<u64>{0, 2048, card / 2, card * 7 / 8, card}) {
            for (u64 non_image : std::initializer_list<u64>{MiB(300), MiB(1950), MiB(3500), MiB(card) / 2}) {
                for (u64 images : std::initializer_list<u64>{MiB(200), MiB(900), MiB(card) / 3}) {
                    for (bool new_second : {false, true}) {
                        for (bool low_vram : {false, true}) {
                            Inputs in = CardInputs(start_budget, non_image + images, images, 80);
                            in.current.pressure += MiB(7);
                            in.new_second = new_second;
                            in.low_vram = low_vram;
                            const Result now = Derive(in, [&] { return MiB(live_mib); });
                            const Marks old = OldPassMarks(in.current, in.start, false, 0, 80, new_second, low_vram,
                                                           MiB(live_mib));
                            same &= now.marks == old;
                            no_hysteresis &= !now.hysteresis;
                            bool latched = false;
                            for (u64 step = 0; step < 40; ++step) {
                                const u64 used = old.pressure - MiB(200) + MiB(step * 10);
                                same_mode &= Pressured(latched, used, now.marks.pressure) == (used >= old.pressure);
                                latched = Latch(now.hysteresis, latched, used, now.marks.pressure);
                            }
                        }
                    }
                }
            }
        }
    }
    Expect(same, "budget on (the default): every pass derives today's marks");
    Expect(no_hysteresis, "budget on (the default): no hysteresis");
    Expect(same_mode, "budget on (the default): pressure mode is today's used >= mark on every pass");
}

void TestHysteresis() {
    const u64 mark = MiB(3000);
    bool latched = false;
    const auto pass = [&](u64 used) {
        const bool on = Pressured(latched, used, mark);
        latched = Latch(true, latched, used, mark);
        return on;
    };
    Expect(!pass(mark - 1), "under the mark: no pressure");
    Expect(pass(mark + MiB(1)), "1 MiB over the mark: pressure");
    Expect(pass(mark - MiB(1)), "1 MiB under the mark after pressure: still pressure");
    Expect(pass(mark - mark / 100 * 4), "4% under the mark: still pressure");
    Expect(pass(ExitMark(mark)), "at 5% under the mark: still pressure");
    Expect(!pass(ExitMark(mark) - 1), "past 5% under the mark: pressure ends");
    Expect(!pass(mark - MiB(1)), "back between the marks: no pressure until the mark itself");
    Expect(pass(mark), "at the mark: pressure again");
    Expect(ExitMark(mark) == mark / 100 * 95, "the exit mark is 5% under the pressure mark");

    latched = false;
    int flips = 0, plain_flips = 0;
    bool previous = false, plain_previous = false;
    for (u64 i = 0; i < 1000; ++i) {
        const u64 used = mark - mark / 50 + (mark / 25) * (i % 7) / 6;
        const bool on = pass(used);
        flips += on != previous;
        previous = on;
        const bool plain = Pressured(false, used, mark);
        plain_flips += plain != plain_previous;
        plain_previous = plain;
    }
    Expect(flips == 1 && plain_flips > 100, "1000 passes 2% around the mark enter pressure once and never flip back");

    Expect(!Latch(false, true, mark * 2, mark), "no hysteresis: nothing is latched");
}

void TestUnreachable() {
    const u64 start_budget = MiB(4721);
    const u64 pressure_ceiling = start_budget - MiB(1024);
    const u64 critical_ceiling = start_budget - MiB(512);
    bool always_reachable = true, under_ceilings = true, at_ceiling = true;
    for (u64 non_image = MiB(500); non_image <= MiB(6000); non_image += MiB(37)) {
        for (u64 images = 0; images <= MiB(3000); images += MiB(53)) {
            const Inputs in = CardInputs(start_budget, non_image + images, images, 0);
            const Result r = Derive(in, [] { return u64(0); });
            under_ceilings &= r.marks.pressure <= pressure_ceiling && r.marks.critical <= critical_ceiling &&
                              r.marks.critical > r.marks.pressure;
            if (non_image + ImageBudget(start_budget) <= pressure_ceiling) {
                if (Pressured(false, in.used, r.marks.pressure)) {
                    const u64 target = ExitMark(r.marks.pressure);
                    always_reachable &= in.used - images < target;
                }
            } else {
                at_ceiling &= r.marks.pressure == pressure_ceiling;
            }
        }
    }
    Expect(always_reachable, "budget off: under the ceiling, pressure only starts when freeing images can bring usage under the exit mark");
    Expect(under_ceilings, "budget off: the marks never pass 1 GiB and 512 MiB under the start budget, critical over pressure");
    Expect(at_ceiling, "budget off: non-image VRAM past the ceiling holds the pressure mark at the ceiling");

    const Inputs stuck = CardInputs(start_budget, MiB(1950) + MiB(800), MiB(800), 0);
    Expect(NonImage(stuck.used, stuck.image_bytes) >= stuck.start.pressure, "the reported case: non-image VRAM alone is over shadPS4's mark");
    const Result r = Derive(stuck, [] { return u64(0); });
    Expect(!Pressured(false, stuck.used, r.marks.pressure), "the reported case: no eviction for pressure, only the idle rule");
    Expect(NonImage(MiB(100), MiB(300)) == 0, "image bytes over usage count no non-image VRAM");
}

void TestDistinctCount() {
    u32 seen[3] = {};
    u32 report = 1;
    u64 kept = 0;
    for (int visit = 0; visit < 50; ++visit)
        for (u32& image : seen) kept += FirstInReport(image, report);
    Expect(kept == 3, "three images kept on 50 passes each count three in one report");
    ++report;
    kept = 0;
    for (int visit = 0; visit < 50; ++visit) kept += FirstInReport(seen[0], report);
    Expect(kept == 1, "the next report counts an image again, once");
    u32 fresh = 0;
    Expect(FirstInReport(fresh, 1), "a new image counts in the first report");
}

std::string Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    std::string s = text.str();
    std::string out;
    for (char c : s)
        if (c != '\r') out += c;
    return out;
}

void TestInFlightGuards() {
    const std::string src = Read("gpu/shadps4/video_core/texture_cache/texture_cache.cpp");
    Expect(!src.empty(), "texture_cache.cpp is readable from the engine folder");
    const std::string guard = "        if (recent_tick != 0 && (image.tick_accessed_last >= recent_tick ||\n"
                              "                                 !scheduler.IsFree(image.tick_accessed_last))) {\n";
    const auto at = src.find(guard);
    Expect(at != std::string::npos, "the recent-use and GPU-tick guard is unchanged");
    const auto clean_up = src.find("const auto clean_up = [&](ImageId image_id) {");
    const auto free_at = src.find("        FreeImage(image_id);\n", clean_up);
    const auto store_at = src.find("StoreTiledImage(image_id,", clean_up);
    Expect(clean_up != std::string::npos && at > clean_up && at < store_at && at < free_at,
           "the guard runs before any write-back or free in the collector");
    Expect(src.find(": 48, 60);") != std::string::npos, "BB_GC_RECENT_FRAMES still defaults to 48 passes");
    Expect(src.find(": 32, 240);") != std::string::npos, "BB_IMAGE_FREE_DELAY_FRAMES still defaults to 32 passes");
    Expect(src.find("recent_frames == 0\n            ? 0\n            : (gc_tick >= recent_frames") != std::string::npos,
           "the recent tick still counts every image as recent until enough passes ran");
    Expect(src.find("scheduler.DeferOperation") != std::string::npos, "the graveyard still waits for the GPU before destroying");
}

int RunSuite() {
    failures = 0;
    TestStartMarks();
    TestDerivation();
    TestDefaultUnchanged();
    TestHysteresis();
    TestUnreachable();
    TestDistinctCount();
    TestInFlightGuards();
    return failures;
}
}

int main() {
#ifdef LANTERN_GC_MARKS_MUTANTS
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("gc marks mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("gc marks mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("gc marks mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("gc marks mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("gc marks: %d failures\n", failures);
        return 1;
    }
    std::printf("gc marks: all tests passed\n");
    return 0;
#endif
}
