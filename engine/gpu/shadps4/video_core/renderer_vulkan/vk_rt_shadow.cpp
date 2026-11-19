// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_rt_shadow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>

#include <xxhash.h>

#include "bbport_settings.h"
#include "common/thread.h"
#include "lantern_switches.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/host_shaders/rt_shadow_comp.h"
#include "video_core/host_shaders/rt_shadow_filter_comp.h"
#include "video_core/host_shaders/rt_unproject_bounds_comp.h"
#include "video_core/host_shaders/rt_unproject_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_effect_pass.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {
namespace {
constexpr u32 MaxParams = 8192;
static_assert(1 + 4 * MaxParams == Shader::MotionVectors::CaptureParamsCapacity);
constexpr u32 MaxPositions = 8u << 20;
constexpr u32 MaxInstances = 8192;
constexpr u32 MaxInstancesPerDraw = 64;
constexpr u32 MaxTrianglesPerBlas = 2u << 20;
constexpr u32 NewTrianglesPerFrame = 200000;
constexpr u32 RebuiltTrianglesPerFrame = 300000;
constexpr u32 RebuildEveryFrames = 120;
constexpr u32 RebuildSpread = 32;
constexpr u64 UnusedFrames = 240;
constexpr u64 KeptFrames = 2;
constexpr vk::DeviceSize ScratchBytes = 192ull << 20;
constexpr u32 NewEntriesPerFrame = 32;
constexpr u64 RetryFrames = 30;
constexpr u64 AsChunkBytes = 64ull << 20;
constexpr u64 IndexChunkBytes = 16ull << 20;
constexpr u64 SetupCopyBytes = 4ull << 20;
constexpr std::size_t SetupInFlight = 1024;
constexpr u32 LandPerFrame = 96;
constexpr u64 FreeWaitNs = 2'000'000;
constexpr VkBufferUsageFlags InputUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr u8 ShadowMask = 1, CameraMask = 2, CutShadowMask = 4, CutCameraMask = 8;

struct TracePush {
    u32 buf_off;
    u32 frame_index;
    std::array<float, 2> size;
    u32 rays;
    float light_tan;
    float bias_m;
    float max_distance;
};
static_assert(sizeof(TracePush) == 32);

struct FilterPush {
    u32 buf_off;
    s32 radius;
    std::array<float, 2> size;
    u32 keep_map;
};
static_assert(sizeof(FilterPush) == 20);

struct UnprojectPush {
    std::array<float, 12> rows;
    u32 first;
    u32 count;
    u32 box;
};
static_assert(offsetof(UnprojectPush, box) == 56 && sizeof(UnprojectPush) == 60);
constexpr u32 BoxWords = 6;
constexpr u32 NoBox = UINT32_MAX;

constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr auto AsBuild = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;

s64 NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::array<float, 16> Invert(const float* m, bool& ok) {
    std::array<double, 32> t{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            t[r * 8 + c] = m[r * 4 + c];
        }
        t[r * 8 + 4 + r] = 1.0;
    }
    ok = false;
    std::array<float, 16> out{};
    for (int p = 0; p < 4; ++p) {
        int best = p;
        for (int r = p + 1; r < 4; ++r) {
            if (std::abs(t[r * 8 + p]) > std::abs(t[best * 8 + p])) {
                best = r;
            }
        }
        if (std::abs(t[best * 8 + p]) < 1e-12) {
            return out;
        }
        for (int c = 0; c < 8; ++c) {
            std::swap(t[p * 8 + c], t[best * 8 + c]);
        }
        const double scale = 1.0 / t[p * 8 + p];
        for (int c = 0; c < 8; ++c) {
            t[p * 8 + c] *= scale;
        }
        for (int r = 0; r < 4; ++r) {
            if (r != p) {
                const double f = t[r * 8 + p];
                for (int c = 0; c < 8; ++c) {
                    t[r * 8 + c] -= f * t[p * 8 + c];
                }
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = float(t[r * 8 + 4 + c]);
        }
    }
    ok = true;
    return out;
}

u64 IndexHash(const void* indices, u32 count, u32 size) {
    return indices ? XXH3_64bits(indices, std::size_t(count) * size) : 0;
}

template <class Index>
bool AppendTriangles(std::span<const Index> source, bool strip, bool restart, u32 lo,
                     std::vector<u32>& out) {
    const Index cut = std::numeric_limits<Index>::max();
    const auto emit = [&](u32 a, u32 b, u32 c) {
        if (a != b && b != c && a != c) {
            out.insert(out.end(), {a - lo, b - lo, c - lo});
        }
    };
    if (!strip) {
        for (std::size_t i = 0; i + 2 < source.size(); i += 3) {
            if (restart && (source[i] == cut || source[i + 1] == cut || source[i + 2] == cut)) {
                return false;
            }
            emit(source[i], source[i + 1], source[i + 2]);
        }
        return true;
    }
    u32 run = 0;
    u32 a = 0, b = 0;
    for (const Index index : source) {
        if (restart && index == cut) {
            run = 0;
            continue;
        }
        if (run >= 2) {
            emit(a, b, index);
        }
        a = b;
        b = index;
        ++run;
    }
    return true;
}
}

RtShadow::RtShadow(const Instance& instance_, Scheduler& scheduler_, CameraMotion& camera_motion_,
                   const ObjectMotion& object_motion)
    : instance{instance_}, scheduler{scheduler_}, camera_motion{camera_motion_} {
    if (!BbSettings::Get().startup_effects[BbSettings::EffectRayTracing]) {
        return;
    }
    if (!instance.IsRayQuerySupported()) {
        std::printf("Ray tracing: off - this GPU or driver has no ray query\n");
        return;
    }
    if (!instance.GetPhysicalDevice().getFeatures().vertexPipelineStoresAndAtomics) {
        std::printf("Ray tracing: off - no vertex shader stores\n");
        return;
    }
    if (!camera_motion.Enabled()) {
        std::printf("Ray tracing: off - the camera is not tracked (BB_UPSCALER=none)\n");
        return;
    }
    if (const char* env = std::getenv("LANTERN_RT_SCENE_MB"); env && env[0]) {
        blas_limit = u64(std::clamp(std::strtoul(env, nullptr, 10), 64ul, 4096ul)) << 20;
    }
    if (const char* env = std::getenv("LANTERN_RT_SHADOW_RAYS"); env && env[0]) {
        rays = std::clamp(u32(std::strtoul(env, nullptr, 10)), 1u, 16u);
    }
    capturing = Setup(object_motion);
    if (capturing) {
        std::printf("Ray tracing: on, sun %u rays a pixel, lamps and bounced light\n", rays);
        const auto& switches = Lantern::Switches::Get();
        diag = switches.frame_stats;
        if (diag) {
            diag_window.start_ns = NowNs();
            std::printf("Ray tracing CPU: on (BB_FRAME_STATS), a line every 5 s\n");
        }
        camera_map = switches.rt_camera_map;
        std::printf("Ray tracing camera map: %s (LANTERN_RT_CAMERA_MAP=%d)\n",
                    camera_map ? "on" : "off, linear search", camera_map ? 1 : 0);
        if (switches.rt_new_budget > 0) {
            new_budget.max_entries = NewEntriesPerFrame;
            new_budget.max_ns = u64(switches.rt_new_budget) * 1000;
            std::printf("Ray tracing new-entry budget: on (LANTERN_RT_NEW_BUDGET=%ld), %u new entries "
                        "and %ld us of MakeEntry a frame, failed entries wait %llu frames; characters "
                        "never wait, meshes within 25 m use the whole budget, the rest its first half\n",
                        switches.rt_new_budget, NewEntriesPerFrame, switches.rt_new_budget,
                        (unsigned long long)RetryFrames);
        }
        pool = switches.rt_pool;
        if (pool) {
            retired = std::make_shared<Retired>();
            std::printf("Ray tracing pool: on (LANTERN_RT_POOL=1), geometry in %llu MB chunks, indices "
                        "in %llu MB chunks\n",
                        (unsigned long long)(AsChunkBytes >> 20),
                        (unsigned long long)(IndexChunkBytes >> 20));
        }
        keep_previous = switches.rt_keep_previous;
        std::printf("Ray tracing keep previous: %s (LANTERN_RT_KEEP_PREVIOUS=%d)\n",
                    keep_previous ? "on, a mesh over budget keeps its last BLAS for up to 4 frames"
                                  : "off, a mesh over budget leaves the scene for the frame",
                    keep_previous ? 1 : 0);
        cutout_map = switches.rt_cutout;
        if (cutout_map) {
            std::printf("Ray tracing cut-outs: on (LANTERN_RT_CUTOUT=1), sun rays pass animated cut-out "
                        "casters and the sun keeps the game's alpha-tested map for them; lamps and "
                        "bounce still hit them\n");
        }
        cutout_lamps = cutout_map && switches.rt_cutout_lamps;
        if (cutout_lamps) {
            std::printf("Ray tracing cut-out lamps: on (LANTERN_RT_CUTOUT_LAMPS=1), lamp rays pass "
                        "animated cut-outs as sun rays do; bounce still hits them\n");
        }
        rigid = switches.rt_rigid;
        if (rigid) {
            std::printf("Ray tracing rigid motion: on (LANTERN_RT_RIGID=1), a mesh whose bone palette "
                        "differs from the one it was captured with is captured again and refit; a "
                        "GPU-written palette makes it dynamic\n");
        }
        setup_worker = switches.rt_setup_worker;
        if (setup_worker) {
            copy_budget = {.max_bytes = SetupCopyBytes, .max_in_flight = SetupInFlight};
            setup_thread = std::jthread([this](std::stop_token stop) { SetupLoop(stop); });
            std::printf("Ray tracing setup worker: on (LANTERN_RT_SETUP_WORKER=1), new meshes other than "
                        "characters are made on a worker thread from a copy of their indices (%llu KB a "
                        "frame, far meshes the first half, %zu in flight) and join the scene at the next "
                        "frame, up to %u meshes and %u triangles a frame, nearest first; characters are "
                        "made at once\n",
                        (unsigned long long)(SetupCopyBytes >> 10), SetupInFlight, LandPerFrame,
                        NewTrianglesPerFrame);
        }
        free_off_thread = switches.rt_free_off_thread;
        if (free_off_thread) {
            free_thread = std::jthread([this](std::stop_token stop) { FreeLoop(stop); });
            std::printf("Ray tracing frees off thread: on (LANTERN_RT_FREE_OFF_THREAD=1), freed scene "
                        "buffers and acceleration structures go back on their own thread once the GPU "
                        "is done with them, not on the draw thread\n");
        }
        evict_hold = switches.rt_evict_hold;
        if (evict_hold) {
            std::printf("Ray tracing eviction hold: on (LANTERN_RT_EVICT_HOLD=1), above 7/8 of the "
                        "scene frees down to 5/8, sparing what was seen in the last 90 frames until "
                        "the cap\n");
        }
        if (switches.rt_persistent_scene) {
            persistent = MakeBounds();
            if (persistent) {
                std::printf("Ray tracing persistent scene: on (LANTERN_RT_PERSISTENT_SCENE=1), still meshes "
                            "stay %llu frames unseen unless memory runs short or a stretch without a "
                            "camera flushes them, and are checked against their indices when they come "
                            "back; ones drawn in the last %llu frames but not this one cast when wholly "
                            "outside the view and within %.0f m, nearest first; one not drawn inside the "
                            "view stays out until drawn again\n",
                            (unsigned long long)RtRules::StaticFrames,
                            (unsigned long long)RtRules::RecentFrames, double(RtRules::ShadowReachMeters));
            } else {
                std::printf("Ray tracing persistent scene: off - the bounds buffer or pipeline could not "
                            "be made\n");
            }
        }
    } else {
        std::printf("Ray tracing: off - setup failed\n");
    }
}

