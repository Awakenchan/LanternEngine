// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

#include "gpu/shim/engine/dynamic_state_skip.h"

namespace {
namespace Dss = BbEngine::DynamicStateSkip;

int failures = 0;
bool quiet = false;

void Expect(bool ok, const char* what) {
    if (!ok) {
        if (!quiet) {
            std::printf("FAIL: %s\n", what);
        }
        ++failures;
    }
}

constexpr std::array<Dss::WordRange, 3> kRanges{{{2, 3}, {10, 1}, {20, 4}}};
constexpr std::uint32_t kRegWords = 32;

Dss::Facts Facts(Dss::Mode mode, Dss::Reason reason, std::uint32_t skips) {
    Dss::Facts facts;
    facts.mode = mode;
    facts.reason = reason;
    facts.skips_since_check = skips;
    return facts;
}

void TestDecide() {
    using Dss::Action;
    using Dss::Reason;
    Expect(Dss::Decide(Facts(Dss::Off, Reason::Unchanged, 0)) == Action::Compute, "mode 0 always computes");
    Expect(Dss::Decide(Facts(Dss::On, Reason::Unchanged, 0)) == Action::Skip, "mode 1 skips unchanged inputs");
    Expect(Dss::Decide(Facts(Dss::On, Reason::Unchanged, Dss::kSampleInterval - 2)) == Action::Skip,
           "mode 1 skips until the sample point");
    Expect(Dss::Decide(Facts(Dss::On, Reason::Unchanged, Dss::kSampleInterval - 1)) ==
               Action::ComputeAndCompare,
           "mode 1 compares one in kSampleInterval unchanged draws");
    Expect(Dss::Decide(Facts(Dss::Compare, Reason::Unchanged, 0)) == Action::ComputeAndCompare,
           "mode 2 compares every unchanged draw");
    for (Reason reason : {Reason::FirstUse, Reason::OtherObject, Reason::Invalidated, Reason::RegsChanged,
                          Reason::ExtrasChanged}) {
        for (Dss::Mode mode : {Dss::Off, Dss::On, Dss::Compare}) {
            Expect(Dss::Decide(Facts(mode, reason, 0)) == Action::Compute, "a changed input computes");
        }
    }
}

void TestSameAndKeep() {
    std::uint32_t words[kRegWords]{};
    std::uint32_t kept[Dss::kMaxRegWords]{};
    for (std::uint32_t i = 0; i < kRegWords; ++i) {
        words[i] = i * 7 + 1;
    }
    Expect(!Dss::SameAndKeep(words, kRanges, kept), "first words differ from a zero copy");
    Expect(Dss::SameAndKeep(words, kRanges, kept), "the copy is kept after a difference");
    words[4] ^= 1;
    Expect(!Dss::SameAndKeep(words, kRanges, kept), "the last word of a range is compared");
    Expect(Dss::SameAndKeep(words, kRanges, kept), "the changed range is kept");
    words[23] ^= 1;
    Expect(!Dss::SameAndKeep(words, kRanges, kept), "the last range is compared");
    words[0] ^= 1;
    words[15] ^= 1;
    Expect(Dss::SameAndKeep(words, kRanges, kept), "words outside the ranges are not inputs");
    words[2] ^= 1;
    words[21] ^= 1;
    Expect(!Dss::SameAndKeep(words, kRanges, kept), "two changed ranges differ");
    Expect(Dss::SameAndKeep(words, kRanges, kept), "every changed range is kept, not only the first");
    Expect(Dss::TotalWords(kRanges) == 8, "total words of the ranges");
}

struct Model {
    Dss::Snapshot snapshot;
    std::uint32_t words[kRegWords]{};
    std::array<std::uint32_t, 4> extras{};
};

Dss::Facts Observe(Model& m, Dss::Mode mode, const void* owner, bool invalidated) {
    return Dss::Observe(m.snapshot, mode, owner, invalidated, m.words, kRanges, m.extras);
}

void TestObserve() {
    using Dss::Reason;
    Model m;
    int a = 0, b = 0;
    m.words[3] = 5;
    Expect(Observe(m, Dss::On, &a, false).reason == Reason::FirstUse, "first draw is first use");
    Dss::Settle(m.snapshot, Dss::Action::Compute, &a);
    Expect(Observe(m, Dss::On, &a, false).reason == Reason::Unchanged, "same inputs, same state");
    Expect(Observe(m, Dss::On, &b, false).reason == Reason::OtherObject, "another state object recomputes");
    Expect(Observe(m, Dss::On, &a, true).reason == Reason::Invalidated, "invalidated state recomputes");
    m.extras[3] = 9;
    Expect(Observe(m, Dss::On, &a, false).reason == Reason::ExtrasChanged, "other inputs are compared");
    Expect(Observe(m, Dss::On, &a, false).reason == Reason::Unchanged, "other inputs are kept");
    m.words[20] = 1;
    m.extras[0] = 1;
    Expect(Observe(m, Dss::On, &a, false).reason == Reason::RegsChanged, "register words are compared");
    Expect(Observe(m, Dss::On, &a, false).reason == Reason::Unchanged, "both kept after one call");
    Dss::Forget(m.snapshot);
    Expect(Observe(m, Dss::On, &a, false).reason == Reason::FirstUse, "forgotten state is first use");
}

void TestSettle() {
    Dss::Snapshot snapshot;
    int a = 0;
    Dss::Settle(snapshot, Dss::Action::Skip, &a);
    Dss::Settle(snapshot, Dss::Action::Skip, &a);
    Expect(snapshot.known && snapshot.owner == &a && snapshot.skips_since_check == 2, "skips are counted");
    Dss::Settle(snapshot, Dss::Action::ComputeAndCompare, &a);
    Expect(snapshot.skips_since_check == 0, "a compare restarts the sample count");
    Dss::Settle(snapshot, Dss::Action::Skip, &a);
    Dss::Settle(snapshot, Dss::Action::Compute, &a);
    Expect(snapshot.skips_since_check == 1, "a compute keeps the sample count");
}

void TestSamplerAcrossRecomputes() {
    Dss::Snapshot snapshot;
    int a = 0;
    std::uint32_t skips_in_a_row = 0, longest = 0, compares = 0;
    for (int draw = 0; draw < 4000; ++draw) {
        const bool changed = draw % 100 == 0;
        Dss::Facts facts;
        facts.mode = Dss::On;
        facts.reason = changed ? Dss::Reason::RegsChanged : Dss::Reason::Unchanged;
        facts.skips_since_check = snapshot.skips_since_check;
        const Dss::Action action = Dss::Decide(facts);
        if (action == Dss::Action::Skip) {
            longest = std::max(longest, ++skips_in_a_row);
        } else if (action == Dss::Action::ComputeAndCompare) {
            ++compares;
            skips_in_a_row = 0;
        }
        Dss::Settle(snapshot, action, &a);
    }
    Expect(compares >= 4000 / Dss::kSampleInterval - 1, "runs shorter than the sample interval are still sampled");
    Expect(longest < Dss::kSampleInterval, "never more than kSampleInterval - 1 skips without a compare");
}

void TestPublish() {
    Dss::Totals totals;
    {
        Dss::ThreadTally tally{&totals};
        for (std::uint32_t i = 0; i < Dss::kPublishEvery - 1; ++i) {
            Dss::Count(tally, Dss::Reason::Unchanged, Dss::Action::Skip, 10);
        }
        Expect(totals.published[Dss::Draws].load() == 0, "counts stay local before the batch");
        Dss::Count(tally, Dss::Reason::Unchanged, Dss::Action::Skip, 10);
        Expect(totals.published[Dss::Draws].load() == Dss::kPublishEvery, "a full batch is published");
        Dss::Count(tally, Dss::Reason::RegsChanged, Dss::Action::Compute, 10);
        Dss::Count(tally, Dss::Reason::RegsChanged, Dss::Action::Compute, Dss::kReportFrames);
        Expect(totals.published[Dss::Draws].load() == Dss::kPublishEvery + 2,
               "the first draw in a new window publishes");
        Expect(totals.published[Dss::RegsChanged].load() == 2 && totals.published[Dss::Skipped].load() ==
                   Dss::kPublishEvery,
               "each tally is added, not replaced");
        Dss::Count(tally, Dss::Reason::Unchanged, Dss::Action::ComputeAndCompare, Dss::kReportFrames);
    }
    Expect(totals.published[Dss::Draws].load() == Dss::kPublishEvery + 3, "a thread's exit publishes");
    Expect(totals.published[Dss::Compared].load() == 1, "compares are counted");
}

void TestClaim() {
    Dss::Totals totals;
    Expect(!Dss::ClaimReport(totals, Dss::kReportFrames - 1).due, "no report before the first window ends");
    const Dss::Claim first = Dss::ClaimReport(totals, Dss::kReportFrames);
    Expect(first.due && first.frames == Dss::kReportFrames, "the first window is claimed");
    Expect(!Dss::ClaimReport(totals, Dss::kReportFrames).due, "a window is claimed once");
    Expect(!Dss::ClaimReport(totals, 2 * Dss::kReportFrames - 1).due, "no report inside the window");
    const Dss::Claim late = Dss::ClaimReport(totals, 2 * Dss::kReportFrames + 150);
    Expect(late.due && late.frames == Dss::kReportFrames + 150, "the real number of frames covered");
    Expect(!Dss::ClaimReport(totals, 2 * Dss::kReportFrames + 151).due, "the late claim moves to the next window");
    Expect(Dss::ClaimReport(totals, 3 * Dss::kReportFrames).due, "the next window starts on the boundary");
}

void TestTakeDelta() {
    std::atomic<std::uint64_t> shown{0};
    Expect(Dss::TakeDelta(shown, 10) == 10, "a delta from zero");
    Expect(Dss::TakeDelta(shown, 5) == 0 && shown.load() == 10, "an older total never lowers what was shown");
    Expect(Dss::TakeDelta(shown, 12) == 2, "a later delta");
}

void TestSuspect() {
    using Dss::WindowCounts;
    Expect(!Dss::Suspect(Dss::Off, WindowCounts{}), "no draws, nothing suspect");
    Expect(Dss::Suspect(Dss::Off, WindowCounts{100, 0, 0, 0}), "draws without coverage are suspect");
    Expect(!Dss::Suspect(Dss::Off, WindowCounts{100, 50, 0, 0}), "census without compares is fine");
    Expect(!Dss::Suspect(Dss::On, WindowCounts{100, Dss::kSampleInterval - 1, 0, 0}),
           "mode 1 under one sample interval may have no compare");
    Expect(Dss::Suspect(Dss::On, WindowCounts{1000, Dss::kSampleInterval, 0, 0}),
           "mode 1 over one sample interval without a compare");
    Expect(Dss::Suspect(Dss::Compare, WindowCounts{100, 1, 0, 0}), "mode 2 covered without a compare");
    Expect(!Dss::Suspect(Dss::Compare, WindowCounts{100, 10, 10, 0}), "mode 2 compares every covered draw");
    Expect(Dss::Suspect(Dss::Compare, WindowCounts{100, 10, 10, 10}), "every compare differing");
    Expect(!Dss::Suspect(Dss::Compare, WindowCounts{100, 10, 10, 1}), "a single difference is not suspect");
}

void TestDifference() {
    Dss::Totals compare_totals;
    const Dss::DifferenceOutcome first = Dss::OnDifference(Dss::Compare, compare_totals);
    const Dss::DifferenceOutcome second = Dss::OnDifference(Dss::Compare, compare_totals);
    Expect(!first.turn_off && !second.turn_off, "mode 2 keeps comparing after a difference");
    Expect(first.print && !second.print, "the first difference prints once");
    Dss::Totals skip_totals;
    const Dss::DifferenceOutcome skip = Dss::OnDifference(Dss::On, skip_totals);
    Expect(skip.turn_off && skip.print, "mode 1 turns itself off and prints");
}

void TestTwoThreads() {
    constexpr std::uint32_t kDraws = 200000;
    Dss::Totals totals;
    std::atomic<std::uint64_t> frame{0};
    std::atomic<std::uint64_t> reported{0}, claims{0}, frames_claimed{0};
    std::atomic<std::uint64_t> last_claimed{0};
    const auto draw_thread = [&](bool advances) {
        Dss::ThreadTally tally{&totals};
        for (std::uint32_t i = 0; i < kDraws; ++i) {
            const std::uint64_t now = advances && i % 64 == 0 ? frame.fetch_add(1) + 1 : frame.load();
            Dss::Count(tally, Dss::Reason::Unchanged, Dss::Action::Skip, now);
            const Dss::Claim claim = Dss::ClaimReport(totals, now);
            if (claim.due) {
                Dss::Publish(tally);
                reported += Dss::TakeDelta(totals.shown[Dss::Draws], totals.published[Dss::Draws].load());
                ++claims;
                frames_claimed += claim.frames;
                std::uint64_t seen = last_claimed.load();
                while (now > seen && !last_claimed.compare_exchange_weak(seen, now)) {
                }
            }
        }
    };
    std::thread a(draw_thread, true);
    std::thread b(draw_thread, false);
    a.join();
    b.join();
    reported += Dss::TakeDelta(totals.shown[Dss::Draws], totals.published[Dss::Draws].load());
    Expect(totals.published[Dss::Draws].load() == 2ull * kDraws, "no draw is lost across two threads");
    Expect(reported.load() == 2ull * kDraws, "every published draw is reported once");
    Expect(claims.load() == frame.load() / Dss::kReportFrames, "one claim per window ended");
    Expect(frames_claimed.load() == last_claimed.load(), "claimed frames add up to the last claim");
}

std::uint64_t StateOf(const Model& m) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& range : kRanges) {
        for (std::uint32_t i = 0; i < range.count; ++i) {
            h = (h ^ m.words[range.first + i]) * 1099511628211ull;
        }
    }
    for (std::uint32_t extra : m.extras) {
        h = (h ^ extra) * 1099511628211ull;
    }
    return h;
}

