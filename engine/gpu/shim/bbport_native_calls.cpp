// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbport_native_calls.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace BbNativeCalls {
namespace {

Mode ReadMode() {
    const char* env = std::getenv("BB_NATIVE_CALLS");
    if (!env || !env[0] || std::strcmp(env, "0") == 0) {
        return Mode::Off;
    }
    if (std::strcmp(env, "verify") == 0) {
        return Mode::Verify;
    }
    if (std::strcmp(env, "1") == 0) {
        return Mode::Native;
    }
    std::printf("Native calls: BB_NATIVE_CALLS=%s not understood (verify or 1), off\n", env);
    return Mode::Off;
}

const char* ModeName(Mode mode) {
    return mode == Mode::Verify ? "verify" : mode == Mode::Native ? "1" : "off";
}

struct Slot {
    std::atomic<u32> lock{0};
    u64 address = 0;
    DrawRecord record{};
};
constexpr u32 SlotBits = 16;
std::unique_ptr<Slot[]> slots;
std::atomic<bool> slots_ready{false};

Slot& SlotFor(u64 address) {
    const u64 h = (address >> 2) * 0x9E3779B97F4A7C15ull;
    return slots[h >> (64 - SlotBits)];
}

void Lock(Slot& s) {
    while (s.lock.exchange(1, std::memory_order_acquire)) {
        while (s.lock.load(std::memory_order_relaxed)) {
            __builtin_ia32_pause();
        }
    }
}

void Unlock(Slot& s) {
    s.lock.store(0, std::memory_order_release);
}

std::atomic<u64> written_records{0}, written_fallback{0}, tokens_carried{0}, tokens_written{0};
u64 verified = 0, mismatches = 0, unrecorded = 0, trace_trailers = 0, decoded = 0;
u64 window_draws = 0, packet_ticks = 0, draw_ticks = 0, timed_draws = 0;
u64 window_tsc = 0;
std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
u64 mismatch_logged = 0;
std::chrono::steady_clock::time_point mismatch_window = std::chrono::steady_clock::now();

const char* KindName(u32 kind) {
    return kind == DrawAuto ? "drawIndexAuto" : kind == DrawIndex ? "drawIndex"
           : kind == DrawIndexOffset                              ? "drawIndexOffset"
                                                                  : "?";
}

}

Mode GetMode() {
    static const Mode mode = [] {
        const Mode m = ReadMode();
        if (m == Mode::Verify) {
            slots = std::make_unique<Slot[]>(std::size_t(1) << SlotBits);
            slots_ready.store(true, std::memory_order_release);
        }
        if (m != Mode::Off) {
            std::printf("Native calls: BB_NATIVE_CALLS=%s (dcb.drawIndexAuto, drawIndex, "
                        "drawIndexOffset)\n",
                        ModeName(m));
        }
        return m;
    }();
    return mode;
}

void Remember(u64 address, const DrawRecord& record) {
    if (!slots_ready.load(std::memory_order_acquire)) {
        return;
    }
    Slot& s = SlotFor(address);
    Lock(s);
    s.address = address;
    s.record = record;
    Unlock(s);
}

bool Take(u64 address, DrawRecord& record) {
    if (!slots_ready.load(std::memory_order_acquire)) {
        return false;
    }
    Slot& s = SlotFor(address);
    Lock(s);
    const bool found = s.address == address && s.record.kind != DrawNone;
    if (found) {
        record = s.record;
        s.record.kind = DrawNone;
    }
    Unlock(s);
    return found;
}

void NoteVerified(bool ok) {
    ++verified;
    mismatches += !ok;
}

void NoteUnrecorded() {
    ++unrecorded;
}

void NoteTraceTrailer() {
    ++trace_trailers;
}

