// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string_view>
#include <thread>

#include "bbport_freeze.h"
#include "bbport_settings.h"
#include "common/assert.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/videoout/driver.h"
#include "core/platform.h"
#include "imgui/renderer/imgui_core.h"
#include "lantern_frame_gen_cap.h"
#include "lantern_frame_report.h"
#include "lantern_pacing.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::VideoOut {
namespace {

constexpr s32 VideoHandle = 1;
constexpr s32 ErrorHandle = s32(0x8029000B);

static_assert(sizeof(BufferAttribute) == sizeof(SysVideoOutAttribute));
static_assert(sizeof(BufferAttributeGroup) == sizeof(SysVideoOutGroup));

struct FlipRequest {
    Vulkan::Frame* frame = nullptr;
    s64 argument = 0;
    s32 index = -1;
    bool from_gpu = false;

    explicit operator bool() const noexcept {
        return frame != nullptr;
    }
};

class Presentation {
public:
    Presentation() {
        const char* separate = std::getenv("BB_PRESENT_THREAD");
        separate_swap = !(separate && separate[0] == '0');
        if (separate_swap) {
            swap_thread = std::jthread([this](std::stop_token token) { SwapLoop(token); });
        }
        flip_thread = std::jthread([this](std::stop_token token) { FlipLoop(token); });
    }

    VideoOutPort port;

    void Prepare(s32 index, s64 argument, bool from_gpu) {
        Vulkan::Frame* frame;
        if (index == -1) {
            frame = presenter->PrepareBlankFrame(false);
        } else {
            const SysVideoOutGroup* group = nullptr;
            u64 address = 0;
            ASSERT_MSG(sys_video_out_buffer(index, &group, &address), "Flip of an unregistered buffer");
            frame = presenter->PrepareFrame(*reinterpret_cast<const BufferAttributeGroup*>(group), address);
        }
        {
            std::scoped_lock lock{request_mutex};
            requests.push({frame, argument, index, from_gpu});
        }
        request_ready.notify_one();
    }

private:
    FlipRequest TakeRequest() {
        std::scoped_lock lock{request_mutex};
        if (requests.empty()) {
            return {};
        }
        const FlipRequest request = requests.front();
        requests.pop();
        return request;
    }

    void RunOnSwapThread(std::function<void()> work, bool only_when_idle = false) {
        if (!separate_swap) {
            work();
            return;
        }
        {
            std::scoped_lock lock{swap_mutex};
            if (only_when_idle && (swap_busy || !swap_work.empty())) {
                return;
            }
            swap_work.push_back(std::move(work));
        }
        swap_ready.notify_one();
    }

