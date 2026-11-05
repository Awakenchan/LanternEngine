// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/vk_ao_spatial.h"

#include <cstdio>
#include "bbport_settings.h"
#include "common/logging/log.h"
#include "video_core/host_shaders/gtao_copy_comp.h"
#include "video_core/host_shaders/gtao_denoise_comp.h"
#include "video_core/host_shaders/gtao_denoise_full_comp.h"
#include "video_core/host_shaders/gtao_full_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {
namespace {
constexpr auto Format = vk::Format::eR8G8B8A8Unorm;
constexpr auto FullFormat = vk::Format::eR8Unorm;
constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr auto All = vk::PipelineStageFlagBits2::eAllCommands;
constexpr auto ReadWrite = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
constexpr vk::ImageSubresourceRange Range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
struct Params {
    std::array<s32, 2> origin;
    std::array<u32, 2> extent;
    u32 scene_size;
    u32 buf_off;
};
static_assert(sizeof(Params) == 24);
struct FullParams {
    std::array<u32, 2> extent;
    float strength;
    u32 buf_off;
};
static_assert(sizeof(FullParams) == 16);

void Barrier(vk::CommandBuffer cmd, vk::PipelineStageFlags2 source,
             vk::AccessFlags2 source_access, vk::PipelineStageFlags2 destination,
             vk::AccessFlags2 destination_access) {
    const vk::MemoryBarrier2 barrier{.srcStageMask = source, .srcAccessMask = source_access,
        .dstStageMask = destination, .dstAccessMask = destination_access};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

void ToGeneral(vk::CommandBuffer cmd, vk::Image image) {
    const vk::ImageMemoryBarrier2 transition{
        .dstStageMask = Compute, .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
        .image = image, .subresourceRange = Range};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &transition});
}
}

SpatialAo::SpatialAo(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                     SceneTargets& scene_targets_, VideoCore::TextureCache& texture_cache_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      scene_targets{scene_targets_}, texture_cache{texture_cache_} {
    const auto& settings = BbSettings::Get();
    enabled = settings.startup_ao_quality == 1 && settings.startup_effects[BbSettings::EffectGtao];
    if (!enabled) return;
    CreatePass(denoise, GTAO_DENOISE_COMP);
    CreatePass(copy, GTAO_COPY_COMP);
    CreatePass(full_denoise, GTAO_DENOISE_FULL_COMP);
    CreateFullPass();
}

void SpatialAo::CreatePass(Pass& pass, std::span<const u32> code) {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings{{
        {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute}}};
    pass.set = Check<"create AO spatial descriptors">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Params)};
    pass.layout = Check<"create AO spatial layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &*pass.set,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
    const auto module = CompileSPV(code, device);
    pass.pipeline = Check<"create AO spatial pipeline">(device.createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
        .layout = *pass.layout}));
    device.destroyShaderModule(module);
}

void SpatialAo::CreateFullPass() {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 6> bindings{{
        {0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {4, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eCompute},
        {5, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute}}};
    full.set = Check<"create full AO descriptors">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(FullParams)};
    full.layout = Check<"create full AO layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &*full.set,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
    const auto module = CompileSPV(GTAO_FULL_COMP, device);
    full.pipeline = Check<"create full AO pipeline">(device.createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
        .layout = *full.layout}));
    device.destroyShaderModule(module);
    sampler = Check<"create full AO sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge, .maxLod = 0.0f}));
}

bool SpatialAo::EnsureScratch(Scratch& target, u32 width, u32 height, const char* name) {
    if (target.image && target.width == width && target.height == height) return false;
    if (target.image) {
        scheduler.DeferOperation(
            [image = std::move(target.image), view = std::move(target.view)]() mutable {
                view.reset();
                image.Destroy();
            });
    }
    const auto device = instance.GetDevice();
    target.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    target.image.Create({.imageType = vk::ImageType::e2D, .format = Format,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
        .initialLayout = vk::ImageLayout::eUndefined});
    target.view = Check<"create AO spatial scratch view">(device.createImageViewUnique({
        .image = target.image, .viewType = vk::ImageViewType::e2D, .format = Format,
        .subresourceRange = Range}));
    SetObjectName(device, vk::Image(target.image), name);
    target.width = width;
    target.height = height;
    return true;
}

vk::Image SpatialAo::AcquireTarget(VideoCore::ImageId id, const VideoCore::ImageViewInfo& info) {
    auto& image = texture_cache.GetImage(id);
    scheduler.EndRendering();
    vk::Image target = image.GetImage();
    auto usage = image.usage_flags;
    if (scene_targets.Reduced() && scene_targets.Eligible(image) &&
        scene_targets.ProxyCurrent(image)) {
        const auto proxy = scene_targets.Read(id, info, Compute, ReadWrite);
        target = proxy.image;
        usage = proxy.usage;
    } else {
        runtime.Transit(&image, vk::ImageLayout::eGeneral, Compute, ReadWrite);
    }
    runtime.FlushBarriers();
    if (!(usage & vk::ImageUsageFlagBits::eStorage) ||
        !(usage & vk::ImageUsageFlagBits::eSampled))
        return {};
    return target;
}