RtShadow::~RtShadow() {
    StopSetup();
    if (!tried) {
        return;
    }
    scheduler.Finish();
    StopFree();
    const auto device = instance.GetDevice();
    for (auto& [key, entry] : entries) {
        device.destroyAccelerationStructureKHR(entry->handle);
        Free(entry->storage);
        Free(entry->indices);
    }
    const auto drop = [&](SetupResult& result) {
        if (result.made.entry) {
            device.destroyAccelerationStructureKHR(result.made.entry->handle);
            Free(result.made.entry->storage);
            Free(result.made.entry->indices);
        }
    };
    for (auto& [mesh, result] : setup_done) {
        drop(result);
    }
    setup_ready.Drain([&](const RtRules::ReadyMesh&, SetupResult&& result) { drop(result); });
    for (auto* chunks : {&as_chunks, &index_chunks}) {
        for (Chunk& chunk : *chunks) {
            vmaClearVirtualBlock(chunk.block);
            vmaDestroyVirtualBlock(chunk.block);
            Free(chunk.buffer);
        }
    }
    if (tlas) {
        device.destroyAccelerationStructureKHR(tlas);
    }
    if (raw_view) {
        device.destroyImageView(raw_view);
    }
    if (raw_image) {
        vmaDestroyImage(instance.GetAllocator(), raw_image, raw_allocation);
    }
    for (Buffer* b : {&capture_addresses, &both_addresses, &params, &positions, &scratch,
                      &instance_data, &tlas_storage, &tlas_scratch, &bounds}) {
        Free(*b);
    }
}

RtShadow::DiagTimer::DiagTimer(u64* sink_) noexcept
    : sink{sink_}, start{sink_ ? u64(NowNs()) : 0} {}

RtShadow::DiagTimer::~DiagTimer() {
    if (sink) {
        *sink += u64(NowNs()) - start;
    }
}

void RtShadow::DiagFrameEnd() {
    auto& w = diag_window;
    const auto& f = diag_frame;
    ++w.frames;
    w.prepares += f.prepares;
    w.prepare_ns += f.prepare_ns;
    w.prepare_max_ns = std::max(w.prepare_max_ns, f.prepare_ns);
    w.makes += f.makes;
    w.make_ns += f.make_ns;
    w.make_max_ns = std::max(w.make_max_ns, f.make_ns);
    w.makes_max = std::max(w.makes_max, f.makes);
    const u64 capture_only = f.capture_ns > f.prepare_ns ? f.capture_ns - f.prepare_ns : 0;
    w.capture_ns += capture_only;
    w.capture_max_ns = std::max(w.capture_max_ns, capture_only);
    w.record_ns += f.record_ns;
    w.record_max_ns = std::max(w.record_max_ns, f.record_ns);
    w.scene_ns += f.scene_ns;
    w.scene_max_ns = std::max(w.scene_max_ns, f.scene_ns);
    w.handoff_ns += f.handoff_ns;
    w.handoff_max_ns = std::max(w.handoff_max_ns, f.handoff_ns);
    w.thread_max_ns = std::max(w.thread_max_ns, f.capture_ns + f.record_ns + f.scene_ns + f.handoff_ns);
    diag_frame = {};
    const s64 now = NowNs();
    if (now - w.start_ns < 5'000'000'000ll) {
        return;
    }
    const double frames = double(std::max<u64>(w.frames, 1));
    const double seconds = double(now - w.start_ns) / 1e9;
    const auto ms = [](u64 ns) { return double(ns) / 1e6; };
    const auto per = [&](u64 v) { return double(v) / frames; };
    std::printf("Ray tracing CPU (%.1f s, %llu frames): PrepareDraw %.1f draws, %.3f ms a frame, max "
                "%.3f; MakeEntry %.2f a frame, max %u, %.3f ms a frame, max %.3f; over budget a frame: "
                "new cap %.2f, rebuilt cap %.2f, scene memory %.2f, video memory %.2f, index buffer "
                "%.2f, storage %.2f, positions %.2f, scratch %.2f, params %.2f; BLAS built %.2f, refit "
                "%.2f, evicted %.2f a frame; %zu BLAS, scene %.0f MB\n",
                seconds, (unsigned long long)w.frames, per(w.prepares), ms(w.prepare_ns) / frames,
                ms(w.prepare_max_ns), per(w.makes), w.makes_max, ms(w.make_ns) / frames,
                ms(w.make_max_ns), per(w.over[OverNewCap]), per(w.over[OverRebuiltCap]),
                per(w.over[OverSceneMemory]), per(w.over[OverVideoMemory]),
                per(w.over[OverIndexBuffer]), per(w.over[OverStorage]), per(w.over[OverPositions]),
                per(w.over[OverScratch]), per(w.over[OverParams]), per(w.built), per(w.refit),
                per(w.evicted), entries.size(), double(blas_bytes) / 1048576.0);
    std::printf("Ray tracing draw thread (%.1f s): capture %.3f ms a frame, max %.3f; PrepareDraw %.3f; "
                "build recording %.3f, max %.3f; scene %.3f, max %.3f; all of it %.3f ms a frame, max %.3f\n",
                seconds, ms(w.capture_ns) / frames, ms(w.capture_max_ns), ms(w.prepare_ns) / frames,
                ms(w.record_ns) / frames, ms(w.record_max_ns), ms(w.scene_ns) / frames, ms(w.scene_max_ns),
                ms(w.capture_ns + w.prepare_ns + w.record_ns + w.scene_ns + w.handoff_ns) / frames,
                ms(w.thread_max_ns));
    if (setup_worker) {
        const auto& u = setup_window;
        std::printf("Ray tracing setup worker (LANTERN_RT_SETUP_WORKER=1): a frame %.2f meshes sent with "
                    "%.1f KB of indices copied, %.2f landed, %.2f retired, %.2f failed; %.2f draws waited "
                    "for the worker, %.2f deferred by the copy budget, %.2f waiting after a failure, %.2f "
                    "refused for scene memory; %.2f characters made on the draw thread; hand-off %.3f ms "
                    "a frame, max %.3f; worker busy %.3f ms a frame; %zu in flight, %zu ready\n",
                    per(u.sent), per(u.copied) / 1024.0, per(u.landed), per(u.retired), per(u.failed),
                    per(u.waiting), per(u.deferred), per(u.backoff), per(u.refused), per(u.characters),
                    ms(w.handoff_ns) / frames, ms(w.handoff_max_ns),
                    ms(setup_busy_ns.exchange(0, std::memory_order_relaxed)) / frames, setup_keys.Size(),
                    setup_ready.Size());
        setup_window = {};
    }
    const auto& p = path_window;
    std::printf("Ray tracing camera map (LANTERN_RT_CAMERA_MAP=%d): %.1f cameras a frame, max %llu\n",
                camera_map ? 1 : 0, per(p.cameras), (unsigned long long)p.cameras_max);
    std::printf("Ray tracing keep previous (LANTERN_RT_KEEP_PREVIOUS=%d): a frame %.2f meshes over "
                "budget kept their last BLAS, %.2f left the scene (%.2f of them older than %llu "
                "frames), %.2f due rebuilds refit instead\n",
                keep_previous ? 1 : 0, per(p.kept), per(p.not_kept), per(p.too_old),
                (unsigned long long)RtRules::KeepFrames, per(p.refit_instead));
    if (new_budget.On()) {
        std::printf("Ray tracing new-entry budget (LANTERN_RT_NEW_BUDGET): a frame %.2f deferred for "
                    "time, %.2f for count, %.2f waiting after a failure, %.2f refused before the "
                    "scan; MakeEntry %.3f ms a frame; %zu keys waiting; made a frame %.2f characters "
                    "(never wait), %.2f within 25 m, %.2f farther or unknown (first half of the "
                    "budget only); deferred %.2f near, %.2f far\n",
                    per(p.deferred_time), per(p.deferred_count), per(p.backoff), per(p.refused),
                    ms(p.budget_ns) / frames, retry.Size(), per(p.tier_made_character),
                    per(p.tier_made_near), per(p.tier_made_far), per(p.deferred_near),
                    per(p.deferred_far));
    }
    if (pool) {
        const auto pool_lock = PoolLock();
        u64 as_bytes = 0, index_bytes = 0;
        for (const auto* chunks : {&as_chunks, &index_chunks}) {
            for (const Chunk& chunk : *chunks) {
                VmaStatistics used{};
                vmaGetVirtualBlockStatistics(chunk.block, &used);
                (chunks == &as_chunks ? as_bytes : index_bytes) += used.allocationBytes;
            }
        }
        std::printf("Ray tracing pool (LANTERN_RT_POOL=1): %zu geometry chunks, %.0f MB used; %zu "
                    "index chunks, %.0f MB used; %.2f pooled and %.2f own buffers a frame%s\n",
                    as_chunks.size(), double(as_bytes) / 1048576.0, index_chunks.size(),
                    double(index_bytes) / 1048576.0, per(p.pooled), per(p.dedicated),
                    pool_full ? "; no room for more chunks now" : "");
        if (pool_returned) {
            std::printf("Ray tracing pool (LANTERN_RT_POOL=1): %u empty chunks returned\n", pool_returned);
            pool_returned = 0;
        }
    }
    if (cutout_map) {
        std::printf("Ray tracing cut-outs (LANTERN_RT_CUTOUT=1): a frame %.1f casters left to the "
                    "map, %.1f camera draws\n",
                    per(p.cut_casters), per(p.cut_camera));
    }
    if (rigid) {
        std::printf("Ray tracing rigid motion (LANTERN_RT_RIGID=1): a frame %.1f checks, %.1f still, "
                    "%.2f moved, %.1f unknown (%.1f on pools over 16 KiB), %.2f GPU-written, %.2f "
                    "captured again, %.1f KB of palettes hashed\n",
                    per(p.rigid_checks), per(p.rigid_still), per(p.rigid_moved),
                    per(p.rigid_unknown), per(p.rigid_pooled), per(p.rigid_written), per(p.rigid_refits),
                    per(p.rigid_bytes) / 1024.0);
        std::printf("Ray tracing moving meshes (LANTERN_RT_RIGID=1): a frame %.1f refit, %.1f unchanged and kept, "
                    "%.1f seen only by bounce rays waiting for their turn (one refit in %llu frames)\n",
                    per(p.pose_refits), per(p.pose_still), per(p.pose_waited),
                    (unsigned long long)RtRules::CutRefitEvery);
    }
    if (evict_hold) {
        std::printf("Ray tracing eviction hold (LANTERN_RT_EVICT_HOLD=1): %.2f passes, %.2f entries "
                    "spared a frame, %.2f evicted a frame\n",
                    per(p.evict_passes), per(p.spared), per(w.evicted));
    }
    if (persistent) {
        const auto& s = scene_window;
        std::printf("Ray tracing persistent scene (LANTERN_RT_PERSISTENT_SCENE=1): a frame %.1f casters "
                    "outside the view added, max %llu; %.2f gone (in the view, not drawn), %.1f beyond "
                    "%.0f m, %.2f over the cap; boxes read %.1f, without a box %.2f; %llu still meshes "
                    "past the unused limit kept at the last pass; %llu came back with other indices "
                    "and were made again; %llu flush passes after a stretch without a camera\n",
                    per(s.added), (unsigned long long)s.added_max, per(s.gone), per(s.far),
                    double(RtRules::ShadowReachMeters), per(s.capped), per(s.boxes), per(s.no_box),
                    (unsigned long long)s.kept_static, (unsigned long long)s.stale,
                    (unsigned long long)s.flushes);
        const u64 kept_static = s.kept_static;
        scene_window = {};
        scene_window.kept_static = kept_static;
    }
    path_window = {};
    w = {};
    w.start_ns = now;
}

