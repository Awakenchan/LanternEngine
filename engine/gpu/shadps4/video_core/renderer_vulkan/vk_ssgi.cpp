// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_ssgi.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "common/assert.h"
#include "bbport_settings.h"
#include "video_core/host_shaders/ssgi_composite_comp.h"
#include "video_core/host_shaders/ssgi_denoise_comp.h"
#include "video_core/host_shaders/ssgi_gather_comp.h"
#include "video_core/host_shaders/ssgi_gather_ray_comp.h"
#include "video_core/host_shaders/ssgi_prep_comp.h"
#include "video_core/host_shaders/ssgi_probe_composite_comp.h"
#include "video_core/host_shaders/ssgi_probe_trace_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_effect_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_rt_shadow.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {
namespace {
struct Params {
    std::array<float, 4> proj;
    std::array<float, 12> reproject;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    std::array<float, 2> prev_jitter;
    std::array<float, 2> prev_proj;
    u32 frame;
    u32 history;
    float strength;
    float radius;
    float feedback;
    float ao_amount;
    float albedo_on;
    float step_size;
};
static_assert(sizeof(Params) == 128);

std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        }
        r[i * 4 + 3] = a[i * 4] * b[3] + a[i * 4 + 1] * b[7] + a[i * 4 + 2] * b[11] + a[i * 4 + 3];
    }
    return r;
}

constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr auto AllCommands = vk::PipelineStageFlagBits2::eAllCommands;
constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr auto SampledImage = vk::DescriptorType::eSampledImage;
constexpr auto StorageImage = vk::DescriptorType::eStorageImage;

