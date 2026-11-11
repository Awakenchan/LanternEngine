// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace BbEngine::RingConstants {
enum Mode : std::uint8_t { Off, On, Compare };

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

enum Counter : std::uint8_t {
    Fast,
    Compared,
    Clamped,
    GpuModified,
    PendingWrite,
    BytesDiffer,
    ExactFence,
    ExactZpass,
    ExactDma,
    ExactWriteData,
    ExactShader,
    kCounters
};

enum Writer : std::uint8_t { WriterFence, WriterZpass, WriterDma, WriterWriteData, WriterShader, kWriters };

struct QueuedWrite {
    std::uint64_t address = 0, size = 0, packet = 0;
    Writer writer = WriterShader;
};

constexpr bool Overlaps(std::uint64_t a, std::uint64_t a_size, std::uint64_t b, std::uint64_t b_size) {
    return a_size != 0 && b_size != 0 && a < b + b_size && b < a + a_size;
}

inline constexpr const char* kCounterNames[kCounters] = {
    "copied without checks", "compared",           "clamped",          "GPU-modified",
    "pending GPU write",     "bytes differ",       "exact: fence",     "exact: occlusion dump",
    "exact: DMA",            "exact: WRITE_DATA",  "exact: shader"};

struct Totals {
    std::atomic<std::uint64_t> counts[kCounters]{};
};

inline Totals g_totals;
inline Mode g_mode = Off;

inline Mode Start() {
    g_mode = Parse(std::getenv("LANTERN_RING_CONSTANTS"));
    return g_mode;
}

inline void Note(Counter c) {
    g_totals.counts[c].store(g_totals.counts[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

struct Window {
    std::uint64_t counts[kCounters]{};
};

inline Window Take(Window& last) {
    Window now, delta;
    for (int c = 0; c < kCounters; ++c) {
        now.counts[c] = g_totals.counts[c].load(std::memory_order_relaxed);
        delta.counts[c] = now.counts[c] - last.counts[c];
    }
    last = now;
    return delta;
}
}
