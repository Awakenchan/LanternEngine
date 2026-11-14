// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: frame generation (frame_gen in lantern.ini, BB_FRAME_GEN=fsr3). FSR 3.1 frame
// interpolation (the vendored FireBurn/FSR-Vulkan 3.1.6 C API) makes one frame between two of the
// game's own: 2x the frames shown, at the cost of holding each real frame back by half a frame.
//
// Where the pieces come from:
// - The upscaler (vk_temporal_upscaler.cpp), right after its camera motion pass: the scene depth
//   (copied to R32F), the RG16F motion vectors, the jitter and the camera. A frame the upscaler
//   did not run on (menus, movies, loading screens, the 3D previews) has none: no generation.
// - The upscaler too, before the first UI draw: a copy of the finished frame without the UI
//   (HUD-less). That is what is interpolated, so the UI does not smear with the motion.
// - The presenter (PrepareFrame): the final frame with the UI. A small compute pass
//   (frame_gen_compose.comp) takes it where it differs from the HUD-less copy (the UI is drawn
//   there) and the interpolated frame everywhere else; the result goes through the same post
//   process pass into a presentation frame of its own.
// - The swap thread (Presenter::Present): the generated frame first, then the real one half the
//   game's smoothed frame interval later. A generated frame with no free slot is dropped; the
//   game never waits for one.
//
// bbport: with DLSS Frame Generation (frame_gen=dlss, vk_streamline.h) the same captures are its
// inputs: Prepare hands out a job without an FSR context, the presenter copies the captures into
// the presentation frame's slot and DLSS-G (Streamline) generates and presents the extra frames.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

#include "common/types.h"
#include "lantern_frame_gen_pacing.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

struct FfxVkFsr3_3_1_6FrameGenerationContext;

namespace Vulkan {

class Instance;
class Scheduler;

class FrameGenerator {
public:
    using Clock = std::chrono::steady_clock;

    FrameGenerator(const Instance& instance, Scheduler& draw_scheduler);
    ~FrameGenerator();

    /// FSR 3.1 chosen in the menu or lantern.ini, not failed, and not paused by the overload guard.
    [[nodiscard]] bool Enabled() const;
    /// bbport: DLSS Frame Generation chosen, and Streamline runs it on this GPU (vk_streamline.h).
    [[nodiscard]] static bool DlssWanted();
    [[nodiscard]] static int SessionSetting();
    [[nodiscard]] static bool BuiltInWanted();
    [[nodiscard]] static bool FsrSdkWanted();
    /// The upscaler's captures are wanted: FSR 3.1 or DLSS Frame Generation.
    [[nodiscard]] bool Capturing() const {
        return Enabled() || DlssWanted() || FsrSdkWanted();
    }
    /// The swapchain should have one more image (Enabled, also while idle in menus).
    [[nodiscard]] bool WantsExtraImage() const {
        return Enabled();
    }

    // Draw thread (the upscaler).

    /// Start of a frame in the command stream: the inputs below belong to the next frame.
    void OnFrameStart();
    /// Right after the upscaler's motion pass: the scene depth (`depth_view`, depth aspect,
    /// General layout) and the motion vectors (`motion`, RG16F, General), render size `w` x `h`.
    /// `jitter` in render pixels with the upscaler's sign.
    void CaptureInputs(Scheduler& scheduler, vk::ImageView depth_view, vk::Image motion, u32 w,
                       u32 h, std::array<float, 2> jitter, float near_plane, float vertical_fov,
                       const CameraMotion::Matrices& camera);
    /// The upscaled inputs of this frame were captured (the HUD-less copy is wanted).
    [[nodiscard]] bool WantsHudless() const {
        return inputs_frame && !hudless_frame;
    }
    /// The finished frame before its UI: `image` (RGBA8, General layout), `w` x `h`.
    void CaptureHudless(Scheduler& scheduler, vk::Image image, vk::Format format, u32 w, u32 h);

    // Presenter.

