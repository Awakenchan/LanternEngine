// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipe_census.h"
#include "../lantern_switches.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#if __has_include("bb_build_commit.h")
#include "bb_build_commit.h"
#endif
#ifndef BB_BUILD_COMMIT
#define BB_BUILD_COMMIT "unknown"
#endif

namespace BbEngine::PipeCensus {
namespace {
constexpr std::size_t kCacheLine = 64;
constexpr u32 kOrdinalBits = 24;
constexpr u64 kOrdinalMask = (u64(1) << kOrdinalBits) - 1;

struct alignas(kCacheLine) StageB {
    std::atomic<u64> depth[kDepthBuckets]{};
    std::atomic<u64> packets{0}, cycles{0};
};
struct alignas(kCacheLine) StageA {
    std::atomic<u64> waits{0}, wait_cycles{0}, open_waits{0}, open_wait_cycles{0};
    std::atomic<u64> begins{0}, ends{0}, open{0};
    std::atomic<u64> no_prepared[kNoPrepared]{};
    std::atomic<u64> pending{0}, compute_writes{0}, misses{0}, misses_unseen{0}, clamps{0};
    std::atomic<u64> nested_starts{0}, nested_frees{0}, trailing_pads{0};
    std::atomic<bool> compute_unseen{false};
};
struct alignas(kCacheLine) Workers {
    std::atomic<u64> unavailable[kUnavailable]{};
    std::atomic<u64> abandoned_buffers{0}, abandoned_draws{0}, late{0};
};
struct alignas(kCacheLine) Position {
    std::atomic<u64> gpu{0};
};
struct GuestWaits {
    std::mutex mutex;
    char names[kWaitSlots][kWaitNameBytes]{};
    u32 used = 0;
    u64 counts[kWaitSlots][kWaitSites]{}, ns[kWaitSlots][kWaitSites]{};
};

StageB g_b;
StageA g_a;
Workers g_workers;
Position g_position;
GuestWaits g_waits;

void Bump(std::atomic<u64>& counter, u64 by = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

void Add(std::atomic<u64>& counter, u64 by = 1) {
    counter.fetch_add(by, std::memory_order_relaxed);
}

u64 Load(const std::atomic<u64>& counter) {
    return counter.load(std::memory_order_relaxed);
}

u32 SlotFor(const char* thread) {
    for (u32 slot = 0; slot < g_waits.used; ++slot) {
        if (!std::strncmp(g_waits.names[slot], thread, kWaitNameBytes - 1)) return slot;
    }
    if (g_waits.used < kWaitSlots - 1) {
        const u32 slot = g_waits.used++;
        std::snprintf(g_waits.names[slot], kWaitNameBytes, "%s", thread);
        return slot;
    }
    std::snprintf(g_waits.names[kWaitSlots - 1], kWaitNameBytes, "%s", "other threads");
    g_waits.used = kWaitSlots;
    return kWaitSlots - 1;
}
}

bool On() {
    return Lantern::Switches::Get().frame_stats;
}

void NoteStageB(u64 depth, u64 cycles) {
    Bump(g_b.depth[DepthBucket(depth)]);
    Bump(g_b.packets);
    Bump(g_b.cycles, cycles);
}

void NoteStageAWait(u64 cycles) {
    Bump(g_a.waits);
    Bump(g_a.wait_cycles, cycles);
    if (Load(g_a.open)) {
        Bump(g_a.open_waits);
        Bump(g_a.open_wait_cycles, cycles);
    }
}

void NoteOcclusion(bool begin) {
    if (begin) {
        Bump(g_a.begins);
        Bump(g_a.open);
        return;
    }
    Bump(g_a.ends);
    if (const u64 open = Load(g_a.open)) g_a.open.store(open - 1, std::memory_order_relaxed);
}

void NoteUnavailable(Unavailable why) {
    Add(g_workers.unavailable[why < kUnavailable ? why : UnavailableFailed]);
}

void NoteAbandoned(u64 draws) {
    Add(g_workers.abandoned_buffers);
    Add(g_workers.abandoned_draws, draws);
}

void NoteGpuPosition(u64 seq, u32 handed) {
    g_position.gpu.store(seq << kOrdinalBits | std::min<u64>(handed, kOrdinalMask), std::memory_order_relaxed);
}

bool GpuPassed(u64 seq, u32 ordinal) {
    const u64 at = g_position.gpu.load(std::memory_order_relaxed);
    const u64 gpu_seq = at >> kOrdinalBits, handed = at & kOrdinalMask;
    return gpu_seq > seq || (gpu_seq == seq && handed > ordinal);
}

void NotePreparedLate() {
    Add(g_workers.late);
}

void NoteNoPrepared(NoPrepared why) {
    Bump(g_a.no_prepared[why < kNoPrepared ? why : NoPreparedNoSubmission]);
}

void NotePending() {
    Bump(g_a.pending);
}

void NoteComputeWrite() {
    Bump(g_a.compute_writes);
    g_a.compute_unseen.store(true, std::memory_order_relaxed);
}

void NoteRebase() {
    g_a.compute_unseen.store(false, std::memory_order_relaxed);
}

void NoteChecksumMiss() {
    Bump(g_a.misses);
    if (g_a.compute_unseen.load(std::memory_order_relaxed)) Bump(g_a.misses_unseen);
}

void NoteShClamp() {
    if (g_a.clamps.fetch_add(1, std::memory_order_relaxed) == 0) {
        std::printf("GPU: a compute SET_SH_REG ran past the compute registers; the copy is clamped to them\n");
    }
}

void NoteNestedStart() {
    Add(g_a.nested_starts);
}

void NoteNestedFreed() {
    Add(g_a.nested_frees);
}

void NoteTrailingPad() {
    Add(g_a.trailing_pads);
}

void NoteGuestWait(const char* thread, WaitSite site, u64 ns) {
    std::scoped_lock lock(g_waits.mutex);
    const u32 slot = SlotFor(thread ? thread : "?");
    const u32 at = site < kWaitSites ? site : WaitPreviousFrame;
    ++g_waits.counts[slot][at];
    g_waits.ns[slot][at] += ns;
}

Window Read() {
    Window w;
    for (u32 d = 0; d < kDepthBuckets; ++d) w.counts[Depth + d] = Load(g_b.depth[d]);
    w.counts[StageBPackets] = Load(g_b.packets);
    w.counts[StageBCycles] = Load(g_b.cycles);
    w.counts[StageAWaits] = Load(g_a.waits);
    w.counts[StageAWaitCycles] = Load(g_a.wait_cycles);
    w.counts[OpenWaits] = Load(g_a.open_waits);
    w.counts[OpenWaitCycles] = Load(g_a.open_wait_cycles);
    w.counts[OcclusionBegins] = Load(g_a.begins);
    w.counts[OcclusionEnds] = Load(g_a.ends);
    for (u32 u = 0; u < kUnavailable; ++u) w.counts[Unavailables + u] = Load(g_workers.unavailable[u]);
    w.counts[AbandonedBuffers] = Load(g_workers.abandoned_buffers);
    w.counts[AbandonedDraws] = Load(g_workers.abandoned_draws);
    w.counts[PreparedLate] = Load(g_workers.late);
    w.counts[PendingDraws] = Load(g_a.pending);
    for (u32 n = 0; n < kNoPrepared; ++n) w.counts[NoPrepareds + n] = Load(g_a.no_prepared[n]);
    w.counts[ComputeWrites] = Load(g_a.compute_writes);
    w.counts[ChecksumMisses] = Load(g_a.misses);
    w.counts[ChecksumMissesUnseen] = Load(g_a.misses_unseen);
    w.counts[ShClamps] = Load(g_a.clamps);
    w.counts[NestedStarts] = Load(g_a.nested_starts);
    w.counts[NestedFrees] = Load(g_a.nested_frees);
    w.counts[TrailingPads] = Load(g_a.trailing_pads);
    std::scoped_lock lock(g_waits.mutex);
    for (u32 slot = 0; slot < kWaitSlots; ++slot) {
        for (u32 site = 0; site < kWaitSites; ++site) {
            w.counts[WaitCounts + slot * kWaitSites + site] = g_waits.counts[slot][site];
            w.counts[WaitNs + slot * kWaitSites + site] = g_waits.ns[slot][site];
        }
    }
    return w;
}

Window Take(Window& last) {
    const Window now = Read();
    Window d;
    for (u32 c = 0; c < kCounters; ++c) d.counts[c] = now.counts[c] - last.counts[c];
    for (const u32 total : {ShClamps, NestedStarts, NestedFrees, TrailingPads}) d.counts[total] = now.counts[total];
    last = now;
    return d;
}

void WaitName(u32 slot, char* out, std::size_t room) {
    if (!room) return;
    std::scoped_lock lock(g_waits.mutex);
    std::snprintf(out, room, "%s", slot < g_waits.used ? g_waits.names[slot] : "");
}

const char* BuildCommit() {
    return BB_BUILD_COMMIT;
}

namespace {
struct Writer {
    char* out;
    std::size_t room, used = 0;
    template <typename... A> void Put(const char* format, A... args) {
        if (used >= room) return;
        const int n = std::snprintf(out + used, room - used, format, args...);
        if (n > 0) used += static_cast<std::size_t>(n);
    }
    int Length() const {
        return static_cast<int>(used < room ? used : room - 1);
    }
};

double PerFrame(u64 count, const FrameInputs& in) {
    return in.frames ? double(count) / double(in.frames) : 0.0;
}

double MsPerFrame(u64 cycles, const FrameInputs& in) {
    return in.frames && in.tsc_hz > 0 ? 1e3 * double(cycles) / in.tsc_hz / double(in.frames) : 0.0;
}

double Pct(u64 part, u64 all) {
    return all ? 100.0 * double(part) / double(all) : 0.0;
}
}

int FormatPipe(char* out, std::size_t room, const char* when, const Window& w, const FrameInputs& in) {
    Writer o{out, room};
    o.Put("Pipe census (%s, build %s, %llu frames): stage A %.2f ms/frame busy (GPU command thread CPU time), "
          "waiting for stage B %.2f ms/frame in %.1f waits/frame; stage B %.2f ms/frame busy, %.0f packets/frame; "
          "stage B queue at packet start:",
          when, in.commit, static_cast<unsigned long long>(in.frames), in.frames ? in.stage_a_cpu_us / 1e3 / double(in.frames) : 0.0,
          MsPerFrame(w.counts[StageAWaitCycles], in), PerFrame(w.counts[StageAWaits], in), MsPerFrame(w.counts[StageBCycles], in),
          PerFrame(w.counts[StageBPackets], in));
    for (u32 d = 0; d < kDepthBuckets; ++d) o.Put(" %s %.1f%%", kDepthNames[d], Pct(w.counts[Depth + d], w.counts[StageBPackets]));
    o.Put("; YEBIS records %.1f/frame; occlusion begins %.1f/frame, ends %.1f/frame; stage A waited with a query open %.1f/frame, %.3f ms/frame",
          PerFrame(in.yebis_records, in), PerFrame(w.counts[OcclusionBegins], in), PerFrame(w.counts[OcclusionEnds], in),
          PerFrame(w.counts[OpenWaits], in), MsPerFrame(w.counts[OpenWaitCycles], in));
    return o.Length();
}

int FormatPreparation(char* out, std::size_t room, const char* when, const Window& w, const FrameInputs& in) {
    Writer o{out, room};
    u64 unavailable = 0, missing = 0;
    for (u32 u = 0; u < kUnavailable; ++u) unavailable += w.counts[Unavailables + u];
    for (u32 n = 0; n < kNoPrepared; ++n) missing += w.counts[NoPrepareds + n];
    o.Put("Draw preparation census (%s): unavailable %.1f/frame (", when, PerFrame(unavailable, in));
    for (u32 u = 0; u < kUnavailable; ++u) o.Put("%s%s %.1f", u ? ", " : "", kUnavailableNames[u], PerFrame(w.counts[Unavailables + u], in));
    o.Put("); pending at use %.1f/frame, prepared after the GPU thread passed them %.1f/frame; buffers abandoned %.2f/frame "
          "(%.1f draws/frame); draws with no prepared state %.1f/frame (",
          PerFrame(w.counts[PendingDraws], in), PerFrame(w.counts[PreparedLate], in), PerFrame(w.counts[AbandonedBuffers], in),
          PerFrame(w.counts[AbandonedDraws], in), PerFrame(missing, in));
    for (u32 n = 0; n < kNoPrepared; ++n) o.Put("%s%s %.1f", n ? ", " : "", kNoPreparedNames[n], PerFrame(w.counts[NoPrepareds + n], in));
    o.Put("); compute-queue register writes %.1f/frame; checksum misses %.1f/frame, %.1f while a compute write was unseen by the "
          "workers; compute SET_SH_REG copies clamped %llu so far",
          PerFrame(w.counts[ComputeWrites], in), PerFrame(w.counts[ChecksumMisses], in), PerFrame(w.counts[ChecksumMissesUnseen], in),
          static_cast<unsigned long long>(w.counts[ShClamps]));
    o.Put("; nested command buffers started %llu, freed %llu, not freed %llu so far; compute pad dwords at a buffer's end %llu so far",
          static_cast<unsigned long long>(w.counts[NestedStarts]), static_cast<unsigned long long>(w.counts[NestedFrees]),
          static_cast<unsigned long long>(w.counts[NestedStarts] - w.counts[NestedFrees]),
          static_cast<unsigned long long>(w.counts[TrailingPads]));
    return o.Length();
}

int FormatWaits(char* out, std::size_t room, const char* when, const Window& w, const FrameInputs& in,
                const char (*names)[kWaitNameBytes]) {
    Writer o{out, room};
    o.Put("Guest frame waits by thread (%s):", when);
    bool any = false;
    for (u32 slot = 0; slot < kWaitSlots; ++slot) {
        for (u32 site = 0; site < kWaitSites; ++site) {
            const u64 count = w.counts[WaitCounts + slot * kWaitSites + site];
            if (!count) continue;
            any = true;
            o.Put(" %s (%s) %.2f/frame %.2f ms/frame;", names[slot][0] ? names[slot] : "?", kWaitSiteNames[site], PerFrame(count, in),
                  in.frames ? double(w.counts[WaitNs + slot * kWaitSites + site]) / 1e6 / double(in.frames) : 0.0);
        }
    }
    if (!any) o.Put("%s", " none");
    return o.Length();
}
}
