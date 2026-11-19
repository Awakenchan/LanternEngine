// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "bbport_platform.h"
#include "video_core/renderer_vulkan/pipeline_compile_queue.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/texture_cache/watched_pages.h"

using namespace std::chrono_literals;

namespace {

void TestJobQueue() {
    Vulkan::PipelineJobQueue queue;
    int a = 0, b = 0, c = 0;
    std::vector<char> order;
    queue.Push(&a, [&] { order.push_back('a'); });
    queue.Push(&b, [&] { order.push_back('b'); });
    queue.Push(&c, [&] { order.push_back('c'); });
    assert(queue.Size() == 3);
    assert(queue.Unfinished() == 3);
    auto job = queue.Take(&b);
    assert(job);
    assert(queue.Unfinished() == 3);
    job();
    assert(queue.Unfinished() == 2);
    assert(!queue.Take(&b));
    assert(queue.Size() == 2);
    auto popped = queue.Pop();
    assert(queue.Unfinished() == 2);
    popped();
    queue.Pop()();
    assert(queue.Unfinished() == 0);
    assert((order == std::vector<char>{'b', 'a', 'c'}));
    assert(!queue.Take(&a) && queue.Size() == 0);

    constexpr int Jobs = 2000;
    std::vector<int> owners(Jobs);
    std::vector<std::atomic<int>> runs(Jobs);
    for (int i = 0; i < Jobs; ++i) {
        queue.Push(&owners[i], [&, i] {
            runs[i].fetch_add(1);
            std::this_thread::yield();
        });
    }
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        while (!stop.load()) {
            queue.Pop()();
        }
    });
    for (int i = Jobs - 1; i >= 0; i -= 3) {
        if (auto j = queue.Take(&owners[i])) {
            j();
        }
    }
    queue.Push(nullptr, [&] { stop.store(true); });
    worker.join();
    assert(queue.Size() == 0);
    assert(queue.Unfinished() == 0);
    for (int i = 0; i < Jobs; ++i) {
        assert(runs[i].load() == 1);
    }
}

void TestSkipBudget() {
    using Vulkan::WithinSkipBudget;
    constexpr std::uint64_t Ms = 1'000'000;
    std::uint64_t first = 0;
    assert(WithinSkipBudget(first, 5 * Ms, 1000 * Ms) && first == 5 * Ms);
    assert(WithinSkipBudget(first, 500 * Ms, 1000 * Ms) && first == 5 * Ms);
    assert(WithinSkipBudget(first, 1004 * Ms, 1000 * Ms));
    assert(!WithinSkipBudget(first, 1005 * Ms, 1000 * Ms));
    assert(!WithinSkipBudget(first, 9000 * Ms, 1000 * Ms));
    std::uint64_t never = 0;
    assert(!WithinSkipBudget(never, 7 * Ms, 0));
    std::uint64_t at_zero = 0;
    assert(WithinSkipBudget(at_zero, 0, 10) && at_zero != 0);
}

void TestWatchedPages() {
    auto pages = std::make_unique<VideoCore::WatchedPages>();
    constexpr std::uint64_t P = 4096;
    const std::uint64_t base = 0x2'4000'0000ull;
    assert(!pages->Any(base, 8));
    const std::uint64_t begin = base + 0x800, end = base + 2 * P + 0x100;
    pages->Add(begin, end - begin, 1);
    assert(pages->Any(base, 8) && pages->Any(base + P, 8) && pages->Any(base + 2 * P + 0xff0, 8));
    assert(!pages->Any(base + 3 * P, 8) && !pages->Any(base - 8, 8));
    pages->Add(begin, base + P - begin, -1);
    assert(!pages->Any(base + 0x900, 8) && pages->Any(base + P, 8));
    pages->Add(base + 2 * P, end - (base + 2 * P), -1);
    assert(!pages->Any(base + 2 * P + 8, 8) && pages->Any(base + P + 8, 8));
    pages->Add(base + P + 0x200, 0x100, 1);
    pages->Add(base + P, P, -1);
    assert(pages->Any(base + P, 8));
    pages->Add(base + P + 0x200, 0x100, -1);
    assert(!pages->Any(base, 3 * P));
    pages->Add(base + 5 * P, 0, -1);
    assert(!pages->Any(base + 5 * P, 1));
    assert(pages->Any(std::uint64_t{1} << 41, 8));
    pages->Add(base + 9 * P, P, -1);
    assert(!pages->Any(base + 9 * P, 8));
}

