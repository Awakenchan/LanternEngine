// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>

#ifdef LANTERN_VBLANK_WATCH_MUTANTS
namespace LanternVblankWatch {
inline constexpr int kToday = 8;
inline int g_mutant = 0;
inline bool Mutant(int n) {
    return g_mutant == n || (g_mutant == kToday && (n == 1 || n == 2));
}
}
#define LANTERN_VBLANK_WATCH_PICK(n, wrong, right) (::LanternVblankWatch::Mutant(n) ? (wrong) : (right))
#else
#define LANTERN_VBLANK_WATCH_PICK(n, wrong, right) (right)
#endif

namespace LanternVblankWatch {

inline constexpr int kMutants = 8;
inline constexpr double kLearn = 120;
inline constexpr double kRelearn = 1200;
inline constexpr double kLateFloorNs = 1e6;

enum class Event { None, Varies, SteadyAgain };

struct Learner {
    bool variable = false, was_steady = false, measured = false, published = false;
    double period = 0, predicted = 0, base = 0, refreshes = 0;
    int off_score = 0;

    void Restart(double now) {
        period = refreshes = 0;
        predicted = base = now;
        off_score = 0;
    }

    void Reset() {
        variable = was_steady = false;
        Restart(0);
    }

    Event Wake(double now) {
        measured = false;
        if (predicted == 0) {
            Restart(now);
            return Event::None;
        }
        const double interval = now - predicted;
        if (period == 0) {
            period = interval;
            refreshes = 1;
            predicted = now;
            return Event::None;
        }
        Event event = Event::None;
        const double whole = std::round(interval / period);
        const double residual = interval - whole * period;
        const double early = period * 0.1;
        const double late = LANTERN_VBLANK_WATCH_PICK(2, early, std::max(early, kLateFloorNs));
        if (whole < 1 || residual < -LANTERN_VBLANK_WATCH_PICK(3, late, early) || residual > late) {
            if (whole >= 1) {
                predicted += whole * period;
                refreshes += whole;
            } else {
                predicted = now;
            }
            if ((off_score += 2) >= 16) {
                if (was_steady && LANTERN_VBLANK_WATCH_PICK(5, true, !variable)) {
                    event = Event::Varies;
                }
                variable = variable || was_steady;
                Restart(now);
            }
        } else {
            off_score = std::max(0, off_score - 1);
            refreshes += whole;
            period = (now - base) / refreshes;
            predicted += whole * period;
            predicted += (now - predicted) * 0.1;
        }
        const double needed = variable ? LANTERN_VBLANK_WATCH_PICK(4, kLearn, kRelearn) : kLearn;
        const bool steady = period > 0 && refreshes >= needed && off_score <= 2;
        if (steady && variable && LANTERN_VBLANK_WATCH_PICK(1, false, true)) {
            variable = false;
            event = Event::SteadyAgain;
        }
        was_steady = was_steady || steady;
        measured = true;
        published = steady && !variable;
        return event;
    }
};

inline unsigned LockRefreshes(double hz, double nominal, unsigned limit) {
    if (hz <= 0 || limit == 0) {
        return 0;
    }
    const double ratio = hz / double(limit);
    const double whole = std::round(ratio);
    const bool display_rate = LANTERN_VBLANK_WATCH_PICK(6, true, nominal > 0 && std::abs(hz - nominal) < nominal * 0.005);
    const bool share = whole >= 1 && std::abs(ratio - whole) < whole * LANTERN_VBLANK_WATCH_PICK(7, 0.05, 0.01);
    return display_rate && share ? unsigned(whole) : 0;
}

}
