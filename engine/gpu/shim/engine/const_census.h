// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace BbEngine::ConstCensus {
enum Class : std::uint8_t { UploadRing, GxBuffer, Other, kClasses };

inline constexpr const char* kClassNames[kClasses] = {"GX upload ring", "GX buffers", "other memory"};

struct Range {
    std::uint64_t base = 0, bytes = 0;
};

constexpr bool Inside(std::uint64_t address, Range range) {
    return range.bytes != 0 && address >= range.base && address - range.base < range.bytes;
}

template <typename Map, typename Bytes>
bool ContainingRange(const Map& ranges, std::uint64_t address, Bytes&& bytes, Range* out, int steps = 8) {
    auto at = ranges.upper_bound(address);
    for (int step = 0; step < steps && at != ranges.begin(); ++step) {
        --at;
        const Range found{at->first, bytes(at->second)};
        if (found.bytes == 0) continue;
        if (!Inside(address, found)) return false;
        *out = found;
        return true;
    }
    return false;
}

template <typename BufferAt> Class Classify(std::uint64_t address, Range ring, BufferAt&& buffer_at) {
    if (Inside(address, ring)) return UploadRing;
    if (buffer_at(address)) return GxBuffer;
    return Other;
}

struct Totals {
    std::atomic<std::uint64_t> copies[kClasses]{};
    std::atomic<std::uint64_t> bytes[kClasses]{};
};

inline Totals g_totals;
inline bool g_on = false;

inline bool Start() {
    const char* value = std::getenv("LANTERN_CONST_CENSUS");
    g_on = value && value[0] == '1';
    return g_on;
}

inline void Count(Class c, std::uint64_t size) {
    g_totals.copies[c].store(g_totals.copies[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    g_totals.bytes[c].store(g_totals.bytes[c].load(std::memory_order_relaxed) + size, std::memory_order_relaxed);
}

struct Window {
    std::uint64_t copies[kClasses]{};
    std::uint64_t bytes[kClasses]{};
};

inline Window Take(Window& last) {
    Window now, delta;
    for (int c = 0; c < kClasses; ++c) {
        now.copies[c] = g_totals.copies[c].load(std::memory_order_relaxed);
        now.bytes[c] = g_totals.bytes[c].load(std::memory_order_relaxed);
        delta.copies[c] = now.copies[c] - last.copies[c];
        delta.bytes[c] = now.bytes[c] - last.bytes[c];
    }
    last = now;
    return delta;
}
}
