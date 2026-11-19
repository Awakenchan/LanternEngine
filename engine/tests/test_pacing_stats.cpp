// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gpu/shim/lantern_frame_clock.h"
#include "gpu/shim/lantern_pacing_stats.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool Near(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance;
}

void EnableStats() {
#ifdef _WIN32
    _putenv_s("BB_FRAME_STATS", "1");
#else
    setenv("BB_FRAME_STATS", "1", 1);
#endif
}
}

int main(int argc, char** argv) {
    using namespace LanternPacingStats;

    {
        const Summary empty = Summarize(nullptr, 0);
        Expect(empty.count == 0 && empty.mean == 0.0 && empty.sd == 0.0, "empty window: zeros");
        std::uint32_t one[] = {7};
        const Summary single = Summarize(one, 1);
        Expect(single.count == 1 && single.mean == 7.0 && single.sd == 0.0 && single.min == 7.0 && single.max == 7.0,
               "one sample: mean, no spread");
        std::uint32_t known[] = {2, 4, 4, 4, 5, 5, 7, 9};
        const Summary s = Summarize(known, 8);
        Expect(s.mean == 5.0, "mean of the textbook set");
        Expect(Near(s.sd, 2.0, 1e-12), "population sd of the textbook set is 2");
        Expect(s.min == 2.0 && s.max == 9.0, "min and max");
        std::uint32_t big[] = {4000000000u, 4000000002u};
        const Summary b = Summarize(big, 2);
        Expect(b.mean == 4000000001.0 && Near(b.sd, 1.0, 1e-6), "no overflow near the top of 32 bits");
    }

    {
        Expect(Percentile(nullptr, 0, 95.0) == 0.0, "percentile of nothing");
        std::uint32_t v[100];
        for (std::uint32_t i = 0; i < 100; ++i) v[i] = 100 - i;
        Expect(Percentile(v, 100, 95.0) == 95.0, "p95 of 1..100 is 95 (nearest rank)");
        for (std::uint32_t i = 0; i < 100; ++i) v[i] = i + 1;
        Expect(Percentile(v, 100, 50.0) == 50.0, "p50 of 1..100 is 50");
        Expect(Percentile(v, 100, 100.0) == 100.0, "p100 is the max");
        Expect(Percentile(v, 100, 0.0) == 1.0, "p0 is the min");
        std::uint32_t three[] = {30, 10, 20};
        Expect(Percentile(three, 3, 95.0) == 30.0, "p95 of three samples is the largest");
        std::uint32_t one[] = {5};
        Expect(Percentile(one, 1, 95.0) == 5.0, "p95 of one sample");
    }

    {
        static Ring<8> ring;
        std::uint32_t out[8];
        Expect(ring.Drain(out) == 0, "new ring is empty");
        for (std::uint32_t i = 1; i <= 5; ++i) ring.Push(i);
        Expect(ring.Drain(out) == 5 && out[0] == 1 && out[4] == 5, "drain returns what was pushed, in order");
        Expect(ring.Drain(out) == 0, "a second drain is empty");
        for (std::uint32_t i = 1; i <= 20; ++i) ring.Push(i);
        const std::size_t n = ring.Drain(out);
        Expect(n == 8 && out[0] == 13 && out[7] == 20, "an overfull window keeps the newest samples");
        ring.Push(99);
        Expect(ring.Drain(out) == 1 && out[0] == 99, "drains continue after wrapping");
    }

    {
        using namespace LanternFrameClock;
        Expect(FloorDiv(-7, 2) == -4 && FloorDiv(7, 2) == 3 && CeilDiv(-7, 2) == -3 && CeilDiv(7, 2) == 4,
               "floor and ceiling division round the right way below zero");
        const std::int64_t vb = 8333333;
        const Grid locked{1000, vb, vb};
        const std::int64_t s3 = 1000 + 3 * vb;
        Expect(NextStart(12345, 0, locked) == 12345, "the first frame starts at once");
        Expect(NextStart(s3 - vb + 100, s3 - vb, locked) == s3, "an early frame waits for its slot");
        Expect(NextStart(s3 + 10, s3 - vb, locked) == s3 + 10, "a frame past its slot starts at once");
        Expect(NextStart(s3 + vb / 2, s3 - vb, locked) == s3 + vb / 2, "an overrun never waits for another slot");
        Expect(NextStart(s3 - vb + 100, s3 - vb + 2000000, locked) == s3 + 2000000 - vb / 8,
               "off the grid, an early frame moves an eighth of a period toward it");
        Expect(NextStart(s3 - vb + 100, s3 - vb + 500000, locked) == s3, "within an eighth, it lands on the slot");
        Expect(NextStart(s3 - vb + 100, s3 - vb - 20000, locked) == s3,
               "a slot a hair late is taken, never a whole period later");
        const Grid two{1000, 2 * vb, vb};
        Expect(NextStart(1000 + 5 * vb, 1000 + 4 * vb, two) == 1000 + 6 * vb, "two refreshes a frame: two refreshes apart");
        std::int64_t carry = 0;
        Expect(Step(s3, s3 - vb, locked, carry) == vb && carry == 0, "one slot: one refresh");
        Expect(Step(s3 + vb, s3 - vb, locked, carry) == 2 * vb && carry == 0, "two slots: two refreshes");
        Expect(Step(5, 0, locked, carry) == vb && carry == 0, "the first step is one period");
        carry = 0;
        Expect(Step(s3 + vb / 4, s3 - vb, locked, carry) == vb && carry == vb / 4,
               "an overrun is a whole refresh and its remainder is carried");
        Expect(Step(s3 + vb / 4 + vb + vb / 4 + vb / 4, s3 + vb / 4, locked, carry) == 2 * vb && carry < 0 && carry >= -vb / 4 - 1,
               "the carried remainder is paid back on the next step");
        const Grid own{0, 8333333, 0};
        carry = 0;
        Expect(NextStart(100, 0, own) == 100, "own clock, first frame: no wait");
        Expect(NextStart(100, 50, own) == 50 + 8333333, "own clock: one period after the last start");
        Expect(NextStart(20000000, 50, own) == 20000000, "own clock: a late frame starts at once");
        Expect(Step(50 + 8333333, 50, own, carry) == 8333333, "own clock: the step is the interval");
        Expect(StepMicros(8333333, 1.0f / 120) == 8333, "a period of 1/120 is 8333 us, the game's own target");
        Expect(StepMicros(8264463, 1.0f / 120) == 8333, "never under the game's target, so its limiter never waits");
        Expect(StepMicros(16666667, 1.0f / 120) == 16667, "two refreshes");
        Expect(PacesTarget(1.0f / 120, 120.0f) && !PacesTarget(1.0f / 30, 120.0f) && !PacesTarget(0.0f, 120.0f),
               "paced only while the game's target is the chosen rate");
        Expect(Near(OffWholePeriods(16666666.0, double(vb)), 0.0, 1.0) &&
                   Near(OffWholePeriods(8.0e6, double(vb)), double(vb) - 8.0e6, 1.0),
               "distance from whole refreshes");

        struct Case {
            int rate;
            std::int64_t work_us;
        };
        const Case cases[] = {{120, 5000}, {120, 8500}, {120, 9000}, {120, 10000}, {120, 12000},
                              {60, 10000}, {60, 17000}, {60, 18000}, {60, 20000}, {60, 24000}};
        constexpr int kFrames = 6000;
        for (const Case& c : cases) {
            const std::int64_t refresh = 1000000000 / c.rate;
            const std::int64_t game_target = std::int64_t(1000000 / c.rate) * 1000;
            const std::int64_t work = c.work_us * 1000;
            const Grid g{0, refresh, refresh};
            constexpr std::int64_t kFirstStart = 1;
            std::int64_t start = kFirstStart, step_sum = 0, step_carry = 0;
            bool whole = true;
            for (int i = 1; i < kFrames; ++i) {
                const std::int64_t next = NextStart(start + work, start, g);
                const std::int64_t step = Step(next, start, g, step_carry);
                whole = whole && step % refresh == 0;
                step_sum += step;
                start = next;
            }
            const double paced = double(kFrames - 1) / (double(start - kFirstStart) / 1e9);
            std::int64_t old = 0;
            for (int i = 1; i < kFrames; ++i) old = std::max(old + work, old + game_target);
            const double before = double(kFrames - 1) / (double(old) / 1e9);
            const bool early = work <= refresh;
            std::printf("throughput: %d FPS locked on %d Hz, work %.1f ms: paced %.1f FPS, before %.1f FPS\n", c.rate,
                        c.rate, double(c.work_us) / 1000.0, paced, before);
            char what[128];
            std::snprintf(what, sizeof what, "%d on %d Hz, %.1f ms work: paced at least as fast as the game's limiter",
                          c.rate, c.rate, double(c.work_us) / 1000.0);
            Expect(early ? paced >= double(c.rate) - 0.01 : paced >= before - 0.01, what);
            std::snprintf(what, sizeof what, "%d on %d Hz, %.1f ms work: every step whole refreshes, sum within one",
                          c.rate, c.rate, double(c.work_us) / 1000.0);
            Expect(whole && std::llabs(step_sum - (start - kFirstStart)) <= refresh, what);
        }

        std::int64_t start = locked.anchor;
        int on_slot = 0, overruns = 0;
        std::uint32_t seed = 12345;
        for (int i = 0; i < 2000; ++i) {
            seed = seed * 1103515245u + 12345u;
            const std::int64_t work = 2000000 + std::int64_t(seed >> 8) % 4000000 + (i % 97 == 0 ? 6000000 : 0);
            overruns += work > vb;
            start = NextStart(start + work, start, locked);
            on_slot += (start - locked.anchor) % vb == 0;
        }
        Expect(overruns > 0 && on_slot >= 2000 - 5 * overruns, "early frames return to the flip slots within a few frames of an overrun");

        std::int64_t game_last = 0, now = 0;
        int old_off = 0;
        seed = 12345;
        for (int i = 0; i < 2000; ++i) {
            seed = seed * 1103515245u + 12345u;
            const std::int64_t finish = now + 2000000 + std::int64_t(seed >> 8) % 5000000;
            now = std::max(finish, game_last + 8333000);
            if (game_last) old_off += OffWholePeriods(double(now - game_last), 8334720.0) > 1000.0;
            game_last = now;
        }
        Expect(old_off == 1999, "the game's own 8333 us limiter on a 119.98 Hz display: no step is a whole refresh");
    }

    {
        char line[640];
        const std::size_t quiet = Line(line, sizeof line);
        Expect(quiet > 0 && std::strstr(line, "game steps none seen") && std::strstr(line, "flips taken 0"),
               "empty window still prints a line");
        Line(line, sizeof line, 312.46);
        Expect(std::strncmp(line, "Frame pacing+ at 312.5 s:", 25) == 0, "the line carries the seconds since start");
    }

    {
        using namespace LanternFrameClock;
        constexpr int kCalls = 2000000;
        const auto bench = [&](const char* what) {
            Grid grid;
            std::int64_t seen = 0;
            const std::int64_t begin = SteadyNs();
            for (int i = 0; i < kCalls; ++i) {
                Current(grid);
                seen += grid.period;
            }
            const double ns = double(SteadyNs() - begin) / kCalls;
            std::printf("cost: %.1f ns a frame-start grid read, %s\n", ns, what);
            Expect(seen > 0, "a grid read was timed");
        };
        Publish(1, 8333333, 8333333);
        bench("uncontended");
        std::atomic<bool> done{false};
        std::thread video_out([&] {
            std::int64_t slot = 1;
            while (!done.load(std::memory_order_relaxed)) {
                Publish(slot += 8333333, 8333333, 8333333);
                std::this_thread::sleep_for(std::chrono::nanoseconds(8333333));
            }
        });
        bench("VideoOut publishing at 120 Hz");
        done.store(true, std::memory_order_relaxed);
        video_out.join();
        Clear();
    }

    EnableStats();
    {
        Step(1.0f / 120.0f, 1.0f / 120.0f);
        PresentTaken(1);
        PadRead();
        Presented();
        char line[640];
        Line(line, sizeof line);
        const bool on = Enabled();
        if (!on) {
            std::printf("note: flag was cached off before the environment was set; recorder checks skipped\n");
        } else {
            std::uint32_t scratch[kRing];
            for (int i = 0; i < 50; ++i) {
                Step(0.008f + 0.001f * (i % 2), 0.0085f);
                PresentTaken(std::size_t(i % 3));
                PadRead();
                PadRead();
                Presented();
            }
            Presented();
            Expect(g_state.step.Drain(scratch) == 50, "every step after the first gives an interval");
            Expect(g_state.dt.Drain(scratch) == 50, "game dt recorded each step");
            const Summary off = Summarize(scratch, g_state.dt_off.Drain(scratch));
            Expect(off.count == 50 && Near(off.mean / 1e6, 0.5, 0.01), "|dt - average| is 0.5 ms");
            Expect(g_state.present.Drain(scratch) == 50, "flip intervals");
            const Summary depth = Summarize(scratch, g_state.depth.Drain(scratch));
            Expect(depth.count == 50 && depth.max == 2.0, "queue depth at take");
            Expect(g_state.pad.Drain(scratch) == 50, "one pad latency per present that had a read before it");
            Step(0.0f, 0.0085f);
            Step(2.0f, 0.0085f);
            Expect(g_state.dt.Drain(scratch) == 0, "unreadable or absurd dt is left out");
            g_state.step.Drain(scratch);

            for (int i = 0; i < 12; ++i) {
                Step(0.0083347f, 0.0083f, 8334720);
                Paced(250000);
                PresentTaken(1);
                PadRead();
                Presented();
            }
            Line(line, sizeof line);
            std::printf("example: %s\n", line);
            Expect(std::strstr(line, "game steps 12,") != nullptr, "line counts the steps");
            Expect(std::strstr(line, "flips taken 12,") != nullptr, "line counts the flips");
            Expect(std::strstr(line, "pad read to next present") != nullptr, "line names the pad metric");
            Expect(std::strstr(line, "dt off whole display periods mean 0.000 ms") != nullptr,
                   "steps of one refresh are on whole display periods");
            Expect(std::strstr(line, "paced at frame start 12, wait mean 0.250 ms") != nullptr, "line counts paced starts");
            Expect(std::strlen(line) < 600, "the line fits the frame report's buffer");
            Step(0.0100f, 0.0100f, 8334720);
            Step(0.0100f, 0.0100f, 8334720);
            Line(line, sizeof line);
            Expect(std::strstr(line, "dt off whole display periods mean 1.665 ms") != nullptr,
                   "a 10 ms step is 1.665 ms off one refresh");
            Expect(std::strstr(line, "not paced at frame start") != nullptr, "no paced starts in this window");
            Step(0.0100f, 0.0100f);
            Step(0.0100f, 0.0100f);
            Line(line, sizeof line);
            Expect(std::strstr(line, "flips not locked to the display") != nullptr, "no vblank: no whole-period check");

            const bool bench = argc > 1 && std::strcmp(argv[1], "--bench") == 0;
            const int rounds = bench ? 2000000 : 200000;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < rounds; ++i) {
                Step(0.0083f, 0.0083f);
                PresentTaken(1);
                PadRead();
                Presented();
            }
            const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            const double per_frame = ns / rounds;
            std::printf("cost: %.0f ns a frame (step + flip take + pad read + present, all four recorders on)\n",
                        per_frame);
            Expect(per_frame < 1000.0, "under 1 us a frame");
        }
    }

    if (failures) {
        std::printf("pacing-stats-test: %d failed\n", failures);
        return 1;
    }
    std::printf("pacing-stats-test: all passed\n");
    return 0;
}