bool RtShadow::Make(Buffer& out, vk::DeviceSize size, bool host, VkBufferUsageFlags usage) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO};
    if (host) {
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &raw, &out.allocation,
                        &info) != VK_SUCCESS) {
        return false;
    }
    out.buffer = raw;
    out.mapped = info.pMappedData;
    out.address = instance.GetDevice().getBufferAddress({.buffer = out.buffer});
    return out.address != 0;
}

void RtShadow::Free(Buffer& buffer) {
    if (buffer.buffer && buffer.allocation) {
        vmaDestroyBuffer(instance.GetAllocator(), buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

bool RtShadow::MakeBounds() {
    using enum vk::DescriptorType;
    if (!EffectPass::Create(instance, bounds_set, bounds_layout, bounds_pipeline, {eStorageBuffer, eStorageBuffer},
                            sizeof(UnprojectPush), RT_UNPROJECT_BOUNDS_COMP, "Ray tracing positions and bounds")) {
        return false;
    }
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(4) * MaxParams * BoxWords * sizeof(u32),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &raw, &bounds.allocation, &info) !=
        VK_SUCCESS) {
        return false;
    }
    bounds.buffer = raw;
    bounds.mapped = info.pMappedData;
    for (auto& pending : bounds_pending) {
        pending.reserve(MaxParams);
    }
    return bounds.mapped != nullptr;
}

u32 RtShadow::BoundsBox(Entry& entry) {
    if (!persistent || entry.dynamic || entry.rebuilt) {
        return NoBox;
    }
    const u32 slot = u32(frame % 4);
    auto& pending = bounds_pending[slot];
    if (pending.size() >= MaxParams) {
        return NoBox;
    }
    if (entry.box_pending != NoBox) {
        bounds_pending[entry.box_slot][entry.box_pending] = nullptr;
    }
    const u32 box = slot * MaxParams + u32(pending.size());
    u32* words = static_cast<u32*>(bounds.mapped) + u64(box) * BoxWords;
    for (u32 i = 0; i < 3; ++i) {
        words[i] = RtRules::EmptyLow;
        words[3 + i] = RtRules::EmptyHigh;
    }
    entry.box_slot = u8(slot);
    entry.box_pending = u32(pending.size());
    pending.push_back(&entry);
    slot_ticks[slot] = scheduler.CurrentTick();
    return box;
}

void RtShadow::ReadBounds(u32 slot) {
    auto& pending = bounds_pending[slot];
    const u32* words = static_cast<const u32*>(bounds.mapped) + u64(slot) * MaxParams * BoxWords;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        Entry* entry = pending[i];
        if (!entry) {
            continue;
        }
        entry->has_box = RtRules::BoxFromBits(words + i * BoxWords, entry->box);
        entry->box_pending = NoBox;
        ++(entry->has_box ? scene_window.boxes : scene_window.no_box);
    }
    pending.clear();
}

void RtShadow::PickOffscreen(const std::array<float, 12>& view, const std::array<float, 12>& inv_view,
                             const std::array<float, 4>& proj, u32 room) {
    offscreen.clear();
    offscreen_candidates.clear();
    offscreen_casters.clear();
    const std::array<float, 3> at{inv_view[3], inv_view[7], inv_view[11]};
    u64 far = 0;
    for (auto& [key, item] : entries) {
        Entry& entry = *item;
        if (entry.seen_frame == frame || !entry.has_box || entry.stale ||
            !RtRules::RecentCaster(frame, entry.seen_frame) ||
            !RtRules::StaticCaster(entry.built, entry.dynamic, entry.rebuilt) ||
            RtRules::Hidden(entry.seen_frame, entry.gone_frame)) {
            continue;
        }
        const float distance = RtRules::BoxDistance(entry.box, at.data());
        switch (RtRules::ClassifyUndrawn(RtRules::OutsideFrustum(entry.box, view, proj), distance,
                                         RtRules::ShadowReachMeters)) {
        case RtRules::Undrawn::Add:
            offscreen_casters.push_back({u32(offscreen_candidates.size()), distance});
            offscreen_candidates.push_back(&entry);
            break;
        case RtRules::Undrawn::Gone:
            entry.gone_frame = frame;
            ++scene_window.gone;
            break;
        case RtRules::Undrawn::Far:
            ++far;
            break;
        }
    }
    pick_capped = RtRules::KeepNearest(offscreen_casters, room);
    pick_far = far;
    for (const RtRules::Caster& caster : offscreen_casters) {
        offscreen.push_back(offscreen_candidates[caster.id]);
    }
}

void RtShadow::Destroy(Entry& entry) {
    if (entry.box_pending != NoBox) {
        bounds_pending[entry.box_slot][entry.box_pending] = nullptr;
        entry.box_pending = NoBox;
    }
    blas_bytes -= std::min(blas_bytes, entry.bytes);
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    DeferFree([device, allocator, handle = entry.handle, storage = entry.storage,
               indices = entry.indices, retired = retired,
               slices = std::array{entry.storage_slice, entry.index_slice}] {
        device.destroyAccelerationStructureKHR(handle);
        if (storage.allocation) {
            vmaDestroyBuffer(allocator, storage.buffer, storage.allocation);
        }
        if (indices.allocation) {
            vmaDestroyBuffer(allocator, indices.buffer, indices.allocation);
        }
        if (retired && (slices[0].block || slices[1].block)) {
            std::scoped_lock lock{retired->mutex};
            for (const Slice& slice : slices) {
                if (slice.block) {
                    retired->slices.push_back(slice);
                }
            }
        }
    });
}

void RtShadow::DeferFree(Common::UniqueFunction<void>&& free) {
    if (!free_off_thread) {
        scheduler.DeferOperation(std::move(free));
        return;
    }
    {
        std::scoped_lock lock{free_mutex};
        free_queue.Add(scheduler.CurrentTick(), std::move(free));
    }
    free_wake.notify_one();
}

void RtShadow::FreeLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("bb:RtFree");
    Semaphore& semaphore = *scheduler.GetWorkSemaphore();
    const auto device = instance.GetDevice();
    const vk::Semaphore handle = semaphore.Handle();
    while (true) {
        u64 tick = 0;
        {
            std::unique_lock lock{free_mutex};
            if (!free_wake.wait(lock, stop, [&] { return !free_queue.Empty(); })) {
                return;
            }
            tick = free_queue.FirstTick();
        }
        if (!semaphore.IsFree(tick)) {
            semaphore.Refresh();
        }
        while (!semaphore.IsFree(tick)) {
            if (stop.stop_requested()) {
                return;
            }
            const vk::SemaphoreWaitInfo wait_info = {
                .semaphoreCount = 1,
                .pSemaphores = &handle,
                .pValues = &tick,
            };
            const vk::Result result = device.waitSemaphores(&wait_info, FreeWaitNs);
            if (result == vk::Result::eSuccess) {
                semaphore.Refresh();
            } else if (result != vk::Result::eTimeout) {
                return;
            }
        }
        std::vector<Common::UniqueFunction<void>> done;
        {
            std::scoped_lock lock{free_mutex};
            done = free_queue.TakeDone(semaphore.KnownGpuTick());
        }
        for (auto& free : done) {
            free();
        }
    }
}

void RtShadow::StopFree() {
    if (!free_off_thread) {
        return;
    }
    free_thread.request_stop();
    free_thread.join();
    free_off_thread = false;
    Semaphore& semaphore = *scheduler.GetWorkSemaphore();
    semaphore.Refresh();
    std::vector<Common::UniqueFunction<void>> done;
    {
        std::scoped_lock lock{free_mutex};
        done = free_queue.TakeDone(semaphore.KnownGpuTick());
    }
    for (auto& free : done) {
        free();
    }
}

std::unique_lock<std::mutex> RtShadow::PoolLock() {
    return setup_worker ? std::unique_lock{pool_mutex} : std::unique_lock<std::mutex>{};
}

