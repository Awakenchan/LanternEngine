// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace BbEngine::RingConstants {
enum Mode : std::uint8_t { Off, On, Compare };

#ifdef LANTERN_RING_MUTANTS
enum class Mutant : std::uint8_t {
    None,
    OffCopiesFast,
    ThresholdExclusive,
    CompareSkipsCheck,
    GpuModifiedAgrees,
    QueueKeepsReached,
    QueueLosesCount,
    ExactCountsReached,
    FirstDifferingSkipsFirst,
    SuspectMissesEmptyRing,
    QueueCountsReachedEviction,
    kCount
};
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

inline Mode Parse(const char* value) {
    if (!value || !value[0]) return Off;
    if (value[0] == '1') return On;
    if (value[0] == '2') return Compare;
    return Off;
}

struct Ring {
    std::uint64_t base = 0, end = 0;
    constexpr bool Known() const {
        return end > base;
    }
    constexpr bool Holds(std::uint64_t address, std::uint64_t size) const {
        return size != 0 && address >= base && address < end && size <= end - address;
    }
};

inline void Copy(std::uint8_t* dst, std::uint64_t address, std::uint64_t size) {
    std::memcpy(dst, reinterpret_cast<const void*>(address), size);
}

enum class Route : std::uint8_t { Untracked, General, Counted, Fast, Checked };

inline Route Decide(Mode mode, bool counting, bool read_only, Ring ring, std::uint64_t address, std::uint64_t size,
                    std::uint64_t threshold) {
    if (!read_only || (mode == Off && !counting)) return Route::Untracked;
#ifdef LANTERN_RING_MUTANTS
    if (Mutated(Mutant::ThresholdExclusive) && size >= threshold) return Route::General;
#endif
    if (size > threshold || !ring.Holds(address, size)) return Route::General;
#ifdef LANTERN_RING_MUTANTS
    if (Mutated(Mutant::OffCopiesFast) && mode == Off) return Route::Fast;
    if (Mutated(Mutant::CompareSkipsCheck) && mode == Compare) return Route::Fast;
#endif
    if (mode == On) return Route::Fast;
    if (mode == Compare) return Route::Checked;
    return Route::Counted;
}

enum Writer : std::uint8_t { WriterFence, WriterZpass, WriterDma, WriterWriteData, WriterShader, kWriters };

struct QueuedWrite {
    std::uint64_t address = 0, size = 0, packet = 0;
    Writer writer = WriterShader;
};

constexpr bool Overlaps(std::uint64_t a, std::uint64_t a_size, std::uint64_t b, std::uint64_t b_size) {
    return a_size != 0 && b_size != 0 && a < b + b_size && b < a + a_size;
}

inline constexpr std::uint32_t kQueuedWrites = 65536;

template <std::uint32_t Capacity>
struct QueuedWrites {
    std::array<QueuedWrite, Capacity> slots{};
    std::uint64_t oldest = 0, next = 0;

    std::uint64_t Size() const {
        return next - oldest;
    }
    template <class Reached>
    bool Push(const QueuedWrite& write, Reached reached) {
        bool overwrote = false;
        bool drop = next - oldest == Capacity;
#ifdef LANTERN_RING_MUTANTS
        if (Mutated(Mutant::QueueCountsReachedEviction)) drop = false;
#endif
        if (drop) DropReached(reached);
        if (next - oldest == Capacity) {
            ++oldest;
            overwrote = true;
        }
        slots[next % Capacity] = write;
        ++next;
#ifdef LANTERN_RING_MUTANTS
        if (Mutated(Mutant::QueueLosesCount)) return false;
#endif
        return overwrote;
    }
    template <class Reached>
    void DropReached(Reached reached) {
#ifdef LANTERN_RING_MUTANTS
        if (Mutated(Mutant::QueueKeepsReached)) return;
#endif
        while (oldest != next && reached(slots[oldest % Capacity].packet)) ++oldest;
    }
    template <class Reached>
    std::uint32_t ExactWriters(std::uint64_t address, std::uint64_t size, Reached reached) const {
        std::uint32_t writers = 0;
        for (std::uint64_t i = oldest; i != next; ++i) {
            const QueuedWrite& write = slots[i % Capacity];
            bool done = reached(write.packet);
#ifdef LANTERN_RING_MUTANTS
            if (Mutated(Mutant::ExactCountsReached)) done = false;
#endif
            if (!done && Overlaps(write.address, write.size, address, size)) writers |= 1u << write.writer;
        }
        return writers;
    }
};

enum Reason : std::uint8_t {
    ReasonBytes = 1u << 0,
    ReasonClamped = 1u << 1,
    ReasonGpuModified = 1u << 2,
    ReasonQueuedWrite = 1u << 3,
};

