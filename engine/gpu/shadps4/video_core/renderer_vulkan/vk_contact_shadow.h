// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

class ContactShadow {
public:
    ContactShadow(const Instance& instance, Scheduler& scheduler,
                  VideoCore::TextureCache& texture_cache, Runtime& runtime,
                  SceneTargets& scene_targets, CameraMotion& camera_motion, RtShadow& rt_shadow);
    ~ContactShadow();

    struct Inputs {
        vk::Buffer constants;
        vk::DeviceSize constants_offset = 0;
        vk::DeviceSize constants_range = 0;
        vk::Buffer uniforms;
        vk::DeviceSize uniforms_offset = 0;
        vk::DeviceSize uniforms_range = 0;
        vk::ImageView shadow_map;
        vk::ImageLayout shadow_layout = vk::ImageLayout::eUndefined;
        u32 constants_offset_word = 0;
        u32 frame_index = 0;
    };

    void OnShadowDraw(VideoCore::ImageId target, const Inputs& inputs);
    void OnBlurDraw(VideoCore::ImageId target, VAddr source);

    void Flush(bool native_blur = false);

private:
    bool Ready();

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;
    CameraMotion& camera_motion;
    RtShadow& rt_shadow;

    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::UniqueSampler sampler;
    VideoCore::ImageId pending;
    Inputs pending_inputs;
    bool use_contact = false;
    bool use_pcss = false;
    bool use_filter = false;
    bool use_rt = false;
    float length_m = 0.6f;
    float thickness_m = 0.35f;
    float strength = 0.75f;
    float flip = 1.0f;
    u32 debug = 0;
    float light_tan = 0.02f;
    float max_penumbra_m = 0.3f;
    float min_penumbra_m = 0.0f;
    float bias_m = 0.04f;
    bool tried = false;
    bool failed = false;
    u64 skipped_flushes = 0;
};

}