bool RtShadow::Suballocate(std::vector<Chunk>& chunks, bool host, VkBufferUsageFlags usage,
                           u64 chunk_bytes, u64 size, u64 align, Buffer& out, u64& offset,
                           Slice& slice, u64 at_frame, u32& pooled) {
    if (!pool || size > chunk_bytes / 2) {
        return false;
    }
    auto pool_lock = PoolLock();
    const VmaVirtualAllocationCreateInfo request{.size = size, .alignment = align};
    const auto take = [&](const Chunk& chunk) {
        VmaVirtualAllocation allocation{};
        VkDeviceSize at = 0;
        if (vmaVirtualAllocate(chunk.block, &request, &allocation, &at) != VK_SUCCESS) {
            return false;
        }
        slice = {chunk.block, allocation};
        offset = at;
        out = {.buffer = chunk.buffer.buffer,
               .mapped = chunk.buffer.mapped ? static_cast<u8*>(chunk.buffer.mapped) + at : nullptr,
               .address = chunk.buffer.address + at};
        ++pooled;
        return true;
    };
    for (const Chunk& chunk : chunks) {
        if (take(chunk)) {
            return true;
        }
    }
    if (pool_full && at_frame < pool_retry_frame) {
        return false;
    }
    if (pool_lock.owns_lock()) {
        pool_lock.unlock();
    }
    Chunk chunk;
    const VmaVirtualBlockCreateInfo block_ci{.size = chunk_bytes};
    const bool created = EffectPass::HasRoom(instance, chunk_bytes) &&
                         Make(chunk.buffer, chunk_bytes, host, usage) &&
                         vmaCreateVirtualBlock(&block_ci, &chunk.block) == VK_SUCCESS;
    if (pool_lock.mutex()) {
        pool_lock.lock();
    }
    if (!created) {
        Free(chunk.buffer);
        if (!pool_full) {
            std::printf("Ray tracing pool: no room for another %llu MB chunk (%zu geometry, %zu index "
                        "chunks); new geometry gets its own buffers, another chunk is tried every "
                        "120 frames\n",
                        (unsigned long long)(chunk_bytes >> 20), as_chunks.size(), index_chunks.size());
        }
        pool_full = true;
        pool_retry_frame = at_frame + 120;
        ++pool_fails;
        return false;
    }
    if (pool_full) {
        std::printf("Ray tracing pool: chunks again after %u failed tries\n", pool_fails);
    }
    pool_full = false;
    pool_fails = 0;
    chunks.push_back(chunk);
    return take(chunks.back());
}

void RtShadow::ReturnEmptyChunks() {
    const auto pool_lock = PoolLock();
    const auto allocator = instance.GetAllocator();
    for (auto* chunks : {&as_chunks, &index_chunks}) {
        std::erase_if(*chunks, [&](Chunk& chunk) {
            if (vmaIsVirtualBlockEmpty(chunk.block) != VK_TRUE) {
                return false;
            }
            vmaDestroyVirtualBlock(chunk.block);
            DeferFree([allocator, buffer = chunk.buffer] {
                vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
            });
            ++pool_returned;
            return true;
        });
    }
}

void RtShadow::FreeNow(Buffer& buffer, Slice& slice) {
    if (slice.block) {
        const auto pool_lock = PoolLock();
        vmaVirtualFree(slice.block, slice.allocation);
        slice = {};
        buffer = {};
        return;
    }
    Free(buffer);
}

void RtShadow::DrainRetired() {
    if (!retired) {
        return;
    }
    std::vector<Slice> slices;
    {
        std::scoped_lock lock{retired->mutex};
        slices.swap(retired->slices);
    }
    const auto pool_lock = PoolLock();
    for (const Slice& slice : slices) {
        vmaVirtualFree(slice.block, slice.allocation);
    }
}