    void SwapLoop(std::stop_token token) {
        Common::SetCurrentThreadName("bb:Present");
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock lock{swap_mutex};
                swap_busy = false;
                if (!swap_ready.wait(lock, token, [this] { return !swap_work.empty(); })) {
                    return;
                }
                work = std::move(swap_work.front());
                swap_work.pop_front();
                swap_busy = true;
            }
            work();
        }
    }

    void Flip(const FlipRequest& request) {
        BbFreeze::NoteFlip();
        presenter->NoteFlipQueued(1);
        RunOnSwapThread([frame = request.frame] {
            presenter->NoteFlipQueued(-1);
            presenter->SetHDR(false);
            presenter->Present(frame);
        });
        const SysVideoOutGroup* group = nullptr;
        u64 address = 0;
        Vulkan::FrameCapture::OnFlip(sys_video_out_buffer(request.index, &group, &address) ? address : 0);
        const u32 background_limit = Frontend::BackgroundFrameLimit();
        if (background_limit && Frontend::WindowInBackground()) {
            const auto due = last_flip + std::chrono::nanoseconds(1000000000 / background_limit);
            if (std::chrono::steady_clock::now() < due) {
                std::this_thread::sleep_until(due);
            }
        }
        last_flip = std::chrono::steady_clock::now();
        LanternFrameReport::OnFlip();
        sys_video_out_flip_done(request.index, request.argument, request.from_gpu);
    }

    void DrawIdleFrame() {
        if (!sys_video_out_is_open()) {
            RunOnSwapThread(
                [] {
                    Vulkan::Frame* frame = presenter->PrepareBlankFrame(true);
                    presenter->Present(frame, false, false);
                },
                true);
        } else if (ImGui::Core::MustKeepDrawing()) {
            RunOnSwapThread(
                [] {
                    if (Vulkan::Frame* frame = presenter->PrepareLastFrame()) {
                        presenter->Present(frame, true);
                    }
                },
                true);
        }
    }

    bool FlipSlotOpen() const {
        return vblanks % (u64(sys_video_out_flip_rate()) + 1) == 0;
    }

    u32 LimitFromSettings() {
        u32 limit = EmulatorSettings.GetFrameLimit();
        const std::string_view mode = EmulatorSettings.GetPresentMode();
        const u32 refresh = Frontend::DisplayRefreshHz();
        if (EmulatorSettings.IsUncappedVblank() && mode != "Immediate") {
            const char* keep = std::getenv("BB_CAP_TO_REFRESH");
            if (!(keep && keep[0] == '0') && refresh >= 30 && (limit == 0 || limit > refresh)) {
                std::printf("VideoOut: frame limit %u FPS lowered to the display's %u Hz "
                            "(BB_CAP_TO_REFRESH=0 keeps it)\n",
                            limit, refresh);
                limit = refresh;
            }
        } else if (refresh >= 30 && limit > refresh + 1 && mode == "Mailbox") {
            std::printf("VideoOut: the %u FPS preset is faster than the display's %u Hz: about %u%% of the "
                        "frames are rendered and never shown (a lower FPS preset saves that GPU work)\n",
                        limit, refresh, 100 - refresh * 100 / limit);
        }
        return limit;
    }

    static u32 LimitWithFrameGeneration(u32 base) {
        static const bool variable_timestep = [] {
            const char* fps = std::getenv("BB_FPS");
            return fps && std::string_view{fps} == "uncap";
        }();
        static int delivered = 0;
        const auto& settings = BbSettings::Get();
        if (!variable_timestep || !settings.frame_gen_active.load(std::memory_order_relaxed)) {
            delivered = 0;
            return base;
        }
        const int asked = std::clamp(settings.frame_gen_multiplier.load(std::memory_order_relaxed), 2,
                                     BbSettings::FrameGenFramesMax + 1);
        const bool dynamic = settings.frame_gen.load(std::memory_order_relaxed) == BbSettings::FrameGenDlss &&
                             settings.frame_gen_dynamic.load(std::memory_order_relaxed) &&
                             settings.dlss_fg_dynamic_supported.load(std::memory_order_relaxed) &&
                             settings.frame_gen_frames.load(std::memory_order_relaxed) > 1;
        delivered = LanternFrameGenCap::DeliveredMultiplier(
            asked, dynamic, settings.frame_gen_game_fps.load(std::memory_order_relaxed),
            settings.frame_gen_shown_fps.load(std::memory_order_relaxed), delivered);
        return LanternFrameGenCap::Limit(base, true, delivered, Frontend::DisplayRefreshHz(),
                                         settings.frame_gen_cap.load(std::memory_order_relaxed));
    }

    static std::chrono::nanoseconds PeriodOf(u32 limit) {
        return limit ? std::chrono::nanoseconds(1000000000 / limit) : std::chrono::nanoseconds(0);
    }

    void PreciseLoop(std::stop_token token, u32 base_limit, std::chrono::nanoseconds vblank_period) {
        using Clock = LanternPacing::Clock;
        u32 limit = base_limit;
        const auto pace = [&] { return limit ? PeriodOf(limit) : vblank_period; };
        LanternPacing::Cadence ticks{vblank_period, Clock::now() + vblank_period};
        LanternPacing::Cadence flips{pace()};
        const auto max_spin = std::min<std::chrono::nanoseconds>(std::chrono::microseconds(1500), vblank_period / 8);
        const bool lock_allowed = LanternPacing::VblankPacing() && EmulatorSettings.GetPresentMode() != "Immediate";
        bool locked = false;
        int lock_reports = 0;

        const auto lock_to_display = [&] {
            std::optional<LanternPacing::VblankTime> vblank;
            if (lock_allowed && limit && !BbSettings::Get().frame_gen_active.load(std::memory_order_relaxed)) {
                vblank = LanternPacing::Vblank();
            }
            u32 refreshes = 0;
            double hz = 0;
            if (vblank && vblank->period.count() > 0) {
                hz = 1e9 / double(vblank->period.count());
                const double nominal = Frontend::DisplayRefreshHz();
                const double ratio = hz / double(limit);
                const double whole = std::round(ratio);
                if (nominal > 0 && std::abs(hz - nominal) < nominal * 0.005 && whole >= 1 &&
                    std::abs(ratio - whole) < whole * 0.01) {
                    refreshes = u32(whole);
                }
            }
            if ((refreshes != 0) != locked && lock_reports < 8) {
                ++lock_reports;
                if (refreshes) {
                    std::printf("VideoOut: flips locked to the display's vblanks (%.3f Hz, %u refresh%s a frame)\n", hz,
                                refreshes, refreshes == 1 ? "" : "es");
                } else {
                    std::printf("VideoOut: flips keep their own clock (no steady vblanks to lock to)\n");
                }
            }
            if (!refreshes) {
                if (locked) {
                    flips.SetPeriod(pace());
                }
                locked = false;
                return;
            }
            locked = true;
            flips.SetPeriod(vblank->period * refreshes);
            const auto offset = std::min<std::chrono::nanoseconds>(flips.Period() / 16 + std::chrono::microseconds(300),
                                                                   vblank->period / 2);
            flips.Align(vblank->last, vblank->period, offset);
        };

        const auto take = [&](const FlipRequest& request, Clock::time_point now) {
            flips.Taken(now);
            lock_to_display();
            Flip(request);
        };

        while (!token.stop_requested()) {
            const u32 wanted = LimitWithFrameGeneration(base_limit);
            if (wanted != limit) {
                std::printf("VideoOut: frame limit %u FPS%s\n", wanted,
                            wanted != base_limit ? " (frame generation: the delivered multiplier's share)" : "");
                limit = wanted;
                flips.SetPeriod(pace());
            }
            const Clock::time_point next_tick = ticks.Slot();
            const auto now = Clock::now();
            if (flips.Ready(now) && FlipSlotOpen()) {
                if (const FlipRequest request = TakeRequest()) {
                    take(request, now);
                } else {
                    DrawIdleFrame();
                }
            }
            ++vblanks;
            while (sys_video_out_flip_rate() == 0 && !token.stop_requested()) {
                {
                    std::unique_lock lock{request_mutex};
                    if (!request_ready.wait_until(lock, next_tick - max_spin, [this] { return !requests.empty(); })) {
                        break;
                    }
                }
                if (flips.Slot() - flips.Period() / 16 >= next_tick) {
                    break;
                }
                LanternPacing::WaitUntil(flips.Slot() - flips.Period() / 16, true, max_spin);
                if (const FlipRequest request = TakeRequest()) {
                    take(request, Clock::now());
                }
            }
            LanternPacing::WaitUntil(next_tick, !limit, max_spin);
            ticks.Tick(Clock::now());
        }
    }

    void TickLoop(std::stop_token token, u32 base_limit, std::chrono::nanoseconds vblank_period, bool immediate) {
        Common::AccurateTimer timer{vblank_period};
        u32 limit = base_limit;
        auto next_flip = std::chrono::steady_clock::now();
        while (!token.stop_requested()) {
            timer.Start();
            const auto next_tick = std::chrono::steady_clock::now() + vblank_period;
            if (immediate) {
                limit = LimitWithFrameGeneration(base_limit);
            }
            const auto period = PeriodOf(limit);
            const auto now = std::chrono::steady_clock::now();
            if ((!limit || now >= next_flip) && FlipSlotOpen()) {
                const FlipRequest request = TakeRequest();
                if (request) {
                    if (limit) {
                        next_flip = std::max(next_flip + period, now - period);
                    }
                    Flip(request);
                } else if (timer.GetTotalWait().count() < 0) {
                    DrawIdleFrame();
                }
            }
            ++vblanks;
            if (!immediate || sys_video_out_flip_rate() != 0) {
                timer.End();
                continue;
            }
            while (!token.stop_requested()) {
                {
                    std::unique_lock lock{request_mutex};
                    if (!request_ready.wait_until(lock, next_tick, [this] { return !requests.empty(); })) {
                        break;
                    }
                }
                if (std::chrono::steady_clock::now() < next_flip) {
                    if (next_flip >= next_tick) {
                        break;
                    }
                    std::this_thread::sleep_until(next_flip);
                }
                const auto woke = std::chrono::steady_clock::now();
                if (const FlipRequest request = TakeRequest()) {
                    next_flip = std::max(next_flip + period, woke - period);
                    Flip(request);
                }
            }
            std::this_thread::sleep_until(next_tick);
        }
    }

    void FlipLoop(std::stop_token token) {
        const std::chrono::nanoseconds vblank_period(1000000000 / EmulatorSettings.GetVblankFrequency());
        Common::SetCurrentThreadName("bb:VideoOut");
        Common::SetCurrentThreadRealtime(vblank_period);
        const u32 limit = LimitFromSettings();
        std::printf("VideoOut: vblank %u Hz, frame limit %u FPS, precise pacing %s\n", EmulatorSettings.GetVblankFrequency(),
                    limit, LanternPacing::Enabled() ? "on" : "off");
        const bool immediate = limit != 0 || EmulatorSettings.IsUncappedVblank();
        if (immediate && LanternPacing::Enabled()) {
            PreciseLoop(token, limit, vblank_period);
        } else {
            TickLoop(token, limit, vblank_period, immediate);
        }
    }

    std::mutex swap_mutex;
    std::condition_variable_any swap_ready;
    std::deque<std::function<void()>> swap_work;
    bool swap_busy = false;
    bool separate_swap = true;
    std::jthread swap_thread;
    std::mutex request_mutex;
    std::condition_variable request_ready;
    std::queue<FlipRequest> requests;
    std::chrono::steady_clock::time_point last_flip = std::chrono::steady_clock::now();
    u64 vblanks = 0;
    std::jthread flip_thread;
};