inline std::uint8_t Judge(bool clamped, bool gpu_modified, bool queued_write, bool bytes_differ) {
#ifdef LANTERN_RING_MUTANTS
    if (Mutated(Mutant::GpuModifiedAgrees)) gpu_modified = false;
#endif
    return static_cast<std::uint8_t>((bytes_differ ? ReasonBytes : 0) | (clamped ? ReasonClamped : 0) |
                                     (gpu_modified ? ReasonGpuModified : 0) | (queued_write ? ReasonQueuedWrite : 0));
}

inline std::uint64_t FirstDiffering(const std::uint8_t* a, const std::uint8_t* b, std::uint64_t size) {
    std::uint64_t at = 0;
#ifdef LANTERN_RING_MUTANTS
    if (Mutated(Mutant::FirstDifferingSkipsFirst) && size) at = 1;
#endif
    if (std::memcmp(a + at, b + at, size - at) == 0) return size;
    while (a[at] == b[at]) ++at;
    return at;
}

enum Counter : std::uint8_t {
    Total,
    Covered,
    Fast,
    RingFull,
    Checked,
    Differ,
    DifferBytes,
    DifferClamped,
    DifferGpuModified,
    DifferQueuedWrite,
    CoarsePending,
    ExactFence,
    ExactZpass,
    ExactDma,
    ExactWriteData,
    ExactShader,
    QueueOverwritten,
    OtherWriters,
    kCounters
};

static_assert(ExactShader - ExactFence + 1 == kWriters);

inline constexpr const char* kWriterNames[kWriters] = {"fence", "occlusion dump", "DMA", "WRITE_DATA", "shader"};

struct Counts {
    std::uint64_t c[kCounters]{};
};

inline constexpr std::uint32_t kPublishEvery = 256;

struct Tally {
    Counts counts;
    std::uint32_t unpublished = 0;
    std::thread::id writer{};

    void Add(Counter counter) {
        ++counts.c[counter];
    }
    void Note(Route route) {
        if (route == Route::Untracked) return;
        ++counts.c[Total];
        if (route != Route::General) ++counts.c[Covered];
    }
    void NoteCheck(std::uint8_t reasons, bool coarse_only, std::uint32_t exact_writers) {
        ++counts.c[Checked];
        if (reasons) ++counts.c[Differ];
        if (reasons & ReasonBytes) ++counts.c[DifferBytes];
        if (reasons & ReasonClamped) ++counts.c[DifferClamped];
        if (reasons & ReasonGpuModified) ++counts.c[DifferGpuModified];
        if (reasons & ReasonQueuedWrite) ++counts.c[DifferQueuedWrite];
        if (coarse_only) ++counts.c[CoarsePending];
        for (std::uint32_t w = 0; w < kWriters; ++w) {
            if (exact_writers >> w & 1) ++counts.c[ExactFence + w];
        }
    }
    bool Due() {
        return ++unpublished >= kPublishEvery;
    }
};

struct Difference {
    std::uint64_t address = 0, size = 0, clamped = 0, packet = 0, offset = 0;
    std::uint8_t reasons = 0, fast_byte = 0, general_byte = 0;
    std::uint32_t exact_writers = 0;
};

struct Totals {
    std::atomic<std::uint64_t> counts[kCounters]{};
    std::atomic<bool> counting{false};
    std::atomic<bool> difference_taken{false}, difference_ready{false}, difference_shown{false};
    Difference first;
};

inline void Publish(Tally& tally, Totals& totals, Mode mode) {
    if (mode == Compare) {
        const std::thread::id me = std::this_thread::get_id();
        if (tally.writer == std::thread::id{}) tally.writer = me;
        else if (tally.writer != me) ++tally.counts.c[OtherWriters];
    }
    for (int c = 0; c < kCounters; ++c) totals.counts[c].store(tally.counts.c[c], std::memory_order_relaxed);
    tally.unpublished = 0;
}

inline void RecordFirst(Totals& totals, const Difference& difference) {
    if (totals.difference_taken.exchange(true, std::memory_order_relaxed)) return;
    totals.first = difference;
    totals.difference_ready.store(true, std::memory_order_release);
}

inline bool TakeFirst(Totals& totals, Difference& out) {
    if (!totals.difference_ready.load(std::memory_order_acquire)) return false;
    if (totals.difference_shown.exchange(true, std::memory_order_relaxed)) return false;
    out = totals.first;
    return true;
}

inline Counts Take(Totals& totals, Counts& last) {
    Counts now, delta;
    for (int c = 0; c < kCounters; ++c) {
        now.c[c] = totals.counts[c].load(std::memory_order_relaxed);
        delta.c[c] = now.c[c] - last.c[c];
    }
    last = now;
    return delta;
}