std::atomic<int> long_packets{0};
std::atomic<std::uint64_t> handled{0};
void Handler(void*, const u8* packet, u32 size) {
    u32 sleep_ms = 0;
    std::memcpy(&sleep_ms, packet, sizeof(sleep_ms));
    if (sleep_ms) {
        long_packets.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    assert(size == sizeof(u32));
    handled.fetch_add(1, std::memory_order_relaxed);
}

void Post(Vulkan::DrawPipe& pipe, u32 sleep_ms) {
    u8* out = pipe.Begin(sizeof(u32));
    std::memcpy(out, &sleep_ms, sizeof(sleep_ms));
    pipe.Commit(sizeof(u32));
}

std::uint64_t ThreadCpuUs() {
    BbPlatform::Usage usage;
    return BbPlatform::GetUsage(true, usage) ? usage.user_us + usage.sys_us : 0;
}

void TestDrawPipeWaits() {
    Vulkan::DrawPipe pipe(&Handler, nullptr);
    Post(pipe, 0);
    Post(pipe, 300);
    const std::uint64_t cpu_before = ThreadCpuUs();
    const auto start = std::chrono::steady_clock::now();
    pipe.Drain();
    const auto waited = std::chrono::steady_clock::now() - start;
    const std::uint64_t cpu_us = ThreadCpuUs() - cpu_before;
    assert(pipe.Idle() && handled.load() == 2);
    assert(waited >= 250ms);
    std::printf("drain behind a 300 ms packet: waited %lld ms, %llu us on the CPU, %llu sleeps\n",
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()),
                static_cast<unsigned long long>(cpu_us),
                static_cast<unsigned long long>(pipe.sleeps));
    if (Vulkan::DrawPipe::SleepEnabled()) {
        assert(pipe.sleeps >= 1);
        assert(cpu_us < 60'000);
    }

    const u64 number = pipe.NextPacket() + 1;
    Post(pipe, 20);
    Post(pipe, 20);
    Post(pipe, 200);
    pipe.WaitForPacket(number, Vulkan::DrawPipe::ReasonConstRam);
    assert(pipe.ReachedPacket(number));
    pipe.Drain();

    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        std::uint64_t last = handled.load();
        for (int quiet = 0; !finished.load(); std::this_thread::sleep_for(100ms)) {
            const std::uint64_t now = handled.load();
            quiet = now == last ? quiet + 1 : 0;
            last = now;
            if (quiet > 100) {
                std::fprintf(stderr, "draw pipe wait hung (lost wake-up)\n");
                std::abort();
            }
        }
    });
    std::uint64_t rng = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 20000; ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        Post(pipe, (rng & 1023) == 0 ? 2 : 0);
        switch (rng >> 61) {
        case 0:
            pipe.Drain();
            assert(pipe.Idle());
            break;
        case 1:
            pipe.WaitForPacket(pipe.NextPacket() - 1, Vulkan::DrawPipe::ReasonRasterizer);
            break;
        default:
            break;
        }
    }
    pipe.Drain();
    finished.store(true);
    watchdog.join();
    assert(pipe.Idle() && handled.load() == pipe.packets);
    std::printf("%llu packets, %llu drains that waited, %llu slept, %d long packets\n",
                static_cast<unsigned long long>(pipe.packets),
                static_cast<unsigned long long>(pipe.drains),
                static_cast<unsigned long long>(pipe.sleeps), long_packets.load());
}

}

int main() {
    TestJobQueue();
    TestSkipBudget();
    TestWatchedPages();
    TestDrawPipeWaits();
    std::printf("pipeline compile tests passed\n");
    return 0;
}
