// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/vk_volumetric_light.h"

#include <algorithm>
#include <utility>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "bbport_settings.h"
#include "common/assert.h"
#include "video_core/host_shaders/volumetric_light_integrate_comp.h"
#include "video_core/host_shaders/volumetric_light_repair_comp.h"
#include "video_core/host_shaders/volumetric_light_tint_comp.h"
#include "video_core/host_shaders/volumetric_light_composite_comp.h"
#include "video_core/host_shaders/volumetric_light_filter_comp.h"
#include "video_core/host_shaders/point_volumetric_comp.h"
#include "video_core/renderer_vulkan/vk_effect_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {
namespace {
constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr u32 FogLightsAt = 256, FogMaxLights = 64;
constexpr u32 FogBytes = FogLightsAt + FogMaxLights * 64;
static_assert(FogBytes <= 65536, "vkCmdUpdateBuffer takes at most 64 KiB");
struct Params {
    u32 offsets, steps;
    float reach, density, strength, anisotropy, bias, tilt;
};
static_assert(sizeof(Params) == 32);
struct TintParams {
    u32 offset_bytes, record_count;
    float blend;
    u32 skip_fallback;
};
static_assert(sizeof(TintParams) == 16);
// The fog's color moves 95% of the way to a new value in about 1.5 s (three time constants). After
// a longer gap than this (the first frame, a load) there is no earlier color worth fading from.
constexpr float TintFadeConstant = 0.5f, TintSnapAfter = 1.0f;

void Before(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 b{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = Compute,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
}

void After(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 b{
        .srcStageMask = Compute,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
}
}

VolumetricLight::VolumetricLight(const Instance& instance_, Scheduler& scheduler_,
                               VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                               SceneTargets& scene_targets_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, scene_targets{scene_targets_} {
}

bool VolumetricLight::Ready() {
    if (tried) {
        return !failed;
    }
    tried = true;
    using enum vk::DescriptorType;
    bool made = EffectPass::StorageFormat(instance, vk::Format::eR16G16B16A16Sfloat) &&
                EffectPass::StorageFormat(instance, vk::Format::eR32Sfloat) &&
                EffectPass::CreateSampler(instance, sampler, vk::Filter::eNearest);
    made = made &&
           CreatePass(integrate,
                      {eStorageBuffer, eStorageBuffer, eCombinedImageSampler, eCombinedImageSampler,
                       eStorageImage, eStorageImage},
                      VOLUMETRIC_LIGHT_INTEGRATE_COMP, sizeof(Params), "Volumetric integrate") &&
           CreatePass(repair,
                      {eStorageBuffer, eStorageBuffer, eCombinedImageSampler, eCombinedImageSampler,
                       eStorageImage, eStorageImage, eStorageImage},
                      VOLUMETRIC_LIGHT_REPAIR_COMP, sizeof(Params), "Volumetric repair") &&
           CreatePass(tint, {eStorageBuffer, eStorageBuffer},
                      VOLUMETRIC_LIGHT_TINT_COMP, sizeof(TintParams), "Volumetric tint") &&
           CreatePass(filter, {eCombinedImageSampler, eStorageImage},
                      VOLUMETRIC_LIGHT_FILTER_COMP, sizeof(u32), "Volumetric filter") &&
           CreatePass(composite,
                      {eCombinedImageSampler, eCombinedImageSampler, eStorageImage, eStorageBuffer,
                       eCombinedImageSampler, eCombinedImageSampler},
                      VOLUMETRIC_LIGHT_COMPOSITE_COMP, sizeof(u32), "Volumetric composite") &&
           CreatePass(point,
                      {eStorageBuffer, eStorageBuffer, eCombinedImageSampler, eStorageImage},
                      POINT_VOLUMETRIC_COMP, 3 * sizeof(u32), "Point volumetric");
    if (!made) {
        failed = true;
        std::printf("Volumetric lighting: off - this GPU cannot run the passes\n");
        return false;
    }
    tint_state = std::make_unique<VideoCore::Buffer>(
        instance, 0, 16, VideoCore::MemoryType::DeviceLocal, "Volumetric sky colour");
    fog_data = std::make_unique<VideoCore::Buffer>(
        instance, 0, FogBytes, VideoCore::MemoryType::DeviceLocal, "Lamp fog lights");
    return true;
}

VolumetricLight::~VolumetricLight() = default;

bool VolumetricLight::Enabled() {
    const auto& settings = BbSettings::Get();
    return !failed &&
           (settings.effects[BbSettings::EffectVolumetricLight].load(std::memory_order_relaxed) ||
            settings.effects[BbSettings::EffectPointFog].load(std::memory_order_relaxed)) &&
           Ready();
}

bool VolumetricLight::CreatePass(Pass& pass, std::initializer_list<vk::DescriptorType> types,
                                 std::span<const u32> code, u32 push_size, const char* name,
                                 bool dispatch_base) {
    return EffectPass::Create(instance, pass.set, pass.layout, pass.pipeline, types, push_size,
                              code, name, dispatch_base);
}

void VolumetricLight::EnsureImage(u32 w, u32 h) {
    if (fog && depth_width == w && depth_height == h) return;
    if (fog) {
        scheduler.DeferOperation(
            [image = std::move(fog), view = std::move(fog_view),
             filtered_image = std::move(filtered_fog), filtered_image_view = std::move(filtered_view),
             repair_image = std::move(repair_fog), repair_image_view = std::move(repair_view),
             point_image = std::move(point_fog), point_image_view = std::move(point_view),
             z = std::move(depth_snapshot), zv = std::move(depth_view)]() mutable {
                view.reset();
                image.Destroy();
                filtered_image_view.reset();
                filtered_image.Destroy();
                repair_image_view.reset();
                repair_image.Destroy();
                point_image_view.reset();
                point_image.Destroy();
                zv.reset();
                z.Destroy();
            });
    }
    depth_width = w;
    depth_height = h;
    width = (w + 1) / 2;
    height = (h + 1) / 2;
    const auto device = instance.GetDevice();
    fog = VideoCore::UniqueImage(device, instance.GetAllocator());
    fog.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    fog_view = Check<"create volumetric image view">(device.createImageViewUnique({
        .image = vk::Image(fog), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat, .subresourceRange = ColorRange,
    }));
    filtered_fog = VideoCore::UniqueImage(device, instance.GetAllocator());
    filtered_fog.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    filtered_view = Check<"create filtered volumetric view">(device.createImageViewUnique({
        .image = vk::Image(filtered_fog), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat, .subresourceRange = ColorRange,
    }));
    repair_fog = VideoCore::UniqueImage(device, instance.GetAllocator());
    repair_fog.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    repair_view = Check<"create volumetric repair view">(device.createImageViewUnique({
        .image = vk::Image(repair_fog), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat, .subresourceRange = ColorRange,
    }));
    point_fog = VideoCore::UniqueImage(device, instance.GetAllocator());
    point_fog.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    point_view = Check<"create point volumetric view">(device.createImageViewUnique({
        .image = vk::Image(point_fog), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat, .subresourceRange = ColorRange,
    }));
    depth_snapshot = VideoCore::UniqueImage(device, instance.GetAllocator());
    depth_snapshot.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR32Sfloat,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    depth_view = Check<"create volumetric depth snapshot view">(device.createImageViewUnique({
        .image = vk::Image(depth_snapshot), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR32Sfloat, .subresourceRange = ColorRange,
    }));
    SetObjectName(device, vk::Image(fog), "Directional volumetric scattering");
    SetObjectName(device, vk::Image(depth_snapshot), "Volumetric opaque depth snapshot");
    SetObjectName(device, vk::Image(filtered_fog), "Volumetric filtered scattering");
    SetObjectName(device, vk::Image(repair_fog), "Volumetric thin-surface repair");
    std::printf("Volumetric lighting: %ux%u, %u steps, range %.1f m, strength %.3f\n",
                width, height, steps, reach, strength);
}

void VolumetricLight::Integrate(VideoCore::ImageId shadow_target,
                                const vk::DescriptorImageInfo& depth,
                                const ContactShadow::Inputs& in) {
    Reset();
    if (!Enabled() || !shadow_target || !depth.imageView || !in.shadow_map ||
        !in.constants || !in.uniforms) return;
    if (in.constants_range < (in.constants_offset_word & 255u) + 336u ||
        in.uniforms_range < ((in.constants_offset_word >> 8) & 255u) + 296u) {
        failed = true;
        std::printf("Volumetric lighting: unexpected shadow constant ranges; off\n");
        return;
    }
    auto& target = texture_cache.GetImage(shadow_target);
    u32 w = target.info.size.width, h = target.info.size.height;
    if (scene_targets.Reduced() && scene_targets.EligibleScene(target)) {
        w = scene_targets.Size().width;
        h = scene_targets.Size().height;
    }
    if ((!fog || depth_width != w || depth_height != h) &&
        !EffectPass::HasRoom(instance, 2 * 8ull * ((w + 1) / 2) * ((h + 1) / 2) + 2 * 8ull * w * h + 4ull * w * h)) {
        failed = true;
        std::printf("Volumetric lighting: off - not enough video memory\n");
        return;
    }
    scheduler.EndRendering();
    EnsureImage(w, h);
    const float intensity = BbSettings::Get().graphics_volumetric_intensity.load(
        std::memory_order_relaxed);
    const float live_strength = strength *
        std::clamp(std::isfinite(intensity) ? intensity : 1.0f, 0.0f, 3.0f) *
        float(BbSettings::Get().effects[BbSettings::EffectVolumetricLight].load(
            std::memory_order_relaxed));
    const Params params{in.constants_offset_word, steps, reach, density, live_strength,
                        anisotropy, 0.02f, 0.0f};
    const vk::DescriptorImageInfo di{*sampler, depth.imageView, depth.imageLayout};
    const vk::DescriptorImageInfo si{*sampler, in.shadow_map, in.shadow_layout};
    const vk::DescriptorImageInfo out{{}, *fog_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo repaired{{}, *repair_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo zout{{}, *depth_view, vk::ImageLayout::eGeneral};
    scheduler.Record([image = vk::Image(point_fog)](vk::CommandBuffer cmd) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image, .subresourceRange = ColorRange,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        cmd.clearColorImage(image, vk::ImageLayout::eGeneral,
                            vk::ClearColorValue{std::array<float, 4>{}}, ColorRange);
    });
    for (u32 pass_index = 0; pass_index < 2; ++pass_index) {
        const bool repairing = pass_index == 1;
        const auto& pass = repairing ? repair : integrate;
        EffectPass::Mark(7, "volumetric sunlight");
        scheduler.RecordCrumb(
        {.name = repairing ? "Volumetric thin-surface repair" : "Directional volumetric integration"},
        [params, di, si, out, repaired, zout, in, repairing,
         image = repairing ? vk::Image(repair_fog) : vk::Image(fog),
         zimage = vk::Image(depth_snapshot),
         w = repairing ? depth_width : width, h = repairing ? depth_height : height,
         pipeline = *pass.pipeline, layout = *pass.layout](vk::CommandBuffer cmd) {
            Before(cmd);
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = Compute, .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image, .subresourceRange = ColorRange,
            };
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
            if (!repairing) {
                auto depth_barrier = barrier;
                depth_barrier.image = zimage;
                cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &depth_barrier});
            }
            const vk::DescriptorBufferInfo cb{in.constants, in.constants_offset, in.constants_range};
            const vk::DescriptorBufferInfo ub{in.uniforms, in.uniforms_offset, in.uniforms_range};
            const std::array<vk::WriteDescriptorSet, 7> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &cb},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &ub},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &di},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &si},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = repairing ? &repaired : &out},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &zout},
                {.dstBinding = 6, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out},
            }};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0,
                                    std::span{writes.data(), repairing ? 7u : 6u});
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
            cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            After(cmd);
        });
    }
    ready = true;
}

