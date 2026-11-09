// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>

namespace Vulkan {
class CameraMotion;
class Instance;
class ObjectMotion;
class Scheduler;

class RtShadow {
public:
    RtShadow(const Instance& instance, Scheduler& scheduler, CameraMotion& camera_motion,
             const ObjectMotion& object_motion);
    ~RtShadow();

    [[nodiscard]] bool Capturing() const noexcept {
        return capturing;
    }
    [[nodiscard]] bool Wanted() const;
    [[nodiscard]] bool Complete() const;
    [[nodiscard]] bool KeepMap() const noexcept {
        return alpha_casters != 0;
    }

    struct Draw {
        u64 shader = 0;
        u64 geometry = 0;
        u64 positions = 0;
        u64 indices = 0;
        u64 palette = 0;
        const float* constants = nullptr;
        u32 index_size = 0;
        u32 index_count = 0;
        u32 first_vertex = 0;
        u32 first_instance = 0;
        u32 instances = 1;
        bool restart = false;
        bool strip = false;
        bool triangles = false;
        bool shadow_pass = false;
        bool camera_pass = false;
        bool solid = false;
        bool skinned = false;
        bool alpha = false;
    };
    u32 PrepareDraw(const Draw& draw);
    void BindAddresses(vk::PipelineLayout layout, u32 mode);

    bool PrepareScene();
    [[nodiscard]] vk::AccelerationStructureKHR Scene() const noexcept {
        return tlas;
    }

    struct Inputs {
        vk::Buffer constants;
        vk::DeviceSize constants_offset = 0;
        vk::DeviceSize constants_range = 0;
        vk::Buffer uniforms;
        vk::DeviceSize uniforms_offset = 0;
        vk::DeviceSize uniforms_range = 0;
        vk::ImageView depth;
        vk::ImageView target;
        u32 constants_offset_word = 0;
        u32 frame_index = 0;
        u32 width = 0;
        u32 height = 0;
    };
    bool Record(const Inputs& inputs);

    void RecordBuilds();

    void OnFrameEnd();

private:
    struct Buffer {
        vk::Buffer buffer;
        VmaAllocation allocation{};
        void* mapped = nullptr;
        u64 address = 0;
    };
    struct Entry {
        Buffer storage;
        Buffer indices;
        vk::AccelerationStructureKHR handle;
        u64 address = 0;
        u64 palette = 0;
        u32 first_vertex = 0;
        u32 vertices = 0;
        u32 triangles = 0;
        u32 instances = 1;
        u32 first_instance = 0;
        u64 scratch = 0;
        u64 update_scratch = 0;
        u64 bytes = 0;
        u64 seen_frame = 0;
        u64 built_frame = 0;
        u32 rebuild_every = 120;
        u8 mask = 1;
        bool built = false;
        bool dynamic = false;
        bool rebuilt = false;
    };
    struct Build {
        Entry* entry;
        u32 store;
        bool update;
        std::array<float, 12> to_world;
    };

    bool Setup(const ObjectMotion& object_motion);
    bool Make(Buffer& out, vk::DeviceSize size, bool host, VkBufferUsageFlags usage);
    void Free(Buffer& buffer);
    void Destroy(Entry& entry);
    bool ToWorld(const float* constants, bool perspective, std::array<float, 12>& out) const;
    bool MakeRaw(u32 width, u32 height);
    u32 Param(u32 store, const Entry& entry);
    std::unique_ptr<Entry> MakeEntry(const Draw& draw, u8 mask);

    const Instance& instance;
    Scheduler& scheduler;
    CameraMotion& camera_motion;

    bool capturing = false;
    bool tried = false;
    bool failed = false;

    vk::UniqueDescriptorSetLayout address_layout;
    vk::UniqueDescriptorPool address_pool;
    vk::DescriptorSet capture_set;
    vk::DescriptorSet both_set;
    vk::UniqueDescriptorSetLayout trace_set, filter_set, unproject_set;
    vk::UniquePipelineLayout trace_layout, filter_layout, unproject_layout;
    vk::UniquePipeline trace_pipeline, filter_pipeline, unproject_pipeline;
    vk::UniqueSampler sampler;

    Buffer capture_addresses, both_addresses, params, positions, scratch, instance_data,
        tlas_storage, tlas_scratch;
    vk::AccelerationStructureKHR tlas;
    vk::Image raw_image;
    VmaAllocation raw_allocation{};
    vk::ImageView raw_view;
    u32 raw_width = 0, raw_height = 0;

    std::unordered_map<u64, std::unique_ptr<Entry>> entries;
    std::unordered_set<u64> shared_keys;
    std::unordered_map<u64, u32> occurrences;
    std::vector<const float*> frame_cameras;
    std::vector<Entry*> frame_entries;
    std::vector<Build> builds;
    std::array<u64, 4> slot_ticks{};
    u64 frame = 1;
    u32 params_used = 0;
    u32 positions_used = 0;
    u32 new_triangles = 0;
    u32 rebuilt_triangles = 0;
    u64 scratch_used = 0;
    u32 alpha_casters = 0;
    u32 missed = 0;
    u32 tlas_count = 0;
    u32 rays = 1;
    u64 scratch_align = 256;
    u64 blas_bytes = 0;
    u64 blas_limit = 320ull << 20;

    struct Stats {
        u64 draws = 0, camera_draws = 0, not_ortho = 0, no_camera = 0, alpha = 0, unsupported = 0,
            over_budget = 0, instanced = 0, dynamic_draws = 0, strips = 0, frames = 0, instances = 0,
            triangles = 0, shared_builds = 0, merged = 0, cut_characters = 0,
            built_triangles = 0, evicted = 0;
    } stats;
};

}
