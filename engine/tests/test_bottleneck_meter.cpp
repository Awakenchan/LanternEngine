// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/bottleneck_meter.h"
#include "gpu/shim/lantern_ab_flip.h"
#include "gpu/shim/lantern_meter_threads.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace M = BbEngine::BottleneckMeter;
namespace T = Lantern::Meter::Threads;
using u64 = std::uint64_t;
using Stage = M::Stage;

namespace {
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++g_failed;
    }
}

constexpr u64 kWindowNs = 5'000'000'000ull;

u64 Pct(double percent) {
    return u64(double(kWindowNs) * percent / 100.0);
}

M::Window Base() {
    M::Window w;
    w.window_ns = kWindowNs;
    w.frames = 400;
    w.draws = 400 * 1200;
    w.pipe = true;
    w.stage_a_known = true;
    w.stage_a_idle_ns = Pct(30);
    w.stage_a_waited_ns = Pct(1);
    w.stage_b_busy_ns = Pct(60);
    w.gpu_known = true;
    w.gpu_busy_ns = Pct(60);
    w.gx_workers = 5;
    w.gx_worker_max_ns = Pct(30);
    w.gx_worker_sum_ns = Pct(120);
    w.gx_render_known = true;
    w.gx_render_ns = Pct(2);
    w.main_known = true;
    w.main_ns = Pct(60);
    w.back_pressure_ns = Pct(0);
    return w;
}

void TestNoFrames() {
    M::Window w = Base();
    w.frames = 0;
    Check(M::Decide(w) == Stage::NoFrames, "a window without frames names no stage");
    w = Base();
    w.window_ns = 0;
    Check(M::Decide(w) == Stage::NoFrames, "an empty window names no stage");
}

void TestStall() {
    M::Window w = Base();
    w.stall_ns = Pct(30);
    Check(M::Decide(w) == Stage::Stall, "a window that is 30% long frames is a stall");
    w = Base();
    w.stall_ns = Pct(10);
    Check(M::Decide(w) != Stage::Stall, "10% of long frames is not a stall");
}

void TestCapped() {
    M::Window w = Base();
    w.cap_fps = 120;
    w.frames = 595;
    w.cap_wait_ns = Pct(10);
    Check(M::Decide(w) == Stage::Capped, "at the cap with limiter sleep over 5% is capped");
    w.stall_ns = Pct(10);
    Check(M::Decide(w) == Stage::Capped, "a few long frames do not hide the cap");
    w = Base();
    w.cap_fps = 120;
    w.frames = 400;
    w.cap_wait_ns = Pct(10);
    Check(M::Decide(w) != Stage::Capped, "80 FPS under a 120 cap is not capped");
    w = Base();
    w.cap_fps = 0;
    w.frames = 595;
    w.cap_wait_ns = Pct(10);
    Check(M::Decide(w) != Stage::Capped, "no cap is never capped");
    w = Base();
    w.cap_fps = 120;
    w.frames = 595;
    w.cap_wait_ns = Pct(3);
    Check(M::Decide(w) != Stage::Capped, "3% limiter sleep is not capped");
}

void TestPresent() {
    M::Window w = Base();
    w.present_wait_ns = Pct(15);
    Check(M::Decide(w) == Stage::Present, "15% present wait is present");
    w.present_wait_ns = Pct(8);
    Check(M::Decide(w) != Stage::Present, "8% present wait is not present");
}

void TestGpu() {
    M::Window w = Base();
    w.gpu_busy_ns = Pct(96);
    Check(M::Decide(w) == Stage::Gpu, "GPU 96% busy is the GPU");
    w = Base();
    w.stage_b_busy_ns = Pct(96);
    w.gpu_busy_ns = Pct(50);
    Check(M::Decide(w) == Stage::StageB, "stage B full with the GPU half busy is stage B");
    w = Base();
    w.gpu_busy_ns = Pct(99);
    w.gpu_known = false;
    Check(M::Decide(w) != Stage::Gpu, "an unmeasured GPU is never named");
}