void VolumetricLight::Tint(const vk::DescriptorBufferInfo& lighting, u32 offset_bytes) {
    if (!ready || tinted || !Enabled()) return;
    constexpr u32 RecordBytes = 72 * sizeof(float);
    if (!lighting.buffer || lighting.range < offset_bytes + RecordBytes) {
        failed = true;
        std::printf("Volumetric lighting: unexpected directional lighting table; off\n");
        return;
    }
    // BB_VOLUMETRIC_STEADY=0 (More > Engine > Steady sunlight color): the color as it was before the
    // fallback record was skipped and the fade added.
    static const bool steady = [] {
        const char* env = std::getenv("BB_VOLUMETRIC_STEADY");
        return !(env && env[0] == '0');
    }();
    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::chrono::duration<float>(now - tint_time).count();
    tint_time = now;
    const TintParams params{
        offset_bytes, u32(std::min<vk::DeviceSize>((lighting.range - offset_bytes) / RecordBytes, 256)),
        steady && seconds <= TintSnapAfter ? 1.0f - std::exp(-seconds / TintFadeConstant) : 1.0f,
        steady ? 1u : 0u};
    scheduler.EndRendering();
    const vk::DescriptorBufferInfo state{tint_state->Handle(), 0, 16};
    EffectPass::Mark(8, "volumetric tint");
    scheduler.RecordCrumb(
        {.name = "Resolved directional volumetric tint"},
        [params, lighting, state, pipeline = *tint.pipeline,
         layout = *tint.layout](vk::CommandBuffer cmd) {
            Before(cmd);
            const std::array<vk::WriteDescriptorSet, 2> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &lighting},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &state},
            }};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
            cmd.dispatch(1, 1, 1);
            After(cmd);
        });
    tinted = true;
    if (!tint_reported) {
        tint_reported = true;
        std::printf("Volumetric lighting: resolved primary diffuse RGB, %u region records\n",
                    params.record_count);
        std::printf("Volumetric lighting: steady sunlight color %s\n", steady ? "on" : "off");
    }
}

