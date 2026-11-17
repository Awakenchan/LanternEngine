// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#ifdef LANTERN_FG_PACING_MUTANTS
namespace LanternFrameGenPacing {
inline int g_mutant = 0;
}
#define LANTERN_FG_PACING_PICK(n, wrong, right) (::LanternFrameGenPacing::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_FG_PACING_PICK(n, wrong, right) (right)
#endif

namespace LanternFrameGenPacing {

inline constexpr int kMutants = 15;

inline bool LockWanted(bool lock_allowed, std::uint32_t limit, bool frame_gen_active, bool fsr3) {
    return lock_allowed && limit != 0 &&
           LANTERN_FG_PACING_PICK(1, !frame_gen_active, LANTERN_FG_PACING_PICK(2, true, !frame_gen_active || fsr3));
}

inline const char* UnlockedReason(bool frame_gen_active, bool fsr3, bool have_vblank) {
    if (frame_gen_active && !fsr3) {
        return "no steady vblanks to lock to";
    }
    if (LANTERN_FG_PACING_PICK(3, true, !have_vblank)) {
        return frame_gen_active ? "no steady vblanks to lock to; frame generation places its frames by its own timer"
                                : "no steady vblanks to lock to";
    }
    return "the frame rate is not a whole share of the display's refresh";
}

inline std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

inline std::int64_t SlotOffsetNs(std::int64_t frame_period, std::int64_t vblank_period) {
    return std::min<std::int64_t>(frame_period / 16 + 300000, vblank_period / 2);
}

inline std::int64_t RealSlotNs(std::int64_t generated_done, std::int64_t vblank_last, std::int64_t vblank_period,
                               std::int64_t frame_period) {
    if (vblank_period <= 0 || frame_period <= 0) {
        return 0;
    }
    const std::int64_t refreshes = (frame_period + vblank_period / 2) / vblank_period;
    if (refreshes < LANTERN_FG_PACING_PICK(4, 1, 2)) {
        return 0;
    }
    const std::int64_t k = LANTERN_FG_PACING_PICK(5, FloorDiv(generated_done - vblank_last, vblank_period) + 1,
                                                  FloorDiv(generated_done - vblank_last, vblank_period));
    const std::int64_t step = LANTERN_FG_PACING_PICK(6, std::int64_t(1), refreshes / 2);
    return vblank_last + (k + step) * vblank_period +
           LANTERN_FG_PACING_PICK(7, std::int64_t(0), SlotOffsetNs(frame_period, vblank_period));
}

inline bool CapToRefresh(bool even_pacing_setting, bool fsr3) {
    return LANTERN_FG_PACING_PICK(8, even_pacing_setting, even_pacing_setting || fsr3);
}

inline bool CapInputShown(bool attempted, [[maybe_unused]] bool reached) {
    return LANTERN_FG_PACING_PICK(9, reached, attempted);
}

enum Note : unsigned { NoteHeld = 1, NoteUneven = 2, NoteUnshowable = 4, NoteLowReal = 8 };
inline constexpr std::uint32_t kLowRealFps = 40;
inline constexpr std::uint32_t kLowestKnownRefreshHz = 30;

inline unsigned Notes(std::uint32_t base, std::uint32_t limit, int delivered, std::uint32_t refresh, float game_fps) {
    unsigned notes = 0;
    if ((limit != 0 && limit < kLowRealFps) ||
        (game_fps > 1.0f && game_fps < LANTERN_FG_PACING_PICK(10, 20.0f, float(kLowRealFps)))) {
        notes |= NoteLowReal;
    }
    if (delivered <= 1 || refresh < kLowestKnownRefreshHz) {
        return notes;
    }
    const std::uint64_t shown = std::uint64_t(limit) * std::uint64_t(delivered);
    if (limit == 0 || LANTERN_FG_PACING_PICK(11, shown >= refresh, shown > refresh)) {
        notes |= NoteUnshowable;
    } else if (refresh % shown != 0) {
        notes |= NoteUneven;
    }
    if (base != 0 && limit != 0 && LANTERN_FG_PACING_PICK(12, limit <= base, limit < base)) {
        notes |= NoteHeld;
    }
    return notes;
}

inline float ShownPercent(float previous, bool reached) {
    const float sample = reached ? 100.0f : 0.0f;
    if (previous < 0.0f) {
        return sample;
    }
    return LANTERN_FG_PACING_PICK(13, sample, previous * 0.95f + sample * 0.05f);
}

enum Kind : std::uint8_t { KindReal = 0, KindGenerated = 1 };

struct PresentRecord {
    std::uint64_t t_present1_ns = 0;
    std::uint32_t acquire_wait_us = 0;
    std::uint8_t kind = KindReal;
    std::uint8_t ok = 0;
};

inline constexpr std::size_t kRecords = 1024;

class PresentLog {
public:
    void Add(const PresentRecord& record) {
        ring[next] = record;
        next = (next + 1) % kRecords;
        count = std::min(count + 1, kRecords);
    }
    std::size_t Ordered(PresentRecord* out) const {
        const std::size_t first = (next + kRecords - count) % kRecords;
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = ring[LANTERN_FG_PACING_PICK(14, i % kRecords, (first + i) % kRecords)];
        }
        return count;
    }
    void Clear() {
        count = 0;
    }
    std::size_t Count() const {
        return count;
    }

private:
    std::array<PresentRecord, kRecords> ring{};
    std::size_t next = 0;
    std::size_t count = 0;
};

