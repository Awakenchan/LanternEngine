// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {
class CameraMotion;
class Instance;
class RtShadow;
class Runtime;
class SceneTargets;
class Scheduler;

class ScreenSpaceGI {
public:
    ScreenSpaceGI(const Instance& instance, Scheduler& scheduler,
                  VideoCore::TextureCache& texture_cache, Runtime& runtime,
                  SceneTargets& scene_targets, CameraMotion& camera_motion, RtShadow& rt_shadow);
    ~ScreenSpaceGI();

    void OnOpaqueScene(VideoCore::ImageId color, VideoCore::ImageId depth,
                       std::array<float, 2> jitter);

    void OnGBufferDraw(VideoCore::ImageId depth, VideoCore::ImageId albedo,
                       VideoCore::ImageId normal, VideoCore::ImageId material,
                       VideoCore::ImageId specular);

    void OnAoDraw(VideoCore::ImageId ao);

    void OnFrameEnd();

private:
    struct OwnImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0;
    };
    struct Pass {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    struct ViewEntry {
        vk::Image image;
        u64 uid = 0;
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view;
        u64 last_use = 0;
    };

    bool Ready();
    bool CreatePass(Pass& pass, std::initializer_list<vk::DescriptorType> types,
                    std::span<const u32> code, const char* name);

    bool EnsureImage(OwnImage& image, u32 width, u32 height, vk::Format format, const char* name);
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;
    RtShadow& rt_shadow;
    CameraMotion& camera_motion;

    Pass prep, gather, gather_ray, denoise, composite, probe_trace, probe_composite;
    OwnImage geo;
    std::array<OwnImage, 2> light;
    std::array<OwnImage, 2> filtered;
    std::array<OwnImage, 2> probe_geo;
    std::array<OwnImage, 2> probe_light;
    std::array<std::array<OwnImage, 3>, 2> probe_sh;
    std::array<OwnImage, 2> probe_diffuse;
    std::array<OwnImage, 2> probe_specular;
    bool probe_history_valid = false;
    std::array<float, 12> prev_view{};
    std::array<float, 2> prev_proj{};
    std::array<float, 2> prev_jitter{};
    bool prev_camera_valid = false;
    u32 frame = 0;
    bool done_this_frame = false;
    bool history_valid = false;
    bool failed = false;
    bool tried = false;
    VideoCore::ImageId gbuffer_depth, gbuffer_albedo, gbuffer_normal;
    VideoCore::ImageId gbuffer_material, gbuffer_specular;
    VideoCore::ImageId ao_image;
    std::array<ViewEntry, 8> view_cache{};
    u64 view_uses = 0;
};

}
