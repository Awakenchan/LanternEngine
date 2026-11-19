// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/types.h"
#include <array>
#include <memory>
#include <vector>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/rt_lamp_rules.h"
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
        u32 stride = 16;
        bool radius_in_w = false;
        u64 shader = 0;
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
        vk::Format format = vk::Format::eUndefined;
    };
    bool EnsureImage(OwnImage& own, u32 width, u32 height, vk::Format format, const char* name);
    void RefreshGameLights(u32 width, u32 height);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;
    RtShadow& rt_shadow;
    vk::UniqueDescriptorSetLayout ray_set_layout;
    vk::UniquePipelineLayout ray_layout;
    vk::UniquePipeline ray_pipeline;
    vk::UniqueDescriptorSetLayout ray_set_layout16;
    vk::UniquePipelineLayout ray_layout16;
    vk::UniquePipeline ray_pipeline16;
    vk::UniqueDescriptorSetLayout set_layout16;
    vk::UniquePipelineLayout layout16;
    vk::UniquePipeline pipeline16;
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
        bool unknown = false;
    };
    std::vector<Tile> tiles;
    void AddGameLights(std::vector<Tile>& batch, u32 width, u32 height);
    void ProbeCone(const RtLamps::ConeRecord& cone, const std::vector<u32>& words);
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
    u64 skipped_draws = 0;
    u64 skips_reported = 0;
    u32 tiles_logged = 0;
    u64 tiles_logged_frame = 0;
    u64 mixed_flushes = 0;
    u64 mixed_reported = 0;
    bool game_lights = false;
    std::vector<RtLamps::WorldLight> game_world;
    u64 game_stamp = 0;
    std::vector<RtLamps::Placed> game_placed;
    u32 game_frame = ~0u;
    float game_nearest_m = 0.0f;
    u32 game_torn = 0;
    RtLamps::BatchCount game_count{};
    std::array<u32, 16> frame_constants{};
    bool frame_constants_valid = false;
    u32 unknown_draws = 0;
    u32 unknown_dropped = 0;
    RtLamps::DrawnLights drawn;
    std::vector<RtLamps::ConeRecord> cone_records;
    static constexpr u32 MaxConesLogged = 16;
    u32 cones_logged = 0;
    u64 game_ns_frame = 0, game_ns_max = 0, game_rows = 0;
    u32 game_room_cut = 0;
    std::vector<std::array<s32, 3>> cones_seen;
    std::array<u64, 4> unknown_hashes{};
};

}
