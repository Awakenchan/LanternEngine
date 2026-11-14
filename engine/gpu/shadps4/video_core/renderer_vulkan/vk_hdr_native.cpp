// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: native HDR output (vk_hdr_native.h, hdr_native.comp, docs/hdr.md).

#include "video_core/renderer_vulkan/vk_hdr_native.h"
#include "video_core/renderer_vulkan/vk_hdr_channel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>

#include <vk_mem_alloc.h>

#include "bbport_settings.h"
#include "common/assert.h"
#include "video_core/host_shaders/hdr_native_exposure_comp.h"
#include "video_core/host_shaders/hdr_native_gain_comp.h"
#include "video_core/host_shaders/hdr_native_luma_comp.h"
#include "video_core/host_shaders/hdr_native_snapshot_comp.h"
#include "video_core/host_shaders/hdr_native_stats_comp.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

/// Push constants of hdr_native.comp.
struct ExposureParams {
    u32 count;
    u32 reset;
    float min_samples;
    float rate;
    u32 groups_x;
    float min_tile;
};
struct GainParams {
    float strength;
    float white;
    float knee;
    float rolloff;
};

/// SDR brightness (linear brightest channel) where native mode starts to brighten: everything
/// darker is the game's SDR picture exactly. Lower than expand mode's 0.8: the gain here is
/// measured, not guessed, and the sky and lit fog sit in the game's shoulder too.
constexpr float Knee = 0.5f;
/// Exposure smoothing per frame (the game's eye adaptation is slower), and the least share of a
/// frame's pixels in the midtones to measure it (a black or white-out frame keeps the last one).
constexpr float ExposureRate = 0.1f;
constexpr u32 MinSampleShare = 200;
/// hdr_native.comp's state: the Exposure struct, the tone curve's bins and the tiles' exposures
/// (a vec4 each).
constexpr u32 CurveBins = 16;
constexpr u32 Tiles = 8 * 6;
constexpr vk::DeviceSize StateSize = (1 + CurveBins + Tiles) * 4 * sizeof(float);
/// vec4s per STATS group in the partial sums: the exposure, then two curve bins per vec4.
constexpr u32 PartialStride = 1 + CurveBins / 2;
/// Highlights are kept as they are up to this share of the range from SDR white to the peak (in
/// stops), then rolled off towards the peak (hdr_native.comp RollOff).
constexpr float RollOffStart = 0.7f;

constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr auto AllCommands = vk::PipelineStageFlagBits2::eAllCommands;
constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;