bool RtShadow::Setup(const ObjectMotion& object_motion) {
    tried = true;
    const auto device = instance.GetDevice();
    using enum vk::DescriptorType;
    const VkBufferUsageFlags as_storage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    if (!EffectPass::StorageFormat(instance, vk::Format::eR8Unorm) ||
        !EffectPass::StorageFormat(instance, vk::Format::eR8G8B8A8Unorm) ||
        !EffectPass::CreateSampler(instance, sampler, vk::Filter::eNearest) ||
        !EffectPass::Create(instance, trace_set, trace_layout, trace_pipeline,
                            {eStorageBuffer, eCombinedImageSampler, eStorageImage,
                             eAccelerationStructureKHR},
                            sizeof(TracePush), RT_SHADOW_COMP, "Ray traced shadows") ||
        !EffectPass::Create(instance, filter_set, filter_layout, filter_pipeline,
                            {eStorageBuffer, eCombinedImageSampler, eCombinedImageSampler,
                             eStorageImage, eStorageBuffer},
                            sizeof(FilterPush), RT_SHADOW_FILTER_COMP, "Ray traced shadow filter") ||
        !EffectPass::Create(instance, unproject_set, unproject_layout, unproject_pipeline,
                            {eStorageBuffer}, u32(offsetof(UnprojectPush, box)), RT_UNPROJECT_COMP,
                            "Ray tracing positions")) {
        return false;
    }
    if (!Make(params, vk::DeviceSize(1 + 4 * MaxParams) * 32, true, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !Make(positions, vk::DeviceSize(MaxPositions) * 16, false,
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | InputUsage) ||
        !Make(scratch, ScratchBytes, false, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !Make(instance_data,
              vk::DeviceSize(4 * MaxInstances) * sizeof(vk::AccelerationStructureInstanceKHR), true,
              InputUsage) ||
        !Make(capture_addresses, 16, true, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !Make(both_addresses, 32, true, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
        return false;
    }
    std::memset(params.mapped, 0, 32);
    const std::array<u64, 2> capture_table{params.address, positions.address};
    std::memcpy(capture_addresses.mapped, capture_table.data(), sizeof(capture_table));
    const auto motion_table = object_motion.AddressTable();
    const std::array<u64, 4> both_table{motion_table.params, motion_table.positions, params.address,
                                        positions.address};
    std::memcpy(both_addresses.mapped, both_table.data(), sizeof(both_table));
    address_layout = ObjectMotion::CreateAddressLayout(device);
    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eUniformBuffer, 2};
    auto [pool_result, pool] = device.createDescriptorPoolUnique(
        {.maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &pool_size});
    if (pool_result != vk::Result::eSuccess) {
        return false;
    }
    address_pool = std::move(pool);
    const std::array<vk::DescriptorSetLayout, 2> layouts{*address_layout, *address_layout};
    auto [set_result, sets] = device.allocateDescriptorSets(
        {.descriptorPool = *address_pool, .descriptorSetCount = 2, .pSetLayouts = layouts.data()});
    if (set_result != vk::Result::eSuccess) {
        return false;
    }
    capture_set = sets[0];
    both_set = sets[1];
    const vk::DescriptorBufferInfo capture_info{capture_addresses.buffer, 0, 16};
    const vk::DescriptorBufferInfo both_info{both_addresses.buffer, 0, 32};
    const std::array<vk::WriteDescriptorSet, 2> writes{{
        {.dstSet = capture_set, .dstBinding = Shader::MotionVectors::AddressBinding,
         .descriptorCount = 1, .descriptorType = vk::DescriptorType::eUniformBuffer,
         .pBufferInfo = &capture_info},
        {.dstSet = both_set, .dstBinding = Shader::MotionVectors::AddressBinding,
         .descriptorCount = 1, .descriptorType = vk::DescriptorType::eUniformBuffer,
         .pBufferInfo = &both_info},
    }};
    device.updateDescriptorSets(writes, {});

    const vk::AccelerationStructureGeometryKHR geometry{
        .geometryType = vk::GeometryTypeKHR::eInstances,
        .geometry = {.instances = vk::AccelerationStructureGeometryInstancesDataKHR{}},
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };
    const vk::AccelerationStructureBuildGeometryInfoKHR info{
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
    const u32 max_instances = MaxInstances;
    const auto sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, info, max_instances);
    if (!Make(tlas_storage, sizes.accelerationStructureSize, false, as_storage) ||
        !Make(tlas_scratch, sizes.buildScratchSize, false, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        return false;
    }
    const auto [result, handle] = device.createAccelerationStructureKHR({
        .buffer = tlas_storage.buffer,
        .size = sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
    });
    if (result != vk::Result::eSuccess) {
        return false;
    }
    tlas = handle;
    scratch_align = std::max<u64>(
        1, instance.GetPhysicalDevice()
               .getProperties2<vk::PhysicalDeviceProperties2,
                               vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
               .get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
               .minAccelerationStructureScratchOffsetAlignment);
    return true;
}

bool RtShadow::Wanted() const {
    return capturing && !failed &&
           BbSettings::Get().effects[BbSettings::EffectRayTracing].load(std::memory_order_relaxed);
}

bool RtShadow::Complete() const {
    return Wanted() && !frame_entries.empty() &&
           camera_motion.PassMatrices().valid;
}

void RtShadow::BindAddresses(vk::PipelineLayout layout, u32 mode) {
    const vk::DescriptorSet set = mode == 3u ? both_set : capture_set;
    scheduler.Record([layout, set](vk::CommandBuffer command) {
        command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout,
                                   Shader::MotionVectors::DescriptorSet, set, {});
    });
}

bool RtShadow::ToWorld(const float* constants, bool perspective, std::array<float, 12>& out) const {
    const float* proj = constants + 52;
    const bool ortho = proj[12] == 0.0f && proj[13] == 0.0f && proj[14] == 0.0f && proj[15] == 1.0f;
    if (!ortho && !perspective) {
        return false;
    }
    bool ok = false;
    const auto inverse = Invert(proj, ok);
    if (!ok) {
        return false;
    }
    const float* inv_view = constants + 180;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = inv_view[r * 4 + 3] * inverse[12 + c];
            for (int k = 0; k < 3; ++k) {
                sum += inv_view[r * 4 + k] * inverse[k * 4 + c];
            }
            out[r * 4 + c] = sum;
        }
    }
    return std::ranges::all_of(out, [](float v) { return std::isfinite(v); });
}

u32 RtShadow::Param(u32 store, const Entry& entry) {
    if (params_used >= MaxParams) {
        return 0;
    }
    const u32 slot = u32(frame % 4);
    const u32 index = 1 + slot * MaxParams + params_used++;
    auto* param = static_cast<u32*>(params.mapped) + index * 8;
    param[0] = store;
    param[1] = 0;
    param[2] = entry.vertices;
    param[3] = Shader::MotionVectors::FlagStore;
    param[4] = entry.first_vertex;
    param[5] = entry.first_instance;
    param[6] = entry.instances;
    param[7] = 0;
    slot_ticks[slot] = scheduler.CurrentTick();
    return index;
}

std::unique_ptr<RtShadow::Entry> RtShadow::MakeEntry(const Draw& draw, u8 mask) {
    Made made = CreateEntry({.indices = reinterpret_cast<const void*>(draw.indices),
                       .palette = draw.palette,
                       .index_size = draw.index_size,
                       .index_count = draw.index_count,
                       .first_vertex = draw.first_vertex,
                       .first_instance = draw.first_instance,
                       .instances = draw.instances,
                       .restart = draw.restart,
                       .strip = draw.strip,
                       .skinned = draw.skinned},
                      mask, blas_bytes, frame);
    Settle(made);
    if (made.entry) {
        blas_bytes += made.entry->bytes;
    }
    return std::move(made.entry);
}

void RtShadow::Settle(const Made& made) {
    path_window.pooled += made.pooled;
    path_window.dedicated += made.dedicated;
    if (made.unsupported) {
        ++stats.unsupported;
    }
    if (made.over < OverCount) {
        CountOver(Over(made.over));
    }
    if (made.result == vk::Result::eSuccess) {
        return;
    }
    create_failed = true;
    ++create_failures;
    if (made.result == vk::Result::eErrorDeviceLost) {
        failed = true;
        std::printf("Ray tracing: off - the device was lost making a geometry structure\n");
    } else if (create_failures == 1 || create_failures % 256 == 0) {
        std::printf("Ray tracing: a geometry structure of %llu bytes (%u triangles) could not be made "
                    "(%s, %llu so far); that object waits and tries again, ray tracing stays on\n",
                    static_cast<unsigned long long>(made.as_bytes), made.triangles,
                    vk::to_string(made.result).c_str(), static_cast<unsigned long long>(create_failures));
    }
}

RtShadow::Made RtShadow::CreateEntry(const Source& draw, u8 mask, u64 scene_bytes, u64 at_frame) {
    const auto device = instance.GetDevice();
    Made made;
    u32 lo = UINT32_MAX, hi = 0;
    const auto scan = [&]<class Index>(const Index* data) {
        const Index cut = std::numeric_limits<Index>::max();
        for (u32 i = 0; i < draw.index_count; ++i) {
            if (draw.restart && data[i] == cut) {
                continue;
            }
            lo = std::min(lo, u32(data[i]));
            hi = std::max(hi, u32(data[i]));
        }
    };
    std::vector<u32> list;
    list.reserve(std::size_t(draw.index_count) * (draw.strip ? 3 : 1));
    const void* source = draw.indices;
    bool good = false;
    if (draw.index_size == 2) {
        scan(static_cast<const u16*>(source));
        good = lo <= hi && AppendTriangles<u16>({static_cast<const u16*>(source), draw.index_count},
                                                draw.strip, draw.restart, lo, list);
    } else {
        scan(static_cast<const u32*>(source));
        good = lo <= hi && AppendTriangles<u32>({static_cast<const u32*>(source), draw.index_count},
                                                draw.strip, draw.restart, lo, list);
    }
    const u64 vertex_count = u64(hi - lo + 1);
    if (!good || list.empty() || list.size() / 3 * draw.instances > MaxTrianglesPerBlas ||
        vertex_count * draw.instances > MaxPositions / 4) {
        made.unsupported = true;
        return made;
    }
    const std::size_t per_instance = list.size();
    list.resize(per_instance * draw.instances);
    for (u32 i = 1; i < draw.instances; ++i) {
        for (std::size_t k = 0; k < per_instance; ++k) {
            list[i * per_instance + k] = list[k] + i * u32(vertex_count);
        }
    }
    auto entry = std::make_unique<Entry>();
    entry->first_vertex = u32(s64(lo) + s64(s32(draw.first_vertex)));
    entry->vertices = u32(vertex_count);
    entry->instances = draw.instances;
    entry->first_instance = draw.first_instance;
    entry->triangles = u32(list.size() / 3);
    entry->palette = draw.palette;
    entry->index_hash = persistent ? IndexHash(draw.indices, draw.index_count, draw.index_size) : 0;
    entry->dynamic = draw.skinned;
    entry->mask = mask;
    if (scene_bytes + vk::DeviceSize(entry->triangles) * 64 > blas_limit) {
        made.over = OverSceneMemory;
        return made;
    }
    if (!EffectPass::HasRoom(instance, vk::DeviceSize(entry->triangles) * 96)) {
        made.over = OverVideoMemory;
        return made;
    }
    const u64 index_bytes = list.size() * sizeof(u32);
    u64 index_offset = 0;
    if (!Suballocate(index_chunks, true, InputUsage, IndexChunkBytes, index_bytes, 16, entry->indices,
                     index_offset, entry->index_slice, at_frame, made.pooled)) {
        made.dedicated += pool ? 1 : 0;
        if (!Make(entry->indices, index_bytes, true, InputUsage)) {
            made.over = OverIndexBuffer;
            return made;
        }
    }
    std::memcpy(entry->indices.mapped, list.data(), list.size() * sizeof(u32));
    vk::AccelerationStructureGeometryKHR geometry{
        .geometryType = vk::GeometryTypeKHR::eTriangles,
        .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
            .vertexFormat = vk::Format::eR32G32B32Sfloat,
            .vertexStride = 16,
            .maxVertex = entry->vertices * entry->instances - 1,
            .indexType = vk::IndexType::eUint32,
        }},
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };
    const vk::AccelerationStructureBuildGeometryInfoKHR info{
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace |
                 vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
    const auto sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, info, entry->triangles);
    entry->scratch = sizes.buildScratchSize;
    entry->update_scratch = sizes.updateScratchSize;
    entry->bytes = sizes.accelerationStructureSize + list.size() * sizeof(u32);
    if (!Suballocate(as_chunks, false, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                     AsChunkBytes, sizes.accelerationStructureSize, 256, entry->storage,
                     entry->storage_offset, entry->storage_slice, at_frame, made.pooled)) {
        made.dedicated += pool ? 1 : 0;
        if (!Make(entry->storage, sizes.accelerationStructureSize, false,
                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR)) {
            FreeNow(entry->indices, entry->index_slice);
            made.over = OverStorage;
            return made;
        }
    }
    const auto [result, handle] = device.createAccelerationStructureKHR({
        .buffer = entry->storage.buffer,
        .offset = entry->storage_offset,
        .size = sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
    });
    if (result != vk::Result::eSuccess) {
        FreeNow(entry->indices, entry->index_slice);
        FreeNow(entry->storage, entry->storage_slice);
        made.over = OverStorage;
        made.result = result;
        made.as_bytes = sizes.accelerationStructureSize;
        made.triangles = entry->triangles;
        return made;
    }
    entry->handle = handle;
    entry->address = device.getAccelerationStructureAddressKHR({.accelerationStructure = handle});
    made.entry = std::move(entry);
    return made;
}

void RtShadow::Retire(Entry& entry) {
    entry.bytes = 0;
    Destroy(entry);
}

void RtShadow::SubmitSetup(const Draw& draw, u8 mask, u64 key, bool shared) {
    if (setup_keys.Pending(key)) {
        ++setup_window.waiting;
        return;
    }
    if (retry.Blocked(key, frame)) {
        ++setup_window.backoff;
        CountOver(OverBackoff);
        return;
    }
    const u64 reserve = RtRules::TriangleBound(draw.index_count, draw.instances, draw.strip) * 64;
    if (!RtRules::FitsWithPending(blas_bytes, setup_keys.Bytes(), blas_limit, reserve)) {
        if (RtRules::FitsScene(blas_bytes, blas_limit, reserve)) {
            ++setup_window.deferred;
            CountOver(OverDeferred);
            return;
        }
        ++setup_window.refused;
        retry.Fail(key, frame, RetryFrames);
        CountOver(OverSceneMemory);
        return;
    }
    if (draw.camera_pass) {
        eye = {draw.constants[183], draw.constants[187], draw.constants[191]};
        eye_valid = true;
    }
    const float distance =
        draw.anchor && eye_valid
            ? RtRules::Distance(eye.data(), std::array{draw.anchor[3], draw.anchor[7], draw.anchor[11]}.data())
            : -1.0f;
    const u64 bytes = u64(draw.index_count) * draw.index_size;
    if (!copy_budget.Allow(RtRules::PickTier(false, distance), bytes, setup_keys.Size())) {
        ++setup_window.deferred;
        CountOver(OverDeferred);
        return;
    }
    copy_budget.Spend(bytes);
    SetupJob job{.mesh = {.key = key, .distance = distance},
                 .source = {.palette = draw.palette,
                            .index_size = draw.index_size,
                            .index_count = draw.index_count,
                            .first_vertex = draw.first_vertex,
                            .first_instance = draw.first_instance,
                            .instances = draw.instances,
                            .restart = draw.restart,
                            .strip = draw.strip,
                            .skinned = draw.skinned},
                 .scene_bytes = blas_bytes + setup_keys.Bytes(),
                 .frame = frame,
                 .mask = mask,
                 .shared = shared};
    job.data.resize(std::size_t(bytes));
    std::memcpy(job.data.data(), reinterpret_cast<const void*>(draw.indices), std::size_t(bytes));
    setup_keys.Submitted(key, reserve);
    ++setup_window.sent;
    setup_window.copied += bytes;
    {
        std::scoped_lock lock{setup_mutex};
        setup_jobs.push_back(std::move(job));
    }
    setup_wake.notify_one();
}

void RtShadow::SetupLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("bb:RtSetup");
    while (true) {
        SetupJob job;
        {
            std::unique_lock lock{setup_mutex};
            if (!setup_wake.wait(lock, stop, [&] { return !setup_jobs.empty(); }) || stop.stop_requested()) {
                return;
            }
            const auto nearest = std::ranges::min_element(
                setup_jobs, [](const SetupJob& a, const SetupJob& b) { return RtRules::Nearer(a.mesh, b.mesh); });
            job = std::move(*nearest);
            if (nearest != setup_jobs.end() - 1) {
                *nearest = std::move(setup_jobs.back());
            }
            setup_jobs.pop_back();
        }
        const s64 start = NowNs();
        Source source = job.source;
        source.indices = job.data.data();
        SetupResult result{.made = CreateEntry(source, job.mask, job.scene_bytes, job.frame), .shared = job.shared};
        RtRules::ReadyMesh mesh = job.mesh;
        if (result.made.entry) {
            mesh.triangles = result.made.entry->triangles;
        } else {
            mesh.unbounded = true;
        }
        setup_busy_ns.fetch_add(u64(NowNs() - start), std::memory_order_relaxed);
        std::scoped_lock lock{setup_mutex};
        setup_done.emplace_back(mesh, std::move(result));
    }
}