Presentation* presentation;

const SysVideoOutHost host = {
    [] { liverpool->SetVoPort(&presentation->port); },
    [](const SysVideoOutGroup* group, u64 address) {
        Vulkan::FrameCapture::AddDisplayBuffer(address);
        presenter->RegisterVideoOutSurface(*reinterpret_cast<const BufferAttributeGroup*>(group), address);
    },
    [](s32 index, s64 argument, int from_gpu) {
        if (from_gpu) {
            presentation->Prepare(index, argument, true);
        } else {
            liverpool->SendCommand([=] { presentation->Prepare(index, argument, false); });
        }
    },
    [] { presentation->port.SignalVoLabel(); },
};

} // namespace

s32 SubmitEopFlip(s32 handle, u32 buffer, s64 argument) {
    if (handle != VideoHandle) {
        return ErrorHandle;
    }
    Platform::IrqC::Instance()->RegisterOnce(Platform::InterruptId::GfxFlip, [=](Platform::InterruptId) {
        volatile const u64& label = sys_video_out_labels()[buffer];
        if (label != 1) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            while (label != 1 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            if (label != 1) {
                static std::atomic<u32> reported{0};
                if (reported.fetch_add(1, std::memory_order_relaxed) < 5) {
                    std::printf("VideoOut: flip IRQ for buffer %u before its label was set; flipping anyway\n", buffer);
                }
            }
        }
        sys_video_out_queue_flip(s32(buffer), argument, 1);
    });
    return 0;
}

s32 BufferLabelAddress(s32 handle, uintptr_t* address) {
    if (handle != VideoHandle) {
        return ErrorHandle;
    }
    *address = reinterpret_cast<uintptr_t>(sys_video_out_labels());
    return 16;
}

void Start() {
    presentation = new Presentation();
    sys_video_out_set_host(&host);
}

} // namespace Libraries::VideoOut
