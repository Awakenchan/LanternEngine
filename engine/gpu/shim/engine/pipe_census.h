// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::PipeCensus {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

bool On();

inline constexpr u32 kDepthBuckets = 5;
inline constexpr u64 kDepthBucketTops[kDepthBuckets - 1] = {0, 2, 8, 32};
inline constexpr const char* kDepthNames[kDepthBuckets] = {"0", "1-2", "3-8", "9-32", ">32"};
constexpr u32 DepthBucket(u64 depth) {
    u32 bucket = 0;
    while (bucket < kDepthBuckets - 1 && depth > kDepthBucketTops[bucket]) ++bucket;
    return bucket;
}

enum Unavailable : std::uint8_t { UnavailableTessellation, UnavailableNoKey, UnavailableFailed, UnavailableFault, kUnavailable };
inline constexpr const char* kUnavailableNames[kUnavailable] = {"tessellation", "no pipeline key", "worker failed", "fault"};

enum NoPrepared : std::uint8_t { NoPreparedRebase, NoPreparedNoSubmission, NoPreparedPastEnd, NoPreparedSwitchedOff, kNoPrepared };
inline constexpr const char* kNoPreparedNames[kNoPrepared] = {"after a rebase", "no current submission", "past the buffer's draws",
                                                              "switched off"};

inline constexpr u32 kWaitSlots = 8;
inline constexpr std::size_t kWaitNameBytes = 16;
enum WaitSite : std::uint8_t { WaitPreviousFrame, WaitGpuIdle, kWaitSites };
inline constexpr const char* kWaitSiteNames[kWaitSites] = {"previous frame", "GPU idle"};

inline constexpr u32 Depth = 0;
inline constexpr u32 StageBPackets = Depth + kDepthBuckets;
inline constexpr u32 StageBCycles = StageBPackets + 1;
inline constexpr u32 StageAWaits = StageBCycles + 1;
inline constexpr u32 StageAWaitCycles = StageAWaits + 1;
inline constexpr u32 OpenWaits = StageAWaitCycles + 1;
inline constexpr u32 OpenWaitCycles = OpenWaits + 1;
inline constexpr u32 OcclusionBegins = OpenWaitCycles + 1;
inline constexpr u32 OcclusionEnds = OcclusionBegins + 1;
inline constexpr u32 Unavailables = OcclusionEnds + 1;
inline constexpr u32 AbandonedBuffers = Unavailables + kUnavailable;
inline constexpr u32 AbandonedDraws = AbandonedBuffers + 1;
inline constexpr u32 PreparedLate = AbandonedDraws + 1;
inline constexpr u32 PendingDraws = PreparedLate + 1;
inline constexpr u32 NoPrepareds = PendingDraws + 1;
inline constexpr u32 ComputeWrites = NoPrepareds + kNoPrepared;
inline constexpr u32 ChecksumMisses = ComputeWrites + 1;
inline constexpr u32 ChecksumMissesUnseen = ChecksumMisses + 1;
inline constexpr u32 ShClamps = ChecksumMissesUnseen + 1;
inline constexpr u32 NestedStarts = ShClamps + 1;
inline constexpr u32 NestedFrees = NestedStarts + 1;
inline constexpr u32 TrailingPads = NestedFrees + 1;
inline constexpr u32 WaitCounts = TrailingPads + 1;
inline constexpr u32 WaitNs = WaitCounts + kWaitSlots * kWaitSites;
inline constexpr u32 kCounters = WaitNs + kWaitSlots * kWaitSites;

struct Window {
    u64 counts[kCounters]{};
};

void NoteStageB(u64 depth, u64 cycles);
void NoteStageAWait(u64 cycles);
void NoteOcclusion(bool begin);
void NoteUnavailable(Unavailable why);
void NoteAbandoned(u64 draws);
void NoteGpuPosition(u64 seq, u32 handed);
bool GpuPassed(u64 seq, u32 ordinal);
void NotePreparedLate();
void NoteNoPrepared(NoPrepared why);
void NotePending();
void NoteComputeWrite();
void NoteRebase();
void NoteChecksumMiss();
void NoteShClamp();
void NoteNestedStart();
void NoteNestedFreed();
void NoteTrailingPad();
void NoteGuestWait(const char* thread, WaitSite site, u64 ns);
Window Read();
Window Take(Window& last);
void WaitName(u32 slot, char* out, std::size_t room);
const char* BuildCommit();

struct FrameInputs {
    u64 frames = 0;
    double seconds = 0, tsc_hz = 0;
    double stage_a_cpu_us = 0;
    u64 yebis_records = 0;
    const char* commit = "";
};
int FormatPipe(char* out, std::size_t room, const char* when, const Window& w, const FrameInputs& in);
int FormatPreparation(char* out, std::size_t room, const char* when, const Window& w, const FrameInputs& in);
int FormatWaits(char* out, std::size_t room, const char* when, const Window& w, const FrameInputs& in,
                const char (*names)[kWaitNameBytes]);
}