void BeforeCompute(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = AllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = Compute,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

void BetweenPasses(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = Compute,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = Compute,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

void AfterCompute(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = Compute,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = AllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

vk::ImageMemoryBarrier2 ToGeneral(vk::Image image) {
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

vk::DescriptorImageInfo Info(vk::ImageView view) {
    return {.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
}

vk::WriteDescriptorSet Write(u32 binding, vk::DescriptorType type,
                             const vk::DescriptorImageInfo* image) {
    return {.dstBinding = binding, .descriptorCount = 1, .descriptorType = type,
            .pImageInfo = image};
}

}

ScreenSpaceGI::ScreenSpaceGI(const Instance& instance_, Scheduler& scheduler_,
                             VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                             SceneTargets& scene_targets_, CameraMotion& camera_motion_,
                             RtShadow& rt_shadow_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, scene_targets{scene_targets_}, rt_shadow{rt_shadow_},
      camera_motion{camera_motion_} {
}

bool ScreenSpaceGI::Ready() {
    if (tried) {
        return !failed;
    }
    tried = true;
    const bool made =
        EffectPass::StorageFormat(instance, vk::Format::eR16G16B16A16Sfloat) &&
        EffectPass::StorageFormat(instance, vk::Format::eR32G32B32A32Sfloat) &&
        CreatePass(prep, {SampledImage, StorageImage, SampledImage}, SSGI_PREP_COMP,
                   "SSGI prep") &&
        CreatePass(gather, {SampledImage, SampledImage, SampledImage, StorageImage, SampledImage},
                   SSGI_GATHER_COMP, "SSGI gather") &&
        (!rt_shadow.Capturing() ||
         CreatePass(gather_ray, {SampledImage, SampledImage, SampledImage, StorageImage, SampledImage,
                                 vk::DescriptorType::eAccelerationStructureKHR},
                    SSGI_GATHER_RAY_COMP, "Ray traced bounce")) &&
        CreatePass(denoise, {SampledImage, SampledImage, StorageImage}, SSGI_DENOISE_COMP,
                   "SSGI denoise") &&
        CreatePass(composite, {SampledImage, SampledImage, StorageImage, SampledImage, SampledImage},
                   SSGI_COMPOSITE_COMP, "SSGI composite") &&
        CreatePass(probe_trace,
                   {SampledImage, SampledImage, SampledImage, SampledImage, SampledImage,
                    SampledImage, SampledImage, SampledImage, SampledImage, StorageImage,
                    StorageImage, StorageImage, StorageImage, StorageImage},
                   SSGI_PROBE_TRACE_COMP, "SSGI probe trace") &&
        CreatePass(probe_composite,
                   {SampledImage, SampledImage, SampledImage, SampledImage, SampledImage,
                    SampledImage, StorageImage, SampledImage, SampledImage, SampledImage,
                    SampledImage, SampledImage, SampledImage, StorageImage, StorageImage},
                   SSGI_PROBE_COMPOSITE_COMP, "SSGI probe composite");
    if (!made) {
        failed = true;
        std::printf("SSGI: off - this GPU cannot run the passes\n");
    }
    return made;
}

ScreenSpaceGI::~ScreenSpaceGI() {
    const auto device = instance.GetDevice();
    for (const auto& entry : view_cache) {
        if (entry.view) {
            device.destroyImageView(entry.view);
        }
    }
}

bool ScreenSpaceGI::CreatePass(Pass& pass, std::initializer_list<vk::DescriptorType> types,
                               std::span<const u32> code, const char* name) {
    return EffectPass::Create(instance, pass.set_layout, pass.layout, pass.pipeline, types,
                              sizeof(Params), code, name);
}

bool ScreenSpaceGI::EnsureImage(OwnImage& own, u32 width, u32 height, vk::Format format,
                                const char* name) {
    if (own.image && own.width == width && own.height == height) {
        return false;
    }
    if (own.image) {
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
    SetObjectName(device, vk::Image(own.image), "SSGI {}", name);
    own.view = Check<"create SSGI image view">(device.createImageViewUnique({
        .image = vk::Image(own.image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = ColorRange,
    }));
    own.width = width;
    own.height = height;
    return true;
}

vk::ImageView ScreenSpaceGI::CachedView(const VideoCore::Image& image, vk::Format format,
                                        vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &view_cache[0];
    for (auto& entry : view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
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
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check<"create SSGI view">(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

void ScreenSpaceGI::OnGBufferDraw(VideoCore::ImageId depth, VideoCore::ImageId albedo,
                                  VideoCore::ImageId normal, VideoCore::ImageId material,
                                  VideoCore::ImageId specular) {
    gbuffer_depth = depth;
    gbuffer_albedo = albedo;
    gbuffer_normal = normal;
    gbuffer_material = material;
    gbuffer_specular = specular;
}

void ScreenSpaceGI::OnAoDraw(VideoCore::ImageId ao) {
    ao_image = ao;
}

void ScreenSpaceGI::OnFrameEnd() {
    done_this_frame = false;
    ao_image = {};
    gbuffer_depth = {};
    gbuffer_albedo = {};
    gbuffer_normal = {};
    gbuffer_material = {};
    gbuffer_specular = {};
}

void ScreenSpaceGI::OnOpaqueScene(VideoCore::ImageId color_id, VideoCore::ImageId depth_id,
                                  std::array<float, 2> jitter) {
    if (done_this_frame || failed || !color_id || !depth_id ||
        (!BbSettings::Get().effects[BbSettings::EffectGi].load(std::memory_order_relaxed) &&
         !rt_shadow.Wanted()) ||
        !Ready()) {
        return;
    }
    const auto proj = camera_motion.PassProjection();
    if (proj[0] == 0.0f || proj[1] == 0.0f) {
        return;
    }
    done_this_frame = true;
    auto& color = texture_cache.GetImage(color_id);
    auto& depth = texture_cache.GetImage(depth_id);
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat) {
        failed = true;
        std::printf("SSGI: the scene color is not RGBA16F; indirect lighting is off\n");
        return;
    }

    const bool reduced = scene_targets.Reduced() && scene_targets.EligibleScene(color);
    u32 width = color.info.size.width, height = color.info.size.height;
    if (reduced) {
        width = scene_targets.Size().width;
        height = scene_targets.Size().height;
    } else if (const auto render = camera_motion.RenderSize(); render[0] && render[1]) {
        width = std::min(width, render[0]);
        height = std::min(height, render[1]);
    }

    vk::ImageView color_view{}, depth_view{}, albedo_view{};
    VideoCore::Image* albedo = nullptr;
    if (gbuffer_albedo && gbuffer_depth == depth_id) {
        albedo = &texture_cache.GetImage(gbuffer_albedo);
        if (albedo->info.pixel_format != vk::Format::eR8G8B8A8Srgb) {
            albedo = nullptr;
        }
    }
    if (reduced) {
        VideoCore::ImageViewInfo color_info, depth_info;
        color_info.format = color.info.pixel_format;
        depth_info.format = depth.info.pixel_format;
        const auto c = scene_targets.Read(color_id, color_info, Compute,
                                          vk::AccessFlagBits2::eShaderRead |
                                              vk::AccessFlagBits2::eShaderWrite);
        if (!(c.usage & vk::ImageUsageFlagBits::eStorage)) {
            failed = true;
            std::printf("SSGI: the reduced scene color is not a storage image; off\n");
            return;
        }
        color_view = c.view;
        depth_view = scene_targets.Read(depth_id, depth_info).view;
        if (albedo && scene_targets.EligibleScene(*albedo)) {
            VideoCore::ImageViewInfo albedo_info;
            albedo_info.format = albedo->info.pixel_format;
            albedo_view = scene_targets.Read(gbuffer_albedo, albedo_info).view;
        }
    } else {
        if (!(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
            failed = true;
            std::printf("SSGI: the scene color is not a storage image; off\n");
            return;
        }
        color_view = CachedView(color, color.info.pixel_format, vk::ImageAspectFlagBits::eColor);
        depth_view = CachedView(depth, depth.info.pixel_format, vk::ImageAspectFlagBits::eDepth);
        scheduler.EndRendering();
        runtime.Transit(&color, vk::ImageLayout::eGeneral, Compute,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        runtime.Transit(&depth, vk::ImageLayout::eGeneral, Compute,
                        vk::AccessFlagBits2::eShaderRead);
        if (albedo && albedo->info.size.width >= width && albedo->info.size.height >= height) {
            albedo_view = CachedView(*albedo, albedo->info.pixel_format,
                                     vk::ImageAspectFlagBits::eColor);
            runtime.Transit(albedo, vk::ImageLayout::eGeneral, Compute,
                            vk::AccessFlagBits2::eShaderRead);
        }
        runtime.FlushBarriers();
    }
    scheduler.EndRendering();
    vk::ImageView ao_view{};
    constexpr float ao_amount = 1.0f;
    if (ao_image) {
        auto& ao = texture_cache.GetImage(ao_image);
        if (reduced && scene_targets.EligibleScene(ao)) {
            VideoCore::ImageViewInfo ao_info;
            ao_info.format = ao.info.pixel_format;
            ao_view = scene_targets.Read(ao_image, ao_info).view;
        } else {
            ao_view = CachedView(ao, ao.info.pixel_format, vk::ImageAspectFlagBits::eColor);
            runtime.Transit(&ao, vk::ImageLayout::eGeneral, Compute,
                            vk::AccessFlagBits2::eShaderRead);
            runtime.FlushBarriers();
        }
    }
    const bool has_ao = ao_view != vk::ImageView{};
    const bool has_albedo = albedo_view != vk::ImageView{};

    vk::ImageView normal_view{};
    const bool use_probes =
        BbSettings::Get().effects[BbSettings::EffectGi].load(std::memory_order_relaxed) &&
        BbSettings::Get().graphics_gi_probes.load(std::memory_order_relaxed);
    const float normal_amount = 0.6f;
    if (gbuffer_normal && gbuffer_depth == depth_id) {
        auto& normal = texture_cache.GetImage(gbuffer_normal);
        if (normal.info.pixel_format == vk::Format::eR8G8B8A8Unorm) {
            if (reduced) {
                if (scene_targets.EligibleScene(normal)) {
                    VideoCore::ImageViewInfo normal_info;
                    normal_info.format = normal.info.pixel_format;
                    normal_view = scene_targets.Read(gbuffer_normal, normal_info).view;
                }
            } else if (normal.info.size.width >= width && normal.info.size.height >= height) {
                normal_view = CachedView(normal, normal.info.pixel_format,
                                         vk::ImageAspectFlagBits::eColor);
                runtime.Transit(&normal, vk::ImageLayout::eGeneral, Compute,
                                vk::AccessFlagBits2::eShaderRead);
                runtime.FlushBarriers();
            }
        }
    }
    const bool has_normal = normal_view != vk::ImageView{};
    const u32 half_width = (width + 1) / 2, half_height = (height + 1) / 2;
    if ((!geo.image || geo.width != width || geo.height != height) &&
        !EffectPass::HasRoom(instance, 16ull * width * height + 4ull * 8 * half_width * half_height)) {
        failed = true;
        std::printf("SSGI: off - not enough video memory\n");
        return;
    }
    bool created = EnsureImage(geo, width, height, vk::Format::eR32G32B32A32Sfloat, "geometry");
    if (use_probes) {
        const auto material_view = [&](VideoCore::ImageId id) -> vk::ImageView {
            if (!id || gbuffer_depth != depth_id) {
                return {};
            }
            auto& image = texture_cache.GetImage(id);
            if (image.info.pixel_format != vk::Format::eR8G8B8A8Unorm) {
                return {};
            }
            if (reduced) {
                if (!scene_targets.EligibleScene(image)) {
                    return {};
                }
                VideoCore::ImageViewInfo info;
                info.format = image.info.pixel_format;
                return scene_targets.Read(id, info).view;
            }
            if (image.info.size.width < width || image.info.size.height < height) {
                return {};
            }
            const auto view =
                CachedView(image, image.info.pixel_format, vk::ImageAspectFlagBits::eColor);
            runtime.Transit(&image, vk::ImageLayout::eGeneral, Compute,
                            vk::AccessFlagBits2::eShaderRead);
            runtime.FlushBarriers();
            return view;
        };
        const vk::ImageView roughness_view = material_view(gbuffer_material);
        const vk::ImageView specular_view = material_view(gbuffer_specular);
        const bool has_material = roughness_view && specular_view;

        constexpr u32 Tile = 16, Oct = 8;
        const u32 tiles_x = (width + Tile - 1) / Tile, tiles_y = (height + Tile - 1) / Tile;
        if ((!probe_geo[0].image || probe_geo[0].width != tiles_x ||
             probe_geo[0].height != tiles_y || !probe_diffuse[0].image ||
             probe_diffuse[0].width != width || probe_diffuse[0].height != height) &&
            !EffectPass::HasRoom(instance, 2ull * tiles_x * tiles_y * (16 + 8 * Oct * Oct + 24) +
                                               2ull * width * height * 16)) {
            failed = true;
            std::printf("SSGI: off - not enough video memory for probes\n");
            return;
        }
        bool probes_created = false;
        for (u32 i = 0; i < 2; ++i) {
            probes_created |= EnsureImage(probe_geo[i], tiles_x, tiles_y,
                                          vk::Format::eR32G32B32A32Sfloat, "probes");
            probes_created |= EnsureImage(probe_light[i], tiles_x * Oct, tiles_y * Oct,
                                          vk::Format::eR16G16B16A16Sfloat, "probe radiance");
            for (auto& image : probe_sh[i]) {
                probes_created |= EnsureImage(image, tiles_x, tiles_y,
                                              vk::Format::eR16G16B16A16Sfloat, "probe SH");
            }
            probes_created |= EnsureImage(probe_diffuse[i], width, height,
                                          vk::Format::eR16G16B16A16Sfloat, "probe diffuse");
            probes_created |= EnsureImage(probe_specular[i], width, height,
                                          vk::Format::eR16G16B16A16Sfloat, "probe specular");
        }
        if (probes_created || created) {
            probe_history_valid = false;
        }
        static bool printed_probes = false;
        if (!printed_probes) {
            printed_probes = true;
            std::printf("SSGI probes: %ux%u tiles over %ux%u, albedo %s, materials %s\n", tiles_x,
                        tiles_y, width, height, has_albedo ? "G-buffer" : "approximated",
                        has_material ? "G-buffer" : "not found (no specular)");
        }

        const float gi = std::clamp(
            BbSettings::Get().graphics_gi_strength.load(std::memory_order_relaxed), 0.0f, 12.0f);
        const auto camera = camera_motion.PassMatrices();
        const bool use_previous = prev_camera_valid && camera.valid;
        constexpr std::array<float, 12> Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        // Indirect lighting strength 3 gives a physically scaled bounce x4.5, close to the
        // brightness of the older screen-space GI at the same setting.
        Params params{
            .proj = proj,
            .reproject = use_previous ? Multiply(prev_view, camera.inv_view) : Identity,
            .size = {float(width), float(height)},
            .jitter = jitter,
            .prev_jitter = use_previous ? prev_jitter : jitter,
            .prev_proj = use_previous ? prev_proj : std::array<float, 2>{proj[0], proj[1]},
            .frame = frame,
            .history = probe_history_valid && use_previous ? 1u : 0u,
            .strength = gi * 1.5f,
            .radius = gi * (1.0f / 6.0f),
            .feedback = 0.8f,
            .ao_amount = has_ao ? ao_amount : 0.0f,
            .albedo_on = has_albedo ? 1.0f : 0.0f,
            .step_size = has_material ? 1.0f : 0.0f,
        };
        const u32 current = frame & 1;
        const auto images = [](std::array<OwnImage, 3>& sh) {
            return std::array{vk::Image(sh[0].image), vk::Image(sh[1].image),
                              vk::Image(sh[2].image)};
        };
        const auto views = [](std::array<OwnImage, 3>& sh) {
            return std::array{*sh[0].view, *sh[1].view, *sh[2].view};
        };
        scheduler.EndRendering();
        EffectPass::Mark(1, "indirect light");
        scheduler.RecordCrumb(
            {.name = "SSGI probes"},
            [params, init_previous = !probe_history_valid, width, height, tiles_x, tiles_y,
             color_view, depth_view, albedo_view = has_albedo ? albedo_view : *geo.view,
             ao_view = has_ao ? ao_view : *geo.view,
             normal_view = has_normal ? normal_view : *geo.view,
             roughness_view = has_material ? roughness_view : *geo.view,
             specular_view = has_material ? specular_view : *geo.view,
             normal_rows = has_normal && camera.valid ? camera.view : Identity,
             normal_amount = has_normal && camera.valid ? normal_amount : 0.0f,
             geo_image = vk::Image(geo.image), geo_view = *geo.view,
             probe_image = vk::Image(probe_geo[current].image),
             probe_view = *probe_geo[current].view,
             old_probe_image = vk::Image(probe_geo[current ^ 1].image),
             old_probe_view = *probe_geo[current ^ 1].view,
             light_image = vk::Image(probe_light[current].image),
             light_view = *probe_light[current].view,
             old_light_image = vk::Image(probe_light[current ^ 1].image),
             old_light_view = *probe_light[current ^ 1].view,
             sh_image = images(probe_sh[current]), sh_view = views(probe_sh[current]),
             old_sh_image = images(probe_sh[current ^ 1]),
             old_sh_view = views(probe_sh[current ^ 1]),
             diffuse_image = vk::Image(probe_diffuse[current].image),
             diffuse_view = *probe_diffuse[current].view,
             old_diffuse_image = vk::Image(probe_diffuse[current ^ 1].image),
             old_diffuse_view = *probe_diffuse[current ^ 1].view,
             specular_out_image = vk::Image(probe_specular[current].image),
             specular_out_view = *probe_specular[current].view,
             old_specular_image = vk::Image(probe_specular[current ^ 1].image),
             old_specular_view = *probe_specular[current ^ 1].view, prep_pipeline = *prep.pipeline,
             prep_layout = *prep.layout, trace_pipeline = *probe_trace.pipeline,
             trace_layout = *probe_trace.layout, composite_pipeline = *probe_composite.pipeline,
             composite_layout = *probe_composite.layout](vk::CommandBuffer cmdbuf) mutable {
                BeforeCompute(cmdbuf);
                std::array<vk::ImageMemoryBarrier2, 16> barriers{};
                u32 count = 0;
                for (const vk::Image image : {geo_image, probe_image, light_image, sh_image[0],
                                              sh_image[1], sh_image[2], diffuse_image,
                                              specular_out_image}) {
                    barriers[count++] = ToGeneral(image);
                }
                if (init_previous) {
                    for (const vk::Image image : {old_probe_image, old_light_image,
                                                  old_sh_image[0], old_sh_image[1],
                                                  old_sh_image[2], old_diffuse_image,
                                                  old_specular_image}) {
                        barriers[count++] = ToGeneral(image);
                    }
                }
                cmdbuf.pipelineBarrier2(
                    {.imageMemoryBarrierCount = count, .pImageMemoryBarriers = barriers.data()});

                const auto run = [&](vk::Pipeline pipeline, vk::PipelineLayout layout,
                                     std::span<const vk::WriteDescriptorSet> writes, u32 groups_x,
                                     u32 groups_y) {
                    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
                    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0,
                                                writes);
                    cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                                         sizeof(Params), &params);
                    cmdbuf.dispatch(groups_x, groups_y, 1);
                };
                const u32 full_x = (width + 7) / 8, full_y = (height + 7) / 8;
                {
                    const std::array infos{Info(depth_view), Info(geo_view), Info(normal_view)};
                    const std::array writes{Write(0, SampledImage, &infos[0]),
                                            Write(1, StorageImage, &infos[1]),
                                            Write(2, SampledImage, &infos[2])};
                    Params prep_params = params;
                    prep_params.reproject = normal_rows;
                    prep_params.step_size = normal_amount;
                    std::swap(params, prep_params);
                    run(prep_pipeline, prep_layout, writes, full_x, full_y);
                    std::swap(params, prep_params);
                }
                BetweenPasses(cmdbuf);
                {
                    const std::array infos{
                        Info(depth_view),     Info(geo_view),       Info(color_view),
                        Info(albedo_view),    Info(old_probe_view), Info(old_light_view),
                        Info(old_sh_view[0]), Info(old_sh_view[1]), Info(old_sh_view[2]),
                        Info(probe_view),     Info(light_view),     Info(sh_view[0]),
                        Info(sh_view[1]),     Info(sh_view[2])};
                    std::array<vk::WriteDescriptorSet, 14> writes;
                    for (u32 i = 0; i < writes.size(); ++i) {
                        writes[i] = Write(i, i < 9 ? SampledImage : StorageImage, &infos[i]);
                    }
                    run(trace_pipeline, trace_layout, writes, tiles_x, tiles_y);
                }
                BetweenPasses(cmdbuf);
                {
                    const std::array infos{
                        Info(geo_view),   Info(probe_view),  Info(light_view),
                        Info(sh_view[0]), Info(sh_view[1]),  Info(sh_view[2]),
                        Info(color_view), Info(albedo_view), Info(ao_view),
                        Info(roughness_view), Info(specular_view), Info(old_diffuse_view),
                        Info(old_specular_view), Info(diffuse_view), Info(specular_out_view)};
                    std::array<vk::WriteDescriptorSet, 15> writes;
                    for (u32 i = 0; i < writes.size(); ++i) {
                        writes[i] = Write(i, i == 6 || i >= 13 ? StorageImage : SampledImage,
                                          &infos[i]);
                    }
                    run(composite_pipeline, composite_layout, writes, full_x, full_y);
                }
                AfterCompute(cmdbuf);
            });
        probe_history_valid = true;
        history_valid = false;
        prev_view = camera.view;
        prev_proj = {proj[0], proj[1]};
        prev_jitter = jitter;
        prev_camera_valid = camera.valid;
        ++frame;
        return;
    }
    probe_history_valid = false;
    for (auto& image : light) {
        created |= EnsureImage(image, half_width, half_height, vk::Format::eR16G16B16A16Sfloat,
                               "light");
    }
    for (auto& image : filtered) {
        EnsureImage(image, half_width, half_height, vk::Format::eR16G16B16A16Sfloat, "filtered");
    }
    if (created) {
        history_valid = false;
    }

    constexpr float strength = 8.0f;
    constexpr float radius = 3.0f;
    constexpr float feedback = 0.8f;
    static bool printed = false;
    if (!printed) {
        printed = true;
        std::printf("SSGI: %ux%u (%s), albedo %s, AO %s, normals %s\n", width, height,
                    reduced ? "reduced scene" : "native scene",
                    has_albedo ? "G-buffer" : "approximated", has_ao ? "bound" : "not found",
                    has_normal ? "G-buffer" : "from depth");
    }

    const auto camera = camera_motion.PassMatrices();
    const bool use_previous = prev_camera_valid && camera.valid;
    constexpr std::array<float, 12> Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Params params{
        .proj = proj,
        .reproject = use_previous ? Multiply(prev_view, camera.inv_view) : Identity,
        .size = {float(width), float(height)},
        .jitter = jitter,
        .prev_jitter = use_previous ? prev_jitter : jitter,
        .prev_proj = use_previous ? prev_proj : std::array<float, 2>{proj[0], proj[1]},
        .frame = frame,
        .history = history_valid && use_previous ? 1u : 0u,
        .strength = strength * std::clamp(BbSettings::Get().graphics_gi_strength.load(
                                              std::memory_order_relaxed),
                                          0.0f, 12.0f),
        .radius = radius,
        .feedback = feedback,
        .ao_amount = has_ao ? ao_amount : 0.0f,
        .albedo_on = has_albedo ? 1.0f : 0.0f,
        .step_size = 1.0f,
    };
    const u32 current = frame & 1;
    scheduler.EndRendering();
    const bool use_rays = gather_ray.pipeline && rt_shadow.PrepareScene();
    if (!use_rays &&
        !BbSettings::Get().effects[BbSettings::EffectGi].load(std::memory_order_relaxed)) {
        return;
    }
    EffectPass::Mark(1, "indirect light");
    scheduler.RecordCrumb(
        {.name = "SSGI"},
        [params, init_previous = !history_valid, width, height, half_width,
         half_height, color_view, depth_view, albedo_view = has_albedo ? albedo_view : *geo.view,
         ao_view = has_ao ? ao_view : *geo.view,
         normal_view = has_normal ? normal_view : *geo.view,
         normal_rows = has_normal && camera.valid ? camera.view : Identity,
         normal_amount = has_normal && camera.valid ? normal_amount : 0.0f,
         geo_image = vk::Image(geo.image), geo_view = *geo.view,
         cur_image = vk::Image(light[current].image), cur_view = *light[current].view,
         old_image = vk::Image(light[current ^ 1].image), old_view = *light[current ^ 1].view,
         filtered_image = std::array{vk::Image(filtered[0].image), vk::Image(filtered[1].image)},
         filtered_view = std::array{*filtered[0].view, *filtered[1].view},
         prep_pipeline = *prep.pipeline, prep_layout = *prep.layout,
         gather_pipeline = use_rays ? *gather_ray.pipeline : *gather.pipeline,
         gather_layout = use_rays ? *gather_ray.layout : *gather.layout, use_rays,
         scene = rt_shadow.Scene(),
         denoise_pipeline = *denoise.pipeline, denoise_layout = *denoise.layout,
         composite_pipeline = *composite.pipeline,
         composite_layout = *composite.layout](vk::CommandBuffer cmdbuf) mutable {
            BeforeCompute(cmdbuf);
            std::array<vk::ImageMemoryBarrier2, 5> barriers{
                ToGeneral(geo_image), ToGeneral(cur_image), ToGeneral(filtered_image[0]),
                ToGeneral(filtered_image[1])};
            u32 barrier_count = 4;
            if (init_previous) {
                barriers[barrier_count++] = ToGeneral(old_image);
            }
            cmdbuf.pipelineBarrier2(
                {.imageMemoryBarrierCount = barrier_count, .pImageMemoryBarriers = barriers.data()});

            const auto run = [&](vk::Pipeline pipeline, vk::PipelineLayout layout,
                                 std::span<const vk::WriteDescriptorSet> writes, u32 groups_x,
                                 u32 groups_y) {
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
                cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(Params),
                                     &params);
                cmdbuf.dispatch(groups_x, groups_y, 1);
            };
            const u32 full_x = (width + 7) / 8, full_y = (height + 7) / 8;
            const u32 half_x = (half_width + 7) / 8, half_y = (half_height + 7) / 8;
            {
                const std::array infos{Info(depth_view), Info(geo_view), Info(normal_view)};
                const std::array writes{Write(0, SampledImage, &infos[0]),
                                        Write(1, StorageImage, &infos[1]),
                                        Write(2, SampledImage, &infos[2])};

                const auto reproject = params.reproject;
                std::memcpy(params.reproject.data(), normal_rows.data(), sizeof(normal_rows));
                params.step_size = normal_amount;
                run(prep_pipeline, prep_layout, writes, full_x, full_y);
                params.reproject = reproject;
                params.step_size = 1.0f;
            }
            BetweenPasses(cmdbuf);
            {
                const std::array infos{Info(geo_view), Info(color_view), Info(old_view),
                                       Info(cur_view), Info(albedo_view)};
                const std::array writes{Write(0, SampledImage, &infos[0]),
                                        Write(1, SampledImage, &infos[1]),
                                        Write(2, SampledImage, &infos[2]),
                                        Write(3, StorageImage, &infos[3]),
                                        Write(4, SampledImage, &infos[4])};
                if (use_rays) {
                    const vk::WriteDescriptorSetAccelerationStructureKHR scene_info{
                        .accelerationStructureCount = 1, .pAccelerationStructures = &scene};
                    const std::array ray_writes{
                        writes[0], writes[1], writes[2], writes[3], writes[4],
                        vk::WriteDescriptorSet{
                            .pNext = &scene_info,
                            .dstBinding = 5,
                            .descriptorCount = 1,
                            .descriptorType = vk::DescriptorType::eAccelerationStructureKHR}};
                    run(gather_pipeline, gather_layout, ray_writes, half_x, half_y);
                } else {
                    run(gather_pipeline, gather_layout, writes, half_x, half_y);
                }
            }
            BetweenPasses(cmdbuf);
            vk::ImageView result_view = cur_view;
            {
                const u32 denoise_passes = use_rays ? 3u : 2u;
                for (u32 i = 0; i < denoise_passes; ++i) {
                    params.step_size = float(1u << i);
                    const std::array infos{Info(geo_view),
                                           Info(i == 0 ? cur_view : filtered_view[(i - 1) & 1]),
                                           Info(filtered_view[i & 1])};
                    const std::array writes{Write(0, SampledImage, &infos[0]),
                                            Write(1, SampledImage, &infos[1]),
                                            Write(2, StorageImage, &infos[2])};
                    run(denoise_pipeline, denoise_layout, writes, half_x, half_y);
                    BetweenPasses(cmdbuf);
                }
                result_view = filtered_view[(denoise_passes - 1) & 1];
            }
            {
                const std::array infos{Info(geo_view), Info(result_view), Info(color_view),
                                       Info(albedo_view), Info(ao_view)};
                const std::array writes{Write(0, SampledImage, &infos[0]),
                                        Write(1, SampledImage, &infos[1]),
                                        Write(2, StorageImage, &infos[2]),
                                        Write(3, SampledImage, &infos[3]),
                                        Write(4, SampledImage, &infos[4])};
                run(composite_pipeline, composite_layout, writes, full_x, full_y);
            }
            AfterCompute(cmdbuf);
        });
    history_valid = true;
    prev_view = camera.view;
    prev_proj = {proj[0], proj[1]};
    prev_jitter = jitter;
    prev_camera_valid = camera.valid;
    ++frame;
}

}