    /// What PrepareFrame's recorded commands need to make this frame's generated one.
    struct Job {
        u64 frame_id = 0;  ///< FSR's frame id (from 1, consecutive while running)
        u64 tick = 0;      ///< the draw scheduler tick the commands go out in
        bool reset = false;
        bool present = false; ///< false on a reset: the generated frame only starts history
        u32 width = 0, height = 0;             ///< display (HUD-less, output) size
        u32 render_width = 0, render_height = 0;
        std::array<float, 2> jitter{};
        float near_plane = 0.1f, vertical_fov = 1.0f, frame_ms = 16.6f;
        vk::Format view_format{}; ///< the real frame's view format (RGBA8/BGRA8, Unorm or Srgb)
        /// bbport: a DLSS Frame Generation job (no FSR context; Record does nothing with it),
        /// with the camera of its frame for DLSS-G's constants.
        bool dlss = false;
        bool sdk = false;
        CameraMotion::Matrices camera{};
    };
    /// PrepareFrame (draw thread): a job when this frame can have a generated one; else the
    /// reason is logged on a change and the next one starts over (reset). May wait for the GPU
    /// when the resources are made again (a size change, the first frame).
    std::optional<Job> Prepare(vk::Extent2D display, vk::Format view_format, bool hdr,
                               bool other_scaler, u64 tick);
    /// Records interpolation and composition (a recording thread). `real` is the final frame's
    /// view (ShaderReadOnlyOptimal, the job's size). False: no generated frame this time.
    bool Record(vk::CommandBuffer cmdbuf, const Job& job, vk::ImageView real);
    /// The composed frame (RGBA16F, ShaderReadOnlyOptimal after Record), for the post process.
    [[nodiscard]] vk::ImageView ComposedView() const {
        return *composed_view;
    }
    /// Native+: FSR's interpolated frame, without the UI (General layout after Record), the
    /// generated frame's scene before its HUD.
    [[nodiscard]] vk::Image GeneratedImage() const {
        return vk::Image(generated_image);
    }
    /// bbport: the captures (General layout) for DLSS Frame Generation's copies: depth (R32F)
    /// and motion (RG16F) at render size, the frame without its UI (RGBA8) at display size, and
    /// a view of it in the real frame's encoding.
    [[nodiscard]] vk::Image DepthImage() const {
        return vk::Image(depth_image);
    }
    [[nodiscard]] vk::Image MotionImage() const {
        return vk::Image(motion_image);
    }
    [[nodiscard]] vk::Image HudlessImage() const {
        return vk::Image(hudless_image);
    }
    [[nodiscard]] vk::ImageView HudlessView(bool srgb) const {
        return srgb ? *hudless_view_srgb : *hudless_view_unorm;
    }

    // Swap thread.

    /// A flip was queued for the swap thread (+1) or reached it (-1).
    void NoteFlipQueued(int delta) {
        queued_flips.fetch_add(delta, std::memory_order_relaxed);
    }
    /// A real frame with a generated one arrived: whether to show the generated one now.
    bool ShouldPresentGenerated();
    /// After the generated frame was presented at `shown`: waits until the real one's slot.
    void WaitForRealSlot(Clock::time_point shown);
    /// After every real frame: its arrival (`start`, before the generated one) and whether a
    /// generated one was shown. Measures the game's interval, the overload guard, the stats.
    void OnRealPresented(Clock::time_point start, bool generated_shown, bool generated_made,
                         bool generated_reached);
    [[nodiscard]] bool Recording() const;
    void NoteSwapchain(vk::PresentModeKHR mode, u32 images) {
        swapchain_mode.store(int(mode), std::memory_order_relaxed);
        swapchain_images.store(images, std::memory_order_relaxed);
    }
    void RecordPresent(bool generated, Clock::time_point done, u32 acquire_wait_us, bool ok);

private:
    /// `fsr`: with the FSR 3.1 context (DLSS Frame Generation needs only the captures).
    bool EnsureResources(u32 w, u32 h, u32 rw, u32 rh, bool fsr);
    void DestroyResources();
    /// Records the images' first barrier (Undefined to General).
    void ToGeneral();
    void CreatePipelines();
    void Idle(const char* reason);
    void Retire(u64 completed_tick);

