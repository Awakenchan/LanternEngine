// SPDX-License-Identifier: GPL-2.0-or-later
#include "lantern_pacing.h"
#include "lantern_vblank_watch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <immintrin.h>
#ifdef _WIN32
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <time.h>
#endif

namespace LanternPacing {
namespace {
using namespace std::chrono_literals;

struct Lateness {
    static constexpr std::size_t Size = 32;
    std::array<std::atomic<std::uint32_t>, Size> us{};
    std::atomic<std::uint32_t> next{0};

    Lateness() {
        for (auto& v : us) {
            v.store(300, std::memory_order_relaxed);
        }
    }
    void Add(std::chrono::nanoseconds late) {
        const auto value = std::clamp<std::int64_t>(late.count() / 1000, 0, 4000);
        us[next.fetch_add(1, std::memory_order_relaxed) % Size].store(
            static_cast<std::uint32_t>(value), std::memory_order_relaxed);
    }
    std::chrono::nanoseconds Margin() const {
        std::array<std::uint32_t, Size> copy;
        for (std::size_t i = 0; i < Size; ++i) {
            copy[i] = us[i].load(std::memory_order_relaxed);
        }
        constexpr std::size_t P90 = Size * 9 / 10;
        std::nth_element(copy.begin(), copy.begin() + P90, copy.end());
        return std::chrono::microseconds(copy[P90] + 50);
    }
};
Lateness lateness;

#ifdef _WIN32
HANDLE ThreadTimer() {
    thread_local const HANDLE timer = [] {
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
        return t ? t : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }();
    return timer;
}
#endif
}

bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("BB_PRECISE_PACING");
        if (env && env[0]) {
            return !(env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' ||
                     env[0] == 'F');
        }
#ifdef _WIN32
        return true;
#else
        return false;
#endif
    }();
    return on;
}

bool Sleep(std::chrono::nanoseconds duration, bool alertable) {
    if (duration <= 0ns) {
        std::this_thread::yield();
        return true;
    }
#ifdef _WIN32
    if (const HANDLE timer = ThreadTimer()) {
        LARGE_INTEGER due{.QuadPart = -std::max<LONGLONG>(1, duration.count() / 100)};
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            return WaitForSingleObjectEx(timer, INFINITE, alertable ? TRUE : FALSE) ==
                   WAIT_OBJECT_0;
        }
    }
    return SleepEx(static_cast<DWORD>(std::max<std::int64_t>(1, duration.count() / 1'000'000)),
                   alertable ? TRUE : FALSE) == 0;
