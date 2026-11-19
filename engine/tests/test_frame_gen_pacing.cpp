// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>

#include "gpu/shim/lantern_frame_gen_cap.h"
#include "gpu/shim/lantern_frame_gen_pacing.h"

namespace {
namespace P = LanternFrameGenPacing;
namespace C = LanternFrameGenCap;

int g_failed = 0;

void Check(bool ok, [[maybe_unused]] const char* what) {
    if (!ok) {
#ifndef LANTERN_FG_PACING_MUTANTS
        std::printf("FAIL: %s\n", what);
#endif
        ++g_failed;
    }
}

bool TodayLock(bool lock_allowed, std::uint32_t limit, bool frame_gen_active) {
    return lock_allowed && limit && !frame_gen_active;
}

void TestLock() {
    for (bool allowed : {false, true}) {
        for (std::uint32_t limit : {0u, 30u, 60u, 144u}) {
            for (bool fsr3 : {false, true}) {
                Check(P::LockWanted(allowed, limit, false, fsr3) == TodayLock(allowed, limit, false),
                      "frame generation off: the lock decision is today's");
            }
            Check(P::LockWanted(allowed, limit, true, false) == TodayLock(allowed, limit, true),
                  "DLSS frame generation: the lock decision is today's");
            Check(P::LockWanted(allowed, limit, true, true) == (allowed && limit != 0),
                  "FSR 3.1 frame generation keeps the vblank lock");
        }
    }
    Check(P::LockWanted(true, 60, true, true), "FSR 3.1 at 60 FPS locks");
    Check(!P::LockWanted(true, 60, true, false), "DLSS at 60 FPS does not lock");
}

void TestUnlockedReason() {
    for (bool fsr3 : {false, true}) {
        Check(std::strcmp(P::UnlockedReason(false, fsr3, false), "no steady vblanks to lock to") == 0,
              "frame generation off without vblanks: today's log text");
        Check(std::strcmp(P::UnlockedReason(false, fsr3, true),
                          "the frame rate is not a whole share of the display's refresh") == 0,
              "frame generation off with steady vblanks says the rate is not a whole share");
    }
    Check(std::strcmp(P::UnlockedReason(true, false, true), "no steady vblanks to lock to") == 0, "DLSS keeps today's reason");
    Check(std::strstr(P::UnlockedReason(true, true, false), "own timer") != nullptr,
          "FSR 3.1 without vblanks says it uses its timer");
    Check(std::strstr(P::UnlockedReason(true, true, true), "whole share") != nullptr,
          "FSR 3.1 with vblanks and an odd rate says so");
}

void TestRealSlot() {
    const std::int64_t vp = 8333333, frame = 16666666, last = 1000000;
    const std::int64_t offset = std::min<std::int64_t>(frame / 16 + 300000, vp / 2);
    Check(P::SlotOffsetNs(frame, vp) == offset, "offset follows the flip loop's");
    Check(P::RealSlotNs(last + vp + 200000, last, vp, frame) == last + 2 * vp + offset,
          "generated just after vblank k: the real one just after k+1");
    Check(P::RealSlotNs(last + vp + 8000000, last, vp, frame) == last + 2 * vp + offset,
          "a generated present late in its refresh still targets k+1");
    Check(P::RealSlotNs(last - 1000000, last, vp, frame) == last + offset,
          "a generated present before the last known vblank");
    const std::int64_t frame30 = 33333333;
    Check(P::RealSlotNs(last + 100000, last, vp, frame30) == last + 2 * vp + P::SlotOffsetNs(frame30, vp),
          "4 refreshes a frame: the real one 2 refreshes after the generated one");
    Check(P::RealSlotNs(last + 100000, last, 0, frame) == 0, "no vblank: the timer");
    Check(P::RealSlotNs(last + 100000, last, vp, 0) == 0, "no frame period: the timer");
    Check(P::RealSlotNs(last + 100000, last, vp, vp) == 0, "one refresh a frame: the timer");
    for (std::int64_t done = last - 3 * vp; done < last + 5 * vp; done += 777777) {
        for (const std::int64_t f : {frame, frame30, 3 * vp}) {
            const std::int64_t slot = P::RealSlotNs(done, last, vp, f);
            Check(slot > done, "the real slot is after the generated present");
            Check(slot - done <= f, "the real slot is within one game frame");
            Check(((slot - P::SlotOffsetNs(f, vp) - last) % vp) == 0, "the real slot sits on a vblank");
        }
    }
}

void TestCap() {
    for (std::uint32_t base : {0u, 30u, 60u, 90u, 120u, 144u, 240u}) {
        for (std::uint32_t refresh : {0u, 60u, 120u, 144u, 240u}) {
            for (int delivered = 0; delivered <= 4; ++delivered) {
                for (bool setting : {false, true}) {
                    Check(C::Limit(base, false, delivered, refresh, P::CapToRefresh(setting, true)) == base,
                          "frame generation off: Uncapped keeps its limit");
                    Check(C::FixedRateLimit(base, false, delivered, refresh) == base,
                          "frame generation off: a fixed rate keeps its limit");
                    Check(P::CapToRefresh(setting, false) == setting, "DLSS keeps the even pacing setting");
                }
            }
        }
    }
    Check(C::Limit(0, true, 2, 120, P::CapToRefresh(false, true)) == 60, "FSR 3.1 Uncapped on 120 Hz: 60");
    Check(C::Limit(0, true, 2, 144, P::CapToRefresh(false, true)) == 72, "FSR 3.1 Uncapped on 144 Hz: 72");
    Check(C::Limit(0, true, 2, 60, P::CapToRefresh(false, true)) == 30, "FSR 3.1 Uncapped on 60 Hz: 30");
    Check(C::Limit(240, true, 2, 120, P::CapToRefresh(false, true)) == 60, "FSR 3.1 with a 240 limit on 120 Hz: 60");
    Check(C::Limit(0, true, 2, 120, P::CapToRefresh(false, false)) == 0, "DLSS Uncapped without even pacing: none");
    Check(C::Limit(0, true, 2, 120, P::CapToRefresh(true, false)) == 60, "DLSS Uncapped with even pacing: 60");
}

void TestNotes() {
    Check(P::Notes(0, 30, 2, 60, 30.0f) == P::NoteLowReal, "60 Hz: the game at 30, every frame shown, low");
    Check(P::Notes(90, 60, 2, 120, 60.0f) == P::NoteHeld, "90 preset on 120 Hz: held at 60");
    Check(P::Notes(60, 60, 2, 120, 60.0f) == 0, "60 on 120 Hz: nothing to say");
    Check(P::Notes(60, 60, 2, 144, 60.0f) == P::NoteUneven, "60 on 144 Hz: uneven");
    Check(P::Notes(60, 60, 2, 30, 60.0f) == P::NoteUnshowable, "a 30 Hz display cannot show them all");
    Check(P::Notes(0, 0, 2, 120, 60.0f) == P::NoteUnshowable, "no limit at all: some never shown");
    Check(P::Notes(60, 60, 2, 120, 35.0f) == P::NoteLowReal, "a measured 35 FPS game is low");
    Check(P::Notes(60, 60, 2, 0, 60.0f) == 0, "unknown refresh: nothing about the display");
    Check(P::Notes(60, 60, 1, 120, 60.0f) == 0, "nothing generated: nothing about the display");
    Check(P::Notes(90, 72, 2, 144, 72.0f) == P::NoteHeld, "90 preset on 144 Hz: held at 72, even");
}

void TestShownCounter() {
    Check(P::CapInputShown(true, false), "the cap's input counts present calls, as today");
    Check(!P::CapInputShown(false, false), "no call: not counted for the cap");
    float pct = -1.0f;
    pct = P::ShownPercent(pct, true);
    Check(pct == 100.0f, "first generated frame reached the swapchain: 100%");
    pct = P::ShownPercent(pct, false);
    Check(pct > 94.9f && pct < 95.1f, "one bail-out moves the count down");
    float all = -1.0f, some = -1.0f;
    for (int i = 0; i < 200; ++i) {
        all = P::ShownPercent(all, true);
        some = P::ShownPercent(some, i % 2 == 0);
    }
    Check(all == 100.0f, "every generated frame shown: 100%");
    Check(some > 40.0f && some < 60.0f, "half of them shown: about 50%");
    float cap_shown_per_real = 1.0f;
    for (int i = 0; i < 200; ++i) {
        cap_shown_per_real = cap_shown_per_real * 0.95f + (P::CapInputShown(true, i % 2 == 0) ? 2.0f : 1.0f) * 0.05f;
    }
    Check(cap_shown_per_real > 1.99f, "bail-outs do not change the cap's multiplier");
    Check(C::DeliveredMultiplier(2, false, 60.0f, 60.0f * cap_shown_per_real, 2) == 2, "the cap keeps 2x");
}

void TestLog() {
    P::PresentLog log;
    for (std::uint64_t i = 0; i < P::kRecords + 6; ++i) {
        log.Add({i, 0, P::KindReal, 1});
    }
    static P::PresentRecord out[P::kRecords];
    Check(log.Ordered(out) == P::kRecords, "the log keeps 1024 records");
    Check(out[0].t_present1_ns == 6 && out[P::kRecords - 1].t_present1_ns == P::kRecords + 5,
          "the oldest records go first");
    log.Clear();
    Check(log.Count() == 0 && log.Ordered(out) == 0, "cleared");
    log.Add({5, 0, P::KindReal, 1});
    log.Add({9, 0, P::KindReal, 1});
    Check(log.Ordered(out) == 2 && out[0].t_present1_ns == 5 && out[1].t_present1_ns == 9, "order after a clear");
}

void TestSummary() {
    static P::PresentRecord records[P::kRecords];
    std::size_t n = 0;
    std::uint64_t t = 0;
    for (int i = 0; i < 100; ++i) {
        t += i == 50 ? 25000000 : 9000000;
        records[n++] = {t, std::uint32_t(i % 2 ? 0 : 7000), std::uint8_t(i % 2 ? P::KindReal : P::KindGenerated),
                        std::uint8_t(i == 10 ? 0 : 1)};
    }
    std::array<double, P::kRecords> scratch{};
    const P::Summary s = P::Summarize(records, n, 8333333, scratch);
    Check(s.calls == 100 && s.real == 50 && s.generated == 50 && s.not_shown == 1, "summary counts");
    Check(s.over == 1, "one gap over 1.5 refreshes");
    Check(s.gap.p50 > 8.99 && s.gap.p50 < 9.01 && s.gap.max > 24.99 && s.gap.max < 25.01, "gap spread");
    Check(s.generated_to_real.p50 > 8.99 && s.generated_to_real.p50 < 9.01, "generated to real");
    Check(s.real_to_generated.max > 24.99, "real to generated holds the long gap");
    Check(s.acquire_wait.max > 6.99 && s.acquire_wait.max < 7.01, "acquire wait");
    Check(P::Summarize(records, n, 0, scratch).over == 0, "no vblank: nothing over");
    char line[512];
    P::FormatSummary(line, sizeof(line), s, 5.0, true, "Fifo", 4, 119.993);
    Check(std::strstr(line, "Frame generation: presents 5.0 s: 100 calls (50 real, 50 generated, 1 not shown)") !=
              nullptr,
          "summary line head");
    Check(std::strstr(line, "over 1.5 refreshes 1") != nullptr, "over count printed with a vblank");
    Check(std::strstr(line, "present mode Fifo, 4 images, refresh 119.993 Hz") != nullptr, "summary line tail");
    P::FormatSummary(line, sizeof(line), s, 5.0, false, "Mailbox", 4, 0.0);
    Check(std::strstr(line, "over 1.5") == nullptr && std::strstr(line, "refresh unknown") != nullptr,
          "no vblank: no over count, refresh unknown");
}

int RunSuite() {
    g_failed = 0;
    TestLock();
    TestUnlockedReason();
    TestRealSlot();
    TestCap();
    TestNotes();
    TestShownCounter();
    TestLog();
    TestSummary();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_FG_PACING_MUTANTS
    int survivors = 0;
    for (int m = 0; m <= P::kMutants; ++m) {
        P::g_mutant = m;
        const int found = RunSuite();
        if (m == 0) {
            if (found) {
                std::printf("frame gen pacing mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("frame gen pacing mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("frame gen pacing mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("frame gen pacing mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("frame-gen-pacing-test: %d failure(s)\n", g_failed);
        return 1;
    }
    std::printf("frame-gen-pacing-test: ok\n");
    return 0;
#endif
}