inline const char* Suspect(Mode mode, bool counting, const Counts& w) {
    const bool counted = counting || mode != Off;
#ifdef LANTERN_RING_MUTANTS
    if (Mutated(Mutant::SuspectMissesEmptyRing)) return nullptr;
#endif
    if (counted && w.c[Total] > 0 && w.c[Covered] == 0) return "bindings counted, none in the upload ring";
    if (mode == On && w.c[Covered] > 0 && w.c[Fast] == 0 && w.c[RingFull] == 0)
        return "bindings in the upload ring, none copied without checks";
    if (mode == Compare && w.c[Covered] > 0 && w.c[Checked] == 0) return "bindings in the upload ring, none checked";
    if (mode == Compare && w.c[Checked] > 0 && w.c[Differ] == w.c[Checked]) return "every check differs";
    return nullptr;
}

struct FrameInputs {
    std::uint64_t frames = 0, draws = 0, stage_a_us = 0;
    bool ring_known = false;
};

inline const char* ModeName(Mode mode) {
    return mode == On ? "1, no checks" : mode == Compare ? "2, compare every use" : "0, off";
}

inline int Format(char* out, std::size_t bytes, Mode mode, bool counting, const Counts& w, const FrameInputs& in) {
    const double frames = in.frames ? double(in.frames) : 1.0;
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    int n = std::snprintf(out, bytes, "Ring constants (%s): stage A %.2f us/draw, %.2f ms/frame, %.0f draws/frame",
                          ModeName(mode), in.draws ? double(in.stage_a_us) / double(in.draws) : 0.0,
                          double(in.stage_a_us) / (frames * 1000.0), double(in.draws) / frames);
    const auto add = [&](const char* format, auto... values) {
        if (n >= 0 && std::size_t(n) < bytes) n += std::snprintf(out + n, bytes - std::size_t(n), format, values...);
    };
    if (!counting && mode == Off) {
        add("%s", "; bindings not counted (LANTERN_CONST_CENSUS=1 counts them)");
        return n;
    }
    add("; bindings %.0f/frame, in the upload ring %.0f/frame (%.1f%%)", double(w.c[Total]) / frames,
        double(w.c[Covered]) / frames, w.c[Total] ? 100.0 * double(w.c[Covered]) / double(w.c[Total]) : 0.0);
    if (!in.ring_known) add("%s", "; upload ring not seen");
    if (mode == On) add("; copied without checks %.0f/frame, ring full %llu", double(w.c[Fast]) / frames, u(w.c[RingFull]));
    if (mode == Compare) {
        add("; checked %llu, differ %llu (bytes %llu, clamped %llu, GPU-modified %llu, queued GPU write %llu)",
            u(w.c[Checked]), u(w.c[Differ]), u(w.c[DifferBytes]), u(w.c[DifferClamped]), u(w.c[DifferGpuModified]),
            u(w.c[DifferQueuedWrite]));
        add("; pending granule without an exact write %llu", u(w.c[CoarsePending]));
        for (int k = 0; k < kWriters; ++k) add("; exact: %s %llu", kWriterNames[k], u(w.c[ExactFence + k]));
        add("; queue overwritten %llu, other writers %llu", u(w.c[QueueOverwritten]), u(w.c[OtherWriters]));
    }
    return n;
}

inline int FormatDifference(char* out, std::size_t bytes, const Difference& d) {
    return std::snprintf(out, bytes,
                         "Ring constants: first difference in CollectRingBindings at packet %llu: address %#llx, "
                         "size %llu, clamped %llu, reasons%s%s%s%s, byte %llu fast %#x general %#x, exact writers %#x; "
                         "LANTERN_RING_CONSTANTS=0 at the next start turns the step off",
                         static_cast<unsigned long long>(d.packet), static_cast<unsigned long long>(d.address),
                         static_cast<unsigned long long>(d.size), static_cast<unsigned long long>(d.clamped),
                         d.reasons & ReasonBytes ? " bytes" : "", d.reasons & ReasonClamped ? " clamped" : "",
                         d.reasons & ReasonGpuModified ? " GPU-modified" : "",
                         d.reasons & ReasonQueuedWrite ? " queued-GPU-write" : "",
                         static_cast<unsigned long long>(d.offset), unsigned(d.fast_byte), unsigned(d.general_byte),
                         d.exact_writers);
}

inline Totals g_totals;
inline Mode g_mode = Off;

inline Mode Start(Mode mode, bool census) {
    g_mode = mode;
    g_totals.counting.store(g_mode != Off || census, std::memory_order_relaxed);
    return g_mode;
}
}
