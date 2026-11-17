// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace BbEngine::GcMarks {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

inline constexpr u64 kMiB = u64(1) << 20;
inline constexpr u64 kGiB = u64(1) << 30;
inline constexpr s64 kDefaultPressure = s64(kGiB + 512 * kMiB);
inline constexpr s64 kDefaultCritical = s64(3 * kGiB);
inline constexpr s64 kTargetThreshold = s64(8 * kGiB);
inline constexpr u64 kMinImageBudget = 512 * kMiB;
inline constexpr u64 kMinCriticalGap = 512 * kMiB;
inline constexpr u64 kExitPercent = 95;

struct Marks {
    u64 trigger = 0;
    u64 pressure = 0;
    u64 critical = 0;
    bool operator==(const Marks&) const = default;
};

struct Inputs {
    Marks start{};
    Marks current{};
    u64 start_budget = 0;
    u64 used = 0;
    u64 image_bytes = 0;
    u64 percent = 0;
    u64 forced_budget = 0;
    bool integrated = false;
    bool low_vram = false;
    bool new_second = false;
};

struct Result {
    Marks marks{};
    bool hysteresis = false;
};

#ifdef LANTERN_GC_MARKS_MUTANTS
enum class Mutant : std::uint8_t { None, NoFloor, NoImageBudgetMinimum, FloorWithoutNonImage, CriticalAtPressure, NoHysteresis,
                                    ExitAtTenPercent, FloorOnDefault, HysteresisOnDefault, CountEveryVisit, BudgetFloorDropped,
                                    StartSmallBudgetDropped, NoCeiling, kCount };
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

inline Marks StartMarks(s64 budget) {
    const s64 min_spacing_expected = budget - s64(kGiB);
    const s64 min_spacing_critical = budget - s64(512 * kMiB);
    const s64 mem_threshold = std::min<s64>(budget, kTargetThreshold);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    Marks marks;
    marks.pressure = static_cast<u64>(
        std::max<u64>(std::min(budget - min_vacancy_expected, min_spacing_expected), kDefaultPressure));
    marks.critical = static_cast<u64>(
        std::max<u64>(std::min(budget - min_vacancy_critical, min_spacing_critical), kDefaultCritical));
    bool small = budget <= kDefaultCritical;
#ifdef LANTERN_GC_MARKS_MUTANTS
    if (Mutated(Mutant::StartSmallBudgetDropped)) small = false;
#endif
    if (small) {
        marks.pressure = static_cast<u64>(budget / 100 * 75);
        marks.critical = static_cast<u64>(budget / 100 * 88);
    }
    marks.trigger = static_cast<u64>((budget - mem_threshold) / 2);
    return marks;
}

inline Marks IntegratedMarks(Marks current, u64 budget) {
    current.trigger = budget / 10 * 7;
    current.pressure = budget / 100 * 85;
    current.critical = budget / 100 * 95;
    return current;
}

inline Marks BudgetMarks(Marks current, const Marks& start, u64 budget, u64 percent, bool small_budget) {
#ifdef LANTERN_GC_MARKS_MUTANTS
    if (Mutated(Mutant::BudgetFloorDropped)) small_budget = true;
#endif
    current.pressure = small_budget ? budget / 100 * percent : std::max(start.pressure, budget / 100 * percent);
    current.critical = small_budget ? budget / 100 * (percent + 10)
                                    : std::max({start.critical, budget / 100 * (percent + 10), current.pressure});
    return current;
}

inline u64 NonImage(u64 used, u64 image_bytes) {
#ifdef LANTERN_GC_MARKS_MUTANTS
    if (Mutated(Mutant::FloorWithoutNonImage)) return 0;
#endif
    return used > image_bytes ? used - image_bytes : 0;
}

inline u64 ImageBudget(u64 budget) {
#ifdef LANTERN_GC_MARKS_MUTANTS
    if (Mutated(Mutant::NoImageBudgetMinimum)) return budget / 4;
#endif
    return std::max(kMinImageBudget, budget / 4);
}

inline u64 Below(u64 budget, u64 room) {
    return budget > room ? budget - room : 0;
}

inline Marks FloorMarks(Marks marks, u64 used, u64 image_bytes, u64 budget) {
#ifdef LANTERN_GC_MARKS_MUTANTS
    if (Mutated(Mutant::NoFloor)) return marks;
#endif
    u64 pressure_ceiling = std::max(marks.pressure, Below(budget, kGiB));
    u64 critical_ceiling = std::max(marks.critical, Below(budget, kMinCriticalGap));
#ifdef LANTERN_GC_MARKS_MUTANTS
    if (Mutated(Mutant::NoCeiling)) pressure_ceiling = critical_ceiling = ~u64(0);
#endif
    const u64 floor = std::max(marks.pressure, NonImage(used, image_bytes) + ImageBudget(budget));
    marks.pressure = std::min(floor, pressure_ceiling);
#ifdef LANTERN_GC_MARKS_MUTANTS
    if (Mutated(Mutant::CriticalAtPressure)) {
        marks.critical = std::min(std::max(marks.critical, marks.pressure), critical_ceiling);
        return marks;
    }
#endif
    marks.critical = std::min(std::max(marks.critical, marks.pressure + kMinCriticalGap), critical_ceiling);
    return marks;
}

template <typename BudgetFn>
inline Result Derive(const Inputs& in, BudgetFn&& live_budget) {
    if (in.integrated || in.forced_budget) {
        const u64 budget = in.forced_budget ? in.forced_budget : live_budget();
        return {budget != 0 ? IntegratedMarks(in.current, budget) : in.current, false};
    }
    if (in.percent != 0) {
#ifdef LANTERN_GC_MARKS_MUTANTS
        if (Mutated(Mutant::FloorOnDefault))
            return {FloorMarks(in.current, in.used, in.image_bytes, in.start_budget), false};
        if (Mutated(Mutant::HysteresisOnDefault) && !in.new_second) return {in.current, true};
#endif
        if (!in.new_second) return {in.current, false};
        const u64 budget = live_budget();
        if (budget == 0) return {in.current, false};
        const bool small_budget = in.low_vram || budget <= static_cast<u64>(kDefaultCritical);
        return {BudgetMarks(in.current, in.start, budget, in.percent, small_budget), false};
    }
    Marks base = in.start;
    base.trigger = in.current.trigger;
    return {FloorMarks(base, in.used, in.image_bytes, in.start_budget), true};
}

inline u64 ExitMark(u64 pressure) {
#ifdef LANTERN_GC_MARKS_MUTANTS
    if (Mutated(Mutant::ExitAtTenPercent)) return pressure / 100 * 90;
#endif
    return pressure / 100 * kExitPercent;
}

inline bool Pressured(bool latched, u64 used, u64 pressure) {
#ifdef LANTERN_GC_MARKS_MUTANTS
    if (Mutated(Mutant::NoHysteresis)) latched = false;
#endif
    return used >= (latched ? ExitMark(pressure) : pressure);
}

inline bool Latch(bool hysteresis, bool latched, u64 used, u64 pressure) {
    return hysteresis && Pressured(latched, used, pressure);
}

inline bool FirstInReport(u32& seen, u32 report) {
#ifdef LANTERN_GC_MARKS_MUTANTS
    if (Mutated(Mutant::CountEveryVisit)) return true;
#endif
    if (seen == report) return false;
    seen = report;
    return true;
}
}