void VolumetricLight::AddPointLights(const u32* constants, const u32* lights, u32 count) {
    if (!ready || !Enabled() ||
        !BbSettings::Get().effects[BbSettings::EffectPointFog].load(std::memory_order_relaxed)) {
        return;
    }
    if (fog_lights.Empty()) {
        std::copy_n(constants, fog_matrix.size(), fog_matrix.begin());
    }
    fog_lights.Add(lights, count);
}

void VolumetricLight::RecordLampFog(std::span<const std::array<u32, 16>> lights, u32 w, u32 h) {
    const auto& settings = BbSettings::Get();
    const float intensity = settings.graphics_point_fog_intensity.load(std::memory_order_relaxed);
    if (lights.empty() || !(intensity > 0.0f) || !std::isfinite(intensity)) {
        return;
    }
    const float glow = settings.graphics_point_fog_glow_size.load(std::memory_order_relaxed);
    std::vector<u32> data(FogLightsAt / sizeof(u32) + lights.size() * 16);
    std::copy(fog_matrix.begin(), fog_matrix.end(), data.begin());
    for (u32 i = 0; i < lights.size(); ++i) {
        std::copy(lights[i].begin(), lights[i].end(), data.begin() + FogLightsAt / 4 + i * 16);
    }
    struct PointParams {
        u32 count;
        float intensity;
        float glow_size; // Inverse-square core as a fraction of the light's radius.
    };
    const PointParams params{u32(lights.size()), std::clamp(intensity, 0.0f, 3.0f),
                             std::isfinite(glow) ? std::clamp(glow, 0.03f, 0.20f) : 0.08f};
    const vk::Buffer buffer = fog_data->Handle();
    const vk::DescriptorBufferInfo constants{buffer, 0, 64};
    const vk::DescriptorBufferInfo records{buffer, FogLightsAt, lights.size() * 64};
    const vk::DescriptorImageInfo depth{*sampler, *depth_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo out{{}, *point_view, vk::ImageLayout::eGeneral};
    EffectPass::Mark(9, "lamp fog");
    scheduler.RecordCrumb(
        {.name = "Lamp fog"},
        [params, constants, records, depth, out, buffer, data = std::move(data), w, h,
         pipeline = *point.pipeline, layout = *point.layout](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 to_transfer{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_transfer});
            cmd.updateBuffer(buffer, 0, data.size() * sizeof(u32), data.data());
            Before(cmd);
            const std::array<vk::WriteDescriptorSet, 4> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &constants},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &records},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &depth},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out},
            }};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
            cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            After(cmd);
        });
}