void TestSequences() {
    std::mt19937 random(1234);
    for (Dss::Mode mode : {Dss::On, Dss::Compare}) {
        Model m;
        std::array<std::uint64_t, 2> states{};
        std::array<bool, 2> invalidated{true, true};
        int owners[2]{};
        std::uint32_t owner = 0, skipped = 0, compared = 0, wrong = 0;
        for (int draw = 0; draw < 20000; ++draw) {
            const std::uint32_t event = random() % 16;
            if (event == 0) {
                m.words[kRanges[random() % kRanges.size()].first] ^= 1u << (random() % 32);
            } else if (event == 1) {
                m.extras[random() % m.extras.size()] ^= 1;
            } else if (event == 2) {
                invalidated[owner] = true;
            } else if (event == 3) {
                owner ^= 1;
            } else if (event == 4) {
                m.words[1] ^= 1;
            } else if (event == 5) {
                const std::uint32_t at = kRanges[2].first + 3;
                m.words[at] ^= 1u << (random() % 32);
            }
            const Dss::Facts facts = Observe(m, mode, &owners[owner], invalidated[owner]);
            const Dss::Action action = Dss::Decide(facts);
            const std::uint64_t computed = StateOf(m);
            if (action == Dss::Action::Skip) {
                ++skipped;
            } else {
                compared += action == Dss::Action::ComputeAndCompare;
                if (action == Dss::Action::ComputeAndCompare && states[owner] != computed) {
                    ++wrong;
                }
                states[owner] = computed;
            }
            invalidated[owner] = false;
            Dss::Settle(m.snapshot, action, &owners[owner]);
            if (states[owner] != computed) {
                ++wrong;
            }
        }
        Expect(wrong == 0, "the kept state always equals the computed state");
        Expect(mode == Dss::Compare ? skipped == 0 && compared > 1000 : skipped > 1000,
               "the sequence exercises skips and compares");
    }
}

