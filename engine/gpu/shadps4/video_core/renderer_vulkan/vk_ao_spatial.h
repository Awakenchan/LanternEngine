// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <string_view>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"

namespace Vulkan {
class Instance;
class Runtime;
class SceneTargets;
class Scheduler;
}
namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

/// graphics_ao_quality=1 (High): denoises the GTAO gather in place and recomputes it at the full
/// internal resolution over the game's bilateral upscale. Per frame, no history.
class SpatialAo {
public:
    SpatialAo(const Instance&, Scheduler&, Runtime&, SceneTargets&, VideoCore::TextureCache&);
    [[nodiscard]] bool Enabled() const { return enabled; }
    /// buf_offsets: the gather's packed constant offsets (PushData::buf_offsets, byte 0 is Cb0).
    void Filter(VideoCore::ImageId, const VideoCore::ImageViewInfo&,
                vk::DescriptorImageInfo normals, vk::DescriptorBufferInfo constants,
                u32 buf_offsets, u32 scene_size, u32 width, u32 height, vk::Rect2D region);
    /// Remembers the gather's normals and target so the next draw, the game's bilateral
    /// upscale, can be replaced by full internal resolution HQ AO.
    void NoteGather(VideoCore::ImageId target, vk::DescriptorImageInfo normals);
    [[nodiscard]] bool UpscalePending() const { return bool(pending.target); }
    [[nodiscard]] VideoCore::ImageId GatherTarget() const { return pending.target; }
    /// The draw after the gather was not the expected upscale: full resolution is skipped.
    void SkipUpscale(std::string_view reason) {
        pending = {};
        WarnFullResolution(reason);
    }
    /// Recomputes HQ AO at the upscale target's resolution and writes it over the upscale.
    /// buf_offsets: the upscale's packed offsets (byte 0 the constants', byte 1 the far plane's).
    void FullResolution(VideoCore::ImageId, const VideoCore::ImageViewInfo&,
                        vk::DescriptorImageInfo depth, vk::DescriptorBufferInfo constants,
                        vk::DescriptorBufferInfo far_constants, u32 buf_offsets, float strength);
    void WarnUnsupported(std::string_view reason);

private:
    struct Pass {
        vk::UniqueDescriptorSetLayout set;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    struct Scratch {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0;
    };
    struct TargetView {
        vk::UniqueImageView view;
        vk::Image image;
        u64 uid = 0;
    };
    struct Pending {
        VideoCore::ImageId target{};
        vk::DescriptorImageInfo normals{};
    };
    void CreatePass(Pass&, std::span<const u32>);
    void CreateFullPass();
    bool EnsureScratch(Scratch&, u32 width, u32 height, const char* name);
    /// The image the target's current content lives in (its proxy when reduced), in the
    /// general layout for compute access; null when it lacks storage or sampled usage.
    vk::Image AcquireTarget(VideoCore::ImageId, const VideoCore::ImageViewInfo&);
    vk::ImageView ViewOf(TargetView&, vk::Image, u64 uid, vk::Format);
    void WarnFullResolution(std::string_view reason);

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    SceneTargets& scene_targets;
    VideoCore::TextureCache& texture_cache;
    Pass denoise, copy, full, full_denoise;
    vk::UniqueSampler sampler;
    Scratch scratch, full_scratch;
    TargetView target_view, full_view;
    Pending pending;
    bool enabled = false, warned = false, reported = false;
    bool full_warned = false, full_reported = false;
};

} // namespace Vulkan
