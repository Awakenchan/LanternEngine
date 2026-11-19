// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>

#ifdef BB_DSS_VARIANTS
namespace BbEngine::DynamicStateSkip {
inline int g_variant = 0;
}
#define BB_DSS_PICK(n, wrong, right) (::BbEngine::DynamicStateSkip::g_variant == (n) ? (wrong) : (right))
#else
#define BB_DSS_PICK(n, wrong, right) (right)
#endif

namespace BbEngine::DynamicStateSkip {

enum Mode : std::uint8_t { Off, On, Compare };

inline constexpr std::uint32_t kSampleInterval = 256;
inline constexpr std::uint32_t kMaxRegWords = 256;
inline constexpr std::uint32_t kMaxExtraWords = 32;
inline constexpr int kVariants = 20;

struct WordRange {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

constexpr std::uint32_t TotalWords(std::span<const WordRange> ranges) {
    std::uint32_t total = 0;
    for (const WordRange& range : ranges) {
        total += range.count;
    }
    return total;
}

enum class Action : std::uint8_t { Compute, Skip, ComputeAndCompare };

enum class Reason : std::uint8_t { Unchanged, FirstUse, OtherObject, Invalidated, RegsChanged, ExtrasChanged };

struct Snapshot {
    const void* owner = nullptr;
    bool known = false;
    std::uint32_t skips_since_check = 0;
    std::uint32_t regs[kMaxRegWords]{};
    std::uint32_t extras[kMaxExtraWords]{};
};

struct Facts {
    Mode mode = Off;
    Reason reason = Reason::FirstUse;
    std::uint32_t skips_since_check = 0;
};

inline bool SameAndKeep(const std::uint32_t* words, std::span<const WordRange> ranges, std::uint32_t* kept) {
    bool same = true;
    std::uint32_t at = 0;
    for (const WordRange& range : ranges) {
        const std::uint32_t count = BB_DSS_PICK(1, 1u, range.count);
        std::uint32_t diff = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            diff |= words[range.first + i] ^ kept[at + i];
        }
        if (diff != 0) {
            same = false;
            if (BB_DSS_PICK(2, false, true)) {
                std::memcpy(kept + at, words + range.first, range.count * sizeof(std::uint32_t));
            }
            if (BB_DSS_PICK(3, true, false)) {
                return false;
            }
        }
        at += range.count;
    }
    return same;
}

inline Facts Observe(Snapshot& snapshot, Mode mode, const void* owner, bool invalidated,
                     const std::uint32_t* reg_words, std::span<const WordRange> ranges,
                     std::span<const std::uint32_t> extras) {
    const WordRange all_extras{0, std::uint32_t(extras.size())};
    const std::span<const WordRange> extra_ranges{&all_extras, 1};
    const bool regs_same = SameAndKeep(reg_words, ranges, snapshot.regs);
    const bool extras_same = BB_DSS_PICK(4, true, SameAndKeep(extras.data(), extra_ranges, snapshot.extras));
    Facts facts{mode, Reason::Unchanged, snapshot.skips_since_check};
    if (!BB_DSS_PICK(5, true, snapshot.known)) {
        facts.reason = Reason::FirstUse;
    } else if (snapshot.owner != BB_DSS_PICK(6, snapshot.owner, owner)) {
        facts.reason = Reason::OtherObject;
    } else if (BB_DSS_PICK(7, false, invalidated)) {
        facts.reason = Reason::Invalidated;
    } else if (!regs_same) {
        facts.reason = Reason::RegsChanged;
    } else if (!extras_same) {
        facts.reason = Reason::ExtrasChanged;
    }
    return facts;
}

inline Action Decide(const Facts& facts) {
    if (BB_DSS_PICK(8, false, facts.mode == Off) || facts.reason != Reason::Unchanged) {
        return Action::Compute;
    }
    if (facts.mode == Compare) {
        return Action::ComputeAndCompare;
    }
    if (facts.skips_since_check + 1 >= BB_DSS_PICK(9, kSampleInterval + 1, kSampleInterval)) {
        return Action::ComputeAndCompare;
    }
    return Action::Skip;
}

inline void Settle(Snapshot& snapshot, Action action, const void* owner) {
    snapshot.known = true;
    snapshot.owner = owner;
    if (action == Action::Skip) {
        ++snapshot.skips_since_check;
    } else if (action == Action::ComputeAndCompare) {
        snapshot.skips_since_check = BB_DSS_PICK(10, snapshot.skips_since_check, 0u);
    } else {
        snapshot.skips_since_check = BB_DSS_PICK(11, 0u, snapshot.skips_since_check);
    }
}

inline void Forget(Snapshot& snapshot) {
    snapshot.known = false;
    snapshot.owner = nullptr;
    snapshot.skips_since_check = 0;
}

inline constexpr std::uint64_t kReportFrames = 300;
inline constexpr std::uint32_t kPublishEvery = 1024;

enum Tally : std::uint32_t {
    Draws,
    Unchanged,
    FirstUse,
    OtherObject,
    Invalidated,
    RegsChanged,
    ExtrasChanged,
    Skipped,
    Compared,
    Differ,
    kTallies
};
static_assert(Unchanged + std::uint32_t(Reason::ExtrasChanged) == ExtrasChanged);

struct Totals {
    std::array<std::atomic<std::uint64_t>, kTallies> published{};
    std::array<std::atomic<std::uint64_t>, kTallies> shown{};
    std::atomic<std::uint64_t> next_report{kReportFrames};
    std::atomic<std::uint64_t> last_report{0};
    std::atomic<bool> difference_reported{false};
};

struct ThreadTally;
inline void Publish(ThreadTally& tally);

struct ThreadTally {
    Totals* totals = nullptr;
    std::array<std::uint64_t, kTallies> counts{};
    std::uint32_t pending = 0;
    std::uint64_t window = 0;

