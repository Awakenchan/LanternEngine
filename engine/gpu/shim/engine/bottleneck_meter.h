// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#ifdef LANTERN_METER_RULE_MUTANTS
namespace BbEngine::BottleneckMeter {
inline int g_mutant = 0;
}
#define LANTERN_METER_RULE_PICK(n, wrong, right) (::BbEngine::BottleneckMeter::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_METER_RULE_PICK(n, wrong, right) (right)
#endif

namespace BbEngine::BottleneckMeter {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr int kMutants = 19;

inline constexpr double kStallShare = 0.25;
inline constexpr u64 kStallFrameNs = 100'000'000;
inline constexpr double kCapWaitShare = 0.05;
inline constexpr double kAtCapRatio = 0.97;
inline constexpr double kPresentShare = 0.10;
inline constexpr double kGpuShare = 0.95;
inline constexpr double kStageBShare = 0.95;
inline constexpr double kStageAWaitedShare = 0.10;
inline constexpr double kNearZeroShare = 0.02;
inline constexpr double kStageBLowShare = 0.90;
inline constexpr double kGxShare = 0.95;
inline constexpr double kMainShare = 0.85;
inline constexpr double kStageBMainShare = 0.85;
inline constexpr double kBarrierShare = 0.20;

enum class Stage : u8 { NoFrames, Stall, Capped, Present, Gpu, StageB, StageA, Gx, Main, Mixed, Count };
inline constexpr std::size_t kStages = static_cast<std::size_t>(Stage::Count);
inline constexpr std::array<const char*, kStages> kStageNames = {
    "no frames", "stall", "capped", "present", "GPU", "stage B", "stage A", "GX (provisional)", "main", "mixed"};
static_assert(kStageNames.size() == kStages);

inline const char* StageName(Stage stage) {
    const auto i = static_cast<std::size_t>(stage);
    return i < kStages ? kStageNames[i] : "?";
}

struct StageASplit {
    u64 sampled = 0;
    u64 draw_ns = 0;
    u64 pipeline_ns = 0;
    u64 key_ns = 0;
    u64 constants_ns = 0;
    u64 inputs_ns = 0;
    u64 packet_ns = 0;
    u64 record_ns = 0;
};

struct Window {
    u64 window_ns = 0;
    u64 frames = 0;
    u64 draws = 0;
    u32 cap_fps = 0;
    u64 stall_ns = 0;
    u64 cap_wait_ns = 0;
    u64 present_wait_ns = 0;
    u64 tick_wait_ns = 0;
    bool gpu_known = false;
    u64 gpu_busy_ns = 0;
    bool pipe = false;
    u64 stage_b_busy_ns = 0;
    u64 stage_a_waited_ns = 0;
    bool stage_a_known = false;
    u64 stage_a_idle_ns = 0;
    u64 stage_a_cpu_ns = 0;
    StageASplit stage_a_split;
    u32 gx_workers = 0;
    u64 gx_worker_max_ns = 0;
    u64 gx_worker_sum_ns = 0;
    bool gx_render_known = false;
    u64 gx_render_ns = 0;
    bool main_known = false;
    u64 main_ns = 0;
    bool barrier_known = false;
    u64 barrier_ns = 0;
    u64 back_pressure_ns = 0;
    u8 half = 0;
};

inline constexpr u8 kHalfNone = 0;
inline constexpr u8 kHalfBefore = 1;
inline constexpr u8 kHalfAfter = 2;
inline constexpr u8 kHalfFlip = 3;
inline constexpr std::array<const char*, 4> kHalfNames = {"", " [A/B before]", " [A/B after]", " [A/B flip in this window]"};

inline u64 StageAWaitNs(const Window& w) {
    return w.stage_a_idle_ns + w.stage_a_waited_ns + w.tick_wait_ns;
}

inline double Share(u64 ns, u64 window_ns) {
    return window_ns ? double(ns) / double(window_ns) : 0.0;
}

inline double Fps(const Window& w) {
    return w.window_ns ? double(w.frames) * 1e9 / double(w.window_ns) : 0.0;
}

inline bool AtCap(const Window& w) {
    if (!w.cap_fps) return LANTERN_METER_RULE_PICK(14, true, false);
    return LANTERN_METER_RULE_PICK(2, true, Fps(w) >= double(w.cap_fps) * kAtCapRatio);
}

inline double GxShareOf(const Window& w) {
    const u64 render = w.gx_render_known ? w.gx_render_ns : 0;
    const u64 worker = LANTERN_METER_RULE_PICK(8, w.gx_worker_sum_ns, w.gx_worker_max_ns);
    return Share(worker > render ? worker : render, w.window_ns);
}

inline Stage Decide(const Window& w) {
    if (LANTERN_METER_RULE_PICK(13, w.window_ns == 0, w.frames == 0 || w.window_ns == 0)) return Stage::NoFrames;
    const auto share = [&](u64 ns) { return Share(ns, w.window_ns); };
    if (share(w.stall_ns) >= LANTERN_METER_RULE_PICK(1, kCapWaitShare, kStallShare)) return Stage::Stall;
    if (share(w.cap_wait_ns) > kCapWaitShare && AtCap(w)) return Stage::Capped;
    if (share(w.present_wait_ns) > LANTERN_METER_RULE_PICK(3, kGpuShare, kPresentShare)) return Stage::Present;
    if (w.gpu_known && share(LANTERN_METER_RULE_PICK(4, w.stage_b_busy_ns, w.gpu_busy_ns)) >= kGpuShare)
        return Stage::Gpu;
    const bool b_full = share(w.stage_b_busy_ns) >= kStageBShare;
    const bool a_waits = LANTERN_METER_RULE_PICK(5, false, share(w.stage_a_waited_ns) >= kStageAWaitedShare);
    if (w.pipe && (b_full || a_waits)) return Stage::StageB;
    const u64 a_waits_ns = w.stage_a_waited_ns + LANTERN_METER_RULE_PICK(19, u64{0}, w.tick_wait_ns);
    const bool a_never_waits = LANTERN_METER_RULE_PICK(6, true, share(a_waits_ns) <= kNearZeroShare);
    const bool b_low = !w.pipe || LANTERN_METER_RULE_PICK(7, true, share(w.stage_b_busy_ns) < kStageBLowShare);
    if (w.stage_a_known && share(w.stage_a_idle_ns) <= kNearZeroShare && a_never_waits && b_low) return Stage::StageA;
    if ((w.gx_workers || w.gx_render_known) && GxShareOf(w) >= kGxShare) return Stage::Gx;
    const bool no_back_pressure = LANTERN_METER_RULE_PICK(9, true, share(w.back_pressure_ns) <= kNearZeroShare);
    const bool b_below_main = !w.pipe || LANTERN_METER_RULE_PICK(18, true, share(w.stage_b_busy_ns) < kStageBMainShare);
    if (w.main_known && share(w.main_ns) >= LANTERN_METER_RULE_PICK(10, kGxShare, kMainShare) && no_back_pressure &&
        b_below_main) {
        const bool waits_for_gx =
            w.barrier_known && share(w.barrier_ns) >= LANTERN_METER_RULE_PICK(16, kMainShare, kBarrierShare);
        return LANTERN_METER_RULE_PICK(15, false, waits_for_gx) ? Stage::Gx : Stage::Main;
    }
    return Stage::Mixed;
}

struct Span {
    u64 begin = 0;
    u64 end = 0;
};

struct SpanUnion {
    u64 covered_until = 0;
    u64 busy = 0;

