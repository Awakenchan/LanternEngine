// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_rt_shadow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>

#include <xxhash.h>

#include "bbport_settings.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/host_shaders/rt_shadow_comp.h"
#include "video_core/host_shaders/rt_shadow_filter_comp.h"
#include "video_core/host_shaders/rt_unproject_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_effect_pass.h"
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
constexpr VkBufferUsageFlags InputUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr u8 ShadowMask = 1, CameraMask = 2;

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
};
static_assert(sizeof(UnprojectPush) == 56);

constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr auto AsBuild = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;

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
    } else {
        std::printf("Ray tracing: off - setup failed\n");
    }
}

RtShadow::~RtShadow() {
    if (!tried) {
        return;
    }
    scheduler.Finish();
    const auto device = instance.GetDevice();
    for (auto& [key, entry] : entries) {
        device.destroyAccelerationStructureKHR(entry->handle);
        Free(entry->storage);
        Free(entry->indices);
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
                      &instance_data, &tlas_storage, &tlas_scratch}) {
        Free(*b);
    }
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
    if (buffer.buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

void RtShadow::Destroy(Entry& entry) {
    blas_bytes -= std::min(blas_bytes, entry.bytes);
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    scheduler.DeferOperation([device, allocator, handle = entry.handle, storage = entry.storage,
                              indices = entry.indices] {
        device.destroyAccelerationStructureKHR(handle);
        vmaDestroyBuffer(allocator, storage.buffer, storage.allocation);
        vmaDestroyBuffer(allocator, indices.buffer, indices.allocation);
    });
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
                            {eStorageBuffer}, sizeof(UnprojectPush), RT_UNPROJECT_COMP,
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
    const auto device = instance.GetDevice();
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
    const void* source = reinterpret_cast<const void*>(draw.indices);
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
        ++stats.unsupported;
        return nullptr;
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
    entry->dynamic = draw.skinned;
    entry->mask = mask;
    if (blas_bytes + vk::DeviceSize(entry->triangles) * 64 > blas_limit ||
        !EffectPass::HasRoom(instance, vk::DeviceSize(entry->triangles) * 96) ||
        !Make(entry->indices, list.size() * sizeof(u32), true, InputUsage)) {
        ++stats.over_budget;
        ++missed;
        return nullptr;
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
    if (!Make(entry->storage, sizes.accelerationStructureSize, false,
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR)) {
        Free(entry->indices);
        ++stats.over_budget;
        ++missed;
        return nullptr;
    }
    const auto [result, handle] = device.createAccelerationStructureKHR({
        .buffer = entry->storage.buffer,
        .size = sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
    });
    if (result != vk::Result::eSuccess) {
        Free(entry->indices);
        Free(entry->storage);
        failed = true;
        return nullptr;
    }
    entry->handle = handle;
    entry->address = device.getAccelerationStructureAddressKHR({.accelerationStructure = handle});
    blas_bytes += entry->bytes;
    return entry;
}

u32 RtShadow::PrepareDraw(const Draw& draw) {
    if (!Wanted() || (!draw.shadow_pass && !draw.camera_pass)) {
        return 0;
    }
    draw.camera_pass ? ++stats.camera_draws : ++stats.draws;
    if (!draw.solid) {
        return 0;
    }
    if (draw.alpha && (draw.skinned || draw.palette != 0)) {
        ++stats.cut_characters;
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
    const u8 mask = draw.camera_pass ? CameraMask : ShadowMask;
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

    u32 camera_index = 0;
    for (; camera_index < frame_cameras.size(); ++camera_index) {
        if (frame_cameras[camera_index] == draw.constants) {
            break;
        }
    }
    if (camera_index == frame_cameras.size()) {
        frame_cameras.push_back(draw.constants);
    }
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
    if (!slot) {
        if (new_triangles >= NewTrianglesPerFrame) {
            ++stats.over_budget;
            ++missed;
            entries.erase(entry_key);
            return 0;
        }
        slot = MakeEntry(draw, mask);
        if (!slot) {
            entries.erase(entry_key);
            return 0;
        }
        slot->rebuilt = shared;
        slot->rebuild_every = RebuildEveryFrames + u32(entry_key % RebuildSpread);
    }
    Entry& entry = *slot;
    if (entry.seen_frame == frame) {
        if ((entry.mask & mask) == 0) {
            entry.mask |= mask;
            tlas_count = 0;
            ++stats.merged;
        }
        return 0;
    }
    entry.mask = mask;
    entry.rebuilt = entry.rebuilt || shared;
    if (entry.palette != draw.palette) {
        entry.palette = draw.palette;
        entry.dynamic = true;
    }
    const bool needs_data = !entry.built || entry.dynamic || entry.rebuilt;
    u32 param = 0;
    if (needs_data) {
        const bool update = entry.built && frame - entry.built_frame < entry.rebuild_every;
        const u64 stored = u64(entry.vertices) * entry.instances;
        const u64 needed = (update ? entry.update_scratch : entry.scratch) + 256;
        const bool over = !update && (entry.rebuilt ? rebuilt_triangles >= RebuiltTrianglesPerFrame
                                                    : new_triangles >= NewTrianglesPerFrame);
        if (positions_used + stored > MaxPositions || scratch_used + needed > ScratchBytes || over) {
            ++stats.over_budget;
            ++missed;
            return 0;
        }
        param = Param(positions_used, entry);
        if (!param) {
            ++stats.over_budget;
            ++missed;
            return 0;
        }
        builds.push_back({&entry, positions_used, update, to_world});
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
        job->unproject.push_back({build.to_world, build.store, e.vertices * e.instances});
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
    }
    builds.clear();
    positions_used = 0;
    scratch_used = 0;
    scheduler.RecordCrumb(
        {.name = "Ray tracing geometry"},
        [job, positions = positions.buffer, unproject_pipeline = *unproject_pipeline,
         unproject_layout = *unproject_layout](vk::CommandBuffer cmdbuf) {
            const auto memory = [&](vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access,
                                    vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
                const vk::MemoryBarrier2 barrier{.srcStageMask = src, .srcAccessMask = src_access,
                                                 .dstStageMask = dst, .dstAccessMask = dst_access};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
            };
            memory(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite, Compute,
                   vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
            const vk::DescriptorBufferInfo position_info{positions, 0, VK_WHOLE_SIZE};
            const vk::WriteDescriptorSet write{.dstBinding = 0,
                                               .descriptorCount = 1,
                                               .descriptorType = vk::DescriptorType::eStorageBuffer,
                                               .pBufferInfo = &position_info};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, unproject_pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, unproject_layout, 0, write);
            for (const UnprojectPush& push : job->unproject) {
                cmdbuf.pushConstants(unproject_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                                     &push);
                cmdbuf.dispatch((push.count + 63) / 64, 1, 1);
            }
            memory(Compute, vk::AccessFlagBits2::eShaderWrite, AsBuild,
                   vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                       vk::AccessFlagBits2::eAccelerationStructureWriteKHR |
                       vk::AccessFlagBits2::eShaderRead);
            cmdbuf.buildAccelerationStructuresKHR(job->infos, job->pointers);
            memory(AsBuild, vk::AccessFlagBits2::eAccelerationStructureWriteKHR, AsBuild,
                   vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                       vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
        });
    tlas_count = 0;
}

bool RtShadow::PrepareScene() {
    const auto camera = camera_motion.PassMatrices();
    if (!Wanted()) {
        return false;
    }
    RecordBuilds();
    if (!Complete() || !camera.valid) {
        return false;
    }
    const u32 count = std::min<u32>(u32(frame_entries.size()), MaxInstances);
    if (count == tlas_count) {
        return true;
    }
    const u32 slot = u32(frame % 4);
    auto* out = static_cast<vk::AccelerationStructureInstanceKHR*>(instance_data.mapped) +
                slot * MaxInstances;
    u64 triangles = 0;
    for (u32 i = 0; i < count; ++i) {
        const Entry* entry = frame_entries[i];
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
    EffectPass::Mark(10, "ray traced sun");
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
        build.entry->built = build.entry->built && build.update;
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
    ++frame;
    if (frame % 64 == 0 || blas_bytes > blas_limit - blas_limit / 8) {
        u64 keep_from = frame > UnusedFrames ? frame - UnusedFrames : 0;
        if (blas_bytes > blas_limit - blas_limit / 8) {
            u64 bytes = blas_bytes;
            const u64 target = blas_limit - blas_limit / 4;
            std::size_t at = 0;
            std::vector<std::pair<u64, u64>> aged;
            aged.reserve(entries.size());
            for (const auto& [key, entry] : entries) {
                aged.emplace_back(entry->seen_frame, entry->bytes);
            }
            std::ranges::sort(aged);
            for (; at < aged.size() && bytes > target && aged[at].first + KeptFrames < frame; ++at) {
                bytes -= std::min(bytes, aged[at].second);
                keep_from = std::max(keep_from, aged[at].first + 1);
            }
        }
        std::erase_if(entries, [&](auto& item) {
            if (item.second->seen_frame >= keep_from) {
                return false;
            }
            Destroy(*item.second);
            ++stats.evicted;
            return true;
        });
    }
    const u32 slot = u32(frame % 4);
    if (slot_ticks[slot]) {
        scheduler.Wait(slot_ticks[slot]);
        slot_ticks[slot] = 0;
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
    occurrences.clear();
}

}
