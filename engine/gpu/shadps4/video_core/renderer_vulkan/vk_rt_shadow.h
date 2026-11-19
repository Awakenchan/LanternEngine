// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/renderer_vulkan/rt_scene_rules.h"
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
        RtRules::Bones bones;
        RtRules::Pose pose;
        const float* constants = nullptr;
        const float* anchor = nullptr;
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
    [[nodiscard]] u32 LampRayMask() const noexcept;
    [[nodiscard]] bool RigidWanted() const noexcept {
        return rigid;
    }
    [[nodiscard]] u64* CaptureClock() noexcept {
        return diag ? &diag_frame.capture_ns : nullptr;
    }
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
    struct Slice {
        VmaVirtualBlock block = nullptr;
        VmaVirtualAllocation allocation = nullptr;
    };
    struct Entry {
        Buffer storage;
        Buffer indices;
        Slice storage_slice;
        Slice index_slice;
        u64 storage_offset = 0;
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
        u64 data_frame = 0;
        u32 rebuild_every = 120;
        u8 mask = 1;
        bool built = false;
        bool dynamic = false;
        bool rebuilt = false;
        bool rigid_pending = false;
        RtRules::RigidBones rigid_bones;
        RtRules::CapturedPose pose;
        RtRules::Box box;
        bool has_box = false;
        u8 box_slot = 0;
        u32 box_pending = UINT32_MAX;
        u64 gone_frame = 0;
        u64 index_hash = 0;
        bool stale = false;
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
    void DeferFree(Common::UniqueFunction<void>&& free);
    void FreeLoop(std::stop_token stop);
    void StopFree();
    bool ToWorld(const float* constants, bool perspective, std::array<float, 12>& out) const;
    bool MakeRaw(u32 width, u32 height);
    u32 Param(u32 store, const Entry& entry);
    std::unique_ptr<Entry> MakeEntry(const Draw& draw, u8 mask);
    struct Source {
        const void* indices = nullptr;
        u64 palette = 0;
        u32 index_size = 0;
        u32 index_count = 0;
        u32 first_vertex = 0;
        u32 first_instance = 0;
        u32 instances = 1;
        bool restart = false;
        bool strip = false;
        bool skinned = false;
    };
    struct Made {
        std::unique_ptr<Entry> entry;
        u32 over = 0xffffffffu;
        bool unsupported = false;
        vk::Result result = vk::Result::eSuccess;
        u64 as_bytes = 0;
        u32 triangles = 0;
        u32 pooled = 0;
        u32 dedicated = 0;
    };
    Made CreateEntry(const Source& source, u8 mask, u64 scene_bytes, u64 at_frame);
    void Settle(const Made& made);
    bool MakeBounds();
    u32 BoundsBox(Entry& entry);
    void ReadBounds(u32 slot);
    void PickOffscreen(const std::array<float, 12>& view, const std::array<float, 12>& inv_view,
                       const std::array<float, 4>& proj, u32 room);
    struct Chunk {
        Buffer buffer;
        VmaVirtualBlock block = nullptr;
    };
    bool Suballocate(std::vector<Chunk>& chunks, bool host, VkBufferUsageFlags usage,
                     u64 chunk_bytes, u64 size, u64 align, Buffer& out, u64& offset, Slice& slice,
                     u64 at_frame, u32& pooled);
    std::unique_lock<std::mutex> PoolLock();
    struct SetupJob {
        RtRules::ReadyMesh mesh;
        Source source;
        std::vector<u8> data;
        u64 scene_bytes = 0;
        u64 frame = 0;
        u8 mask = 0;
        bool shared = false;
    };
    struct SetupResult {
        Made made;
        bool shared = false;
    };
    void SubmitSetup(const Draw& draw, u8 mask, u64 key, bool shared);
    void SetupLoop(std::stop_token stop);
    void HandOffReady();
    void StopSetup();
    void Retire(Entry& entry);
    void FreeNow(Buffer& buffer, Slice& slice);
    void DrainRetired();
    u32 KeepPrevious(Entry& entry);

    enum Over : u32 {
        OverNewCap,
        OverRebuiltCap,
        OverSceneMemory,
        OverVideoMemory,
        OverIndexBuffer,
        OverStorage,
        OverPositions,
        OverScratch,
        OverParams,
        OverDeferred,
        OverBackoff,
        OverCount
    };
    void CountOver(Over reason) noexcept {
        ++stats.over_budget;
        ++missed;
        ++diag_window.over[reason];
    }
    struct DiagTimer {
        u64* sink;
        u64 start;
        explicit DiagTimer(u64* sink_) noexcept;
        ~DiagTimer();
    };
    void DiagFrameEnd();

    const Instance& instance;
    Scheduler& scheduler;
    CameraMotion& camera_motion;

    bool capturing = false;
    bool tried = false;
    bool failed = false;
    bool create_failed = false;
    u64 create_failures = 0;

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
    std::vector<const void*> frame_cameras;
    RtRules::CameraSlots camera_slots;
    bool camera_map = true;
    RtRules::NewEntryBudget new_budget;
    std::array<float, 3> eye{};
    bool eye_valid = false;
    RtRules::RetryGate retry;
    bool evict_hold = false;
    bool keep_previous = true;
    bool rigid = false;
    bool cutout_map = false;
    bool cutout_lamps = false;
    bool pool = false;
    bool pool_full = false;
    u64 pool_retry_frame = 0;
    u32 pool_fails = 0, pool_returned = 0;
    void ReturnEmptyChunks();
    std::vector<Chunk> as_chunks, index_chunks;
    struct Retired {
        std::mutex mutex;
        std::vector<Slice> slices;
    };
    std::shared_ptr<Retired> retired;
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

    bool diag = false;
    struct DiagFrame {
        u64 prepare_ns = 0, make_ns = 0, capture_ns = 0, record_ns = 0, scene_ns = 0, handoff_ns = 0;
        u32 makes = 0, prepares = 0;
    } diag_frame;
    struct PathWindow {
        u64 cameras = 0, cameras_max = 0, deferred_count = 0, deferred_time = 0, backoff = 0,
            tier_made_character = 0, tier_made_near = 0, tier_made_far = 0, deferred_near = 0,
            deferred_far = 0,
            refused = 0, budget_ns = 0, pooled = 0, dedicated = 0, evict_passes = 0, spared = 0,
            kept = 0, not_kept = 0, too_old = 0, refit_instead = 0, rigid_checks = 0, rigid_still = 0, rigid_moved = 0,
            rigid_unknown = 0, rigid_written = 0, rigid_pooled = 0, rigid_refits = 0, rigid_bytes = 0,
            cut_casters = 0, cut_camera = 0, pose_refits = 0, pose_still = 0, pose_waited = 0;
    } path_window;

    bool persistent = false;
    vk::UniqueDescriptorSetLayout bounds_set;
    vk::UniquePipelineLayout bounds_layout;
    vk::UniquePipeline bounds_pipeline;
    Buffer bounds;
    std::array<std::vector<Entry*>, 4> bounds_pending;
    std::vector<Entry*> offscreen;
    std::vector<Entry*> offscreen_candidates;
    std::vector<RtRules::Caster> offscreen_casters;
    u32 offscreen_drawn = UINT32_MAX;
    u32 tlas_drawn = 0;
    u64 pick_far = 0, pick_capped = 0;
    u64 camera_frame = 0, dark_run = 0;
    bool dark = false;
    u32 stale_count = 0;
    struct SceneWindow {
        u64 added = 0, added_max = 0, gone = 0, far = 0, capped = 0, boxes = 0, no_box = 0, kept_static = 0,
            stale = 0, flushes = 0;
    } scene_window;
    struct DiagWindow {
        u64 frames = 0, prepares = 0, prepare_ns = 0, prepare_max_ns = 0, makes = 0,
            make_ns = 0, make_max_ns = 0, built = 0, refit = 0, evicted = 0, capture_ns = 0,
            capture_max_ns = 0, record_ns = 0, record_max_ns = 0, scene_ns = 0, scene_max_ns = 0,
            thread_max_ns = 0, handoff_ns = 0, handoff_max_ns = 0;
        u32 makes_max = 0;
        std::array<u64, OverCount> over{};
        s64 start_ns = 0;
    } diag_window;

    bool setup_worker = false;
    std::mutex pool_mutex;
    std::mutex setup_mutex;
    std::condition_variable_any setup_wake;
    std::vector<SetupJob> setup_jobs;
    std::vector<std::pair<RtRules::ReadyMesh, SetupResult>> setup_done;
    RtRules::HandOff<SetupResult> setup_ready;
    RtRules::SetupKeys setup_keys;
    RtRules::CopyBudget copy_budget;
    std::atomic<u64> setup_busy_ns{0};
    struct SetupWindow {
        u64 sent = 0, copied = 0, landed = 0, retired = 0, failed = 0, waiting = 0, deferred = 0,
            backoff = 0, refused = 0, characters = 0;
    } setup_window;
    std::jthread setup_thread;

    bool free_off_thread = false;
    std::mutex free_mutex;
    std::condition_variable_any free_wake;
    RtRules::TickQueue<Common::UniqueFunction<void>> free_queue;
    std::jthread free_thread;
};

}
