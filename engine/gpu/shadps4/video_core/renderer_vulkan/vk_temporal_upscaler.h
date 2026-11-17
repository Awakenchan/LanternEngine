// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: temporal upscaling of Bloodborne's HDR scene color (docs/upscaler.md). BB_UPSCALER=fsr3
// runs FSR 3.1 (FireBurn/FSR-Vulkan, native Vulkan) on the scene color right before the
// post-processing combine pass, with the scene depth and camera motion vectors, and writes the
// result back so the game's own post, tonemap and UI continue unchanged (Native AA preset).
//
// Guest targets stay at 1920x1080. Host scene targets use output/preset dimensions,
// resized at frame boundaries. At 1080p HDR reconstruction precedes post; other outputs
// reconstruct tonemapped scene proxies before output-size UI/display composition.
// BB_RENDER_RES retains the older startup-patched guest-resolution path.

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include "video_core/renderer_vulkan/upscaler_skips.h"
#include "video_core/renderer_vulkan/vk_hdr_native.h"
#include "video_core/renderer_vulkan/vk_xess.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableUpscaleContext;

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class CameraMotion;
class SceneTargets;
class FrameGenerator;

/// bbport BB_FINAL_DUMP_TRIGGER: the frame as presented (after FSR and post processing) is saved
/// as final_<w>x<h> in BB_DUMP_DIR when the trigger file exists (consumed). `image` in General.
void DumpFinalFrameIfDue(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                         vk::Image image, u32 width, u32 height, vk::Format format);

class TemporalUpscaler {
public:
    TemporalUpscaler(const Instance& instance, Scheduler& scheduler,
                     VideoCore::TextureCache& texture_cache, Runtime& runtime,
                     CameraMotion& camera_motion, SceneTargets& scene_targets,
                     HdrNative& hdr_native);
    ~TemporalUpscaler();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// bbport: frame generation (vk_frame_generation.h) takes its inputs from the upscaled
    /// frames: depth, motion and camera after the motion pass, the frame before its UI.
    void SetFrameGenerator(FrameGenerator* generator) noexcept {
        frame_gen = generator;
    }

    /// A draw into a full-size RGBA16F target with the scene depth: the scene color.
    void OnSceneColor(VideoCore::ImageId color);

    /// A blended (transparent) draw into the scene color that is not a full-screen pass: the
    /// first one of a frame snapshots the opaque scene for the reactive mask.
    [[nodiscard]] u64 TriggerHash() const noexcept {
        return trigger_hash;
    }
    [[nodiscard]] VideoCore::ImageId SceneColor() const noexcept {
        return scene_color;
    }
    void OnBlendedSceneDraw();

    /// A full-screen pass into the scene color: after a snapshot, the reactive mask is taken
    /// before it (the fog composite rewrites every pixel).
    void OnSceneComposite();

    /// Before a compute dispatch: the post-processing combine shader triggers the upscale.
    void OnDispatch(u64 cs_hash);

    /// Start of a frame in the command stream (display pass).
    bool OnFrameStart();
    /// bbport: native HDR output (vk_hdr_native.h). Right before OnFrameStart: the display pass
    /// copies the finished frame into the display buffer at `address`; `frame` is the target
    /// the previous draw rendered into.
    void OnDisplayPass(VAddr address, VideoCore::ImageId frame);
    bool RasterScaling() const;
    /// bbport: LOD bias for scene materials, log2(render / output) while a reduced scene is
    /// upscaled (0 at native size, for TAA and without an upscaler).
    [[nodiscard]] float SceneMipBias() const;
    /// bbport: the state RedirectColor/RedirectDepth/RasterScaling depend on, for memoizing a
    /// draw's render state (Rasterizer::BeginRendering).
    [[nodiscard]] u64 RedirectState() const noexcept {
        return u64(ui_phase) | u64(display_redirect) << 1 | u64(done_this_frame) << 2 |
               u64(ui_color.index) << 8 | u64(ui_depth.index) << 36;
    }


    /// This frame's sub-pixel jitter in pixels (screen x right, y down); zero when off and after
    /// the upscale (post and UI are not jittered).
    [[nodiscard]] std::array<float, 2> Jitter() const noexcept {
        return done_this_frame ? std::array<float, 2>{} : jitter;
    }

    // Scaled presets.