vk::ImageView SpatialAo::ViewOf(TargetView& cache, vk::Image image, u64 uid, vk::Format format) {
    if (cache.image != image || cache.uid != uid) {
        if (cache.view)
            scheduler.DeferOperation([view = std::move(cache.view)]() mutable { view.reset(); });
        cache.view = Check<"create AO spatial target view">(
            instance.GetDevice().createImageViewUnique({
                .image = image, .viewType = vk::ImageViewType::e2D, .format = format,
                .subresourceRange = Range}));
        cache.image = image;
        cache.uid = uid;
    }
    return *cache.view;
}

void SpatialAo::WarnUnsupported(std::string_view reason) {
    if (!warned) {
        warned = true;
        LOG_WARNING(Render_Vulkan, "HQ AO: unsupported target/view ({}); unfiltered HQ AO retained",
                    reason);
    }
}

void SpatialAo::WarnFullResolution(std::string_view reason) {
    if (!full_warned) {
        full_warned = true;
        std::printf("HQ AO: full resolution skipped (%.*s); upscaled HQ AO retained\n",
                    int(reason.size()), reason.data());
    }
}

void SpatialAo::Filter(VideoCore::ImageId id, const VideoCore::ImageViewInfo& info,
                       vk::DescriptorImageInfo normals, vk::DescriptorBufferInfo constants,
                       u32 buf_offsets, u32 scene_size, u32 width, u32 height, vk::Rect2D region) {
    if (!enabled) return;
    auto& image = texture_cache.GetImage(id);
    const u32 cb_offset = buf_offsets & 255u;
    if (!normals.imageView || !constants.buffer || constants.range < cb_offset + 18 * sizeof(u32) ||
        info.format != Format || image.info.pixel_format != Format ||
        image.info.num_samples != 1 || info.range.base.level || info.range.base.layer ||
        info.range.extent.levels != 1 || info.range.extent.layers != 1 ||
        !width || !height || region.offset.x < 0 || region.offset.y < 0 ||
        !region.extent.width || !region.extent.height ||
        u64(region.offset.x) + region.extent.width > width ||
        u64(region.offset.y) + region.extent.height > height) {
        WarnUnsupported(fmt::format(
            "normals={} cb={} range={} view={} image={} samples={} level={}+{} layer={}+{} "
            "size={}x{} region={},{} {}x{}",
            bool(normals.imageView), bool(constants.buffer), u64(constants.range),
            vk::to_string(info.format), vk::to_string(image.info.pixel_format),
            image.info.num_samples, info.range.base.level, info.range.extent.levels,
            info.range.base.layer, info.range.extent.layers, width, height, region.offset.x,
            region.offset.y, region.extent.width, region.extent.height));
        return;
    }
    const vk::Image target = AcquireTarget(id, info);
    if (!target) {
        WarnUnsupported("usage=" + vk::to_string(image.usage_flags));
        return;
    }
    const vk::ImageView view = ViewOf(target_view, target, image.image_uid, Format);
    const bool created = EnsureScratch(scratch, width, height, "HQ AO spatial scratch");
    const Params params{{region.offset.x, region.offset.y},
        {region.extent.width, region.extent.height}, scene_size, cb_offset};
    scheduler.RecordCrumb({.name = "HQ AO spatial filter"},
        [params, created, normals, constants, target = view,
         temp_image = vk::Image(scratch.image), temp = *scratch.view,
         denoise_pipeline = *denoise.pipeline, denoise_layout = *denoise.layout,
         copy_pipeline = *copy.pipeline, copy_layout = *copy.layout](vk::CommandBuffer cmd) {
            Barrier(cmd, All, vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                Compute, ReadWrite);
            if (created) ToGeneral(cmd, temp_image);
            const auto run = [&](vk::Pipeline pipeline, vk::PipelineLayout layout,
                                 vk::ImageView input, vk::ImageView output) {
                const std::array<vk::DescriptorImageInfo, 3> infos{{
                    {.imageView = input, .imageLayout = vk::ImageLayout::eGeneral}, normals,
                    {.imageView = output, .imageLayout = vk::ImageLayout::eGeneral}}};
                std::array<vk::WriteDescriptorSet, 4> writes{};
                for (u32 i = 0; i < infos.size(); ++i)
                    writes[i] = {.dstBinding = i, .descriptorCount = 1,
                        .descriptorType = i == 2 ? vk::DescriptorType::eStorageImage :
                                                  vk::DescriptorType::eSampledImage,
                        .pImageInfo = &infos[i]};
                writes[3] = {.dstBinding = 3, .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eUniformBuffer, .pBufferInfo = &constants};
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
                cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
                cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                                  sizeof(params), &params);
                cmd.dispatch((params.extent[0] + 7) / 8, (params.extent[1] + 7) / 8, 1);
            };
            run(denoise_pipeline, denoise_layout, target, temp);
            Barrier(cmd, Compute, ReadWrite, Compute, ReadWrite);
            run(copy_pipeline, copy_layout, temp, target);
            Barrier(cmd, Compute, ReadWrite,
                All, vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        });
    if (!reported) {
        reported = true;
        std::printf("HQ AO: three directions, spatial denoise %ux%u; per-frame, no history (ghost-free)\n",
                    width, height);
    }
}