void TestStageB() {
    M::Window w = Base();
    w.stage_b_busy_ns = Pct(96);
    Check(M::Decide(w) == Stage::StageB, "stage B 96% busy is stage B");
    w = Base();
    w.stage_b_busy_ns = Pct(70);
    w.stage_a_waited_ns = Pct(12);
    Check(M::Decide(w) == Stage::StageB, "stage A waiting 12% for B is stage B");
    w.pipe = false;
    Check(M::Decide(w) != Stage::StageB, "without the pipe there is no stage B");
}

void TestStageA() {
    M::Window w = Base();
    w.stage_a_idle_ns = Pct(1);
    w.stage_a_waited_ns = Pct(1);
    w.stage_b_busy_ns = Pct(70);
    Check(M::Decide(w) == Stage::StageA, "stage A never idle nor waiting, B at 70%, is stage A");
    w.stage_a_waited_ns = Pct(5);
    Check(M::Decide(w) != Stage::StageA, "stage A waiting 5% for B is not stage A");
    w.stage_a_waited_ns = Pct(1);
    w.stage_b_busy_ns = Pct(92);
    Check(M::Decide(w) != Stage::StageA, "stage B at 92% is not stage A");
    w = Base();
    w.pipe = false;
    w.stage_a_idle_ns = Pct(1);
    w.stage_a_waited_ns = 0;
    Check(M::Decide(w) == Stage::StageA, "without the pipe a busy stage A is stage A");
    w.stage_a_known = false;
    Check(M::Decide(w) != Stage::StageA, "an unmeasured stage A is never named");
}

void TestGx() {
    M::Window w = Base();
    w.gx_worker_max_ns = Pct(96);
    Check(M::Decide(w) == Stage::Gx, "a GX worker 96% busy is GX");
    w = Base();
    w.gx_worker_max_ns = Pct(40);
    w.gx_worker_sum_ns = Pct(200);
    Check(M::Decide(w) != Stage::Gx, "five workers at 40% are not GX");
    w = Base();
    w.gx_render_ns = Pct(97);
    Check(M::Decide(w) == Stage::Gx, "the GX render thread 97% busy is GX");
}

void TestMain() {
    M::Window w = Base();
    w.main_ns = Pct(90);
    Check(M::Decide(w) == Stage::Main, "main 90% busy without back-pressure is main");
    w.main_ns = Pct(87);
    Check(M::Decide(w) == Stage::Main, "main 87% busy is main");
    w.back_pressure_ns = Pct(10);
    Check(M::Decide(w) == Stage::Mixed, "main held back by the pipeline is not main");
    w = Base();
    w.main_ns = Pct(90);
    w.main_known = false;
    Check(M::Decide(w) == Stage::Mixed, "an unmeasured main is never named");
    w = Base();
    w.main_ns = Pct(80);
    Check(M::Decide(w) == Stage::Mixed, "main 80% busy is mixed");
}

void TestSpanUnion() {
    M::SpanUnion u;
    std::vector<M::Span> spans = {{300, 400}, {100, 200}, {150, 250}};
    u.Add(spans);
    Check(u.busy == 250, "out-of-order overlapping spans count once");
    spans = {{350, 500}};
    u.Add(spans);
    Check(u.busy == 350, "a span that overlaps the earlier batch counts only its new part");
    spans = {{420, 480}, {600, 700}};
    u.Add(spans);
    Check(u.busy == 450, "a span inside covered time adds nothing, a gap is not busy");
    spans = {{800, 800}, {900, 850}};
    u.Add(spans);
    Check(u.busy == 450, "empty or reversed spans add nothing");
    M::SpanUnion wrapped;
    constexpr u64 kRange = u64{1} << 32;
    spans = {{kRange - 100, kRange - 50}};
    wrapped.Add(spans, kRange);
    spans = {{10, 30}};
    wrapped.Add(spans, kRange);
    Check(wrapped.busy == 70, "a span after the GPU timer wrapped still counts");
}

