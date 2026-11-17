// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: two-stage draw pipeline (docs/parallel_gpu.md, "Two-stage draw pipeline").
//
// The GPU command thread (stage A) decodes PM4, keeps the register file and selects pipelines;
// for a direct draw it writes a packet (the register blocks changed since the previous packet,
// the stages' user data, the draw parameters) into this ring and goes on decoding. The draw
// recording thread (stage B) owns the caches, the barriers and the command recording while
// packets are in flight: it applies each packet to its own copy of the registers and runs the
// rest of the draw. Anything else stage A does on that state waits for stage B to run dry
// (Drain) and then runs on stage A as before.

#pragma once

#include <cstdlib>
#include "bbport_platform.h"
#include "bbport_threads.h"
#include "bbport_sections.h"
#include "lantern_meter.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <x86intrin.h>

#include "common/assert.h"
#include "common/thread.h"
#include "common/types.h"
#include "engine/pipe_census.h"

namespace Vulkan {

class DrawPipe {
public:
    /// Runs a packet on stage B.
    using Handler = void (*)(void* context, const u8* packet, u32 size);

    DrawPipe(Handler handler_, void* context_) : handler{handler_}, context{context_} {
        ring = std::make_unique<u8[]>(Capacity);
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    ~DrawPipe() {
        Drain();
        thread.request_stop();
        published.fetch_add(0, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// True on the draw recording thread.
    [[nodiscard]] static bool OnStageB() noexcept {
        return on_stage_b;
    }

    [[nodiscard]] u32 StageBThreadId() const noexcept {
        return stage_b_tid.load(std::memory_order_acquire);
    }

    /// Stage A: space for a packet of `size` bytes (8-byte aligned), valid until Commit().
    u8* Begin(u32 size) {
        size = Align(size + sizeof(Header));
        ASSERT(size <= Capacity / 4);
        u64 at = head;
        const u64 offset = at % Capacity;
        if (offset + size > Capacity) {
            // Wrap: the rest of the ring is skipped (a header with zero size marks it).
            WaitForSpace(at, Capacity - offset);
            reinterpret_cast<Header*>(ring.get() + offset)->size = 0;
            at += Capacity - offset;
            head = at;
            published.store(at, std::memory_order_release);
        }
        WaitForSpace(at, size);
        pending_size = size;
        auto* header = reinterpret_cast<Header*>(ring.get() + at % Capacity);
        header->size = size;
        return reinterpret_cast<u8*>(header + 1);
    }

    /// Stage A: hands the packet from Begin() to stage B.
    void Commit(u32 payload_size) {
        reinterpret_cast<Header*>(ring.get() + head % Capacity)->payload = payload_size;
        head += pending_size;
        published.store(head, std::memory_order_seq_cst);
        ++packets;
        committed_packets.store(packets, std::memory_order_relaxed);
        Lantern::Meter::NotePlace();
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
        // bbport: the lookahead thread (BB_DRAWREC_WORKERS) sleeps the same way (WaitForCommit).
        if (ahead_sleeping.load(std::memory_order_seq_cst)) {
            ahead_wake.fetch_add(1, std::memory_order_seq_cst);
            ahead_wake.notify_one();
        }
    }

    /// Why stage A drained (statistics): a PM4 opcode, or one of these.
    enum Reason : u32 {
        ReasonConstRam = 256,
        ReasonCommands,
        ReasonCompute,
        ReasonSubmissionEnd,
        ReasonDraw,
        ReasonRasterizer,
        NumReasons,
    };
    std::array<u64, NumReasons> drains_by_reason{};
    std::array<u64, NumReasons> cycles_by_reason{};

    /// Stage A: waits until stage B has run every committed packet.
    void Drain(u32 reason = ReasonRasterizer) {
        if (consumed.load(std::memory_order_acquire) == head) {
            return;
        }
        ++drains;
        const u32 slot = reason < NumReasons ? reason : ReasonRasterizer;
        ++drains_by_reason[slot];
        const u64 start = __rdtsc();
        WaitUntil([this] { return consumed.load(std::memory_order_acquire) == head; },
                  [this] { return packets; });
        const u64 waited = __rdtsc() - start;
        drain_cycles += waited;
        waited_cycles.store(drain_cycles, std::memory_order_relaxed);
        cycles_by_reason[slot] += waited;
    }

    /// bbport: whether stage A's waits sleep once they outlast a spin (BB_DRAIN_SLEEP=0: they
    /// spin and yield to the end, as before), and how long they spin first (BB_DRAIN_SPIN_US).
    static bool SleepEnabled() {
        static const bool enabled = [] {
            const char* env = std::getenv("BB_DRAIN_SLEEP");
            return !(env && env[0] == '0');
        }();
        return enabled;
    }

    /// Stage A: the number the next committed packet gets (packets are numbered from 1), and
    /// whether stage B has run the packet with that number.
    [[nodiscard]] u64 NextPacket() const noexcept {
        return packets + 1;
    }
    [[nodiscard]] bool ReachedPacket(u64 number) const noexcept {
        return consumed_packets.load(std::memory_order_acquire) >= number;
    }
    /// Stage A: waits until stage B has run the packet `number` (Drain: all of them).
    void WaitForPacket(u64 number, u32 reason) {
        if (ReachedPacket(number)) {
            return;
        }
        ++drains;
        const u32 slot = reason < NumReasons ? reason : ReasonRasterizer;
        ++drains_by_reason[slot];
        const u64 start = __rdtsc();
        WaitUntil([this, number] { return ReachedPacket(number); }, [number] { return number; });
        const u64 waited = __rdtsc() - start;
        drain_cycles += waited;
        waited_cycles.store(drain_cycles, std::memory_order_relaxed);
        cycles_by_reason[slot] += waited;
    }

    /// Stage A: position after the last committed packet; Reached(position) once B ran it.
    [[nodiscard]] u64 Head() const noexcept {
        return head;
    }
    [[nodiscard]] bool Reached(u64 position) const noexcept {
        return consumed.load(std::memory_order_acquire) >= position;
    }

    [[nodiscard]] bool Idle() const noexcept {
        return consumed.load(std::memory_order_acquire) == head;
    }
    /// Stage B: the position of the packet it runs (packets before it have run).
    [[nodiscard]] u64 Consumed() const noexcept {
        return consumed.load(std::memory_order_acquire);
    }
    /// Stage B, inside the handler: the payload of the packet after the one running when stage A
    /// has committed it already (it runs next), else null.
    [[nodiscard]] const u8* PeekNext() const noexcept {
        const u64 next = running_end;
        if (published.load(std::memory_order_acquire) <= next) {
            return nullptr;
        }
        const auto* header = reinterpret_cast<const Header*>(ring.get() + next % Capacity);
        return header->size != 0 ? reinterpret_cast<const u8*>(header + 1) : nullptr;
    }
    /// Stage B: bytes of packets committed and not run yet (the work queued for this thread).
    [[nodiscard]] u64 Backlog() const noexcept {
        return published.load(std::memory_order_acquire) - consumed.load(std::memory_order_relaxed);
    }

    // bbport BB_DRAWREC_WORKERS: the lookahead thread (Rasterizer::LookaheadLoop) reads committed
    // packets between Consumed() and Published(). Stage A never writes there: it waits for room
    // below `consumed + Capacity` (WaitForSpace), so a packet at or past `consumed` is intact.
    /// The end of the committed packets.
    [[nodiscard]] u64 Published() const noexcept {
        return published.load(std::memory_order_acquire);
    }
    /// Packets stage B has run (the number of the packet at Consumed(), counted from 0).
    [[nodiscard]] u64 ConsumedPackets() const noexcept {
        return consumed_packets.load(std::memory_order_acquire);
    }
    /// The packet at `position` (Consumed() <= position < Published()): its payload and size,
    /// and the position after it. A wrap marker gives no payload and the ring's next lap.
    [[nodiscard]] const u8* PacketAt(u64 position, u32& payload, u64& next) const noexcept {
        const auto* header = reinterpret_cast<const Header*>(ring.get() + position % Capacity);
        if (header->size == 0) {
            next = position + (Capacity - position % Capacity);
            payload = 0;
            return nullptr;
        }
        next = position + header->size;
        payload = header->payload;
        return reinterpret_cast<const u8*>(header + 1);
    }
    /// The lookahead thread: returns once a packet is committed past `position`, or on `stop`.
    /// A short spin, then it sleeps until Commit() wakes it (the announce-then-check order of
    /// stage B's own sleep: either this thread sees the packet or stage A sees it sleeping).
    void WaitForCommit(u64 position, std::stop_token& stop) {
        static const auto spin_time = std::chrono::microseconds(20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1;; ++spins) {
            if (published.load(std::memory_order_acquire) > position || stop.stop_requested()) {
                return;
            }
            __builtin_ia32_pause();
            if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                break;
            }
        }
        const u32 seen = ahead_wake.load(std::memory_order_seq_cst);
        ahead_sleeping.store(true, std::memory_order_seq_cst);
        if (published.load(std::memory_order_seq_cst) <= position && !stop.stop_requested()) {
            std::stop_callback on_stop{stop, [this] {
                ahead_wake.fetch_add(1, std::memory_order_seq_cst);
                ahead_wake.notify_all();
            }};
            ahead_wake.wait(seen, std::memory_order_seq_cst);
        }
        ahead_sleeping.store(false, std::memory_order_relaxed);
    }

    /// Statistics (stage A): packets, drains that had to wait, cycles waited, cycles stage B
    /// spent running packets, waits that went to sleep.
    u64 packets = 0, drains = 0, drain_cycles = 0, sleeps = 0;
    std::atomic<u64> busy_cycles{0};
    std::atomic<u64> waited_cycles{0};

private:
    struct Header {
        u32 size;    ///< bytes to the next packet (0: wrap to the ring start)
        u32 payload; ///< bytes written by the producer
    };
    static constexpr u64 Capacity = 16ull << 20;
    static constexpr u64 NoWaiter = ~u64{0};

    static constexpr u32 Align(u32 size) {
        return (size + 63) & ~63u;
    }

    void WaitForSpace(u64 at, u64 size) {
        if (at + size - consumed.load(std::memory_order_acquire) <= Capacity) {
            return;
        }
        WaitUntil(
            [this, at, size] {
                return at + size - consumed.load(std::memory_order_acquire) <= Capacity;
            },
            [this] { return consumed_packets.load(std::memory_order_relaxed) + 1; });
    }

    /// Stage A: waits until `done()`. bbport: spins (most waits end within microseconds),
    /// yields for up to BB_DRAIN_SPIN_US (500), then sleeps until stage B has run packet
    /// `target()` (it wakes this thread, Run). A long packet (an image created or uploaded with
    /// memory short, a pipeline compile) kept this thread in SwitchToThread for all of it: ~200
    /// ms of kernel time in a 330 ms stall, a core taken from the threads doing the work.
    template <typename Done, typename Target>
    void WaitUntil(Done&& done, Target&& target) {
        for (u32 spins = 0; spins < 4096; ++spins) {
            if (done()) {
                return;
            }
            __builtin_ia32_pause();
        }
        static const auto spin_time = std::chrono::microseconds([] {
            const char* env = std::getenv("BB_DRAIN_SPIN_US");
            return env && env[0] ? std::max(0, std::atoi(env)) : 500;
        }());
        const auto sleep_after = std::chrono::steady_clock::now() + spin_time;
        while (!done()) {
            if (!SleepEnabled() || std::chrono::steady_clock::now() < sleep_after) {
                std::this_thread::yield();
                continue;
            }
            // Announced before the check, which stage B's count update and announcement check
            // mirror (sequentially consistent both): either this check sees the packet run, or
            // stage B sees the target and wakes this thread.
            const u64 wait_for = target();
            const u32 seen = drained.load(std::memory_order_seq_cst);
            wake_at.store(wait_for, std::memory_order_seq_cst);
            const bool asleep =
                consumed_packets.load(std::memory_order_seq_cst) < wait_for && !done();
            if (asleep) {
                ++sleeps;
                drained.wait(seen, std::memory_order_seq_cst);
            }
            wake_at.store(NoWaiter, std::memory_order_relaxed);
            if (!asleep) {
                std::this_thread::yield(); // the packet ran, the position follows at once
            }
        }
    }

    /// Stage B: stage A sleeps until the packet just run (WaitUntil).
    void WakeStageA() {
        if (wake_at.exchange(NoWaiter, std::memory_order_seq_cst) != NoWaiter) {
            drained.fetch_add(1, std::memory_order_seq_cst);
            drained.notify_all();
        }
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("bb:DrawRec");
        BbThreads::MakeCritical();
        on_stage_b = true;
        BbSections::recording_thread = true;
        stage_b_tid.store(BbPlatform::CurrentThreadId(), std::memory_order_release);
        const bool census = BbEngine::PipeCensus::On();
        u64 at = 0;
        while (true) {
            // Packets follow each other within microseconds while a frame is decoded: spin,
            // and sleep only after a longer pause (between frames).
            u64 available = published.load(std::memory_order_acquire);
            if (available == at) {
                // With few hardware threads (Steam Deck: 8) a long spin takes time from the
                // game's own threads. BB_PIPE_SPIN_US overrides.
                static const auto spin_time = std::chrono::microseconds([] {
                    if (const char* env = std::getenv("BB_PIPE_SPIN_US")) {
                        return std::max(0, std::atoi(env));
                    }
                    return int(BbThreads::CapSpinUs(BbThreads::Available() >= 12 ? 200 : 50));
                }());
                const auto spin_until = std::chrono::steady_clock::now() + spin_time;
                for (u32 spins = 1; available == at; ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    __builtin_ia32_pause();
                    if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.store(true, std::memory_order_seq_cst);
                        if (published.load(std::memory_order_seq_cst) == at &&
                            !stop.stop_requested()) {
                            wake.wait(seen, std::memory_order_seq_cst);
                        }
                        sleeping.store(false, std::memory_order_relaxed);
                    }
                    available = published.load(std::memory_order_acquire);
                }
            }
            const auto* header = reinterpret_cast<const Header*>(ring.get() + at % Capacity);
            if (header->size == 0) {
                at += Capacity - at % Capacity;
                consumed.store(at, std::memory_order_release);
                continue;
            }
            const u64 queued = census ? committed_packets.load(std::memory_order_relaxed) -
                                            consumed_packets.load(std::memory_order_relaxed)
                                      : 0;
            Lantern::Meter::NotePlace();
            const u64 start = __rdtsc();
            running_end = at + header->size;
            handler(context, reinterpret_cast<const u8*>(header + 1), header->payload);
            const u64 spent = __rdtsc() - start;
            busy_cycles.fetch_add(spent, std::memory_order_relaxed);
            if (census) {
                BbEngine::PipeCensus::NoteStageB(queued > 0 && queued < (u64{1} << 63) ? queued - 1 : 0, spent);
            }
            at += header->size;
            // bbport: the position first, then the count stage A's sleeping waits look at (it
            // finds the position past the packet once the count says so), then whether one of
            // them waits for this packet: a load and a branch per packet.
            consumed.store(at, std::memory_order_release);
            const u64 run = consumed_packets.fetch_add(1, std::memory_order_seq_cst) + 1;
            if (run >= wake_at.load(std::memory_order_seq_cst)) [[unlikely]] {
                WakeStageA();
            }
        }
    }

    static inline thread_local bool on_stage_b = false;
    Handler handler;
    void* context;
    std::unique_ptr<u8[]> ring;
    u64 head = 0; ///< stage A's write position
    u32 pending_size = 0;
    alignas(64) std::atomic<u64> published{0};
    std::atomic<u64> committed_packets{0};
    alignas(64) std::atomic<u64> consumed{0};
    std::atomic<u64> consumed_packets{0}; ///< packets stage B has run (NextPacket numbers)
    u64 running_end = 0; ///< stage B: the end of the packet it runs (PeekNext; its cache line)
    alignas(64) std::atomic<u32> wake{0};
    std::atomic<bool> sleeping{false};
    std::atomic<u32> stage_b_tid{0};
    /// bbport: the lookahead thread's sleep (WaitForCommit), woken by Commit like stage B.
    std::atomic<bool> ahead_sleeping{false};
    std::atomic<u32> ahead_wake{0};
    /// bbport: the packet a sleeping wait of stage A needs run (NoWaiter: none), read by stage B
    /// after every packet (its own line: written only when stage A goes to sleep), and the word
    /// stage A sleeps on (WaitUntil, WakeStageA).
    alignas(64) std::atomic<u64> wake_at{NoWaiter};
    alignas(64) std::atomic<u32> drained{0};
    std::jthread thread;
};

} // namespace Vulkan