    void Add(std::span<Span> spans, u64 range = 0) {
        if (LANTERN_METER_RULE_PICK(11, false, true))
            std::sort(spans.begin(), spans.end(), [](const Span& x, const Span& y) { return x.begin < y.begin; });
        for (const Span& s : spans) {
            if (range && LANTERN_METER_RULE_PICK(17, false, covered_until > s.begin + range / 2)) covered_until = 0;
            if (s.end <= s.begin || s.end <= covered_until) continue;
            const u64 start = LANTERN_METER_RULE_PICK(12, s.begin, s.begin > covered_until ? s.begin : covered_until);
            busy += s.end - start;
            covered_until = s.end;
        }
    }
};

struct ThreadDraws {
    const char* name = "";
    u64 draws = 0;
};

struct Writer {
    char* out;
    std::size_t room;
    std::size_t used = 0;
    [[gnu::format(printf, 2, 3)]] void Put(const char* format, ...) {
        if (!room || used + 1 >= room) return;
        va_list args;
        va_start(args, format);
        const int n = std::vsnprintf(out + used, room - used, format, args);
        va_end(args);
        if (n > 0) used += static_cast<std::size_t>(n) < room - used ? static_cast<std::size_t>(n) : room - used - 1;
    }
};

inline double MsPerFrame(u64 ns, u64 frames) {
    return frames ? double(ns) / double(frames) / 1e6 : 0.0;
}

inline double UsPer(u64 ns, u64 count) {
    return count ? double(ns) / double(count) / 1e3 : 0.0;
}

inline void PutStageASplit(Writer& o, const Window& w) {
    const StageASplit& s = w.stage_a_split;
    if (!w.stage_a_known || !s.sampled || !w.draws) return;
    const u64 waits = StageAWaitNs(w);
    const u64 busy = w.window_ns > waits ? w.window_ns - waits : 0;
    const double draw_us = UsPer(s.draw_ns, s.sampled);
    const double busy_us = UsPer(busy, w.draws);
    o.Put(" (us a draw, %llu of %llu draws timed: PM4 walk, register apply and other packets %.2f, draw %.2f of which "
          "pipeline %.2f with key %.2f, constants %.2f, inputs %.2f, packet %.2f, record %.2f; waits for stage B %.2f)",
          static_cast<unsigned long long>(s.sampled), static_cast<unsigned long long>(w.draws),
          busy_us > draw_us ? busy_us - draw_us : 0.0, draw_us, UsPer(s.pipeline_ns, s.sampled),
          UsPer(s.key_ns, s.sampled), UsPer(s.constants_ns, s.sampled), UsPer(s.inputs_ns, s.sampled),
          UsPer(s.packet_ns, s.sampled), UsPer(s.record_ns, s.sampled), UsPer(w.stage_a_waited_ns, w.draws));
}

inline std::size_t FormatLine(char* out, std::size_t room, const Window& w, Stage stage,
                              std::span<const ThreadDraws> draws, bool main_drew, const char* commit) {
    Writer o{out, room};
    if (room) out[0] = '\0';
    const double seconds = double(w.window_ns) / 1e9;
    o.Put("Limit: %s (%.0f s, %.1f FPS", StageName(stage), seconds, Fps(w));
    if (w.cap_fps) o.Put(", cap %u", w.cap_fps);
    o.Put("; ms a frame: main ");
    if (w.main_known) o.Put("%.2f", MsPerFrame(w.main_ns, w.frames));
    else o.Put("?");
    if (w.barrier_known) o.Put(" (in the GX job barrier %.2f)", MsPerFrame(w.barrier_ns, w.frames));
    o.Put(", GX workers %.2f max %.2f sum (%u)", MsPerFrame(w.gx_worker_max_ns, w.frames),
          MsPerFrame(w.gx_worker_sum_ns, w.frames), w.gx_workers);
    if (w.gx_render_known) o.Put(", GX render %.2f", MsPerFrame(w.gx_render_ns, w.frames));
    if (w.stage_a_known) {
        const u64 waits = StageAWaitNs(w);
        o.Put(", stage A %.2f (waited %.0f%%, idle %.0f%%)",
              MsPerFrame(w.window_ns > waits ? w.window_ns - waits : 0, w.frames),
              100.0 * Share(w.stage_a_waited_ns, w.window_ns), 100.0 * Share(w.stage_a_idle_ns, w.window_ns));
    }
    if (w.pipe) o.Put(", stage B %.2f", MsPerFrame(w.stage_b_busy_ns, w.frames));
    PutStageASplit(o, w);
    if (w.gpu_known) o.Put(", GPU %.2f", MsPerFrame(w.gpu_busy_ns, w.frames));
    o.Put(", present wait %.2f, cap wait %.2f, back-pressure %.2f, CPU waits GPU %.2f",
          MsPerFrame(w.present_wait_ns, w.frames), MsPerFrame(w.cap_wait_ns, w.frames),
          MsPerFrame(w.back_pressure_ns, w.frames), MsPerFrame(w.tick_wait_ns, w.frames));
    if (w.stall_ns) o.Put(", stalled %.1f s", double(w.stall_ns) / 1e9);
    o.Put("; draws a frame %.0f:", w.frames ? double(w.draws) / double(w.frames) : 0.0);
    if (draws.empty()) {
        o.Put(" by thread not counted,");
    } else {
        for (const ThreadDraws& d : draws) {
            if (d.draws) o.Put(" %s %.0f,", d.name, w.frames ? double(d.draws) / double(w.frames) : 0.0);
        }
        if (!main_drew) o.Put(" main 0,");
    }
    if (o.used && out[o.used - 1] == ',') out[--o.used] = '\0';
    o.Put("; engine %s)%s", commit && commit[0] ? commit : "unknown", w.half < kHalfNames.size() ? kHalfNames[w.half] : "");
    return o.used;
}

struct HalfTotals {
    u64 windows = 0;
    u64 window_ns = 0;
    u64 frames = 0;
    u64 main_ns = 0;
    u64 barrier_ns = 0;
    u64 gx_worker_max_ns = 0;
    u64 stage_a_ns = 0;
    u64 stage_b_ns = 0;
    u64 gpu_ns = 0;

