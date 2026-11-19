// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "gpu/shim/engine/pipe_census.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

using namespace BbEngine::PipeCensus;

void Bench() {
    constexpr int kPackets = 4'000'000;
    Window last;
    Take(last);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kPackets; ++i) NoteStageB(u64(i & 63), 1000);
    const double stage_b = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kPackets;
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < kPackets; ++i) NoteGpuPosition(7, u32(i));
    const double position = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kPackets;
    constexpr int kWaits = 200'000;
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < kWaits; ++i) NoteGuestWait("GXRender", WaitPreviousFrame, 10);
    const double waits = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kWaits;
    std::printf("pipe census bench: %.1f ns per stage B packet, %.1f ns per prepared-draw position, %.1f ns per guest frame wait\n",
                stage_b, position, waits);
}
}

int main(int argc, char** argv) {
    Expect(DepthBucket(0) == 0 && DepthBucket(1) == 1 && DepthBucket(2) == 1 && DepthBucket(3) == 2 && DepthBucket(8) == 2 &&
               DepthBucket(9) == 3 && DepthBucket(32) == 3 && DepthBucket(33) == 4 && DepthBucket(~u64{0}) == 4,
           "depth buckets 0, 1-2, 3-8, 9-32, >32");

    Window last;
    Take(last);
    for (const u64 depth : {0, 0, 1, 2, 5, 9, 40, 100}) NoteStageB(depth, 3'000'000);
    NoteStageAWait(1'000'000);
    NoteOcclusion(true);
    NoteStageAWait(2'000'000);
    NoteOcclusion(true);
    NoteOcclusion(false);
    NoteStageAWait(500'000);
    NoteOcclusion(false);
    NoteOcclusion(false);
    NoteStageAWait(4'000'000);
    Window w = Take(last);
    Expect(w.counts[Depth + 0] == 2 && w.counts[Depth + 1] == 2 && w.counts[Depth + 2] == 1 && w.counts[Depth + 3] == 1 &&
               w.counts[Depth + 4] == 2,
           "depth histogram");
    Expect(w.counts[StageBPackets] == 8 && w.counts[StageBCycles] == 24'000'000, "stage B packets and cycles");
    Expect(w.counts[StageAWaits] == 4 && w.counts[StageAWaitCycles] == 7'500'000, "stage A waits");
    Expect(w.counts[OpenWaits] == 2 && w.counts[OpenWaitCycles] == 2'500'000,
           "waits while a query is open; an unpaired end does not go below zero");
    Expect(w.counts[OcclusionBegins] == 2 && w.counts[OcclusionEnds] == 3, "begins and ends");

    FrameInputs in;
    in.frames = 2;
    in.seconds = 1;
    in.tsc_hz = 1e9;
    in.stage_a_cpu_us = 9000;
    in.yebis_records = 20;
    in.commit = "abc1234567";
    char line[2048];
    FormatPipe(line, sizeof line, "5 s", w, in);
    std::printf("%s\n", line);
    Expect(std::strstr(line, "Pipe census (5 s, build abc1234567, 2 frames): stage A 4.50 ms/frame busy (GPU command thread CPU "
                             "time), waiting for stage B 3.75 ms/frame in 2.0 waits/frame; stage B 12.00 ms/frame busy, 4 "
                             "packets/frame; stage B queue at packet start: 0 25.0% 1-2 25.0% 3-8 12.5% 9-32 12.5% >32 25.0%") != nullptr,
           "pipe line, ms per frame given directly");
    Expect(std::strstr(line, "YEBIS records 10.0/frame; occlusion begins 1.0/frame, ends 1.5/frame; stage A waited with a query "
                             "open 1.0/frame, 1.250 ms/frame") != nullptr,
           "YEBIS and occlusion");
    FormatPipe(line, 40, "x", w, in);
    Expect(std::strlen(line) == 39, "a short buffer is cut, not overrun");

    NoteGpuPosition(10, 5);
    Expect(GpuPassed(10, 4) && !GpuPassed(10, 5) && !GpuPassed(10, 9) && GpuPassed(9, 1000) && !GpuPassed(11, 0),
           "the GPU thread passed draws it handed out and earlier submissions");
    NoteGpuPosition(11, 0);
    Expect(GpuPassed(10, 9) && !GpuPassed(11, 0), "a new submission passes the whole previous one");
    NoteGpuPosition(12, 1u << 30);
    Expect(GpuPassed(12, (1u << 24) - 2) && !GpuPassed(13, 0), "an ordinal beyond the field is clamped, not carried into the seq");

    NoteUnavailable(UnavailableTessellation);
    NoteUnavailable(UnavailableFault);
    NoteUnavailable(UnavailableFault);
    NoteUnavailable(Unavailable(200));
    NoteAbandoned(30);
    NoteAbandoned(10);
    NotePreparedLate();
    NotePending();
    NotePending();
    NoteNoPrepared(NoPreparedRebase);
    NoteNoPrepared(NoPreparedPastEnd);
    NoteNoPrepared(NoPrepared(77));
    NoteChecksumMiss();
    NoteComputeWrite();
    NoteChecksumMiss();
    NoteChecksumMiss();
    NoteRebase();
    NoteChecksumMiss();
    NoteShClamp();
    w = Take(last);
    Expect(w.counts[Unavailables + UnavailableTessellation] == 1 && w.counts[Unavailables + UnavailableFault] == 2 &&
               w.counts[Unavailables + UnavailableFailed] == 1,
           "unavailable split; an unknown cause counts as failed");
    Expect(w.counts[AbandonedBuffers] == 2 && w.counts[AbandonedDraws] == 40, "abandoned buffers and their draws");
    Expect(w.counts[NoPrepareds + NoPreparedNoSubmission] == 1, "an unknown cause counts as no submission");
    Expect(w.counts[ComputeWrites] == 1 && w.counts[ChecksumMisses] == 4 && w.counts[ChecksumMissesUnseen] == 2,
           "misses after a compute write count until a rebase hands the workers the GPU thread's state");
    in.frames = 2;
    FormatPreparation(line, sizeof line, "5 s", w, in);
    std::printf("%s\n", line);
    Expect(std::strstr(line, "Draw preparation census (5 s): unavailable 2.0/frame (tessellation 0.5, no pipeline key 0.0, worker "
                             "failed 0.5, fault 1.0); pending at use 1.0/frame, prepared after the GPU thread passed them 0.5/frame; "
                             "buffers abandoned 1.00/frame (20.0 draws/frame); draws with no prepared state 1.5/frame (after a "
                             "rebase 0.5, no current submission 0.5, past the buffer's draws 0.5, switched off 0.0); compute-queue "
                             "register writes 0.5/frame; checksum misses 2.0/frame, 1.0 while a compute write was unseen by the "
                             "workers; compute SET_SH_REG copies clamped 1 so far") != nullptr,
           "preparation line");
    NoteShClamp();
    w = Take(last);
    Expect(w.counts[ShClamps] == 2, "clamps are reported as a running total");

    NoteNestedStart();
    NoteNestedStart();
    NoteNestedStart();
    NoteNestedFreed();
    NoteNestedFreed();
    NoteTrailingPad();
    w = Take(last);
    NoteNestedStart();
    NoteNestedFreed();
    w = Take(last);
    Expect(w.counts[NestedStarts] == 4 && w.counts[NestedFrees] == 3 && w.counts[TrailingPads] == 1,
           "nested buffers and trailing pads are running totals");
    FormatPreparation(line, sizeof line, "5 s", w, in);
    Expect(std::strstr(line, "; nested command buffers started 4, freed 3, not freed 1 so far; compute pad dwords at a buffer's "
                             "end 1 so far") != nullptr,
           "decoder totals on the preparation line");

    for (int i = 0; i < 3; ++i) NoteGuestWait("GXRenderThread", WaitPreviousFrame, 2'000'000);
    NoteGuestWait("GXRenderThread", WaitGpuIdle, 1'000'000);
    NoteGuestWait("MainThread", WaitPreviousFrame, 4'000'000);
    for (int t = 0; t < 10; ++t) {
        char name[16];
        std::snprintf(name, sizeof name, "Worker%d", t);
        NoteGuestWait(name, WaitPreviousFrame, 1);
    }
    NoteGuestWait(nullptr, WaitGpuIdle, 1);
    w = Take(last);
    char names[kWaitSlots][kWaitNameBytes];
    for (u32 slot = 0; slot < kWaitSlots; ++slot) WaitName(slot, names[slot], sizeof names[slot]);
    Expect(!std::strcmp(names[0], "GXRenderThread") && !std::strcmp(names[1], "MainThread") && !std::strcmp(names[7], "other threads"),
           "slots by thread name, the last one for the rest");
    Expect(w.counts[WaitCounts + 0 * kWaitSites + WaitPreviousFrame] == 3 && w.counts[WaitNs + 0 * kWaitSites + WaitPreviousFrame] == 6'000'000,
           "waits and time per thread and site");
    u64 waits = 0;
    for (u32 slot = 0; slot < kWaitSlots; ++slot)
        for (u32 site = 0; site < kWaitSites; ++site) waits += w.counts[WaitCounts + slot * kWaitSites + site];
    Expect(waits == 16, "no wait is lost when the slots run out");
    FormatWaits(line, sizeof line, "5 s", w, in, names);
    std::printf("%s\n", line);
    Expect(std::strstr(line, "Guest frame waits by thread (5 s): GXRenderThread (previous frame) 1.50/frame 3.00 ms/frame; "
                             "GXRenderThread (GPU idle) 0.50/frame 0.50 ms/frame; MainThread (previous frame) 0.50/frame 2.00 ms/frame;") !=
               nullptr,
           "waits line");
    Window empty;
    FormatWaits(line, sizeof line, "5 s", empty, in, names);
    Expect(std::strstr(line, "(5 s): none") != nullptr, "no waits");

    Take(last);
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([] {
            for (int i = 0; i < 100000; ++i) {
                NoteUnavailable(UnavailableNoKey);
                NotePreparedLate();
            }
        });
    }
    for (auto& t : workers) t.join();
    w = Take(last);
    Expect(w.counts[Unavailables + UnavailableNoKey] == 400000 && w.counts[PreparedLate] == 400000, "worker counts from four threads add up");

    Expect(BuildCommit() && BuildCommit()[0], "a build commit is always named");

    if (argc > 1 && !std::strcmp(argv[1], "--bench")) Bench();
    if (failures) {
        std::printf("pipe census: %d failures\n", failures);
        return 1;
    }
    std::printf("pipe census: all tests passed\n");
    return 0;
}