struct Spread {
    double p50 = 0, p95 = 0, p99 = 0, max = 0;
};

inline Spread SpreadOf(double* values, std::size_t n) {
    Spread s;
    if (n == 0) {
        return s;
    }
    std::sort(values, values + n);
    const auto at = [&](double p) {
        const std::size_t rank = std::size_t(p * double(n) + 0.999999);
        return values[std::min(n, std::max<std::size_t>(rank, 1)) - 1];
    };
    s.p50 = at(0.50);
    s.p95 = at(0.95);
    s.p99 = at(0.99);
    s.max = values[n - 1];
    return s;
}

struct Summary {
    std::uint32_t calls = 0, real = 0, generated = 0, not_shown = 0, over = 0;
    Spread gap, generated_to_real, real_to_generated, acquire_wait;
};

inline Summary Summarize(const PresentRecord* records, std::size_t n, std::int64_t vblank_ns,
                         std::array<double, kRecords>& scratch) {
    Summary s;
    s.calls = std::uint32_t(n);
    for (std::size_t i = 0; i < n; ++i) {
        (records[i].kind == KindGenerated ? s.generated : s.real) += 1;
        if (!records[i].ok) {
            ++s.not_shown;
        }
    }
    std::size_t m = 0;
    for (std::size_t i = 1; i < n; ++i) {
        const double gap = double(records[i].t_present1_ns - records[i - 1].t_present1_ns) / 1e6;
        scratch[m++] = gap;
        if (vblank_ns > 0 && gap * 1e6 > LANTERN_FG_PACING_PICK(15, 1.0, 1.5) * double(vblank_ns)) {
            ++s.over;
        }
    }
    s.gap = SpreadOf(scratch.data(), m);
    for (const std::uint8_t from : {std::uint8_t(KindGenerated), std::uint8_t(KindReal)}) {
        m = 0;
        for (std::size_t i = 1; i < n; ++i) {
            if (records[i - 1].kind == from && records[i].kind != from) {
                scratch[m++] = double(records[i].t_present1_ns - records[i - 1].t_present1_ns) / 1e6;
            }
        }
        (from == KindGenerated ? s.generated_to_real : s.real_to_generated) = SpreadOf(scratch.data(), m);
    }
    for (std::size_t i = 0; i < n; ++i) {
        scratch[i] = double(records[i].acquire_wait_us) / 1e3;
    }
    s.acquire_wait = SpreadOf(scratch.data(), n);
    return s;
}

inline int FormatSummary(char* out, std::size_t size, const Summary& s, double window_s, bool have_vblank,
                         const char* present_mode, std::uint32_t images, double refresh_hz) {
    int n = std::snprintf(out, size,
                          "Frame generation: presents %.1f s: %u calls (%u real, %u generated, %u not shown); "
                          "gap ms p50 %.2f p95 %.2f p99 %.1f max %.1f",
                          window_s, s.calls, s.real, s.generated, s.not_shown, s.gap.p50, s.gap.p95, s.gap.p99,
                          s.gap.max);
    const auto more = [&](const char* format, auto... args) {
        if (n >= 0 && std::size_t(n) < size) {
            n += std::snprintf(out + n, size - std::size_t(n), format, args...);
        }
    };
    if (have_vblank) {
        more(", over 1.5 refreshes %u", s.over);
    }
    more("; generated to real p50 %.2f p99 %.1f; real to generated p50 %.2f p99 %.1f; acquire wait ms p50 %.2f "
         "p95 %.1f max %.1f; present mode %s, %u images, ",
         s.generated_to_real.p50, s.generated_to_real.p99, s.real_to_generated.p50, s.real_to_generated.p99,
         s.acquire_wait.p50, s.acquire_wait.p95, s.acquire_wait.max, present_mode, images);
    if (refresh_hz > 0.0) {
        more("refresh %.3f Hz", refresh_hz);
    } else {
        more("%s", "refresh unknown");
    }
    return n;
}

}
