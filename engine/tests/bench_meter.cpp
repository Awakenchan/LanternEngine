// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <string_view>
#include <thread>
#include <vector>
#include "gpu/shim/engine/bottleneck_meter.h"
#include "gpu/shim/lantern_meter.h"
#include "gpu/shim/lantern_meter_threads.h"
#include "gpu/shim/lantern_gpu_stamps.h"
#include "gpu/shim/lantern_ab_flip.h"
#include "gpu/shim/bbport_sections.h"
#include "gpu/shim/lantern_switches.h"
#include "gpu/shim/bbport_platform.h"
#include "gpu/shim/engine/pipe_census.h"
#include "gpu/shadps4/common/thread.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

extern "C" uint64_t runtime_tsc_frequency(void) {
    return 3'000'000'000ull;
}

namespace BbEngine::PipeCensus {
const char* BuildCommit() {
    return "bench";
}
}

namespace Common {
void SetCurrentThreadName(const char*) {}
}

#ifdef _WIN32
namespace BbPlatform {
std::uint64_t ReadThreadCpuClockNs(int) {
    return 0;
}
}
#endif

namespace {
namespace R = BbEngine::BottleneckMeter;
using Clock = std::chrono::steady_clock;
using u64 = std::uint64_t;

constexpr u64 kCalls = 20'000'000;
constexpr u64 kReportRuns = 2'000;
constexpr u64 kListRuns = 10;
constexpr double kListsAtStart = 3.0;

constexpr double kFps = 120.0;
constexpr double kDrawsPerFrame = 2600.0;
constexpr double kPacketsPerFrame = 2600.0;
constexpr double kCmdbufsPerFrame = 60.0;
constexpr double kScopesPerDraw = 9.0;
constexpr u64 kMainSamples = 500;
constexpr double kMainSamplesPerSecond = 5.0;
constexpr double kTicksPerFrame = 13.0;
constexpr double kRecorderLoopsPerFrame = 200.0;
constexpr double kWindowSeconds = 5.0;
constexpr double kLogicalCpus = 12.0;

const char* g_env_meter = "1";
const char* g_env_sample = "";

const char* Env(const char* name, void*) {
    if (std::string_view(name) == "LANTERN_METER") return g_env_meter;
    if (std::string_view(name) == "BB_SAMPLE_THREADS") return g_env_sample;
    return nullptr;
}

void Configure(const char* meter, const char* sample) {
    g_env_meter = meter;
    g_env_sample = sample;
    Lantern::SwitchInput input;
    input.env = Env;
    if (!Lantern::ReloadSwitches(input)) std::printf("bench: switches did not reload\n");
}

template <typename F> double NsPerCall(u64 calls, F&& f) {
    const auto start = Clock::now();
    for (u64 i = 0; i < calls; ++i) f(i);
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count() / double(calls);
}

double g_total_ns_per_s = 0.0;
volatile bool sink_flag = false;

void Row(const char* what, double ns, double per_second, const char* thread) {
    const double cost = ns * per_second;
    g_total_ns_per_s += cost;
    std::printf("  %-38s %8.2f ns a call x %9.0f/s = %8.1f us/s on %s (%.4f%% of that thread)\n", what, ns, per_second,
                cost / 1e3, thread, cost / 1e7);
}
}

