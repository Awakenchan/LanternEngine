// SPDX-License-Identifier: GPL-3.0-or-later
#include "lantern_meter.h"
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include "bbport_platform.h"
#include "bbport_sections.h"
#include "bbport_toggles.h"
#include "common/thread.h"
#include "engine/bottleneck_meter.h"
#include "engine/pipe_census.h"
#include "lantern_ab_flip.h"
#include "lantern_meter_threads.h"

extern "C" uint64_t runtime_tsc_frequency(void);

namespace Lantern::Meter {
namespace {
namespace Rule = BbEngine::BottleneckMeter;

constexpr auto kSampleInterval = std::chrono::milliseconds(200);
constexpr u32 kSamplesPerWindow = 25;
struct BarrierLoop {
    std::uint64_t start;
    std::array<std::uint8_t, 18> bytes;
    std::size_t size;
};
constexpr std::array<BarrierLoop, 2> kBarrierLoops = {{
    {0x21b8890, {0x45, 0x3b, 0x27, 0x0f, 0x8e, 0x97, 0x00, 0x00, 0x00, 0x80, 0x3d, 0x91, 0xe3, 0x4e, 0x03, 0x00, 0x74, 0xee}, 18},
    {0x21b8960, {0x8b, 0x3e, 0x83, 0xff, 0x02, 0x75, 0xf9}, 7},
}};
constexpr u32 kThreadRefreshWindows = 12;
constexpr u32 kStartupWindows = 3;
constexpr std::size_t kLineBytes = 2048;
constexpr const char* kMainName = "main";
constexpr long kRawInputs = kMeasureMode;

struct Report {
    std::mutex mutex;
    std::condition_variable wake;
    bool started = false;
    bool stopping = false;
    Rule::RunTally tally;
    Rule::HalfTotals before;
    Rule::HalfTotals after;
};
Report g_report;
std::mutex g_sources;

struct Previous {
    u64 at_ns = 0;
    u64 frames = 0, draws = 0, stall_ns = 0, cap_wait_ns = 0, flip_hold_ns = 0;
    u64 present_ns = 0, tick_ns = 0, idle_ns = 0, back_pressure_ns = 0, gpu_busy_ns = 0;
    u64 stage_b_cycles = 0, stage_a_waited_cycles = 0, stage_a_cpu_ns = 0;
    std::array<u64, kDrawThreads + 1> slot_draws{};
    std::array<u64, BbSections::ACount> a_cycles{};
    u64 a_sampled = 0;
};


u64 Delta(u64 now, u64& last) {
    const u64 d = now >= last ? now - last : 0;
    last = now;
    return d;
}

u64 CyclesToNs(u64 cycles, u64 hz) {
    return hz ? u64(double(cycles) * 1e9 / double(hz)) : 0;
}

u64 Load(const std::atomic<u64>& value) {
    return value.load(std::memory_order_relaxed);
}

u64 SplitNs(BbSections::AId id, Previous& p, u64 hz) {
    return CyclesToNs(Delta(BbSections::a_sample_cycles[id].load(std::memory_order_relaxed), p.a_cycles[id]), hz);
}

void FillStageASplit(Rule::StageASplit& s, Previous& p, u64 hz) {
    s.sampled = Delta(BbSections::a_sampled_draws.load(std::memory_order_relaxed), p.a_sampled);
    s.draw_ns = SplitNs(BbSections::ADraw, p, hz);
    s.pipeline_ns = SplitNs(BbSections::APipeline, p, hz);
    s.key_ns = SplitNs(BbSections::AKey, p, hz);
    s.constants_ns = SplitNs(BbSections::ARing, p, hz);
    s.inputs_ns = SplitNs(BbSections::AInputs, p, hz);
    s.packet_ns = SplitNs(BbSections::APacket, p, hz);
    s.record_ns = SplitNs(BbSections::ARecord, p, hz);
}

void FillWindow(Rule::Window& w, Previous& p, Threads::Times& times, u64 now) {
    const u64 hz = runtime_tsc_frequency();
    w.window_ns = Delta(now, p.at_ns);
    w.frames = Delta(Load(g_state.frames), p.frames);
    w.draws = Delta(Load(BbStats::draws), p.draws);
    w.stall_ns = Delta(Load(g_state.stall_ns), p.stall_ns);
    const u64 cap_wait = Delta(Load(g_state.cap_wait_ns), p.cap_wait_ns);
    const u64 flip_hold = Delta(Load(g_state.flip_hold_ns), p.flip_hold_ns);
    w.cap_wait_ns = cap_wait > flip_hold ? cap_wait : flip_hold;
    const u32 flip_cap = g_state.flip_cap.load(std::memory_order_relaxed);
    const u32 game_cap = g_state.game_cap.load(std::memory_order_relaxed);
    w.cap_fps = flip_cap && game_cap ? (flip_cap < game_cap ? flip_cap : game_cap) : (flip_cap ? flip_cap : game_cap);
    w.present_wait_ns = Delta(Load(BbStats::present_wait_ns), p.present_ns);
    w.tick_wait_ns = Delta(Load(BbStats::tick_wait_ns), p.tick_ns);
    w.back_pressure_ns = Delta(Load(BbStats::gnm_frame_wait_ns), p.back_pressure_ns);
    w.gpu_known = g_state.gpu_seen.load(std::memory_order_acquire);
    w.gpu_busy_ns = Delta(Load(g_state.gpu_busy_ns), p.gpu_busy_ns);
    const std::atomic<u64>* b = g_state.stage_b_busy_cycles.load(std::memory_order_acquire);
    const std::atomic<u64>* a = g_state.stage_a_waited_cycles.load(std::memory_order_acquire);
    w.pipe = b && a;
    if (w.pipe) {
        w.stage_b_busy_ns = CyclesToNs(Delta(Load(*b), p.stage_b_cycles), hz);
        w.stage_a_waited_ns = CyclesToNs(Delta(Load(*a), p.stage_a_waited_cycles), hz);
    }
    const u64 idle = Delta(Load(BbStats::gpu_idle_ns), p.idle_ns);
    if (const int clock = BbStats::gpu_thread_clock.load(std::memory_order_relaxed); clock != -1) {
        w.stage_a_known = true;
        w.stage_a_idle_ns = idle;
        w.stage_a_cpu_ns = Delta(BbPlatform::ReadThreadCpuClockNs(clock), p.stage_a_cpu_ns);
        FillStageASplit(w.stage_a_split, p, hz);
    }
    w.main_known = times.main_found;
    w.main_ns = times.main_ns;
    w.gx_render_known = times.render_found;
    w.gx_render_ns = times.render_ns;
    w.gx_workers = times.workers;
    w.gx_worker_max_ns = times.worker_max_ns;
    w.gx_worker_sum_ns = times.worker_sum_ns;
}

std::size_t TakeDraws(Previous& p, std::array<Rule::ThreadDraws, kDrawThreads + 1>& out, bool* main_drew) {
    std::size_t n = 0;
    *main_drew = false;
    for (std::size_t i = 0; i < g_state.draw_slots.size(); ++i) {
        DrawSlot& slot = g_state.draw_slots[i];
        if (!slot.used.load(std::memory_order_acquire)) continue;
        const u64 draws = Delta(Load(slot.draws), p.slot_draws[i]);
        out[n++] = {slot.name, draws};
        if (draws && std::strcmp(slot.name, kMainName) == 0) *main_drew = true;
    }
    return n;
}

void PrintRaw(const Rule::Window& w) {
    Log(LogChannel::Always,
        "Limit inputs: window %llu ns, %llu frames, %llu draws, stall %llu, cap wait %llu, present %llu, "
        "tick %llu, GPU busy %llu, stage B %llu, stage A waited %llu, idle %llu, "
        "CPU %llu, main %llu (GX barrier %llu, %llu samples stopped main %llu ns in all), GX render %llu, GX workers "
        "%u max %llu sum %llu, back-pressure %llu ns",
        (unsigned long long)w.window_ns, (unsigned long long)w.frames, (unsigned long long)w.draws,
        (unsigned long long)w.stall_ns, (unsigned long long)w.cap_wait_ns, (unsigned long long)w.present_wait_ns,
        (unsigned long long)w.tick_wait_ns, (unsigned long long)w.gpu_busy_ns,
        (unsigned long long)w.stage_b_busy_ns,
        (unsigned long long)w.stage_a_waited_ns, (unsigned long long)w.stage_a_idle_ns,
        (unsigned long long)w.stage_a_cpu_ns, (unsigned long long)w.main_ns, (unsigned long long)w.barrier_ns,
        (unsigned long long)Load(g_state.main_samples), (unsigned long long)Load(g_state.main_stopped_ns),
        (unsigned long long)w.gx_render_ns,
        w.gx_workers, (unsigned long long)w.gx_worker_max_ns, (unsigned long long)w.gx_worker_sum_ns,
        (unsigned long long)w.back_pressure_ns);
}

struct MainSamples {
    u64 taken = 0;
    u64 in_barrier = 0;
    u64 stopped_ns = 0;
};

void SampleMain(Threads::GuestThreads& threads, MainSamples& s) {
    const u64 lo = g_state.barrier_lo.load(std::memory_order_acquire);
    if (!lo) return;
    u64 rip = 0, stopped = 0;
    if (!threads.SampleMain(&rip, &stopped)) return;
    ++s.taken;
    s.stopped_ns += stopped;
    for (const BarrierLoop& loop : kBarrierLoops) {
        if (rip >= lo + loop.start && rip < lo + loop.start + loop.size) {
            ++s.in_barrier;
            break;
        }
    }
}

void ReportLoop() {
    Threads::GuestThreads threads;
    Previous previous;
    previous.at_ns = SteadyNs();
    bool listed = threads.Refresh();
    bool told = false;
    Threads::Times last = threads.Take();
    std::array<Rule::ThreadDraws, kDrawThreads + 1> draws{};
    const char* commit = BbEngine::PipeCensus::BuildCommit();
    for (u32 window = 1;; ++window) {
        const AbFlip::Half half_at_start = AbFlip::Current();
        MainSamples samples;
        for (u32 tick = 0; tick < kSamplesPerWindow; ++tick) {
            {
                std::unique_lock lock(g_report.mutex);
                if (g_report.wake.wait_for(lock, kSampleInterval, [] { return g_report.stopping; })) return;
            }
            {
                std::scoped_lock sources(g_sources);
                if (auto* collect = g_state.gpu_collect.load(std::memory_order_acquire)) collect();
            }
            SampleMain(threads, samples);
        }
        const bool incomplete = !listed || !last.main_found || last.workers == 0 || last.gone != 0;
        if (window <= kStartupWindows || (incomplete && window % kThreadRefreshWindows == 0)) {
            listed = threads.Refresh();
            if (!listed && !told) {
                told = true;
                Log(LogChannel::Always, "Limit: the game's threads could not be listed; main and GX read as ?");
            }
        }
        Threads::Times times = threads.Take();
        last = times;
        const u64 now = SteadyNs();
        const u64 first = g_state.first_flip_ns.load(std::memory_order_relaxed);
        AbFlip::Advance(first && now > first ? now - first : 0);
        const AbFlip::Half half = AbFlip::Current();
        Rule::Window w;
        {
            std::scoped_lock sources(g_sources);
            FillWindow(w, previous, times, now);
        }
        w.half = half == AbFlip::Half::None ? Rule::kHalfNone
                 : half != half_at_start    ? Rule::kHalfFlip
                 : half == AbFlip::Half::Before ? Rule::kHalfBefore
                                                : Rule::kHalfAfter;
        w.barrier_known = times.main_found && samples.taken > 0;
        w.barrier_ns = samples.taken ? w.window_ns * samples.in_barrier / samples.taken : 0;
        g_state.main_samples.fetch_add(samples.taken, std::memory_order_relaxed);
        g_state.main_stopped_ns.fetch_add(samples.stopped_ns, std::memory_order_relaxed);
        bool main_drew = false;
        const std::size_t n = TakeDraws(previous, draws, &main_drew);
        const Rule::Stage stage = Rule::Decide(w);
        char line[kLineBytes];
        Rule::FormatLine(line, sizeof line, w, stage, std::span<const Rule::ThreadDraws>(draws.data(), n), main_drew,
                         commit);
        LogLine(LogChannel::Always, line);
        if (Switches::Get().meter >= kRawInputs) PrintRaw(w);
        std::scoped_lock lock(g_report.mutex);
        g_report.tally.Add(stage);
        if (w.half == Rule::kHalfBefore) g_report.before.Add(w);
        if (w.half == Rule::kHalfAfter) {
            g_report.after.Add(w);
            if (g_report.after.windows % kThreadRefreshWindows == 0) {
                Rule::FormatHalves(line, sizeof line, g_report.before, g_report.after, commit);
                LogLine(LogChannel::Always, line);
            }
        }
    }
}

void StartReport() {
    {
        std::scoped_lock lock(g_report.mutex);
        if (g_report.started) return;
        g_report.started = true;
    }
    std::thread([] {
        Common::SetCurrentThreadName("bb:Meter");
        ReportLoop();
    }).detach();
}
}

void OnFlip(u64 now_ns) {
    if (!On()) return;
    if (g_state.last_flip_ns) {
        const u64 interval = now_ns > g_state.last_flip_ns ? now_ns - g_state.last_flip_ns : 0;
        if (interval >= Rule::kStallFrameNs) g_state.stall_ns.fetch_add(interval, std::memory_order_relaxed);
    } else {
        g_state.first_flip_ns.store(now_ns, std::memory_order_relaxed);
        StartReport();
    }
    g_state.last_flip_ns = now_ns;
    g_state.frames.fetch_add(1, std::memory_order_relaxed);
}

bool NoteGuestImage(const std::uint8_t* image, std::size_t size) {
    for (const BarrierLoop& loop : kBarrierLoops) {
        if (!image || size < loop.start + loop.size || std::memcmp(image + loop.start, loop.bytes.data(), loop.size) != 0) {
            Log(LogChannel::Always, "Limit: the GX job barrier loop at 0x%llx holds other bytes; main's time there stays main's",
                (unsigned long long)loop.start);
            return false;
        }
    }
    g_state.barrier_lo.store(reinterpret_cast<std::uintptr_t>(image), std::memory_order_release);
    return true;
}

void Detach() {
    std::scoped_lock sources(g_sources);
    g_state.gpu_collect.store(nullptr, std::memory_order_release);
    g_state.stage_b_busy_cycles.store(nullptr, std::memory_order_release);
    g_state.stage_a_waited_cycles.store(nullptr, std::memory_order_release);
}

void OnExit() {
    if (!On()) return;
    char line[kLineBytes];
    char halves[kLineBytes];
    halves[0] = 0;
    {
        std::scoped_lock lock(g_report.mutex);
        g_report.stopping = true;
        Rule::FormatRun(line, sizeof line, g_report.tally, BbEngine::PipeCensus::BuildCommit());
        if (g_report.before.windows || g_report.after.windows)
            Rule::FormatHalves(halves, sizeof halves, g_report.before, g_report.after, BbEngine::PipeCensus::BuildCommit());
    }
    g_report.wake.notify_all();
    LogLine(LogChannel::Always, line);
    if (halves[0]) LogLine(LogChannel::Always, halves);
}
}