/// Everything before (the game's passes and transitions, this pass's inputs, earlier passes
/// reading what this one writes) finished and visible to the compute shaders.
void BeforeCompute(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = AllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = Compute,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

/// The compute shaders' writes visible to what follows; later writes to what they read wait.
void AfterCompute(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = Compute,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = AllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

/// A port image about to be rewritten whole by a compute shader: its old contents go.
vk::ImageMemoryBarrier2 ToWrite(vk::Image image) {
    return {
        .srcStageMask = AllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = Compute,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

vk::DescriptorImageInfo Sampled(vk::ImageView view) {
    return {.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
}

vk::WriteDescriptorSet Write(u32 binding, vk::DescriptorType type,
                             const vk::DescriptorImageInfo* image,
                             const vk::DescriptorBufferInfo* buffer = nullptr) {
    return {
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = image,
        .pBufferInfo = buffer,
    };
}

constexpr auto SampledImage = vk::DescriptorType::eSampledImage;
constexpr auto StorageImage = vk::DescriptorType::eStorageImage;
constexpr auto StorageBuffer = vk::DescriptorType::eStorageBuffer;

void PrintOnce(bool& printed, const char* text) {
    if (!printed) {
        printed = true;
        std::printf("%s\n", text);
    }
}

} // namespace

HdrNative::HdrNative(const Instance& instance_, Scheduler& scheduler_,
                     VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_} {
    CreatePipelines();
    // The exposure state: the exposure, the tone curve and the tiles, which the EXPOSURE pass
    // keeps across frames (cleared by the first pass that uses it).
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = StateSize,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer buffer{};
    const VkResult result = vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci,
                                            &buffer, &state.allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "HDR native: exposure buffer allocation failed");
    state.buffer = vk::Buffer{buffer};
    state.size = StateSize;
}

HdrNative::~HdrNative() {
    // The presenter finished the GPU's work before the rasterizer goes.
    const auto device = instance.GetDevice();
    for (const auto& entry : view_cache) {
        if (entry.view) {
            device.destroyImageView(entry.view);
        }
    }
    for (Buffer* buffer : {&partials, &state}) {
        if (buffer->buffer) {
            vmaDestroyBuffer(instance.GetAllocator(), buffer->buffer, buffer->allocation);
        }
    }
}

bool HdrNative::Wanted() {
    if (HdrChannel::Native16()) return false;
    const auto& s = BbSettings::Get();
    return s.hdr_output.load(std::memory_order_relaxed) != BbSettings::HdrOutputOff &&
           s.hdr_mode.load(std::memory_order_relaxed) == BbSettings::HdrModeNative;
}

void HdrNative::CreatePipelines() {
    const auto device = instance.GetDevice();
    const auto make = [&](std::initializer_list<vk::DescriptorType> types, u32 push_size,
                          std::span<const u32> code, const char* name,
                          vk::UniqueDescriptorSetLayout& set_layout,
                          vk::UniquePipelineLayout& pipeline_layout, vk::UniquePipeline& pipeline) {
        std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
        u32 count = 0;
        for (const vk::DescriptorType type : types) {
            bindings[count] = {.binding = count,
                               .descriptorType = type,
                               .descriptorCount = 1,
                               .stageFlags = vk::ShaderStageFlagBits::eCompute};
            ++count;
        }
        set_layout = Check<"create HDR native descriptor set layout">(
            device.createDescriptorSetLayoutUnique({
                .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                .bindingCount = count,
                .pBindings = bindings.data(),
            }));
        const vk::PushConstantRange push{
            .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = push_size};
        pipeline_layout = Check<"create HDR native pipeline layout">(
            device.createPipelineLayoutUnique({
                .setLayoutCount = 1,
                .pSetLayouts = &*set_layout,
                .pushConstantRangeCount = push_size ? 1u : 0u,
                .pPushConstantRanges = push_size ? &push : nullptr,
            }));
        const auto module = CompileSPV(code, device);
        ASSERT(module);
        SetObjectName(device, module, name);
        pipeline = Check<"create HDR native pipeline">(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *pipeline_layout,
                }));
        device.destroyShaderModule(module);
    };
    make({SampledImage, StorageImage}, 0, HDR_NATIVE_LUMA_COMP, "hdr_native.comp (luma)",
         luma_layout, luma_pipeline_layout, luma_pipeline);
    make({SampledImage, SampledImage, StorageBuffer, StorageBuffer}, sizeof(u32),
         HDR_NATIVE_STATS_COMP, "hdr_native.comp (stats)", stats_layout, stats_pipeline_layout,
         stats_pipeline);
    make({SampledImage, SampledImage, StorageBuffer, StorageBuffer, StorageImage}, sizeof(u32),
         HDR_NATIVE_SNAPSHOT_COMP, "hdr_native.comp (snapshot)", snapshot_layout,
         snapshot_pipeline_layout, snapshot_pipeline);
    make({StorageBuffer, StorageBuffer}, sizeof(ExposureParams), HDR_NATIVE_EXPOSURE_COMP,
         "hdr_native.comp (exposure)", exposure_layout, exposure_pipeline_layout,
         exposure_pipeline);
    make({SampledImage, SampledImage, SampledImage, StorageBuffer, StorageImage},
         sizeof(GainParams), HDR_NATIVE_GAIN_COMP, "hdr_native.comp (gain)", gain_layout,
         gain_pipeline_layout, gain_pipeline);
}

vk::ImageView HdrNative::CachedView(const VideoCore::Image& image, vk::Format format) {
    // As TemporalUpscaler::CachedView: the scene color and the UI target stay the same images
    // frame after frame; a view per frame would be created and destroyed for nothing.
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &view_cache[0];
    for (auto& entry : view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    // Sampled only: the guest image may also be a storage image, which an sRGB view cannot be.
    const vk::ImageViewUsageCreateInfo usage{.usage = vk::ImageUsageFlagBits::eSampled};
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .view = Check<"create HDR native view">(device.createImageView({
            .pNext = &usage,
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = ColorRange,
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

HdrNative::Source HdrNative::Guest(VideoCore::ImageId id, vk::Format format) {
    auto& image = texture_cache.GetImage(id);
    const vk::ImageView view = CachedView(image, format);
    scheduler.EndRendering();
    // NativeAccess (in Transit) resolves a reduced-size scene proxy into the image first, as the
    // game's own compute passes reading it do.
    runtime.Transit(&image, vk::ImageLayout::eGeneral, Compute, vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    return {image.GetImage(), view, image.info.size.width, image.info.size.height};
}

void HdrNative::EnsureImage(OwnImage& own, u32 width, u32 height, vk::Format format,
                            const char* name) {
    if (own.image && own.width == width && own.height == height) {
        return;
    }
    if (own.image) {
        // Recorded passes may still read the old size's image.
        scheduler.DeferOperation(
            [image = std::move(own.image), view = std::move(own.view)]() mutable {
                view.reset();
                image.Destroy();
            });
    }
    const auto device = instance.GetDevice();
    own.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    own.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, vk::Image(own.image), "HDR native {}", name);
    own.view = Check<"create HDR native image view">(device.createImageViewUnique({
        .image = vk::Image(own.image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = ColorRange,
    }));
    own.width = width;
    own.height = height;
}

void HdrNative::EnsureGainMap(std::unique_ptr<HdrGainMap>& map, u32 width, u32 height) {
    if (map && map->width == width && map->height == height) {
        return;
    }
    if (map) {
        // The output size changed. A map handed back by the presenter is idle (its frame's
        // fence was waited); one made at an earlier display pass may still be written.
        scheduler.DeferOperation([old = std::move(map)]() mutable { old.reset(); });
    }
    const auto device = instance.GetDevice();
    map = std::make_unique<HdrGainMap>();
    map->image = VideoCore::UniqueImage(device, instance.GetAllocator());
    map->image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16Sfloat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, vk::Image(map->image), "HDR native gain map");
    map->view = Check<"create HDR gain map view">(device.createImageViewUnique({
        .image = vk::Image(map->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16Sfloat,
        .subresourceRange = ColorRange,
    }));
    map->width = width;
    map->height = height;
}

void HdrNative::EnsurePartials(u32 groups) {
    const vk::DeviceSize size = vk::DeviceSize(std::max(groups, 1u)) * PartialStride * 4 * sizeof(float);
    if (partials.buffer && partials.size >= size) {
        return;
    }
    if (partials.buffer) {
        scheduler.DeferOperation([allocator = instance.GetAllocator(), old = partials] {
            vmaDestroyBuffer(allocator, old.buffer, old.allocation);
        });
    }
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer buffer{};
    const VkResult result = vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci,
                                            &buffer, &partials.allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "HDR native: statistics buffer allocation failed");
    partials.buffer = vk::Buffer{buffer};
    partials.size = size;
}

void HdrNative::OnSceneColor(VideoCore::ImageId id) {
    if (luma_ready || !id || !Wanted()) {
        return;
    }
    const auto& image = texture_cache.GetImage(id);
    if (image.info.pixel_format != vk::Format::eR16G16B16A16Sfloat) {
        static bool printed = false;
        PrintOnce(printed, "HDR native: the scene color is not RGBA16F; native mode falls back");
        return;
    }
    // A reduced scene's resolve (in Guest) counts as the game's: the post would do it anyway.
    const Source scene = Guest(id, vk::Format::eR16G16B16A16Sfloat);
    auto* profiler = GpuProfiler::Get();
    const u64 resume = profiler ? profiler->Current() : 0;
    if (profiler) {
        profiler->Mark(0x4D52'0001ull, [] { return std::string{"HDR native: scene luma"}; });
    }
    const u32 width = (scene.width + 1) / 2, height = (scene.height + 1) / 2;
    EnsureImage(luma, width, height, vk::Format::eR16G16Sfloat, "scene luma");
    scheduler.RecordCrumb(
        {.name = "HDR native scene luma"},
        [scene_view = scene.view, image = vk::Image(luma.image), view = *luma.view, width, height,
         pipeline = *luma_pipeline, layout = *luma_pipeline_layout](vk::CommandBuffer cmdbuf) {
            BeforeCompute(cmdbuf);
            const auto to_write = ToWrite(image);
            cmdbuf.pipelineBarrier2(
                {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
            const std::array infos{Sampled(scene_view), Sampled(view)};
            const std::array writes{Write(0, SampledImage, &infos[0]),
                                    Write(1, StorageImage, &infos[1])};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
            AfterCompute(cmdbuf);
        });
    if (resume) {
        profiler->Resume(resume);
    }
    luma_ready = true;
}

void HdrNative::OnUiStart(const Source& frame) {
    if (!WantsUiStart()) {
        return;
    }
    ui_started = true;
    if (!frame.view || !frame.width || !frame.height || !Wanted()) {
        return;
    }
    EnsureImage(snapshot, frame.width, frame.height, vk::Format::eR8G8B8A8Unorm,
                "frame before UI");
    RecordExposure(frame, true);
    snapshot_ready = true;
    snapshot_source = frame.image;
}

void HdrNative::RecordExposure(const Source& frame, bool copy) {
    auto* profiler = GpuProfiler::Get();
    const u64 resume = profiler ? profiler->Current() : 0;
    if (profiler) {
        profiler->Mark(0x4D52'0002ull, [] { return std::string{"HDR native: exposure"}; });
    }
    const u32 groups_x = (frame.width + 15) / 16, groups_y = (frame.height + 15) / 16;
    EnsurePartials(groups_x * groups_y);
    const u32 min_samples = std::max(1024u, frame.width * frame.height / MinSampleShare);
    const ExposureParams params{
        .count = groups_x * groups_y,
        .reset = measured_last_frame ? 0u : 1u,
        .min_samples = float(min_samples),
        .rate = ExposureRate,
        .groups_x = groups_x,
        // A tile is a 48th of the frame: a quarter of its share of the frame's minimum.
        .min_tile = float(std::max(64u, min_samples / Tiles / 4)),
    };
    const bool clear_state = !state_cleared;
    state_cleared = true;
    scheduler.EndRendering();
    scheduler.RecordCrumb(
        {.name = "HDR native exposure"},
        [frame_view = frame.view, luma = *luma.view, snapshot_image = vk::Image(snapshot.image),
         snapshot_view = copy ? *snapshot.view : vk::ImageView{}, partials = partials.buffer,
         partials_size = partials.size, state = state.buffer, groups_x, groups_y, params,
         clear_state, stats = copy ? *snapshot_pipeline : *stats_pipeline,
         stats_layout = copy ? *snapshot_pipeline_layout : *stats_pipeline_layout,
         exposure = *exposure_pipeline,
         exposure_layout = *exposure_pipeline_layout](vk::CommandBuffer cmdbuf) {
            if (clear_state) {
                cmdbuf.fillBuffer(state, 0, StateSize, 0);
                const vk::MemoryBarrier2 cleared{
                    .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .dstStageMask = Compute,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                                     vk::AccessFlagBits2::eShaderWrite,
                };
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &cleared});
            }
            BeforeCompute(cmdbuf);
            if (snapshot_view) {
                const auto to_write = ToWrite(snapshot_image);
                cmdbuf.pipelineBarrier2(
                    {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
            }
            const std::array infos{Sampled(frame_view), Sampled(luma), Sampled(snapshot_view)};
            const vk::DescriptorBufferInfo partials_info{partials, 0, partials_size};
            const vk::DescriptorBufferInfo state_info{state, 0, StateSize};
            const std::array writes{Write(0, SampledImage, &infos[0]),
                                    Write(1, SampledImage, &infos[1]),
                                    Write(2, StorageBuffer, nullptr, &partials_info),
                                    Write(3, StorageBuffer, nullptr, &state_info),
                                    Write(4, StorageImage, &infos[2])};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, stats);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, stats_layout, 0,
                                        vk::ArrayProxy<const vk::WriteDescriptorSet>(
                                            snapshot_view ? 5u : 4u, writes.data()));
            cmdbuf.pushConstants(stats_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(params.reset), &params.reset);
            cmdbuf.dispatch(groups_x, groups_y, 1);
            // The partial sums, added up by one group (the exposure and the curve) and one per
            // tile, which then update the state the groups above read.
            const vk::MemoryBarrier2 summed{
                .srcStageMask = Compute,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = Compute,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &summed});
            const std::array exposure_writes{Write(0, StorageBuffer, nullptr, &partials_info),
                                             Write(1, StorageBuffer, nullptr, &state_info)};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, exposure);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, exposure_layout, 0,
                                        exposure_writes);
            cmdbuf.pushConstants(exposure_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(params), &params);
            cmdbuf.dispatch(1 + Tiles, 1, 1);
            AfterCompute(cmdbuf);
        });
    if (resume) {
        profiler->Resume(resume);
    }
}

void HdrNative::RecordGain(const Source& frame, const Source& before_ui, const HdrGainMap& map) {
    auto* profiler = GpuProfiler::Get();
    const u64 resume = profiler ? profiler->Current() : 0;
    if (profiler) {
        profiler->Mark(0x4D52'0003ull, [] { return std::string{"HDR native: gain map"}; });
    }
    const auto& s = BbSettings::Get();
    const float paper_white = std::clamp(s.hdr_paper_white.load(), BbSettings::HdrPaperWhiteMin,
                                         BbSettings::HdrPaperWhiteMax);
    const float peak = BbSettings::HdrPeakNits(s);
    // Boost in native mode: the share of the scene's highlight range shown, in log2. Its square
    // root, so the shared default (0.25, chosen for expand mode) shows half of it: a lamp the
    // scene has 10x brighter than SDR white comes out 3.2x.
    const GainParams params{
        .strength = std::sqrt(std::clamp(s.hdr_boost.load(), 0.0f, 1.0f)),
        .white = peak / paper_white,
        .knee = Knee,
        .rolloff = RollOffStart,
    };
    scheduler.EndRendering();
    scheduler.RecordCrumb(
        {.name = "HDR native gain map"},
        [frame_view = frame.view, before_view = before_ui.view, luma = *luma.view,
         state = state.buffer, image = vk::Image(map.image), view = *map.view, width = map.width,
         height = map.height, params, pipeline = *gain_pipeline,
         layout = *gain_pipeline_layout](vk::CommandBuffer cmdbuf) {
            BeforeCompute(cmdbuf);
            const auto to_write = ToWrite(image);
            cmdbuf.pipelineBarrier2(
                {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
            const std::array infos{Sampled(frame_view), Sampled(before_view), Sampled(luma),
                                   Sampled(view)};
            const vk::DescriptorBufferInfo state_info{state, 0, StateSize};
            const std::array writes{Write(0, SampledImage, &infos[0]),
                                    Write(1, SampledImage, &infos[1]),
                                    Write(2, SampledImage, &infos[2]),
                                    Write(3, StorageBuffer, nullptr, &state_info),
                                    Write(4, StorageImage, &infos[3])};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                                 &params);
            cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
            // Read by the HDR encode on the present thread's submission, which waits for this
            // frame's work (Presenter::Present); in General layout, sampled.
            const vk::ImageMemoryBarrier2 to_read{
                .srcStageMask = Compute,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = image,
                .subresourceRange = ColorRange,
            };
            cmdbuf.pipelineBarrier2(
                {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
            AfterCompute(cmdbuf);
        });
    if (resume) {
        profiler->Resume(resume);
    }
}

void HdrNative::OnDisplayPass(VAddr address, const Source& frame) {
    bool made = false;
    bool measured = snapshot_ready;
    if (luma_ready && frame.view && frame.width && frame.height && Wanted()) {
        // With a UI-free copy the display pass must read the image it was taken from, at the
        // same size; without a UI pass this frame (in-engine cutscenes) the frame has no UI and
        // is its own copy. A UI pass without a copy (its target not found): no map, the UI
        // must not be brightened.
        const bool matches = snapshot_ready ? snapshot_source == frame.image &&
                                                  snapshot.width == frame.width &&
                                                  snapshot.height == frame.height
                                            : !ui_started;
        if (matches) {
            if (!snapshot_ready) {
                RecordExposure(frame, false);
                measured = true;
            }
            const Source before_ui =
                snapshot_ready ? Source{vk::Image(snapshot.image), *snapshot.view, snapshot.width,
                                        snapshot.height}
                               : frame;
            std::scoped_lock lock{slots_mutex};
            auto& slot = slots[address];
            EnsureGainMap(slot.map, frame.width, frame.height);
            RecordGain(frame, before_ui, *slot.map);
            slot.valid = true;
            made = true;
        } else {
            static bool printed = false;
            PrintOnce(printed, "HDR native: no UI-free copy of the image the display pass reads "
                               "(UI target not found); such frames fall back to expand");
        }
    }
    if (!made) {
        std::scoped_lock lock{slots_mutex};
        if (const auto it = slots.find(address); it != slots.end()) {
            it->second.valid = false;
        }
    }
    measured_last_frame = measured;
    EndFrame();
}

bool HdrNative::TakeGainMap(VAddr address, std::unique_ptr<HdrGainMap>& map) {
    std::scoped_lock lock{slots_mutex};
    const auto it = slots.find(address);
    if (it == slots.end() || !it->second.valid || !it->second.map) {
        return false;
    }
    std::swap(map, it->second.map);
    it->second.valid = false;
    return true;
}

void HdrNative::EndFrame() {
    luma_ready = false;
    ui_started = false;
    snapshot_ready = false;
    snapshot_source = vk::Image{};
}

} // namespace Vulkan