void RtShadow::StopSetup() {
    if (!setup_worker) {
        return;
    }
    setup_thread.request_stop();
    setup_thread.join();
    setup_worker = false;
}

void RtShadow::HandOffReady() {
    if (!setup_worker) {
        return;
    }
    const DiagTimer handoff_timer{diag ? &diag_frame.handoff_ns : nullptr};
    copy_budget.Reset();
    {
        std::scoped_lock lock{setup_mutex};
        for (auto& [mesh, result] : setup_done) {
            setup_ready.Add(mesh, std::move(result));
        }
        setup_done.clear();
    }
    setup_ready.Take({.entries = LandPerFrame, .triangles = NewTrianglesPerFrame},
                     [&](const RtRules::ReadyMesh& mesh, SetupResult&& result) {
                         setup_keys.Landed(mesh.key);
                         Settle(result.made);
                         if (!result.made.entry) {
                             ++setup_window.failed;
                             if (!failed) {
                                 retry.Fail(mesh.key, frame, RetryFrames);
                             }
                             create_failed = false;
                             return;
                         }
                         Entry& entry = *result.made.entry;
                         const bool taken = entries.contains(mesh.key);
                         const bool fits = RtRules::FitsScene(blas_bytes, blas_limit, u64(entry.triangles) * 64);
                         if (RtRules::Land(failed, taken, fits) == RtRules::Landing::Retire) {
                             ++setup_window.retired;
                             if (!failed && !taken) {
                                 retry.Fail(mesh.key, frame, RetryFrames);
                             }
                             Retire(entry);
                             return;
                         }
                         entry.rebuilt = result.shared;
                         entry.rebuild_every = RebuildEveryFrames + u32(mesh.key % RebuildSpread);
                         entry.seen_frame = frame - 1;
                         blas_bytes += entry.bytes;
                         entries.emplace(mesh.key, std::move(result.made.entry));
                         ++setup_window.landed;
                     });
}

u32 RtShadow::LampRayMask() const noexcept {
    return cutout_lamps ? u32(0xffu & ~u32(CutShadowMask | CutCameraMask)) : 0xffu;
}

u32 RtShadow::KeepPrevious(Entry& entry) {
    if (!RtRules::KeepInScene(keep_previous, entry.built, frame - entry.data_frame)) {
        path_window.not_kept += keep_previous ? 1 : 0;
        path_window.too_old += keep_previous && entry.built ? 1 : 0;
        return 0;
    }
    entry.seen_frame = frame;
    frame_entries.push_back(&entry);
    ++path_window.kept;
    return 0;
}

u32 RtShadow::PrepareDraw(const Draw& draw) {
    if (!Wanted() || (!draw.shadow_pass && !draw.camera_pass)) {
        return 0;
    }
    const DiagTimer prepare_timer{diag ? &diag_frame.prepare_ns : nullptr};
    diag_frame.prepares += diag ? 1 : 0;
    draw.camera_pass ? ++stats.camera_draws : ++stats.draws;
    if (!draw.solid) {
        return 0;
    }
    const bool cut_out = draw.alpha && (draw.skinned || draw.palette != 0);
    if (cut_out) {
        ++stats.cut_characters;
        if (cutout_map) {
            ++(draw.shadow_pass ? path_window.cut_casters : path_window.cut_camera);
            alpha_casters += draw.shadow_pass ? 1 : 0;
        }
    } else if (draw.alpha) {
        ++stats.alpha;
        if (draw.shadow_pass) {
            ++alpha_casters;
        }
        return 0;
    }
    if (!draw.constants) {
        ++stats.no_camera;
        return 0;
    }
    std::array<float, 12> to_world{};
    if (!ToWorld(draw.constants, draw.camera_pass, to_world)) {
        ++stats.not_ortho;
        return 0;
    }
    if (!draw.triangles || !draw.indices || draw.instances == 0 ||
        draw.instances > MaxInstancesPerDraw) {
        draw.instances > MaxInstancesPerDraw ? ++stats.instanced : ++stats.unsupported;
        return 0;
    }
    stats.strips += draw.strip ? 1 : 0;
    const u8 mask = draw.camera_pass ? (cut_out && cutout_lamps ? CutCameraMask : CameraMask)
                    : cut_out && cutout_map ? CutShadowMask
                                            : ShadowMask;
    u64 key;
    if (draw.positions) {
        const std::array<u64, 4> mesh{draw.positions, draw.indices,
                                      (u64(draw.index_count) << 32) | draw.first_vertex,
                                      draw.instances};
        key = XXH3_64bits(mesh.data(), sizeof(mesh));
    } else {
        key = draw.geometry ^ (draw.shader * 0x9e3779b97f4a7c15ull);
        key = XXH3_64bits_withSeed(&draw.indices, sizeof(draw.indices), key);
        const std::array<u32, 4> parts{draw.index_count, draw.first_vertex, draw.instances,
                                       draw.camera_pass ? 1u : 0u};
        key = XXH3_64bits_withSeed(parts.data(), sizeof(parts), key);
    }

    const u32 camera_index = camera_map ? camera_slots.Index(draw.constants)
                                        : RtRules::LinearCameraIndex(frame_cameras, draw.constants);
    const std::array<u64, 2> occurrence_key{key, camera_index};
    const u64 occurrence_hash = XXH3_64bits(occurrence_key.data(), sizeof(occurrence_key));
    const u32 occurrence = occurrences[occurrence_hash]++;
    if (occurrence > 0) {
        shared_keys.insert(key);
    }
    const bool shared = shared_keys.contains(key);
    u64 entry_key = key;
    if (shared) {
        const std::array<u64, 3> copy{key, camera_index, occurrence};
        entry_key = XXH3_64bits(copy.data(), sizeof(copy));
    }

    auto& slot = entries[entry_key];
    if (slot && persistent && !slot->stale && frame - slot->seen_frame > UnusedFrames &&
        RtRules::StaticCaster(slot->built, slot->dynamic, slot->rebuilt) &&
        RtRules::StaleReturn(frame, slot->seen_frame, UnusedFrames, slot->index_hash,
                             IndexHash(reinterpret_cast<const void*>(draw.indices), draw.index_count,
                                       draw.index_size))) {
        slot->stale = true;
        ++stale_count;
    }
    if (slot && slot->stale) {
        return 0;
    }
    if (!slot) {
        if (setup_worker && !draw.skinned) {
            entries.erase(entry_key);
            SubmitSetup(draw, mask, entry_key, shared);
            return 0;
        }
        if (new_triangles >= NewTrianglesPerFrame) {
            CountOver(OverNewCap);
            entries.erase(entry_key);
            return 0;
        }
        const bool budgeted = new_budget.On();
        RtRules::Tier tier = RtRules::Tier::Far;
        if (!budgeted && retry.Blocked(entry_key, frame)) {
            CountOver(OverBackoff);
            entries.erase(entry_key);
            return 0;
        }
        if (budgeted) {
            if (draw.camera_pass) {
                eye = {draw.constants[183], draw.constants[187], draw.constants[191]};
                eye_valid = true;
            }
            const float distance = draw.anchor && eye_valid
                                       ? RtRules::Distance(eye.data(), std::array{draw.anchor[3], draw.anchor[7],
                                                                                  draw.anchor[11]}.data())
                                       : -1.0f;
            tier = RtRules::PickTier(draw.skinned, distance);
            const bool backoff = retry.Blocked(entry_key, frame);
            const bool fits =
                RtRules::FitsScene(blas_bytes, blas_limit,
                                   RtRules::TriangleBound(draw.index_count, draw.instances, draw.strip) * 64);
            if (backoff || !new_budget.AllowTier(tier) || !fits) {
                if (backoff) {
                    ++path_window.backoff;
                    CountOver(OverBackoff);
                } else if (!fits) {
                    ++path_window.refused;
                    retry.Fail(entry_key, frame, RetryFrames);
                    CountOver(OverSceneMemory);
                } else {
                    ++(new_budget.CountLeft() ? path_window.deferred_time : path_window.deferred_count);
                    ++(tier == RtRules::Tier::Near ? path_window.deferred_near : path_window.deferred_far);
                    CountOver(OverDeferred);
                }
                entries.erase(entry_key);
                return 0;
            }
        }
        const s64 make_start = diag || budgeted ? NowNs() : 0;
        setup_window.characters += setup_worker ? 1 : 0;
        slot = MakeEntry(draw, mask);
        if (make_start) {
            const u64 spent = u64(NowNs() - make_start);
            diag_frame.make_ns += diag ? spent : 0;
            diag_frame.makes += diag ? 1 : 0;
            if (budgeted) {
                new_budget.Spend(spent);
                path_window.budget_ns += spent;
                ++(tier == RtRules::Tier::Character ? path_window.tier_made_character
                   : tier == RtRules::Tier::Near    ? path_window.tier_made_near
                                                    : path_window.tier_made_far);
            }
        }
        if (!slot) {
            if ((budgeted || create_failed) && !failed) {
                retry.Fail(entry_key, frame, RetryFrames);
            }
            create_failed = false;
            entries.erase(entry_key);
            return 0;
        }
        slot->rebuilt = shared;
        slot->rebuild_every = RebuildEveryFrames + u32(entry_key % RebuildSpread);
    }
    Entry& entry = *slot;
    if (rigid && entry.built && !entry.dynamic && !entry.rebuilt && !draw.skinned) {
        const bool capturing = entry.seen_frame != frame;
        const auto result = entry.rigid_bones.Check(capturing, draw.camera_pass, draw.bones);
        if (capturing) {
            ++path_window.rigid_checks;
            path_window.rigid_bytes += draw.bones.bytes;
            path_window.rigid_pooled += draw.bones.pooled ? 1 : 0;
            ++(result == RtRules::Rigid::Still     ? path_window.rigid_still
               : result == RtRules::Rigid::Moved   ? path_window.rigid_moved
               : result == RtRules::Rigid::Written ? path_window.rigid_written
                                                   : path_window.rigid_unknown);
        }
        entry.rigid_pending = entry.rigid_pending || result == RtRules::Rigid::Moved;
        entry.dynamic = result == RtRules::Rigid::Written;
    }
    if (entry.seen_frame == frame) {
        if ((entry.mask & mask) == 0) {
            entry.mask |= mask;
            tlas_count = 0;
            ++stats.merged;
        }
        return 0;
    }
    const u8 last_mask = entry.mask;
    entry.mask = mask;
    entry.rebuilt = entry.rebuilt || shared;
    if (entry.palette != draw.palette) {
        entry.palette = draw.palette;
        entry.dynamic = true;
    }
    bool needs_data = !entry.built || entry.dynamic || entry.rebuilt || entry.rigid_pending;
    if (needs_data && entry.built && !entry.rebuilt && !entry.rigid_pending) {
        const u8 cut_bits = cutout_lamps ? u8(CutShadowMask | CutCameraMask) : u8(0);
        const auto refit = RtRules::DynamicRefit(
            rigid, entry.pose.Changed(draw.camera_pass, draw.pose),
            RtRules::CutOnly(last_mask, cut_bits) && RtRules::CutOnly(mask, cut_bits), frame, entry_key);
        if (refit == RtRules::Refit::Now) {
            path_window.pose_refits += rigid ? 1 : 0;
        } else {
            if (refit == RtRules::Refit::Unchanged) {
                ++path_window.pose_still;
                entry.data_frame = frame;
            } else {
                ++path_window.pose_waited;
            }
            stats.dynamic_draws += entry.dynamic ? 1 : 0;
            needs_data = false;
        }
    }
    u32 param = 0;
    if (needs_data) {
        bool update = entry.built && frame - entry.built_frame < entry.rebuild_every;
        bool over = !update && (entry.rebuilt ? rebuilt_triangles >= RebuiltTrianglesPerFrame
                                              : new_triangles >= NewTrianglesPerFrame);
        if (RtRules::RefitInstead(keep_previous, entry.built, over)) {
            update = true;
            over = false;
            ++path_window.refit_instead;
        }
        const u64 stored = u64(entry.vertices) * entry.instances;
        const u64 needed = (update ? entry.update_scratch : entry.scratch) + 256;
        if (positions_used + stored > MaxPositions || scratch_used + needed > ScratchBytes || over) {
            CountOver(positions_used + stored > MaxPositions ? OverPositions
                      : scratch_used + needed > ScratchBytes ? OverScratch
                      : entry.rebuilt                        ? OverRebuiltCap
                                                             : OverNewCap);
            return KeepPrevious(entry);
        }
        param = Param(positions_used, entry);
        if (!param) {
            CountOver(OverParams);
            return KeepPrevious(entry);
        }
        builds.push_back({&entry, positions_used, update, to_world});
        entry.data_frame = frame;
        positions_used += u32(stored);
        scratch_used += needed;
        if (!update) {
            if (entry.rebuilt) {
                rebuilt_triangles += entry.triangles;
                ++stats.shared_builds;
            } else {
                new_triangles += entry.triangles;
                stats.built_triangles += entry.triangles;
            }
        }
        if (!update) {
            entry.built_frame = frame;
        }
        entry.rigid_bones.Captured(draw.camera_pass, draw.bones);
        entry.pose.Captured(draw.camera_pass, draw.pose);
        if (entry.rigid_pending) {
            entry.rigid_pending = false;
            ++path_window.rigid_refits;
        }
        stats.dynamic_draws += entry.dynamic ? 1 : 0;
    }
    entry.seen_frame = frame;
    frame_entries.push_back(&entry);
    return param;
}