#else
    timespec ts{static_cast<time_t>(duration.count() / 1'000'000'000),
                static_cast<long>(duration.count() % 1'000'000'000)};
    return nanosleep(&ts, nullptr) == 0;
#endif
}

namespace {
struct VblankWatch {
    std::mutex mutex;
    std::wstring device;
    unsigned generation = 0;
    bool started = false;
    std::atomic<std::int64_t> last_ns{0}, period_ns{0};
    std::atomic<bool> steady{false};
};
VblankWatch& Watch() {
    static VblankWatch watch;
    return watch;
}

#ifdef _WIN32
using KmtHandle = UINT;
struct KmtOpenAdapterFromHdc {
    HDC hdc;
    KmtHandle adapter;
    LUID luid;
    UINT source;
};
struct KmtWaitForVblank {
    KmtHandle adapter;
    KmtHandle device;
    UINT source;
};
struct KmtCloseAdapter {
    KmtHandle adapter;
};
using KmtOpen = LONG(APIENTRY*)(KmtOpenAdapterFromHdc*);
using KmtWait = LONG(APIENTRY*)(const KmtWaitForVblank*);
using KmtClose = LONG(APIENTRY*)(const KmtCloseAdapter*);

void WatchVblanks() {
    const HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    const auto open = gdi ? reinterpret_cast<KmtOpen>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc")) : nullptr;
    const auto wait = gdi ? reinterpret_cast<KmtWait>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")) : nullptr;
    const auto close = gdi ? reinterpret_cast<KmtClose>(GetProcAddress(gdi, "D3DKMTCloseAdapter")) : nullptr;
    if (!open || !wait || !close) {
        std::printf("VideoOut: the display's vblanks cannot be followed (no D3DKMT)\n");
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    auto& w = Watch();
    std::wstring device;
    unsigned generation = ~0u;
    KmtWaitForVblank target{};
    bool opened = false;
    LanternVblankWatch::Learner learner;
    int failures = 0, refresh_reports = 0;
    const auto close_adapter = [&] {
        if (opened) {
            const KmtCloseAdapter c{target.adapter};
            close(&c);
            opened = false;
        }
    };
    for (;;) {
        {
            std::scoped_lock lock{w.mutex};
            if (w.generation != generation) {
                generation = w.generation;
                device = w.device;
                close_adapter();
                learner.Reset();
            }
        }
        if (!opened) {
            w.steady.store(false, std::memory_order_relaxed);
            learner.Restart(0);
            HDC dc = CreateDCW(nullptr, device.c_str(), nullptr, nullptr);
            if (!dc) {
                dc = CreateDCW(device.c_str(), nullptr, nullptr, nullptr);
            }
            KmtOpenAdapterFromHdc o{dc, 0, {}, 0};
            opened = dc && open(&o) == 0;
            if (dc) {
                DeleteDC(dc);
            }
            if (!opened) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            target = {o.adapter, 0, o.source};
        }
        if (wait(&target) != 0) {
            close_adapter();
            if (++failures == 20) {
                std::printf("VideoOut: the display's vblanks cannot be followed (the wait fails)\n");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(failures < 20 ? 50 : 1000));
            continue;
        }
        failures = 0;
        const double now = double(Clock::now().time_since_epoch().count());
        auto event = learner.Wake(now);
        if (event != LanternVblankWatch::Event::None && refresh_reports < 8) {
            ++refresh_reports;
        } else {
            event = LanternVblankWatch::Event::None;
        }
        if (event == LanternVblankWatch::Event::Varies) {
            std::printf("VideoOut: the display's refresh varies (G-Sync, FreeSync or a mode "
                        "change): flips are not locked to its vblanks\n");
        } else if (event == LanternVblankWatch::Event::SteadyAgain) {
            std::printf("VideoOut: the display's refresh is steady again (%.3f Hz)\n", 1e9 / learner.period);
        }
        if (!learner.measured) {
            continue;
        }
        w.period_ns.store(std::int64_t(learner.period), std::memory_order_relaxed);
        w.last_ns.store(std::int64_t(learner.predicted), std::memory_order_relaxed);
        w.steady.store(learner.published, std::memory_order_relaxed);
    }
}
#endif
}

bool VblankPacing() {
    static const bool on = [] {
        const char* env = std::getenv("BB_VBLANK_PACING");
        return Enabled() && !(env && env[0] == '0');
    }();
    return on;
}

void WatchVblank(const wchar_t* device) {
#ifdef _WIN32
    if (!VblankPacing() || !device || !device[0]) {
        return;
    }
    auto& w = Watch();
    std::scoped_lock lock{w.mutex};
    w.device = device;
    ++w.generation;
    if (!w.started) {
        w.started = true;
        std::thread(WatchVblanks).detach();
    }
#else
    (void)device;
#endif
}

std::optional<VblankTime> Vblank() {
    const auto& w = Watch();
    if (!w.steady.load(std::memory_order_relaxed)) {
        return std::nullopt;
    }
    return VblankTime{Clock::time_point(Clock::duration(w.last_ns.load(std::memory_order_relaxed))),
                      std::chrono::nanoseconds(w.period_ns.load(std::memory_order_relaxed))};
}

void WaitUntil(Clock::time_point deadline, bool exact, std::chrono::nanoseconds max_spin) {
    const auto margin =
        exact ? std::clamp<std::chrono::nanoseconds>(lateness.Margin(), 100us,
                                                     std::max<std::chrono::nanoseconds>(max_spin, 100us))
              : 0ns;
    const auto start = Clock::now();
    if (const auto left = deadline - start; left > margin) {
        const auto sleep = std::chrono::duration_cast<std::chrono::nanoseconds>(left - margin);
        if (Sleep(sleep)) {
            lateness.Add(Clock::now() - start - sleep);
        }
    }
    while (Clock::now() < deadline) {
        _mm_pause();
    }
}
}