void ReportMismatch(const char* what, u64 address, const DrawRecord& record, const u32* actual,
                    u32 words, u32 num_instances) {
    const auto now = std::chrono::steady_clock::now();
    if (mismatch_logged >= 16) {
        if (now - mismatch_window < std::chrono::seconds(5)) {
            return;
        }
        mismatch_window = now;
    }
    ++mismatch_logged;
    u32 expected[8]{};
    const u32 n = EncodeDrawPacket(record, expected);
    char line[512];
    int len = std::snprintf(line, sizeof(line),
                            "Native calls: mismatch (%s) at %#llx: %s count %u offset %u addr %#llx "
                            "pred %u, instances %u; record",
                            what, (unsigned long long)address, KindName(record.kind),
                            record.index_count, record.index_offset,
                            (unsigned long long)record.index_addr, record.predicate, num_instances);
    for (u32 k = 0; k < n && len < int(sizeof(line)) - 12; ++k) {
        len += std::snprintf(line + len, sizeof(line) - len, " %08x", expected[k]);
    }
    len += std::snprintf(line + len, sizeof(line) - len, "; decoded");
    for (u32 k = 0; k < words && len < int(sizeof(line)) - 12; ++k) {
        len += std::snprintf(line + len, sizeof(line) - len, " %08x", actual[k]);
    }
    std::printf("%s\n", line);
    std::fflush(stdout);
}

void NoteWritten(bool as_record) {
    (as_record ? written_records : written_fallback).fetch_add(1, std::memory_order_relaxed);
}

void NoteToken(bool carried) {
    (carried ? tokens_carried : tokens_written).fetch_add(1, std::memory_order_relaxed);
}

void NoteDecoded() {
    ++decoded;
}

bool TimingEnabled() {
    static const bool on = std::getenv("BB_FRAME_STATS") != nullptr;
    return on;
}

void AddPacketTicks(u64 ticks, bool is_draw) {
    packet_ticks += ticks;
    if (is_draw) {
        draw_ticks += ticks;
        ++timed_draws;
    }
}

void MaybeReport() {
    const Mode mode = GetMode();
    const bool timing = TimingEnabled();
    if (mode == Mode::Off && !timing) {
        return;
    }
    if ((++window_draws & 255) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - window_start).count();
    if (secs < 5.0) {
        return;
    }
    const u64 tsc = Ticks();
    const double ticks_per_us = window_tsc && tsc > window_tsc ? (tsc - window_tsc) / (secs * 1e6)
                                                               : 0.0;
    window_tsc = tsc;
    window_start = now;
    char extra[256] = "";
    if (mode == Mode::Verify) {
        std::snprintf(extra, sizeof(extra), " (%llu direct draws without a record, %llu thread "
                      "trace trailers)",
                      (unsigned long long)unrecorded, (unsigned long long)trace_trailers);
        std::printf("Native calls: %llu draws verified, %llu mismatches%s\n",
                    (unsigned long long)verified, (unsigned long long)mismatches, extra);
    } else if (mode == Mode::Native) {
        std::printf("Native calls: %llu draws as records (%llu written, %llu kept as PM4); draw tokens: "
                    "%llu carry their base vertex and instances, %llu written as packets\n",
                    (unsigned long long)decoded,
                    (unsigned long long)written_records.exchange(0, std::memory_order_relaxed),
                    (unsigned long long)written_fallback.exchange(0, std::memory_order_relaxed),
                    (unsigned long long)tokens_carried.exchange(0, std::memory_order_relaxed),
                    (unsigned long long)tokens_written.exchange(0, std::memory_order_relaxed));
    }
    if (timing && ticks_per_us > 0.0 && timed_draws) {
        std::printf("Native calls (%s) GPU command thread: %.0f draws/s, draw packets %.3f us/draw, "
                    "all packets %.3f us/draw\n",
                    ModeName(mode), timed_draws / secs, draw_ticks / ticks_per_us / timed_draws,
                    packet_ticks / ticks_per_us / timed_draws);
    }
    std::fflush(stdout);
    verified = mismatches = unrecorded = trace_trailers = decoded = 0;
    packet_ticks = draw_ticks = timed_draws = 0;
}

}
