// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "gpu/shim/lantern_vblank_watch.h"

namespace {
namespace V = LanternVblankWatch;

int g_failed = 0;
const char* g_first = nullptr;

void Check(bool ok, const char* what) {
    if (!ok) {
#ifdef LANTERN_VBLANK_WATCH_MUTANTS
        if (V::g_mutant == V::kToday) {
            std::printf("  the pre-fix watcher fails: %s\n", what);
        }
#else
        std::printf("FAIL: %s\n", what);
#endif
        if (!g_first) {
            g_first = what;
        }
        ++g_failed;
    }
}

struct Rng {
    std::uint64_t s;
    double Next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return double(s >> 11) * (1.0 / 9007199254740992.0);
    }
    double Range(double lo, double hi) { return lo + (hi - lo) * Next(); }
};

constexpr double kMs = 1e6;

struct Display {
    V::Learner learner;
    double phase = 1e12, period = 5 * kMs, t = 1e12;
    std::int64_t k = 0;
    int varies = 0, again = 0;

    void Note(V::Event e) {
        varies += e == V::Event::Varies;
        again += e == V::Event::SteadyAgain;
    }
    void Wake(double delay) {
        const std::int64_t after = std::int64_t(std::floor((t - phase) / period)) + 1;
        k = std::max(k + 1, after);
        t = phase + double(k) * period + delay;
        Note(learner.Wake(t));
    }
    void Free(double interval) {
        t += interval;
        Note(learner.Wake(t));
    }
    void Mode(double hz) {
        phase += double(k) * period;
        k = 0;
        period = 1e9 / hz;
    }
    bool Steady() const { return learner.published; }
    bool PeriodNear(double hz) const { return std::abs(learner.period - 1e9 / hz) < 1e9 / hz * 0.005; }
};

void Settle(Display& d, Rng& r, int wakes) {
    for (int i = 0; i < wakes; ++i) {
        d.Wake(r.Range(0, 0.2 * kMs));
    }
}

void TestFirstLearn() {
    Display d;
    Rng r{1};
    Settle(d, r, 100);
    Check(!d.Steady(), "200 Hz: not steady before 120 refreshes");
    Settle(d, r, 200);
    Check(d.Steady(), "200 Hz: steady after the first learn");
    Check(d.PeriodNear(200), "200 Hz: the period is 5 ms");
    Check(d.varies == 0 && d.again == 0, "200 Hz: no events while steady");
}

void TestLoadingBurst() {
    Display d;
    Rng r{2};
    Settle(d, r, 3000);
    Check(d.Steady(), "loading burst: steady before the load");
    for (int i = 0; i < 300; ++i) {
        d.Wake(r.Range(1.5 * kMs, 4.5 * kMs));
    }
    Check(!d.Steady(), "loading burst: not steady during the burst");
    Check(d.varies == 1, "loading burst: refresh varies logged once");
    int back = -1;
    for (int i = 0; i < 1600; ++i) {
        d.Wake(r.Range(0, 0.2 * kMs));
        if (d.Steady() && back < 0) {
            back = i;
        }
    }
    Check(back >= 0, "loading burst: steady again within the re-learn window");
    Check(back < 0 || back >= 800, "loading burst: the re-learn is longer than the first learn");
    Check(d.PeriodNear(200), "loading burst: the period is 5 ms again");
    Check(d.again == 1, "loading burst: steady again logged once");
    const int varies = d.varies;
    Settle(d, r, 2000);
    Check(d.Steady() && d.varies == varies, "loading burst: stays steady after recovering");
}

void TestLateWakes() {
    Display d;
    Rng r{3};
    Settle(d, r, 2000);
    int steady = 0;
    for (int i = 0; i < 3000; ++i) {
        d.Wake(i % 2 ? r.Range(0.6 * kMs, 0.95 * kMs) : r.Range(0, 0.2 * kMs));
        steady += d.Steady();
    }
    Check(d.varies == 0, "late wakes under 1 ms at 200 Hz do not read as a varying refresh");
    Check(steady == 3000, "late wakes under 1 ms at 200 Hz keep the vblanks steady");
    Check(d.PeriodNear(200), "late wakes: the period stays 5 ms");
}