void TestMainAndStageAWaits() {
    M::Window w = Base();
    w.main_ns = Pct(90);
    w.stage_b_busy_ns = Pct(88);
    Check(M::Decide(w) == Stage::Mixed, "main is not named while stage B is 88% busy");
    w = Base();
    w.stage_a_idle_ns = Pct(1);
    w.stage_a_waited_ns = Pct(1);
    w.stage_b_busy_ns = Pct(70);
    w.tick_wait_ns = Pct(5);
    Check(M::Decide(w) != Stage::StageA, "stage A waiting on the GPU 5% is not stage A");
    w.tick_wait_ns = 0;
    Check(M::Decide(w) == Stage::StageA, "without GPU waits it is stage A");
    Check(std::string(M::StageName(Stage::Gx)) == "GX (provisional)", "GX is provisional");
}

void TestBarrier() {
    M::Window w = Base();
    w.main_ns = Pct(92);
    w.barrier_known = true;
    w.barrier_ns = Pct(30);
    Check(M::Decide(w) == Stage::Gx, "main saturated, 30% in the GX barrier, is GX");
    w.barrier_ns = Pct(10);
    Check(M::Decide(w) == Stage::Main, "main saturated, 10% in the GX barrier, is main");
    w.barrier_ns = Pct(30);
    w.barrier_known = false;
    Check(M::Decide(w) == Stage::Main, "an unsampled barrier leaves main");
    w = Base();
    w.barrier_known = true;
    w.barrier_ns = Pct(40);
    Check(M::Decide(w) == Stage::Mixed, "the barrier alone does not name GX when main is not saturated");
}

void TestFormat() {
    M::Window w = Base();
    w.stage_b_busy_ns = Pct(96);
    w.cap_fps = 120;
    const M::ThreadDraws draws[] = {{"GXWorker:1", 400 * 412}, {"GXWorker:2", 400 * 300}};
    char line[2048];
    M::FormatLine(line, sizeof line, w, M::Decide(w), draws, false, "abc1234");
    const std::string s = line;
    Check(s.rfind("Limit: stage B (5 s, 80.0 FPS, cap 120;", 0) == 0, "the line starts with the stage in plain words");
    Check(s.find("GXWorker:1 412") != std::string::npos, "draws a frame per thread");
    Check(s.find("main 0") != std::string::npos, "main is named when it drew nothing");
    Check(s.find("stage B 12.00") != std::string::npos, "stage B busy ms a frame");
    Check(s.find("engine abc1234)") != std::string::npos, "the commit id is beside the stage");
    const M::ThreadDraws with_main[] = {{"main", 400}, {"GXWorker:1", 4000}};
    M::FormatLine(line, sizeof line, w, Stage::Main, with_main, true, "abc1234");
    Check(std::strstr(line, "main 0") == nullptr && std::strstr(line, " main 1") != nullptr, "main's own draws are shown");
    w.main_known = false;
    M::FormatLine(line, sizeof line, w, Stage::Mixed, {}, false, "");
    Check(std::strstr(line, "main ?") != nullptr, "an unmeasured main reads ?");
    Check(std::strstr(line, "by thread not counted") != nullptr, "no counted threads say so");
    Check(std::strstr(line, "engine unknown)") != nullptr, "a missing commit says unknown");
    char small[24];
    std::memset(small, 'x', sizeof small);
    const std::size_t n = M::FormatLine(small, sizeof small, w, Stage::Gpu, draws, false, "abc");
    Check(n < sizeof small && small[n] == '\0' && std::strlen(small) == n, "a short buffer stays terminated");
}