int RunAll() {
    failures = 0;
    TestDecide();
    TestSameAndKeep();
    TestObserve();
    TestSettle();
    TestSamplerAcrossRecomputes();
    TestPublish();
    TestClaim();
    TestTakeDelta();
    TestSuspect();
    TestDifference();
    TestTwoThreads();
    TestSequences();
    return failures;
}
}

int main(int argc, char** argv) {
    const bool variants = argc > 1 && std::strcmp(argv[1], "--variants") == 0;
    if (!variants) {
        const int failed = RunAll();
        std::printf("dynamic-state-skip-test: %s (%d failures)\n", failed ? "FAILED" : "passed", failed);
        return failed ? 1 : 0;
    }
#ifdef BB_DSS_VARIANTS
    quiet = true;
    int survivors = 0;
    for (int variant = 1; variant <= Dss::kVariants; ++variant) {
        Dss::g_variant = variant;
        const int failed = RunAll();
        std::printf("variant %d: %s (%d failures)\n", variant, failed ? "caught" : "SURVIVED", failed);
        survivors += failed == 0;
    }
    Dss::g_variant = 0;
    quiet = false;
    const int failed = RunAll();
    std::printf("dynamic-state-skip-variants: %d survivors, shipped lines %s\n", survivors,
                failed ? "FAILED" : "passed");
    return survivors || failed ? 1 : 0;
#else
    std::printf("dynamic-state-skip-test: --variants needs a build with BB_DSS_VARIANTS\n");
    return 1;
#endif
}