    void Add(const Window& w) {
        ++windows;
        window_ns += w.window_ns;
        frames += w.frames;
        main_ns += w.main_ns;
        barrier_ns += w.barrier_ns;
        gx_worker_max_ns += w.gx_worker_max_ns;
        const u64 waits = StageAWaitNs(w);
        stage_a_ns += w.window_ns > waits ? w.window_ns - waits : 0;
        stage_b_ns += w.stage_b_busy_ns;
        gpu_ns += w.gpu_busy_ns;
    }
};

inline std::size_t FormatHalves(char* out, std::size_t room, const HalfTotals& b, const HalfTotals& a, const char* commit) {
    Writer o{out, room};
    if (room) out[0] = '\0';
    const auto fps = [](const HalfTotals& t) { return t.window_ns ? double(t.frames) * 1e9 / double(t.window_ns) : 0.0; };
    o.Put("Limit A/B, before | after: %llu | %llu windows, %.1f | %.1f FPS; ms a frame: main %.2f | %.2f "
          "(GX barrier %.2f | %.2f), GX worker max %.2f | %.2f, stage A %.2f | %.2f, stage B %.2f | %.2f, "
          "GPU %.2f | %.2f; engine %s",
          static_cast<unsigned long long>(b.windows), static_cast<unsigned long long>(a.windows), fps(b), fps(a),
          MsPerFrame(b.main_ns, b.frames), MsPerFrame(a.main_ns, a.frames), MsPerFrame(b.barrier_ns, b.frames),
          MsPerFrame(a.barrier_ns, a.frames), MsPerFrame(b.gx_worker_max_ns, b.frames),
          MsPerFrame(a.gx_worker_max_ns, a.frames), MsPerFrame(b.stage_a_ns, b.frames),
          MsPerFrame(a.stage_a_ns, a.frames), MsPerFrame(b.stage_b_ns, b.frames), MsPerFrame(a.stage_b_ns, a.frames),
          MsPerFrame(b.gpu_ns, b.frames), MsPerFrame(a.gpu_ns, a.frames), commit && commit[0] ? commit : "unknown");
    return o.used;
}

struct RunTally {
    std::array<u64, kStages> windows{};
    void Add(Stage stage) {
        const auto i = static_cast<std::size_t>(stage);
        if (i < kStages) ++windows[i];
    }
};

inline std::size_t FormatRun(char* out, std::size_t room, const RunTally& t, const char* commit) {
    Writer o{out, room};
    if (room) out[0] = '\0';
    u64 total = 0;
    for (const u64 n : t.windows) total += n;
    o.Put("Limit over the run: %llu windows", static_cast<unsigned long long>(total));
    for (std::size_t i = 0; i < kStages; ++i) {
        if (t.windows[i]) {
            o.Put(", %s %llu (%.0f%%)", kStageNames[i], static_cast<unsigned long long>(t.windows[i]),
                  100.0 * double(t.windows[i]) / double(total));
        }
    }
    o.Put("; engine %s", commit && commit[0] ? commit : "unknown");
    return o.used;
}
}