namespace {
std::atomic<bool> g_release{false};

void NameThread(const char* name) {
#ifdef _WIN32
    wchar_t wide[32] = {};
    for (std::size_t i = 0; name[i] && i < 31; ++i) wide[i] = wchar_t(name[i]);
    SetThreadDescription(GetCurrentThread(), wide);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

std::vector<std::thread> GuestLikeThreads(std::atomic<int>& named) {
    static const char* const names[] = {"main", "GXRenderThread", "GXWorker:1", "GXWorker:2",
                                        "GXWorker:3", "GXWorker:4", "GXWorker:5"};
    std::vector<std::thread> threads;
    for (const char* name : names) {
        threads.emplace_back([name, &named] {
            NameThread(name);
            named.fetch_add(1);
            const bool spin = std::string_view(name) == "main";
            while (!g_release.load()) {
                if (!spin) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    return threads;
}
}

int main() {
    volatile u64 sink = 0;
    std::printf("bench meter: %.0f FPS, %.0f draws and %.0f packets a frame, %.0f command buffers, %.0f "
                "frame ticks a frame\n",
                kFps, kDrawsPerFrame, kPacketsPerFrame, kCmdbufsPerFrame, kTicksPerFrame);

    Configure("0", "");
    const double draw_off = NsPerCall(kCalls, [](u64) { Lantern::Meter::NoteDraw(); });
    const double place_off = NsPerCall(kCalls, [](u64) { Lantern::Meter::NotePlace(); });
    const double empty = NsPerCall(kCalls, [&](u64 i) { sink = sink + i; });
    std::printf("off (LANTERN_METER=0, no sampler): NoteDraw %.2f ns, NotePlace %.2f ns, empty loop %.2f ns\n",
                draw_off, place_off, empty);

    Configure("1", "");
    const double draw_on = NsPerCall(kCalls, [](u64) { Lantern::Meter::NoteDraw(); });
    const double sample_decide = NsPerCall(kCalls, [](u64) { sink_flag = Lantern::Meter::SampleStageA(); });
    Configure("2", "");
    const double sample_decide_measure = NsPerCall(kCalls, [](u64) { sink_flag = Lantern::Meter::SampleStageA(); });
    Configure("1", "");
    const double scope_off = NsPerCall(kCalls, [](u64) { BbSections::ScopeA scope{BbSections::AKey}; });
    BbSections::a_timing.store(true);
    BbSections::a_sampled = true;
    const double scope_on = NsPerCall(kCalls / 10, [](u64) { BbSections::ScopeA scope{BbSections::AKey}; });
    BbSections::a_sampled = false;
    BbSections::a_timing.store(false);
    static Lantern::GpuStamps::Slots slots;
    const double stamp = NsPerCall(kCalls / 100, [](u64 i) {
        (void)slots.Begin(i + 1, i);
        (void)slots.End(i + 1, i + 1);
        const Lantern::GpuStamps::Pending p = slots.Outstanding();
        slots.Release(p.count);
    });
    std::vector<R::Span> spans(64);
    R::SpanUnion union_ticks;
    const double span = NsPerCall(kReportRuns, [&](u64 i) {
        for (std::size_t k = 0; k < spans.size(); ++k) spans[k] = {i * 1000 + k * 10, i * 1000 + k * 10 + 15};
        union_ticks.Add(spans);
    }) / double(spans.size());
    const double flip_check = NsPerCall(kReportRuns, [](u64 i) { (void)Lantern::AbFlip::Advance(i); });
    const double cap = NsPerCall(kCalls, [](u64) { Lantern::Meter::NoteCapWait(1000); });
    const double hold = NsPerCall(kCalls, [](u64) { Lantern::Meter::NoteFlipHold(1000); });
    Lantern::Meter::g_state.last_flip_ns = 1;
    const double flip = NsPerCall(kCalls, [](u64 i) { Lantern::Meter::OnFlip(i + 2); });

    R::Window w;
    w.window_ns = 5'000'000'000ull;
    w.frames = 600;
    w.pipe = w.stage_a_known = w.gpu_known = w.main_known = true;
    const R::ThreadDraws draws[] = {{"GXWorker:1", 1}, {"GXWorker:2", 1}, {"GXWorker:3", 1}, {"GXWorker:4", 1},
                                    {"GXWorker:5", 1}};
    char line[2048];
    const double report = NsPerCall(kReportRuns, [&](u64 i) {
        w.main_ns = i;
        R::FormatLine(line, sizeof line, w, R::Decide(w), draws, false, "bench");
    });
    std::atomic<int> named{0};
    std::vector<std::thread> guests = GuestLikeThreads(named);
    while (named.load() < int(std::size(guests))) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Lantern::Meter::Threads::GuestThreads threads;
    const double list_ns = NsPerCall(kListRuns, [&](u64) { (void)threads.Refresh(); });
    const double take_ns = NsPerCall(kReportRuns, [&](u64) { (void)threads.Take(); });
    u64 stopped_total = 0, stopped_samples = 0;
    for (u64 i = 0; i < kMainSamples; ++i) {
        u64 rip = 0, stopped = 0;
        if (threads.SampleMain(&rip, &stopped)) {
            stopped_total += stopped;
            ++stopped_samples;
        }
    }
    const double main_stop = stopped_samples ? double(stopped_total) / double(stopped_samples) : 0.0;

    Configure("1", "load");
    const double place_on = NsPerCall(kCalls, [](u64) { Lantern::Meter::NotePlace(); });
    const double cpu = NsPerCall(kCalls / 10, [](u64) { (void)Lantern::Meter::Threads::CurrentCpu(); });

    std::printf("on (default): per second at the rates above\n");
    Row("NoteDraw (GX workers, all of them)", draw_on, kFps * kDrawsPerFrame, "the GX workers together");
    Row("stage A split check, off (every draw)", sample_decide, kFps * kDrawsPerFrame, "stage A");
    Row("stage A scope, not sampled (9 a draw)", scope_off, kFps * kDrawsPerFrame * kScopesPerDraw, "stage A");
    Row("GPU stamp bookkeeping (begin and end)", stamp, kFps * kCmdbufsPerFrame, "the recording threads");
    Row("GPU span union (per command buffer)", span, kFps * kCmdbufsPerFrame, "bb:Meter");
    Row("A/B flip check", flip_check, 1.0 / kWindowSeconds, "bb:Meter");
    Row("NoteCapWait", cap, kFps, "main");
    Row("NoteFlipHold", hold, kFps, "bb:VideoOut");
    Row("OnFlip", flip, kFps, "bb:VideoOut");
    Row("5 s line (rule and format)", report, 1.0 / kWindowSeconds, "bb:Meter");
    Row("thread CPU times (Take)", take_ns, 1.0 / kWindowSeconds, "bb:Meter");
    Row("main stopped by its IP sample (5 Hz)", main_stop, kMainSamplesPerSecond, "main");
    const double on_total = g_total_ns_per_s;
    std::printf("on total: %.1f us/s = %.4f%% of one core, %.5f%% of %.0f logical CPUs\n", on_total / 1e3,
                on_total / 1e7, on_total / (1e7 * kLogicalCpus), kLogicalCpus);
    std::printf("NoteDraw over an off build: %.2f ns a draw\n", draw_on - draw_off);
    g_total_ns_per_s = 0.0;
    std::printf("measuring package only (LANTERN_METER=2): the stage A split, per second\n");
    Row("stage A split check (every draw)", sample_decide_measure, kFps * kDrawsPerFrame, "stage A");
    Row("stage A scope, sampled (1 draw in 16)", scope_on, kFps * kDrawsPerFrame * kScopesPerDraw / 16.0, "stage A");
    std::printf("stage A split total: %.1f us/s on stage A\n", g_total_ns_per_s / 1e3);

    g_total_ns_per_s = 0.0;
    std::printf("sampler on (BB_SAMPLE_THREADS set): placement notes, per second\n");
    Row("NotePlace main (frame ticks)", place_on, kFps * kTicksPerFrame, "main");
    Row("NotePlace stage A (packets)", place_on, kFps * kPacketsPerFrame, "stage A");
    Row("NotePlace stage B (packets)", place_on, kFps * kPacketsPerFrame, "stage B");
    Row("NotePlace recorders (loops)", place_on, kFps * kRecorderLoopsPerFrame, "the recorders");
    std::printf("placement total: %.1f us/s; CurrentCpu %.2f ns a call (1 in %u notes)\n", g_total_ns_per_s / 1e3, cpu,
                Lantern::Meter::kPlacePublishMask + 1);
    std::printf("thread list (Refresh): %.2f ms a call on bb:Meter, %.0f calls at the start of a session, then only "
                "while main or a worker is missing (every 60 s); %zu threads wanted in this process\n",
                list_ns / 1e6, kListsAtStart, threads.Count());
    g_release.store(true);
    for (std::thread& t : guests) t.join();
    return int(sink & 0);
}