void TestHalves() {
    M::Window w = Base();
    w.half = M::kHalfAfter;
    char line[2048];
    M::FormatLine(line, sizeof line, w, M::Decide(w), {}, false, "abc");
    Check(std::strstr(line, "[A/B after]") != nullptr, "a window after the flip says so");
    M::HalfTotals before, after;
    before.Add(Base());
    M::Window faster = Base();
    faster.frames = 600;
    after.Add(faster);
    M::FormatHalves(line, sizeof line, before, after, "abc");
    Check(std::strstr(line, "1 | 1 windows, 80.0 | 120.0 FPS") != nullptr, "both halves side by side");
    Check(Lantern::AbFlip::HalfAt(0, 999'000'000'000ull) == Lantern::AbFlip::Half::None, "no flip time, no halves");
    Check(Lantern::AbFlip::HalfAt(60, 59'000'000'000ull) == Lantern::AbFlip::Half::Before, "before the flip time");
    Check(Lantern::AbFlip::HalfAt(60, 60'000'000'000ull) == Lantern::AbFlip::Half::After, "at the flip time");
}

void TestRun() {
    M::RunTally t;
    t.Add(Stage::Main);
    t.Add(Stage::Main);
    t.Add(Stage::Main);
    t.Add(Stage::StageB);
    char line[512];
    M::FormatRun(line, sizeof line, t, "abc1234");
    Check(std::strstr(line, "4 windows, stage B 1 (25%), main 3 (75%); engine abc1234") != nullptr, "the run tally");
}

#ifndef LANTERN_METER_RULE_MUTANTS
void TestWanted() {
    T::Kind kind{};
    Check(T::Wanted("main", &kind) && kind == T::Kind::Main, "main is the main thread");
    Check(T::Wanted("GXWorker:3", &kind) && kind == T::Kind::Worker, "GXWorker:3 is a worker");
    Check(T::Wanted("GXRenderThread", &kind) && kind == T::Kind::Render, "GXRenderThread is the render thread");
    Check(!T::Wanted("mainloop", &kind), "mainloop is not main");
    Check(!T::Wanted("", &kind) && !T::Wanted(nullptr, &kind), "no name is not wanted");
}

bool NameThisThread(const char* name) {
#ifdef _WIN32
    wchar_t wide[32] = {};
    for (std::size_t i = 0; name[i] && i < 31; ++i) wide[i] = wchar_t(name[i]);
    return SUCCEEDED(SetThreadDescription(GetCurrentThread(), wide));
#else
    return pthread_setname_np(pthread_self(), name) == 0;
#endif
}

void Burn() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    volatile u64 sink = 0;
    while (std::chrono::steady_clock::now() < until) sink = sink + 1;
}

void TestLiveThreads() {
    if (!NameThisThread("GXWorker:9")) {
        Check(false, "the test thread could not be named");
        return;
    }
    char name[16] = {};
    Check(T::CurrentThreadName(name, sizeof name) && std::strcmp(name, "GXWorker:9") == 0, "the name reads back");
    Check(T::CurrentCpu() != T::kUnknownCpu, "the current CPU is known");
    T::GuestThreads threads;
    Check(threads.Refresh(), "the threads are listed");
    Check(threads.Count() == 1, "the named thread is found once");
    const T::Times first = threads.Take();
    Check(first.workers == 0, "the first reading only primes");
    Burn();
    const T::Times second = threads.Take();
    Check(second.workers == 1 && second.worker_sum_ns > 0 && second.worker_max_ns == second.worker_sum_ns,
          "a busy worker shows CPU time");
    Check(!second.main_found, "no main thread in the test");
}
#endif

int RunSuite() {
    g_failed = 0;
    TestNoFrames();
    TestStall();
    TestCapped();
    TestPresent();
    TestGpu();
    TestStageB();
    TestStageA();
    TestGx();
    TestMain();
    TestSpanUnion();
    TestMainAndStageAWaits();
    TestBarrier();
    TestHalves();
    TestFormat();
    TestRun();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_METER_RULE_MUTANTS
    int survivors = 0;
    for (int m = 0; m <= M::kMutants; ++m) {
        M::g_mutant = m;
        const int found = RunSuite();
        if (m == 0) {
            if (found) {
                std::printf("bottleneck meter mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("bottleneck meter mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("bottleneck meter mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("bottleneck meter mutants: every mutant caught\n");
    return 0;
#else
    RunSuite();
    TestWanted();
    TestLiveThreads();
    if (g_failed) {
        std::printf("bottleneck meter: %d checks failed\n", g_failed);
        return 1;
    }
    std::printf("bottleneck meter: all tests passed\n");
    return 0;
#endif
}