void RtShadow::RecordBuilds() {
    if (builds.empty()) {
        return;
    }
    const DiagTimer record_timer{diag ? &diag_frame.record_ns : nullptr};
    struct Job {
        std::vector<vk::AccelerationStructureGeometryKHR> geometries;
        std::vector<vk::AccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges;
        std::vector<const vk::AccelerationStructureBuildRangeInfoKHR*> pointers;
        std::vector<UnprojectPush> unproject;
    };
    auto job = std::make_shared<Job>();
    job->geometries.reserve(builds.size());
    job->ranges.reserve(builds.size());
    job->unproject.reserve(builds.size());
    u64 scratch_at = 0;
    const auto align = [&](u64 v) { return (v + scratch_align - 1) / scratch_align * scratch_align; };
    for (const Build& build : builds) {
        Entry& e = *build.entry;
        job->unproject.push_back({build.to_world, build.store, e.vertices * e.instances, BoundsBox(e)});
        job->geometries.push_back({
            .geometryType = vk::GeometryTypeKHR::eTriangles,
            .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                .vertexFormat = vk::Format::eR32G32B32Sfloat,
                .vertexData = {.deviceAddress = positions.address + u64(build.store) * 16},
                .vertexStride = 16,
                .maxVertex = e.vertices * e.instances - 1,
                .indexType = vk::IndexType::eUint32,
                .indexData = {.deviceAddress = e.indices.address},
            }},
            .flags = vk::GeometryFlagBitsKHR::eOpaque,
        });
        job->infos.push_back({
            .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
            .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace |
                     vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate,
            .mode = build.update ? vk::BuildAccelerationStructureModeKHR::eUpdate
                                 : vk::BuildAccelerationStructureModeKHR::eBuild,
            .srcAccelerationStructure = build.update ? e.handle : vk::AccelerationStructureKHR{},
            .dstAccelerationStructure = e.handle,
            .geometryCount = 1,
            .pGeometries = &job->geometries.back(),
            .scratchData = {.deviceAddress = scratch.address + scratch_at},
        });
        scratch_at += align(build.update ? e.update_scratch : e.scratch);
        job->ranges.push_back({.primitiveCount = e.triangles});
        job->pointers.push_back(&job->ranges.back());
        e.built = true;
        ++(build.update ? diag_window.refit : diag_window.built);
    }
    builds.clear();
    positions_used = 0;
    scratch_used = 0;
    auto* profiler = GpuProfiler::Get();
    const u64 resume = profiler ? profiler->Current() : 0;
    if (resume) {
        EffectPass::Mark(13, "ray tracing geometry builds");
    }
    scheduler.RecordCrumb(
        {.name = "Ray tracing geometry"},
        [job, positions = positions.buffer,
         unproject_pipeline = persistent ? *bounds_pipeline : *unproject_pipeline,
         unproject_layout = persistent ? *bounds_layout : *unproject_layout,
         bounds = persistent ? bounds.buffer : vk::Buffer{},
         push_size = persistent ? u32(sizeof(UnprojectPush)) : u32(offsetof(UnprojectPush, box))](
            vk::CommandBuffer cmdbuf) {
            const auto memory = [&](vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access,
                                    vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
                const vk::MemoryBarrier2 barrier{.srcStageMask = src, .srcAccessMask = src_access,
                                                 .dstStageMask = dst, .dstAccessMask = dst_access};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
            };
            memory(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite, Compute,
                   vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
            const vk::DescriptorBufferInfo position_info{positions, 0, VK_WHOLE_SIZE};
            const vk::DescriptorBufferInfo bounds_info{bounds, 0, VK_WHOLE_SIZE};
            const std::array<vk::WriteDescriptorSet, 2> writes{{
                {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &position_info},
                {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &bounds_info},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, unproject_pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, unproject_layout, 0,
                                        vk::ArrayProxy<const vk::WriteDescriptorSet>(bounds ? 2u : 1u,
                                                                                     writes.data()));
            for (const UnprojectPush& push : job->unproject) {
                cmdbuf.pushConstants(unproject_layout, vk::ShaderStageFlagBits::eCompute, 0, push_size, &push);
                cmdbuf.dispatch((push.count + 63) / 64, 1, 1);
            }
            memory(Compute, vk::AccessFlagBits2::eShaderWrite, AsBuild,
                   vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                       vk::AccessFlagBits2::eAccelerationStructureWriteKHR |
                       vk::AccessFlagBits2::eShaderRead);
            if (bounds) {
                memory(Compute, vk::AccessFlagBits2::eShaderWrite, vk::PipelineStageFlagBits2::eHost,
                       vk::AccessFlagBits2::eHostRead);
            }
            cmdbuf.buildAccelerationStructuresKHR(job->infos, job->pointers);
            memory(AsBuild, vk::AccessFlagBits2::eAccelerationStructureWriteKHR, AsBuild,
                   vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                       vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
        });
    if (profiler && resume) {
        profiler->Resume(resume);
    }
    tlas_count = 0;
}

bool RtShadow::PrepareScene() {
    const auto camera = camera_motion.PassMatrices();
    if (!Wanted()) {
        return false;
    }
    RecordBuilds();
    const DiagTimer scene_timer{diag ? &diag_frame.scene_ns : nullptr};
    if (!Complete() || !camera.valid) {
        return false;
    }
    const u32 drawn = std::min<u32>(u32(frame_entries.size()), MaxInstances);
    camera_frame = persistent ? frame : 0;
    if (persistent && drawn != offscreen_drawn) {
        PickOffscreen(camera.view, camera.inv_view, camera.proj, MaxInstances - drawn);
        offscreen_drawn = drawn;
    }
    const u32 count = drawn + (persistent ? u32(offscreen.size()) : 0u);
    if (persistent ? RtRules::SceneUnchanged(count, tlas_count, drawn, tlas_drawn) : count == tlas_count) {
        return true;
    }
    tlas_drawn = drawn;
    const u32 slot = u32(frame % 4);
    auto* out = static_cast<vk::AccelerationStructureInstanceKHR*>(instance_data.mapped) +
                slot * MaxInstances;
    u64 triangles = 0;
    for (u32 i = 0; i < count; ++i) {
        const Entry* entry = i < drawn ? frame_entries[i] : offscreen[i - drawn];
        vk::AccelerationStructureInstanceKHR record{
            .mask = entry->mask,
            .accelerationStructureReference = entry->address,
        };
        std::memcpy(&record.transform, camera.view.data(), sizeof(camera.view));
        out[i] = record;
        triangles += entry->triangles;
    }
    if (tlas_count == 0) {
        stats.instances += count;
        stats.triangles += triangles;
        ++stats.frames;
    }
    tlas_count = count;
    slot_ticks[slot] = scheduler.CurrentTick();
    const u64 instances_address =
        instance_data.address + u64(slot) * MaxInstances * sizeof(vk::AccelerationStructureInstanceKHR);
    auto* profiler = GpuProfiler::Get();
    const u64 resume = profiler ? profiler->Current() : 0;
    if (resume) {
        EffectPass::Mark(14, "ray tracing scene");
    }
    scheduler.RecordCrumb(
        {.name = "Ray tracing scene"},
        [count, instances_address, tlas = tlas,
         tlas_scratch = tlas_scratch.address](vk::CommandBuffer cmdbuf) {
            const vk::AccelerationStructureGeometryKHR geometry{
                .geometryType = vk::GeometryTypeKHR::eInstances,
                .geometry = {.instances = vk::AccelerationStructureGeometryInstancesDataKHR{
                    .data = {.deviceAddress = instances_address}}},
                .flags = vk::GeometryFlagBitsKHR::eOpaque,
            };
            const vk::AccelerationStructureBuildGeometryInfoKHR info{
                .type = vk::AccelerationStructureTypeKHR::eTopLevel,
                .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
                .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
                .dstAccelerationStructure = tlas,
                .geometryCount = 1,
                .pGeometries = &geometry,
                .scratchData = {.deviceAddress = tlas_scratch},
            };
            const vk::AccelerationStructureBuildRangeInfoKHR range{.primitiveCount = count};
            const vk::AccelerationStructureBuildRangeInfoKHR* range_pointer = &range;
            const vk::MemoryBarrier2 before{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = AsBuild,
                .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                                 vk::AccessFlagBits2::eAccelerationStructureWriteKHR};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
            cmdbuf.buildAccelerationStructuresKHR(info, range_pointer);
            const vk::MemoryBarrier2 after{.srcStageMask = AsBuild,
                                           .srcAccessMask =
                                               vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                                           .dstStageMask = Compute,
                                           .dstAccessMask =
                                               vk::AccessFlagBits2::eAccelerationStructureReadKHR};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        });
    if (profiler && resume) {
        profiler->Resume(resume);
    }
    return true;
}

bool RtShadow::MakeRaw(u32 width, u32 height) {
    if (raw_image && raw_width == width && raw_height == height) {
        return true;
    }
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    if (raw_image) {
        scheduler.DeferOperation([device, allocator, image = raw_image, memory = raw_allocation,
                                  view = raw_view] {
            device.destroyImageView(view);
            vmaDestroyImage(allocator, image, memory);
        });
        raw_image = vk::Image{};
        raw_view = vk::ImageView{};
    }
    const VkImageCreateInfo image_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage raw{};
    if (vmaCreateImage(allocator, &image_ci, &alloc_ci, &raw, &raw_allocation, nullptr) != VK_SUCCESS) {
        return false;
    }
    raw_image = raw;
    const auto [result, view] = device.createImageView({
        .image = raw_image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    if (result != vk::Result::eSuccess) {
        return false;
    }
    raw_view = view;
    raw_width = width;
    raw_height = height;
    return true;
}

bool RtShadow::Record(const Inputs& in) {
    if (!PrepareScene() || !MakeRaw(in.width, in.height)) {
        return false;
    }
    const TracePush trace{
        .buf_off = in.constants_offset_word,
        .frame_index = 0u,
        .size = {float(in.width), float(in.height)},
        .rays = rays,
        .light_tan = 0.02f,
        .bias_m = 0.01f,
        .max_distance = 800.0f,
    };
    const FilterPush filter{
        .buf_off = in.constants_offset_word,
        .radius = 2,
        .size = {float(in.width), float(in.height)},
        .keep_map = KeepMap() ? 1u : 0u,
    };
    EffectPass::Mark(12, "ray traced sun");
    scheduler.RecordCrumb(
        {.name = "Ray traced sun"},
        [in, trace, filter, tlas = tlas, raw_image = raw_image, raw_view = raw_view,
         sampler = *sampler, trace_pipeline = *trace_pipeline, trace_layout = *trace_layout,
         filter_pipeline = *filter_pipeline,
         filter_layout = *filter_layout](vk::CommandBuffer cmdbuf) {
            const auto memory = [&](vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access,
                                    vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
                const vk::MemoryBarrier2 barrier{.srcStageMask = src, .srcAccessMask = src_access,
                                                 .dstStageMask = dst, .dstAccessMask = dst_access};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
            };
            const vk::ImageMemoryBarrier2 to_general{
                .srcStageMask = Compute,
                .dstStageMask = Compute,
                .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = raw_image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general});

            const vk::DescriptorBufferInfo constants{in.constants, in.constants_offset, in.constants_range};
            const vk::DescriptorBufferInfo uniforms{in.uniforms, in.uniforms_offset, in.uniforms_range};
            const vk::DescriptorImageInfo depth{sampler, in.depth, vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo raw_write{{}, raw_view, vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo raw_read{sampler, raw_view, vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo target{{}, in.target, vk::ImageLayout::eGeneral};
            const vk::WriteDescriptorSetAccelerationStructureKHR scene{
                .accelerationStructureCount = 1, .pAccelerationStructures = &tlas};
            const std::array<vk::WriteDescriptorSet, 4> trace_writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &constants},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &depth},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &raw_write},
                {.pNext = &scene, .dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eAccelerationStructureKHR},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, trace_pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, trace_layout, 0, trace_writes);
            cmdbuf.pushConstants(trace_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(trace), &trace);
            cmdbuf.dispatch((in.width + 7) / 8, (in.height + 7) / 8, 1);
            memory(Compute, vk::AccessFlagBits2::eShaderWrite, Compute,
                   vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
            const std::array<vk::WriteDescriptorSet, 5> filter_writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &constants},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &depth},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &raw_read},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &target},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &uniforms},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, filter_pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, filter_layout, 0, filter_writes);
            cmdbuf.pushConstants(filter_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(filter), &filter);
            cmdbuf.dispatch((in.width + 7) / 8, (in.height + 7) / 8, 1);
            memory(Compute, vk::AccessFlagBits2::eShaderWrite, vk::PipelineStageFlagBits2::eAllCommands,
                   vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        });
    return true;
}