    /// After identifying this draw's targets, before drawing any UI pixels. Menus use
    /// spatial background copy and native UI, without needing scene depth or FSR history.
    void OnDraw(u64 vs_hash, VideoCore::ImageId color, VideoCore::ImageId depth,
                bool native_viewport, bool widened_ui_viewport = false);

    /// A pass's first color target: the last render-size RGBA8 target before the UI is the
    /// game's finished frame.
    void OnColorTarget(VideoCore::ImageId color);

    struct Target {
        vk::ImageView view;
        vk::ImageLayout layout;
        u32 width, height;
        bool native_ui = false;
    };
    /// Render target redirection (UI passes, display pass): false when not redirected.
    /// `view` is the game's view of the image: redirected views mirror its format (sRGB) and,
    /// for sampling, its channel swizzle.
    bool RedirectColor(VideoCore::ImageId color, const VideoCore::ImageViewInfo& view,
                       Target& target);
    bool RedirectDepth(VideoCore::ImageId depth, Target& target);
    /// A sampled image replaced by the upscaled frame (the display pass).
    bool RedirectSampled(VideoCore::ImageId image, const VideoCore::ImageViewInfo& info,
                         vk::ImageView& view, vk::ImageLayout& layout);
    /// Whether RedirectSampled would replace `image` (it may record a barrier then).
    [[nodiscard]] bool RedirectsSampled(VideoCore::ImageId image) const {
        return display_redirect && image == ui_color;
    }

