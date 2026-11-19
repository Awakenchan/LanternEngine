// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "gpu/shim/lantern_steps.h"

namespace {
using namespace Lantern;

constexpr int kUsesPerDraw = 20;
constexpr int kDrawsPerRun = 200000;
constexpr int kColdDraws = 50000;
constexpr int kColdThreads = 3;
constexpr std::size_t kColdBytes = 64 * 1024;
constexpr double kGateNs = 20.0;
constexpr int kRuns = 5;

using Clock = std::chrono::steady_clock;

double Ns(Clock::duration d) {
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}

[[gnu::noinline]] void ColdDraw(ThreadSlot& slot, u64 draw) {
    Crumb::Draw(slot, draw, draw >> 11, draw * 3, draw * 5, draw * 7);
}

[[gnu::noinline]] bool ColdUse(ThreadSlot& slot, StepId id) {
    if (Steps::Get(id) == Mode::Off) return false;
    Steps::Used(slot, id);
    return Steps::Sample(slot, id);
}

[[gnu::noinline]] void Touch(std::vector<unsigned char>& memory, u64 round) {
    for (std::size_t i = 0; i < memory.size(); i += kCacheLine) memory[i] = static_cast<unsigned char>(memory[i] + round);
}

double WarmRun() {
    ThreadSlot& slot = Steps::Slot();
    u64 sink = 0;
    const auto t0 = Clock::now();
    for (u64 d = 0; d < kDrawsPerRun; ++d) {
        Crumb::Draw(slot, d, d >> 11, d, d, d);
        for (int u = 0; u < kUsesPerDraw; ++u) {
            const StepId id = StepId(u % kStepCount);
            if (Steps::Get(id) != Mode::Off) {
                Steps::Used(slot, id);
                sink += Steps::Sample(slot, id);
            }
        }
    }
    const double ns = Ns(Clock::now() - t0) / kDrawsPerRun;
    std::printf("%s", sink == ~0ull ? " " : "");
    return ns;
}

double ColdRun() {
    ThreadSlot& slot = Steps::Slot();
    std::vector<unsigned char> memory(kColdBytes, 1);
    u64 sink = 0;
    const auto t0 = Clock::now();
    for (u64 d = 0; d < kColdDraws; ++d) Touch(memory, d);
    const auto t1 = Clock::now();
    for (u64 d = 0; d < kColdDraws; ++d) {
        Touch(memory, d);
        ColdDraw(slot, 2 * d);
        for (int u = 0; u < kUsesPerDraw; ++u) sink += ColdUse(slot, StepId(u % kStepCount));
        ColdDraw(slot, 2 * d + 1);
    }
    const auto t2 = Clock::now();
    std::printf("%s", sink == ~0ull ? " " : "");
    return (Ns(t2 - t1) - Ns(t1 - t0)) / kColdDraws;
}

double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
}

int main() {
    SwitchInput input;
    std::vector<std::pair<const char*, const char*>> env;
    for (const StepInfo& s : kSteps) {
        if (Row(s.sw).kind == SwitchKind::Mode) env.emplace_back(Row(s.sw).name.data(), "1");
    }
    input.env = [](const char* name, void* context) -> const char* {
        for (const auto& [n, v] : *static_cast<std::vector<std::pair<const char*, const char*>>*>(context)) {
            if (std::string_view(n) == name) return v;
        }
        return nullptr;
    };
    input.context = &env;
    ReloadSwitches(input);
    Steps::ResetForTest();
    Steps::Start(Switches::Get());
    Steps::SetTicksPerSecond(1e9);
    Steps::OnFlip({1'000'000'000ull, false, 16'666'667ull});
    Steps::OnFlip({7'000'000'000ull, false, 16'666'667ull});

    double warm = 0;
    std::thread([&] {
        std::vector<double> runs;
        for (int r = 0; r < kRuns; ++r) runs.push_back(WarmRun());
        warm = Median(runs);
    }).join();
    std::printf("steps-bench: warm (inlined, 1 thread, %d uses + 1 Crumb::Draw per draw, steady masks): %.2f ns/draw "
                "(median of %d runs of %d draws)\n",
                kUsesPerDraw, warm, kRuns, kDrawsPerRun);

    std::vector<double> results(kColdThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kColdThreads; ++t) {
        threads.emplace_back([&results, t] {
            std::vector<double> runs;
            for (int r = 0; r < kRuns; ++r) runs.push_back(ColdRun());
            results[t] = Median(runs);
        });
    }
    for (auto& t : threads) t.join();
    double worst = 0;
    for (int t = 0; t < kColdThreads; ++t) {
        std::printf("steps-bench: cold (noinline, 64 KiB touched between draws, %d uses + 2 Crumb::Draw per draw), "
                    "thread %d: %.2f ns/draw (median of %d runs of %d draws)\n",
                    kUsesPerDraw, t, results[t], kRuns, kColdDraws);
        worst = std::max(worst, results[t]);
    }
    std::printf("steps-bench: cold gate %.0f ns per draw per thread: %s (worst thread %.2f ns)\n", kGateNs,
                worst <= kGateNs ? "within" : "OVER", worst);
    return 0;
}