void RtShadow::OnFrameEnd() {
    if (!capturing) {
        return;
    }
    for (const Build& build : builds) {
        build.entry->built = RtRules::BuiltAfterUnrecorded(build.entry->built, build.update);
        build.entry->pose = {};
    }
    builds.clear();
    if (stats.frames >= 600) {
        const double f = double(stats.frames);
        std::printf("Ray tracing: %.0f shadow + %.0f camera draws, %.0f instances, %.0f triangles a "
                    "frame; %zu BLAS; alpha %.1f, not ortho %.1f, no camera %.1f, unsupported %.1f, "
                    "many instances %.1f, over budget %.1f, moving %.1f, copies rebuilt %.1f, strips "
                    "%.1f, in both passes %.1f, cut-out characters %.1f, new triangles built %.0f, evicted %.1f a "
                    "frame; scene %.0f MB of %.0f\n",
                    double(stats.draws) / f, double(stats.camera_draws) / f,
                    double(stats.instances) / f, double(stats.triangles) / f, entries.size(),
                    double(stats.alpha) / f, double(stats.not_ortho) / f, double(stats.no_camera) / f,
                    double(stats.unsupported) / f, double(stats.instanced) / f,
                    double(stats.over_budget) / f, double(stats.dynamic_draws) / f,
                    double(stats.shared_builds) / f, double(stats.strips) / f,
                    double(stats.merged) / f, double(stats.cut_characters) / f,
                    double(stats.built_triangles) / f, double(stats.evicted) / f,
                    double(blas_bytes) / 1048576.0, double(blas_limit) / 1048576.0);
        stats = {};
    }
    if (persistent) {
        scene_window.added += offscreen.size();
        scene_window.added_max = std::max<u64>(scene_window.added_max, offscreen.size());
        scene_window.far += pick_far;
        scene_window.capped += pick_capped;
        pick_far = pick_capped = 0;
        offscreen.clear();
        offscreen_drawn = UINT32_MAX;
        dark_run = camera_frame == frame ? 0 : dark_run + 1;
        dark = dark || dark_run >= RtRules::DarkFrames;
    }
    if (diag) {
        const u64 cameras = camera_map ? camera_slots.Size() : frame_cameras.size();
        path_window.cameras += cameras;
        path_window.cameras_max = std::max(path_window.cameras_max, cameras);
        DiagFrameEnd();
    }
    ++frame;
    if (frame % 64 == 0 || blas_bytes > blas_limit - blas_limit / 8 || stale_count) {
        std::vector<std::pair<u64, u64>> aged;
        if (blas_bytes > blas_limit - blas_limit / 8) {
            aged.reserve(entries.size());
            for (const auto& [key, entry] : entries) {
                aged.emplace_back(entry->seen_frame, entry->bytes);
            }
            path_window.evict_passes += evict_hold ? 1 : 0;
        }
        const auto plan = RtRules::EvictBefore(
            std::move(aged), blas_bytes, blas_limit, frame,
            {.unused_frames = UnusedFrames, .kept_frames = KeptFrames, .hold = evict_hold});
        const u64 keep_from = plan.keep_from;
        path_window.spared += plan.spared;
        if (frame % 64 == 0) {
            retry.Prune(frame);
        }
        u64 kept_static = 0;
        const bool keep_statics = RtRules::KeepStatics(persistent, dark);
        std::erase_if(entries, [&](auto& item) {
            const Entry& e = *item.second;
            const bool still = RtRules::StaticCaster(e.built, e.dynamic, e.rebuilt);
            if (!e.stale && !RtRules::Evicted(e.seen_frame, plan, keep_statics, still, frame)) {
                kept_static += e.seen_frame < keep_from ? 1 : 0;
                return false;
            }
            scene_window.stale += e.stale ? 1 : 0;
            Destroy(*item.second);
            ++stats.evicted;
            ++diag_window.evicted;
            return true;
        });
        scene_window.kept_static = kept_static;
        scene_window.flushes += persistent && dark ? 1 : 0;
        dark = dark_run >= RtRules::DarkFrames;
        stale_count = 0;
    }
    const u32 slot = u32(frame % 4);
    if (slot_ticks[slot]) {
        scheduler.Wait(slot_ticks[slot]);
        slot_ticks[slot] = 0;
    }
    if (persistent) {
        ReadBounds(slot);
    }
    params_used = 0;
    positions_used = 0;
    scratch_used = 0;
    new_triangles = 0;
    rebuilt_triangles = 0;
    alpha_casters = 0;
    missed = 0;
    tlas_count = 0;
    frame_entries.clear();
    frame_cameras.clear();
    camera_slots.Clear();
    new_budget.Reset();
    DrainRetired();
    if (pool && frame % 64 == 0) {
        ReturnEmptyChunks();
    }
    occurrences.clear();
    HandOffReady();
}

}
