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
#include "lantern_frame_clock.h"
#include "lantern_frame_gen_cap.h"
#include "lantern_frame_gen_pacing.h"
#include "lantern_frame_report.h"
#include "lantern_meter.h"
#include "lantern_pacing_stats.h"
#include "lantern_pacing.h"
#include "lantern_vblank_watch.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::VideoOut {
namespace {

constexpr s32 VideoHandle = 1;
constexpr auto kLowLatencyQueueWait = std::chrono::milliseconds(100);
constexpr s32 ErrorHandle = s32(0x8029000B);

static_assert(sizeof(BufferAttribute) == sizeof(SysVideoOutAttribute));
static_assert(sizeof(BufferAttributeGroup) == sizeof(SysVideoOutGroup));

struct FlipRequest {
    Vulkan::Frame* frame = nullptr;
    s64 argument = 0;
    s32 index = -1;
    bool from_gpu = false;
    std::size_t queued = 0;

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
            std::unique_lock lock{request_mutex};
            if (BbSettings::LowLatency()) {
                request_taken.wait_for(lock, kLowLatencyQueueWait, [this] { return requests.empty(); });
            }
            requests.push({frame, argument, index, from_gpu});
        }
        request_ready.notify_one();
    }

private:
    FlipRequest TakeRequest() {
        FlipRequest request;
        {
            std::scoped_lock lock{request_mutex};
            if (requests.empty()) {
                return {};
            }
            request = requests.front();
            request.queued = requests.size();
            requests.pop();
        }
        request_taken.notify_all();
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
        LanternPacingStats::PresentTaken(request.queued);
        BbFreeze::NoteFlip();
        presenter->NoteFlipQueued(1);
        const bool done_after_present = BbSettings::LowLatency();
        RunOnSwapThread([frame = request.frame, done_after_present, index = request.index, argument = request.argument,
                         from_gpu = request.from_gpu] {
            presenter->NoteFlipQueued(-1);
            presenter->SetHDR(false);
            presenter->Present(frame);
            LanternPacingStats::Presented();
            if (done_after_present) {
                sys_video_out_flip_done(index, argument, from_gpu);
            }
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
        if (!done_after_present) {
            sys_video_out_flip_done(request.index, request.argument, request.from_gpu);
        }
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

    static Vulkan::PresentPolicy::PresentMode PacingPresentMode() {
        return presenter ? presenter->PacingPresentMode()
                         : Vulkan::PresentPolicy::PresentModeFromName(EmulatorSettings.GetPresentMode());
    }

    u32 LimitFromSettings() {
        using Vulkan::PresentPolicy::PresentMode;
        u32 limit = EmulatorSettings.GetFrameLimit();
        const PresentMode mode = PacingPresentMode();
        const u32 refresh = Frontend::DisplayRefreshHz();
        if (EmulatorSettings.IsUncappedVblank() && mode != PresentMode::Immediate) {
            const char* keep = std::getenv("BB_CAP_TO_REFRESH");
            if (!(keep && keep[0] == '0') && refresh >= 30 && (limit == 0 || limit > refresh)) {
                std::printf("VideoOut: frame limit %u FPS lowered to the display's %u Hz "
                            "(BB_CAP_TO_REFRESH=0 keeps it)\n",
                            limit, refresh);
                limit = refresh;
            }
        } else if (refresh >= 30 && limit > refresh + 1 && mode == PresentMode::Mailbox) {
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
        if (!settings.frame_gen_active.load(std::memory_order_relaxed)) {
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
        const bool fsr3 = settings.frame_gen.load(std::memory_order_relaxed) == BbSettings::FrameGenFsr3;
        const u32 refresh = Frontend::DisplayRefreshHz();
        const u32 limit =
            !variable_timestep
                ? LanternFrameGenCap::FixedRateLimit(base, true, delivered, refresh)
                : LanternFrameGenCap::Limit(
                      base, true, delivered, refresh,
                      LanternFrameGenPacing::CapToRefresh(settings.frame_gen_cap.load(std::memory_order_relaxed), fsr3));
        if (fsr3) {
            NoteFrameGenLimit(base, limit, delivered, refresh, settings.frame_gen_game_fps.load(std::memory_order_relaxed));
        }
        return limit;
    }

    static void NoteFrameGenLimit(u32 base, u32 limit, int delivered, u32 refresh, float game_fps) {
        static unsigned logged = 0;
        const unsigned notes = LanternFrameGenPacing::Notes(base, limit, delivered, refresh, game_fps);
        BbSettings::Get().frame_gen_notes.store(notes, std::memory_order_relaxed);
        const unsigned fresh = notes & ~logged;
        if (!fresh) {
            return;
        }
        logged |= fresh;
        if (fresh & LanternFrameGenPacing::NoteHeld) {
            std::printf("Frame generation: the %u FPS frame rate is above half the display's %u Hz: the game runs at "
                        "%u FPS so every generated frame can be shown\n",
                        base, refresh, limit);
        }
        if (fresh & LanternFrameGenPacing::NoteUneven) {
            std::printf("Frame generation: %u game and %u generated frames a second do not divide the display's %u Hz "
                        "evenly: some frames stay on screen longer (%u FPS, half the refresh, paces evenly)\n",
                        limit, limit, refresh, refresh / 2);
        }
        if (fresh & LanternFrameGenPacing::NoteUnshowable) {
            std::printf("Frame generation: the display's %u Hz cannot show every generated frame (game frame limit "
                        "%u): some generated frames are never shown\n",
                        refresh, limit);
        }
        if (fresh & LanternFrameGenPacing::NoteLowReal) {
            std::printf("Frame generation: the game runs at %.0f FPS (limit %u): generated frames from under %u real "
                        "frames a second look and feel worse\n",
                        double(game_fps), limit, LanternFrameGenPacing::kLowRealFps);
        }
        std::fflush(stdout);
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
        const bool lock_allowed =
            LanternPacing::VblankPacing() && PacingPresentMode() != Vulkan::PresentPolicy::PresentMode::Immediate;
        bool locked = false;
        std::chrono::nanoseconds locked_vblank{0};
        int lock_reports = 0;
        bool lock_outcome_owed = false;
        LanternFrameClock::SetWaiter([](std::int64_t deadline) {
            LanternPacing::WaitUntil(Clock::time_point(std::chrono::nanoseconds(deadline)), true);
        });
        const auto publish = [&] {
            if (!limit) {
                LanternFrameClock::Clear();
                return;
            }
            LanternFrameClock::Publish(
                std::chrono::duration_cast<std::chrono::nanoseconds>(flips.Slot().time_since_epoch()).count(),
                flips.Period().count(), locked ? locked_vblank.count() : 0);
        };

        const auto lock_to_display = [&] {
            std::optional<LanternPacing::VblankTime> vblank;
            const bool frame_gen_active = BbSettings::Get().frame_gen_active.load(std::memory_order_relaxed);
            const bool frame_gen_fsr3 =
                BbSettings::Get().frame_gen.load(std::memory_order_relaxed) == BbSettings::FrameGenFsr3;
            if (LanternFrameGenPacing::LockWanted(lock_allowed, limit, frame_gen_active, frame_gen_fsr3)) {
                vblank = LanternPacing::Vblank();
            }
            u32 refreshes = 0;
            double hz = 0;
            if (vblank && vblank->period.count() > 0) {
                hz = 1e9 / double(vblank->period.count());
                refreshes = LanternVblankWatch::LockRefreshes(hz, Frontend::DisplayRefreshHz(), limit);
            }
            if (((refreshes != 0) != locked && lock_reports < 8) || lock_outcome_owed) {
                lock_reports += lock_outcome_owed ? 0 : 1;
                lock_outcome_owed = false;
                if (refreshes) {
                    std::printf("VideoOut: flips locked to the display's vblanks (%.3f Hz, %u refresh%s a frame)\n", hz,
                                refreshes, refreshes == 1 ? "" : "es");
                } else {
                    std::printf("VideoOut: flips keep their own clock (%s)\n",
                                LanternFrameGenPacing::UnlockedReason(frame_gen_active, frame_gen_fsr3,
                                                                      vblank.has_value()));
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
            locked_vblank = vblank->period;
            flips.SetPeriod(vblank->period * refreshes);
            const auto offset = std::min<std::chrono::nanoseconds>(flips.Period() / 16 + std::chrono::microseconds(300),
                                                                   vblank->period / 2);
            flips.Align(vblank->last, vblank->period, offset);
        };

        const auto take = [&](const FlipRequest& request, Clock::time_point now) {
            flips.Taken(now);
            lock_to_display();
            publish();
            Flip(request);
        };

        publish();

        while (!token.stop_requested()) {
            const u32 wanted = LimitWithFrameGeneration(base_limit);
            if (wanted != limit) {
                std::printf("VideoOut: frame limit %u FPS%s\n", wanted,
                            wanted != base_limit ? " (frame generation: the delivered multiplier's share of the frame "
                                                   "rate or of the display's refresh)"
                                                 : "");
                limit = wanted;
                Lantern::Meter::SetFlipCap(limit);
                flips.SetPeriod(pace());
                lock_outcome_owed = lock_allowed && limit != 0;
                publish();
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
                NoteHold(flips.Slot() - flips.Period() / 16);
                LanternPacing::WaitUntil(flips.Slot() - flips.Period() / 16, true, max_spin);
                if (const FlipRequest request = TakeRequest()) {
                    take(request, Clock::now());
                }
            }
            LanternPacing::WaitUntil(next_tick, !limit, max_spin);
            ticks.Tick(Clock::now());
        }
        LanternFrameClock::Clear();
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
                Lantern::Meter::SetFlipCap(limit);
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
            bool held = false;
            while (!token.stop_requested()) {
                {
                    std::unique_lock lock{request_mutex};
                    if (!request_ready.wait_until(lock, next_tick, [this] { return !requests.empty(); })) {
                        break;
                    }
                }
                if (std::chrono::steady_clock::now() < next_flip) {
                    if (next_flip >= next_tick) {
                        held = true;
                        break;
                    }
                    NoteHold(next_flip);
                    std::this_thread::sleep_until(next_flip);
                }
                const auto woke = std::chrono::steady_clock::now();
                if (const FlipRequest request = TakeRequest()) {
                    next_flip = std::max(next_flip + period, woke - period);
                    Flip(request);
                }
            }
            if (held) {
                NoteHold(next_tick);
            }
            std::this_thread::sleep_until(next_tick);
        }
    }

    template <typename TimePoint> static void NoteHold(TimePoint until) {
        const auto now = std::chrono::steady_clock::now();
        if (until > now) {
            Lantern::Meter::NoteFlipHold(
                u64(std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count()));
        }
    }

    void FlipLoop(std::stop_token token) {
        const std::chrono::nanoseconds vblank_period(1000000000 / EmulatorSettings.GetVblankFrequency());
        Common::SetCurrentThreadName("bb:VideoOut");
        Common::SetCurrentThreadRealtime(vblank_period);
        const u32 limit = LimitFromSettings();
        Lantern::Meter::SetFlipCap(limit);
        std::printf("VideoOut: game vblank %u Hz, frame limit %u FPS, precise pacing %s\n", EmulatorSettings.GetVblankFrequency(),
                    limit, LanternPacing::Enabled() ? "on" : "off");
        if (BbSettings::LowLatency()) {
            std::printf("VideoOut: low latency on (BB_LOW_LATENCY): one flip queued at most, the flip is reported done "
                        "after its present, no submissions ahead of the previous frame\n");
        }
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
    std::condition_variable request_taken;
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