void TestEarlyWakes() {
    Display d;
    Rng r{4};
    Settle(d, r, 2000);
    for (int i = 0; i < 400; ++i) {
        d.Wake(i % 2 ? -r.Range(0.6 * kMs, 0.95 * kMs) : r.Range(0, 0.2 * kMs));
    }
    Check(d.varies == 1, "early wakes past a tenth of the period still count against steadiness");
}

void TestRealVrr() {
    Display d;
    Rng r{5};
    bool steady = false;
    for (int i = 0; i < 30000; ++i) {
        d.Free(r.Range(5 * kMs, 12 * kMs));
        steady = steady || d.Steady();
    }
    Check(!steady, "real VRR, 5-12 ms frames: never steady");

    Display e;
    e.period = 1e9 / 165;
    Rng q{6};
    Settle(e, q, 2000);
    Check(e.Steady(), "VRR after a fixed stretch: steady while fixed");
    steady = false;
    for (int i = 0; i < 30000; ++i) {
        e.Free(q.Range(5 * kMs, 12 * kMs));
        steady = steady || (e.varies > 0 && e.Steady());
    }
    Check(!steady && e.again == 0, "VRR after a fixed stretch: never steady again once it varies");
    Check(e.varies == 1, "VRR after a fixed stretch: refresh varies logged once");
}

void TestModeChange() {
    Display d;
    Rng r{7};
    Settle(d, r, 2000);
    d.Mode(144);
    int back = -1;
    for (int i = 0; i < 2500; ++i) {
        d.Wake(r.Range(0, 0.2 * kMs));
        if (d.Steady() && back < 0) {
            back = i;
        }
    }
    Check(d.varies == 1, "200 to 144 Hz without a window event: refresh varies logged");
    Check(back >= 0 && d.Steady(), "200 to 144 Hz without a window event: steady again");
    Check(d.PeriodNear(144), "200 to 144 Hz without a window event: the period is 6.944 ms");

    Display e;
    Rng q{8};
    Settle(e, q, 2000);
    e.Mode(144);
    e.learner.Reset();
    Settle(e, q, 300);
    Check(e.Steady() && e.PeriodNear(144), "200 to 144 Hz with a window event: steady at 144 Hz after the first learn");
    Check(e.varies == 0, "200 to 144 Hz with a window event: no refresh varies");
}

void TestLockRatio() {
    const double displays[] = {144, 165, 200, 240};
    const unsigned limits[] = {72, 82, 100, 120};
    const unsigned want[4][4] = {{2, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, 2, 0}, {0, 0, 0, 2}};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            Check(V::LockRefreshes(displays[i], displays[i], limits[j]) == want[i][j],
                  "lock ratio: frame generation limits 72/82/100/120 at 144/165/200/240 Hz");
        }
    }
    Check(V::LockRefreshes(200.007, 200, 100) == 2, "lock ratio: 200.007 Hz measured on a 200 Hz mode locks");
    Check(V::LockRefreshes(200.9, 200, 100) == 2, "lock ratio: within half a percent of the mode locks");
    Check(V::LockRefreshes(201.1, 200, 100) == 0, "lock ratio: more than half a percent off the mode does not lock");
    Check(V::LockRefreshes(100, 165, 100) == 0, "lock ratio: a steady VRR rate off the display mode does not lock");
    Check(V::LockRefreshes(144, 0, 72) == 0, "lock ratio: no display mode, no lock");
    Check(V::LockRefreshes(144, 144, 0) == 0, "lock ratio: no limit, no lock");
    Check(V::LockRefreshes(144, 144, 144) == 1, "lock ratio: the display rate locks one refresh a frame");
}

int RunSuite() {
    g_failed = 0;
    g_first = nullptr;
    TestFirstLearn();
    TestLoadingBurst();
    TestLateWakes();
    TestEarlyWakes();
    TestRealVrr();
    TestModeChange();
    TestLockRatio();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_VBLANK_WATCH_MUTANTS
    int survivors = 0;
    for (int m = 0; m <= V::kMutants; ++m) {
        V::g_mutant = m;
        const int found = RunSuite();
        if (m == 0) {
            if (found) {
                std::printf("vblank watch mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("vblank watch mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("vblank watch mutants: mutant %d caught by %d checks (first: %s)\n", m, found, g_first);
        }
    }
    if (survivors) return 1;
    std::printf("vblank watch mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("vblank-watch-test: %d failure(s)\n", g_failed);
        return 1;
    }
    std::printf("vblank-watch-test: ok\n");
    return 0;
#endif
}