    const Instance& instance;
    Scheduler& draw_scheduler;

    // Draw thread.
    bool inputs_frame = false;  ///< this frame's inputs were captured
    bool hudless_frame = false; ///< ... and its HUD-less copy
    bool inputs_ready = false, hudless_ready = false; ///< for the next PrepareFrame
    u32 input_width = 0, input_height = 0, hudless_width = 0, hudless_height = 0;
    std::array<float, 2> input_jitter{};
    float input_near = 0.1f, input_fov = 1.0f;
    CameraMotion::Matrices input_camera{};
    bool resources_fsr = false; ///< the resources have the FSR 3.1 context (not DLSS's captures only)
    /// Native+: the HUD-less and generated images are RGBA16F, the chain's gamma-encoded values.
    bool resources16 = false;
    bool need_reset = true;
    u64 next_frame_id = 1;
    Clock::time_point last_prepare{};
    float frame_ms = 16.6f;
    bool running = false;          ///< logged state
    const char* idle_reason = nullptr;
    bool started_logged = false;
    int last_setting = -1;

    // Resources (made on the draw thread after the GPU is idle).
    u32 width = 0, height = 0, render_width = 0, render_height = 0;
    FfxVkFsr3_3_1_6FrameGenerationContext* context = nullptr;
    VideoCore::UniqueImage hudless_image;   ///< RGBA8 Unorm (mutable: Srgb view for the compose); Native+ RGBA16F
    VideoCore::UniqueImage generated_image; ///< FSR's output, RGBA8 Unorm; Native+ RGBA16F
    VideoCore::UniqueImage composed_image;  ///< RGBA16F, the post process's input
    VideoCore::UniqueImage depth_image;     ///< R32F, render size
    VideoCore::UniqueImage motion_image;    ///< RG16F, render size
    vk::UniqueImageView hudless_view_unorm, hudless_view_srgb;
    vk::UniqueImageView generated_view_unorm, generated_view_srgb;
    vk::UniqueImageView composed_view, depth_view;
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout compose_desc_layout, depth_desc_layout;
    vk::UniquePipelineLayout compose_pipeline_layout, depth_pipeline_layout;
    vk::UniquePipeline compose_pipeline, depth_pipeline;
    bool failed = false;

    // Recording threads.
    std::mutex record_mutex;
    std::deque<std::pair<u64, u64>> in_flight; ///< (frame id, tick) not yet retired
    u64 last_recorded_id = 0;
    std::atomic<bool> record_failed{false};

    // Swap thread.
    std::atomic<int> queued_flips{0};
    std::atomic<int> swapchain_mode{-1};
    std::atomic<u32> swapchain_images{0};
    float reached_pct = -1.0f;
    int slot_by_vblank = -1;
    int slot_reports = 0;
    LanternFrameGenPacing::PresentLog present_log;
    std::array<LanternFrameGenPacing::PresentRecord, LanternFrameGenPacing::kRecords> present_ordered{};
    std::array<double, LanternFrameGenPacing::kRecords> present_scratch{};
    u32 stats_reached = 0, stats_vblank_slots = 0, stats_timer_slots = 0;
    std::atomic<s64> paused_until_ns{0};
    Clock::time_point last_real{};
    double interval_ms = 0.0;   ///< smoothed game frame interval
    double load_avg = 0.0;      ///< hitch-tolerant average of (present cycle / interval)
    u32 over_frames = 0;
    // BB_FRAME_STATS (5 s windows).
    Clock::time_point stats_start{};
    u32 stats_real = 0, stats_shown = 0, stats_dropped_backlog = 0, stats_not_made = 0;
    double stats_cycle_ms = 0.0;
};

} // namespace Vulkan