void VolumetricLight::Scene(VideoCore::ImageId color) {
    if (!ready || !Enabled() || !color) return;
    scene = color;
    scene_uid = texture_cache.GetImage(color).image_uid;
}

void VolumetricLight::Composite(VideoCore::ImageId fallback, const char* where) {
    const bool has_result = ready;
    const auto color_id = scene ? scene : fallback;
    const auto uid = scene ? scene_uid : (fallback ? texture_cache.GetImage(fallback).image_uid : 0);
    const bool has_tint = tinted;
    const auto seen_lights = fog_lights.Lights();
    Reset();
    if (!has_result || !Enabled() || !color_id) return;
    if (!has_tint) {
        failed = true;
        std::printf("Volumetric lighting: directional tint draw missing; off\n");
        return;
    }
    auto* color_ptr = texture_cache.TryGetImage(color_id, uid);
    if (!color_ptr) {
        failed = true;
        std::printf("Volumetric lighting: HDR scene expired before composite; off\n");
        return;
    }
    auto& color = *color_ptr;
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat) {
        failed = true;
        std::printf("Volumetric lighting: scene color is not RGBA16F; off\n");
        return;
    }
    scheduler.EndRendering();
    u32 w = color.info.size.width, h = color.info.size.height;
    vk::ImageView target_view{};
    if (scene_targets.Reduced() && scene_targets.EligibleScene(color)) {
        VideoCore::ImageViewInfo info;
        info.format = color.info.pixel_format;
        const auto proxy = scene_targets.Read(color_id, info, Compute,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        if (!(proxy.usage & vk::ImageUsageFlagBits::eStorage)) {
            failed = true;
            std::printf("Volumetric lighting: scene proxy lacks storage support; off\n");
            return;
        }
        w = scene_targets.Size().width;
        h = scene_targets.Size().height;
        target_view = proxy.view;
    } else {
        if (!(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
            failed = true;
            std::printf("Volumetric lighting: scene lacks storage support; off\n");
            return;
        }
        const auto device = instance.GetDevice();
        target_view = Check<"create volumetric scene view">(device.createImageView({
            .image = color.GetImage(), .viewType = vk::ImageViewType::e2D,
            .format = color.info.pixel_format, .subresourceRange = ColorRange,
        }));
        scheduler.DeferOperation([device, target_view] { device.destroyImageView(target_view); });
        runtime.Transit(&color, vk::ImageLayout::eGeneral, Compute,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        runtime.FlushBarriers();
    }
    if (w != depth_width || h != depth_height) {
        failed = true;
        std::printf("Volumetric lighting: scene and shadow viewport sizes differ; off\n");
        return;
    }
    // bbport: BB_POINT_FOG_STEADY=0 draws the original fog (the first lights the game lists); on
    // unless 0. Read once: the launcher passes it at game start, like the engine's other switches.
    static const bool steady_fog = [] {
        const char* env = std::getenv("BB_POINT_FOG_STEADY");
        return !(env && env[0] == '0');
    }();
    if (!BbSettings::Get().effects[BbSettings::EffectPointFog].load(std::memory_order_relaxed)) {
        fog_memory.Clear();
    } else if (!steady_fog) {
        fog_memory.Clear();
        if (!std::exchange(fog_mode_logged, true)) {
            std::printf("Lamp fog: steady fog off (draws the first %u lights the game lists)\n",
                        u32(LampFog::AsListedLimit));
        }
        RecordLampFog(LampFog::AsListed(seen_lights), w, h);
    } else {
        const auto now = std::chrono::steady_clock::now();
        // Bounded: a hitch or a menu without the scene should not count as the lights being gone.
        const float seconds = std::clamp(
            std::chrono::duration<float>(now - fog_memory_time).count(), 0.0f, 0.1f);
        fog_memory_time = now;
        // The memory fades lights at the limit; Select is the bound for the game's list as it is,
        // which the memory passes through without a camera.
        const auto drawn_lights = fog_memory.Update(seen_lights, fog_camera, seconds, FogMaxLights);
        const auto lamp_lights = LampFog::Select(drawn_lights, FogMaxLights);
        if (!std::exchange(fog_mode_logged, true)) {
            std::printf("Lamp fog: steady fog on (draws up to %u lights)\n", FogMaxLights);
        }
        RecordLampFog(lamp_lights.lights, w, h);
    }
    const vk::DescriptorImageInfo raw{*sampler, *fog_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo filtered{{}, *filtered_view, vk::ImageLayout::eGeneral};
    EffectPass::Mark(10, "volumetric filter");
    scheduler.RecordCrumb(
        {.name = "Directional volumetric filter"},
        [raw, filtered, image = vk::Image(filtered_fog), half_w = width, half_h = height,
         pipeline = *filter.pipeline, layout = *filter.layout](vk::CommandBuffer cmd) {
            Before(cmd);
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = Compute, .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image, .subresourceRange = ColorRange,
            };
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
            const std::array<vk::WriteDescriptorSet, 2> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &raw},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &filtered},
            }};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmd.dispatch((half_w + 7) / 8, (half_h + 7) / 8, 1);
            After(cmd);
        });
    const vk::DescriptorImageInfo fi{*sampler, *filtered_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo di{*sampler, *depth_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo out{{}, target_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo repaired{*sampler, *repair_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo points{*sampler, *point_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo state{tint_state->Handle(), 0, 16};
    if (!composite_reported) {
        composite_reported = true;
        std::printf("Volumetric lighting: composite into %ux%u HDR scene at %s\n", w, h, where);
    }
    EffectPass::Mark(11, "volumetric composite");
    scheduler.RecordCrumb(
        {.name = "Directional volumetric composite"},
        [fi, di, out, repaired, points, state, w, h, pipeline = *composite.pipeline,
         layout = *composite.layout](vk::CommandBuffer cmd) {
            Before(cmd);
            const std::array<vk::WriteDescriptorSet, 6> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &fi},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &di},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &state},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &repaired},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &points},
            }};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            After(cmd);
        });
}
}