    explicit ThreadTally(Totals* target) : totals(target) {}
    ThreadTally(const ThreadTally&) = delete;
    ThreadTally& operator=(const ThreadTally&) = delete;
    ~ThreadTally() {
        Publish(*this);
    }
};

inline void Publish(ThreadTally& tally) {
    for (std::uint32_t i = 0; i < kTallies; ++i) {
        if (tally.counts[i] != 0) {
            if (BB_DSS_PICK(12, true, false)) {
                tally.totals->published[i].store(tally.counts[i], std::memory_order_relaxed);
            } else {
                tally.totals->published[i].fetch_add(tally.counts[i], std::memory_order_relaxed);
            }
            tally.counts[i] = 0;
        }
    }
    tally.pending = 0;
}

inline bool ShouldPublish(std::uint32_t pending, std::uint64_t window, std::uint64_t last_window) {
    return pending >= kPublishEvery || BB_DSS_PICK(13, false, window != last_window);
}

inline void Count(ThreadTally& tally, Reason reason, Action action, std::uint64_t frame) {
    ++tally.counts[Draws];
    ++tally.counts[Unchanged + std::uint32_t(reason)];
    tally.counts[Skipped] += action == Action::Skip;
    tally.counts[Compared] += action == Action::ComputeAndCompare;
    const std::uint64_t window = frame / kReportFrames;
    if (ShouldPublish(++tally.pending, window, tally.window)) {
        tally.window = window;
        Publish(tally);
    }
}

struct Claim {
    bool due = false;
    std::uint64_t frames = 0;
};

inline Claim ClaimReport(Totals& totals, std::uint64_t frame) {
    std::uint64_t due = totals.next_report.load(std::memory_order_relaxed);
    if (frame < due) {
        return {};
    }
    const std::uint64_t next = (frame / kReportFrames + BB_DSS_PICK(14, 0u, 1u)) * kReportFrames;
    if (!totals.next_report.compare_exchange_strong(due, next, std::memory_order_relaxed)) {
        return {};
    }
    const std::uint64_t last = totals.last_report.exchange(frame, std::memory_order_relaxed);
    return {true, BB_DSS_PICK(15, kReportFrames, frame - last)};
}

inline std::uint64_t TakeDelta(std::atomic<std::uint64_t>& shown, std::uint64_t total) {
    if (BB_DSS_PICK(16, true, false)) {
        return total - shown.exchange(total, std::memory_order_relaxed);
    }
    std::uint64_t seen = shown.load(std::memory_order_relaxed);
    while (total > seen) {
        if (shown.compare_exchange_weak(seen, total, std::memory_order_relaxed)) {
            return total - seen;
        }
    }
    return 0;
}

struct WindowCounts {
    std::uint64_t draws = 0;
    std::uint64_t covered = 0;
    std::uint64_t compared = 0;
    std::uint64_t differ = 0;
};

inline bool Suspect(Mode mode, const WindowCounts& counts) {
    if (counts.draws > 0 && counts.covered == 0) {
        return true;
    }
    if (mode == On && counts.covered >= BB_DSS_PICK(17, 1u, kSampleInterval) && counts.compared == 0) {
        return true;
    }
    if (mode == Compare && counts.covered >= BB_DSS_PICK(18, kSampleInterval, 1u) && counts.compared == 0) {
        return true;
    }
    return counts.differ > 0 && counts.differ == counts.compared;
}

struct DifferenceOutcome {
    bool turn_off = false;
    bool print = false;
};

inline DifferenceOutcome OnDifference(Mode mode, Totals& totals) {
    const bool first = !totals.difference_reported.exchange(true, std::memory_order_relaxed);
    return {BB_DSS_PICK(19, true, mode == On), BB_DSS_PICK(20, true, first)};
}

}