    /// Presenter: the output-size display buffer standing in for the guest one at `address`.
    struct Display {
        vk::Image image;
        vk::Format format;
        u32 width, height;
        vk::Format guest_format = vk::Format::eUndefined;
    };
    bool DisplayOverride(VAddr address, Display& display);
    bool TakeHudSnapshot(VAddr address, std::unique_ptr<HdrGainMap>& snapshot);
    void PublishHudSnapshot(VAddr address);
    bool Redirecting() const noexcept { return ui_phase || display_redirect; }
    /// BB_PRESENT_DUMP_TRIGGER: whether this presented frame is to be saved (consumes the trigger).
    bool PresentDumpDue();
    /// Saves `image` (General layout, 4 bytes a pixel) as present_<w>x<h> in BB_DUMP_DIR.
    void DumpPresented(vk::Image image, u32 width, u32 height, vk::Format format);

private:
    /// bbport: views of guest images the upscaler reads, kept across frames: FSR 4 registers
    /// images by view in a registry of eight (a new view per frame filled it at Native AA).
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect);
    struct ViewEntry {
        vk::Image image;
        u64 uid = 0;
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view;
        u64 last_use = 0;
    };
    std::array<ViewEntry, 6> view_cache{};
    u64 view_uses = 0;

    void Run();
    void RunScaled();
    /// world: this frame drew the 3D world into `color` (its whole image is copied).
    void RunUiOnly(VideoCore::ImageId color, VideoCore::ImageId depth, bool world);
    void EnsureUiResources(u32 width, u32 height, vk::Format color, vk::Format depth);
    void PrepareUiDepth(VideoCore::ImageId depth);
    /// Render size below the scaled-preset output size (the resolution patch is on).
    [[nodiscard]] bool Scaled() const;
    /// A target of the patched render size: the game allocates it with aligned dimensions
    /// (a 1916x1078 scene in 1916x1080 targets, a 1708x960 one in 1792x960: widths to 128).
    [[nodiscard]] bool RenderTarget(u32 w, u32 h) const {
        if (!scaled_session) return w == 1920 && h == 1080;
        return w >= render_width && h >= render_height && w < render_width + 128 &&
               h < render_height + 64;
    }
    /// bbport: native HDR output: the game's tonemapped frame (the UI target): RGBA8 of the
    /// render size, not a display buffer.
    [[nodiscard]] bool LdrFrame(VideoCore::ImageId id) const;
    /// bbport: native HDR output: the output-size UI image as a native HDR source (UNORM view:
    /// the display-encoded values, whatever its format).
    HdrNative::Source HdrUiImage();
    /// bbport: frame generation: this frame's depth and motion (after the motion pass), and the
    /// frame before its UI (a guest render target, or the output-size UI image).
    void CaptureFrameGenInputs(vk::ImageView depth_view, u32 w, u32 h);
    void CaptureFrameGenHudless(VideoCore::ImageId id);
    void CaptureFrameGenUiImage();
    /// The scene's size inside a `w` x `h` render target: the game's viewport.
    [[nodiscard]] std::array<u32, 2> SceneSize(u32 w, u32 h) const;
    // The scene color is drawn into a reduced SceneTargets proxy (live presets).
    [[nodiscard]] bool ReducedScene(const VideoCore::Image& color) const;
    /// Available and switched on (menu setting, toggle 1 << 24).
    [[nodiscard]] bool Active() const;
    [[nodiscard]] bool ReactiveOn() const;
    bool UiComposed() const;
    vk::Format UiColorFormat(const VideoCore::Image& color) const;
    /// Native+: the chain's gamma-encoded frame decoded to linear light (`encode` false) or
    /// encoded back (true) by hdr_upscale_transfer.comp; with a snapshot image, the encode also
    /// writes the pre-HUD snapshot. Records only what it is given (on a recording thread too).
    void RecordHdrTransfer(vk::CommandBuffer cmd, vk::Image source, vk::ImageView source_view,
                           vk::Image target, vk::ImageView target_view, u32 w, u32 h, bool encode,
                           vk::Image snapshot_image = {}, vk::ImageView snapshot_view = {}) const;
    /// Native+: the pre-HUD snapshot's image at the UI image's size. True when it was made again
    /// (after scheduler.Finish(): a command buffer taken before is gone).
    bool EnsureSnapshot();
    bool EnsureResources(u32 width, u32 height, u32 out_width, u32 out_height, bool hdr);
    void CreatePipelines();
    /// Records the reactive mask pass; false when there is no snapshot this frame.
    bool RecordReactive(vk::ImageView color_view);
    /// FSR 4 is selected, possible in this session (not BB_RENDER_RES) and has not failed.
    [[nodiscard]] bool UseFsr4() const;
    /// Records FSR 4 into output_image; on a permanent failure FSR 3 takes over. `reactive`: this
    /// frame's reactive mask is ready, given to DLSS as its bias-current-colour mask.
    bool RecordFsr4(vk::CommandBuffer cmdbuf, Fsr4Upscaler::Image color, Fsr4Upscaler::Image depth,
                    u32 w, u32 h, u32 ow, u32 oh, float frame_ms, bool reactive = false);
    void RecordTaa(vk::CommandBuffer cmdbuf, vk::ImageView color, vk::ImageView depth);
    /// Additional RCAS sharpening; DLSS applies the whole requested range here.
    void ExtraSharpen(vk::Image target, bool ldr, u32 w, u32 h);
    /// bbport: the menu's mask / motion debug view over the scaled path's output (ldr: the 8-bit
    /// UI image), in place; upscale_merge.comp draws it on the 1080p path. `reactive`: a mask
    /// was made this frame.
    void DebugView(vk::Image target, bool ldr, u32 w, u32 h, u32 ow, u32 oh, bool reactive);
    /// The UI image as a UNORM storage view (its own format may be sRGB).
    vk::ImageView UiStorageView(vk::Image target);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera_motion;
    SceneTargets& scene_targets;
    HdrNative& hdr_native;
    FrameGenerator* frame_gen = nullptr; ///< bbport: SetFrameGenerator
    /// bbport: native HDR output: the game's UI target this frame's UI-free copy came from.
    VideoCore::ImageId hdr_ui_target{};
    int applied_preset = -1;
    float applied_scale = 0.0f; ///< bbport: PresetScale of applied_preset (the Custom ratio moves)
    int applied_upscaler = -1;
    bool dispatched_last_frame = false;
    u32 unresolved_frames = 0;
    UpscalerSkips::Path frame_path = UpscalerSkips::Path::None;
    UpscalerSkips::Reason frame_reason = UpscalerSkips::Reason::NoUiDraw;
    UpscalerSkips::Window path_window{};
    std::chrono::steady_clock::time_point path_window_start = std::chrono::steady_clock::now();
    UpscalerSkips::AfterLoad after_load{};
    void NoteResolvedFrame(bool active);
    bool last_active = false, last_jitter = false;


    bool enabled = false;
    bool failed = false;
    /// An FSR 3 dispatch recorded on a recording thread failed; `failed` at the next frame.
    std::atomic<bool> dispatch_failed{false};
    u64 trigger_hash = 0x9a9cf8a9;
    VideoCore::ImageId scene_color{};
    bool done_this_frame = false;
    u32 preset_file_frames = 0; ///< BB_PRESET_FILE polling
    u32 reset_frames = 0, reset_window_frames = 0;
    std::chrono::steady_clock::time_point reset_window = std::chrono::steady_clock::now();
    float slow_window_ms = 0.0f;
    u32 slow_window_frames = 0, slow_frames = 0, slow_guard_used = 0;
    bool Fsr4TooSlow(float frame_ms);
    float jitter_speed = 0.0f;
    float JitterAmount();
    [[nodiscard]] float CameraSpeed() const;
    int applied_output = -1;
    bool snapshot_taken = false;
    bool opaque_valid = false;
    bool mask_ready = false;
    std::array<float, 2> jitter{};
    u32 jitter_index = 0;
    bool reset = true;
    u32 history_cuts_seen = 0;
    u64 frame_id = 0;
    std::chrono::steady_clock::time_point last_frame{};

    u32 width = 0, height = 0;             ///< render size
    u32 out_width = 0, out_height = 0;     ///< output size of the context
    bool context_hdr = true;
    u32 target_width = 1920, target_height = 1080; ///< output size of scaled presets
    u64 ui_trigger_vs = 0x34e8a281;
    VideoCore::ImageId ldr_target{};
    VideoCore::ImageId ui_color{}, ui_depth{};
    bool ui_phase = false;         ///< from the upscale to the next display pass
    bool display_redirect = false; ///< the display pass of an upscaled frame
    bool ui_read_barrier = false;
    int applied_hdr_path = -1;
    VideoCore::UniqueImage hdr_linear_input;
    vk::UniqueImageView hdr_linear_input_view;
    vk::UniqueDescriptorSetLayout hdr_transfer_desc_layout;
    vk::UniquePipelineLayout hdr_transfer_pipeline_layout;
    vk::UniquePipeline hdr_transfer_pipeline;
    /// The encode that also writes the pre-HUD snapshot (a third binding).
    vk::UniqueDescriptorSetLayout hdr_transfer_snapshot_desc_layout;
    vk::UniquePipelineLayout hdr_transfer_snapshot_pipeline_layout;
    vk::UniquePipeline hdr_transfer_snapshot_pipeline;
    VideoCore::UniqueImage scene_snapshot;
    vk::UniqueImageView scene_snapshot_view;
    u32 snapshot_width = 0, snapshot_height = 0;
    std::atomic<bool> snapshot_ready{false};
    VideoCore::UniqueImage ui_image;
    VideoCore::UniqueImage ui_depth_image;
    vk::UniqueImageView ui_view;
    vk::UniqueImageView ui_depth_view;
    bool depth_blit = false;
    bool scaled_session = false;
    bool early_upscale = false;
    u32 ui_width = 0, ui_height = 0;
    u32 render_width = 0, render_height = 0;
    vk::Format ui_format = vk::Format::eUndefined;
    vk::Format ui_depth_format = vk::Format::eUndefined;
    /// Views of the port's images in the formats/swizzles the game's views use.
    struct MirrorView {
        vk::Format format;
        vk::ComponentMapping mapping;
        vk::UniqueImageView view;
    };
    vk::ImageView Mirror(vk::Image image, std::vector<MirrorView>& views, vk::Format format,
                         vk::ComponentMapping mapping);
    std::vector<MirrorView> ui_views;
    struct DisplayImage {
        std::unique_ptr<HdrGainMap> hud_snapshot;
        vk::Format guest_format = vk::Format::eUndefined;
        VideoCore::UniqueImage image;
        std::vector<MirrorView> views;
        vk::Format format{};
        u32 width = 0, height = 0;
        bool valid = false;
    };
    std::mutex display_mutex;
    int present_dump_remaining = 0, present_dump_index = 0;
    std::unordered_map<VAddr, DisplayImage> displays;
    FfxVkPortableUpscaleContext* context = nullptr;
    /// bbport: FSR 3.1's tuning constants last given to `fsr3_tuned` (ApplyFsr3Tuning).
    std::array<float, 5> fsr3_tuning{};
    FfxVkPortableUpscaleContext* fsr3_tuned = nullptr;
    /// The menu's FSR 3.1 tuning (fsr3_* settings) given to the context when they changed or the
    /// context is new; kept by it for the dispatches that follow.
    void ApplyFsr3Tuning();
    bool resources_ready = false; ///< images below match width/height/out size
    bool resources_fsr4 = false;  ///< made for FSR 4 (no FSR 3 context)
    bool resources_taa = false;
    std::unique_ptr<Fsr4Upscaler> fsr4;
    std::unique_ptr<DlssUpscaler> dlss; ///< NVIDIA devices only; records the FSR 4 frame
    std::unique_ptr<XessUpscaler> xess; ///< with libxess.dll (Windows); records the FSR 4 frame
    bool fsr4_failed = false;
    bool fsr4_building = false; ///< FSR 4's model is built in the background: FSR 3.1 until then
    VideoCore::UniqueImage motion_image;
    VideoCore::UniqueImage output_image;
    vk::UniqueImageView motion_view;
    vk::UniqueImageView output_view;
    VideoCore::UniqueImage opaque_image;   ///< scene color before the blended draws
    VideoCore::UniqueImage reactive_image; ///< R8 reactive mask
    vk::UniqueImageView opaque_view;
    vk::UniqueImageView reactive_view;
    vk::UniqueDescriptorSetLayout reactive_desc_layout;
    vk::UniquePipelineLayout reactive_pipeline_layout;
    vk::UniquePipeline reactive_pipeline;
    vk::UniqueDescriptorSetLayout merge_desc_layout;
    vk::UniquePipelineLayout merge_pipeline_layout;
    vk::UniquePipeline merge_pipeline;
    std::array<VideoCore::UniqueImage, 2> taa_history;
    std::array<vk::UniqueImageView, 2> taa_history_views;
    u32 taa_next = 0;
    vk::UniqueSampler taa_sampler;
    vk::UniqueDescriptorSetLayout taa_desc_layout;
    vk::UniquePipelineLayout taa_pipeline_layout;
    vk::UniquePipeline taa_pipeline;
    vk::UniqueDescriptorSetLayout taa_sharpen_desc_layout;
    vk::UniquePipelineLayout taa_sharpen_pipeline_layout;
    vk::UniquePipeline taa_sharpen_pipeline;
    vk::UniquePipeline taa_sharpen_ldr_pipeline;
    vk::UniqueDescriptorSetLayout debug_desc_layout;
    vk::UniquePipelineLayout debug_pipeline_layout;
    vk::UniquePipeline debug_pipeline, debug_ldr_pipeline;
    // bbport: FSR 4 in linear light on the scaled presets (fsr4_color.comp): the decoded input
    // and the passes that decode it and encode the output again.
    VideoCore::UniqueImage fsr4_linear_image;
    bool fsr4_linear_frame = false; ///< this frame's FSR 4 input was decoded (and the output encoded)
    vk::UniqueImageView fsr4_linear_view;
    vk::UniqueDescriptorSetLayout fsr4_decode_desc_layout;
    vk::UniquePipelineLayout fsr4_decode_pipeline_layout;
    vk::UniquePipeline fsr4_decode_pipeline;
    vk::UniqueDescriptorSetLayout fsr4_encode_desc_layout;
    vk::UniquePipelineLayout fsr4_encode_pipeline_layout;
    vk::UniquePipeline fsr4_encode_pipeline;
    vk::UniqueSampler fsr4_linear_sampler;
    vk::UniqueDescriptorSetLayout fsr4_reactive_desc_layout;
    vk::UniquePipelineLayout fsr4_reactive_pipeline_layout;
    vk::UniquePipeline fsr4_reactive_pipeline;
    /// bbport: FSR 4 takes no reactive mask: its output is blended with `color` (the frame it
    /// upscaled, render size, General layout, same colour space as the output) where the mask
    /// marks blended effects (fsr4_reactive.comp). Recorded after FSR 4 into `cmdbuf`.
    void RecordFsr4Reactive(vk::CommandBuffer cmdbuf, vk::ImageView color, u32 w, u32 h, u32 ow,
                            u32 oh);
    // ExtraSharpen: a copy of the upscaled frame (RCAS reads neighbours) and the target views.
    VideoCore::UniqueImage extra_sharpen_image;
    vk::UniqueImageView extra_sharpen_view;
    u32 extra_sharpen_width = 0, extra_sharpen_height = 0;
    vk::UniqueImageView ui_storage_view;
};

} // namespace Vulkan
