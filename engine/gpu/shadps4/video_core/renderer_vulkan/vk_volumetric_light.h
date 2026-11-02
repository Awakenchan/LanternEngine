// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <span>
#include <vector>

#include <span>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_contact_shadow.h"
#include "video_core/renderer_vulkan/vk_point_shadow.h"

namespace Vulkan {
class SceneTargets;

class VolumetricLight {
public:
    VolumetricLight(const Instance& instance, Scheduler& scheduler,
                    VideoCore::TextureCache& texture_cache, Runtime& runtime,
                    SceneTargets& scene_targets);
    ~VolumetricLight();

    void Reset() { ready = tinted = false; scene = {}; fog_lights.clear(); }
    void Integrate(VideoCore::ImageId shadow_target, const vk::DescriptorImageInfo& depth,
                   const ContactShadow::Inputs& inputs);
    void Tint(const vk::DescriptorBufferInfo& lighting, u32 offset_bytes);
    void Scene(VideoCore::ImageId color);
    void AddPointLights(const u32* constants, const u32* lights, u32 count);
    void Composite(VideoCore::ImageId fallback = {}, const char* where = "HDR post");

private:
    struct Pass {
        vk::UniqueDescriptorSetLayout set;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    bool Ready();
    void RecordLampFog(std::span<const std::array<u32, 16>> lights, u32 w, u32 h);
    bool CreatePass(Pass& pass, std::initializer_list<vk::DescriptorType> types,
                    std::span<const u32> code, u32 push_size, const char* name,
                    bool dispatch_base = false);
    void EnsureImage(u32 width, u32 height);
    bool Enabled();

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;
    Pass integrate, repair, tint, filter, composite, point;
    std::unique_ptr<VideoCore::Buffer> tint_state;
    std::chrono::steady_clock::time_point tint_time{};
    std::unique_ptr<VideoCore::Buffer> fog_data;
    std::array<u32, 16> fog_matrix{};
    std::vector<std::array<u32, 16>> fog_lights;
    vk::UniqueSampler sampler;
    VideoCore::UniqueImage fog;
    vk::UniqueImageView fog_view;
    VideoCore::UniqueImage filtered_fog;
    vk::UniqueImageView filtered_view;
    VideoCore::UniqueImage repair_fog;
    vk::UniqueImageView repair_view;
    VideoCore::UniqueImage point_fog;
    vk::UniqueImageView point_view;
    VideoCore::UniqueImage depth_snapshot;
    vk::UniqueImageView depth_view;
    VideoCore::ImageId scene{};
    u64 scene_uid = 0;
    u32 depth_width = 0, depth_height = 0;
    u32 width = 0, height = 0;
    float strength = 0.08f;
    float reach = 40.0f;
    float density = 0.025f;
    float anisotropy = 0.4f;
    u32 steps = 32;
    bool ready = false;
    bool tinted = false;
    bool tried = false;
    bool failed = false;
    bool composite_reported = false;
    bool tint_reported = false;
};

}