void SpatialAo::NoteGather(VideoCore::ImageId target, vk::DescriptorImageInfo normals) {
    if (!enabled || !normals.imageView) return;
    pending = {target, normals};
}

void SpatialAo::FullResolution(VideoCore::ImageId id, const VideoCore::ImageViewInfo& info,
                               vk::DescriptorImageInfo depth, vk::DescriptorBufferInfo constants,
                               vk::DescriptorBufferInfo far_constants, u32 buf_offsets,
                               float strength) {
    const auto normals = pending.normals;
    pending = {};
    if (!enabled) return;
    auto& image = texture_cache.GetImage(id);
    // gtao.frag reads Cb0 at byte 0 of buf_off and the far plane (its Cb2) at byte 2.
    const u32 cb_offset = buf_offsets & 255u, far_offset = (buf_offsets >> 8) & 255u;
    if (!depth.imageView || !normals.imageView || !constants.buffer ||
        constants.range < cb_offset + 43 * sizeof(u32) || !far_constants.buffer ||
        far_constants.range < far_offset + 3 * sizeof(u32) || info.format != FullFormat ||
        image.info.pixel_format != FullFormat || image.info.num_samples != 1 ||
        info.range.base.level || info.range.base.layer || info.range.extent.levels != 1 ||
        info.range.extent.layers != 1) {
        WarnFullResolution(fmt::format("depth={} normals={} cb={} far={} view={} image={} "
            "samples={} level={}+{} layer={}+{}", bool(depth.imageView), bool(normals.imageView),
            u64(constants.range), u64(far_constants.range), vk::to_string(info.format),
            vk::to_string(image.info.pixel_format), image.info.num_samples,
            info.range.base.level, info.range.extent.levels, info.range.base.layer,
            info.range.extent.layers));
        return;
    }
    const bool proxied = scene_targets.Reduced() && scene_targets.Eligible(image) &&
                         scene_targets.ProxyCurrent(image);
    const auto size = proxied ? scene_targets.ProxySize(image)
                              : SceneResolution::Size{image.info.size.width, image.info.size.height};
    const vk::Image target = AcquireTarget(id, info);
    if (!target || !size.width || !size.height) {
        WarnFullResolution("usage=" + vk::to_string(image.usage_flags));
        return;
    }
    const vk::ImageView view = ViewOf(full_view, target, image.image_uid, FullFormat);
    const bool created = EnsureScratch(full_scratch, size.width, size.height,
                                       "HQ AO full resolution scratch");
    const FullParams gather{{size.width, size.height}, strength, cb_offset | (far_offset << 16)};
    const Params filter{{0, 0}, {size.width, size.height}, 0, cb_offset};
    scheduler.RecordCrumb({.name = "HQ AO full resolution"},
        [gather, filter, created, depth, normals, constants, far_constants, target = view,
         temp_image = vk::Image(full_scratch.image), temp = *full_scratch.view,
         sampler = *sampler, full_pipeline = *full.pipeline, full_layout = *full.layout,
         denoise_pipeline = *full_denoise.pipeline,
         denoise_layout = *full_denoise.layout](vk::CommandBuffer cmd) {
            Barrier(cmd, All, vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                Compute, ReadWrite);
            if (created) ToGeneral(cmd, temp_image);
            const vk::DescriptorImageInfo depth_info{{}, depth.imageView, depth.imageLayout};
            const vk::DescriptorImageInfo normal_info{{}, normals.imageView, normals.imageLayout};
            const vk::DescriptorImageInfo sampler_info{sampler, {}, {}};
            const vk::DescriptorImageInfo scratch_info{{}, temp, vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 6> gather_writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eUniformBuffer, .pBufferInfo = &constants},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eUniformBuffer,
                 .pBufferInfo = &far_constants},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &depth_info},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &normal_info},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampler, .pImageInfo = &sampler_info},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &scratch_info}}};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, full_pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, full_layout, 0,
                                     gather_writes);
            cmd.pushConstants(full_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(gather),
                              &gather);
            cmd.dispatch((gather.extent[0] + 7) / 8, (gather.extent[1] + 7) / 8, 1);
            Barrier(cmd, Compute, ReadWrite, Compute, ReadWrite);

            const vk::DescriptorImageInfo output_info{{}, target, vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 4> denoise_writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &scratch_info},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &normal_info},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &output_info},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eUniformBuffer, .pBufferInfo = &constants}}};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, denoise_pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, denoise_layout, 0,
                                     denoise_writes);
            cmd.pushConstants(denoise_layout, vk::ShaderStageFlagBits::eCompute, 0,
                              sizeof(filter), &filter);
            cmd.dispatch((filter.extent[0] + 7) / 8, (filter.extent[1] + 7) / 8, 1);
            Barrier(cmd, Compute, ReadWrite,
                All, vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        });
    if (!full_reported) {
        full_reported = true;
        std::printf("HQ AO: full internal resolution %ux%u (three directions, spatial denoise)\n",
                    size.width, size.height);
    }
}
} // namespace Vulkan
