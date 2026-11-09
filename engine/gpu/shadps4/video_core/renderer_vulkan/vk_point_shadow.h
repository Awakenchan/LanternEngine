// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/types.h"
#include <array>
#include <memory>
#include <vector>
#include "video_core/buffer_cache/buffer.h"
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

class PointShadow {
public:
    PointShadow(const Instance& instance, Scheduler& scheduler,
                VideoCore::TextureCache& texture_cache, Runtime& runtime,
                SceneTargets& scene_targets, CameraMotion& camera_motion, RtShadow& rt_shadow);
    ~PointShadow();

    struct Inputs {
        const u32* constants = nullptr;
        const u32* lights = nullptr;
        u32 count = 0;
        vk::Rect2D tile{};
    };

    void OnLightDraw(VideoCore::ImageId color0, VideoCore::ImageId color1, const Inputs& inputs);

    void Flush(bool animate_noise);
    [[nodiscard]] bool Pending() const noexcept {
        return !tiles.empty();
    }
    void Ended(u64 hash);
    [[nodiscard]] std::array<VAddr, 2> PendingAddresses() const;
    void Kept() noexcept {
        ++kept_draws;
    }

    void OnFrameEnd();

    void Drop();

private:
    bool Ready();

    struct OwnImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0;
    };
    bool EnsureImage(OwnImage& own, u32 width, u32 height, vk::Format format, const char* name);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;
    RtShadow& rt_shadow;
    vk::UniqueDescriptorSetLayout ray_set_layout;
    vk::UniquePipelineLayout ray_layout;
    vk::UniquePipeline ray_pipeline;
    CameraMotion& camera_motion;

    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::UniqueSampler sampler;
    OwnImage snapshot[2];
    std::array<VideoCore::ImageId, 2> pending{};
    std::array<u64, 2> pending_uid{};
    struct Tile {
        std::array<s32, 4> rect;
        u32 first_light;
        u32 count;
    };
    std::vector<Tile> tiles;
    std::vector<u32> data;
    std::unique_ptr<VideoCore::Buffer> light_data;
    u32 frame_index = 0;
    u32 batches = 0;
    u32 frame_tiles = 0;
    bool one_tile_batches = false;
    bool merged = false;
    u32 kept_draws = 0;
    u32 lamp_pick = 1;
    bool lamp_reuse = true;
    OwnImage history[2];
    std::array<bool, 2> history_laid_out{};
    u32 history_index = 0;
    bool history_written = false;
    bool history_fresh = false;
    std::array<float, 12> last_view{}, pending_view{};
    std::array<float, 16> last_proj{}, pending_proj{};
    bool last_valid = false, pending_valid = false;
    u32 ended_by_draws = 0;
    std::array<u64, 4> ending_hashes{};
    bool reported = false;
    float length_m = 2.5f;
    float thickness_m = 0.6f;
    float strength = 0.85f;
    float fade_start_m = 18.0f;
    float fade_end_m = 30.0f;
    float bias_m = 0.03f;
    u32 steps = 16;
    bool tried = false;
    bool failed = false;
};

}
