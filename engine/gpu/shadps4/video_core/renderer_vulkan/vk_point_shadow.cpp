// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_point_shadow.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "bbport_settings.h"
#include "common/assert.h"
#include "video_core/host_shaders/point_shadow_comp.h"
#include "video_core/host_shaders/point_shadow_ray_comp.h"
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
struct Push {
    u32 first_light;
    u32 frame_index;
    std::array<float, 2> size;
    float length_m;
    float thickness_m;
    float strength;
    float fade_start_m;
    float fade_end_m;
    u32 light_count;
    u32 steps;
    float bias_m;
    std::array<s32, 4> rect;
};
static_assert(sizeof(Push) == 64);

constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr auto TargetFormat = vk::Format::eB10G11R11UfloatPack32;
constexpr u32 ConstantWords = 16, LightWords = 16;
constexpr u32 DataWords = 64 * 1024;
constexpr u32 UpdateWords = 16 * 1024;
constexpr u32 WholeImageBatches = 4;
constexpr u32 TileWords = 8;
constexpr auto HistoryFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr u32 HistoryRead = 1, HistoryWrite = 2;

std::array<float, 16> Invert(const u32* words, bool& ok) {
    std::array<double, 32> t{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            t[r * 8 + c] = std::bit_cast<float>(words[r * 4 + c]);
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
    ok = std::ranges::all_of(out, [](float v) { return std::isfinite(v); });
    return out;
}

std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = sum;
        }
    }
    return out;
}

bool Overlaps(const std::array<s32, 4>& a, const std::array<s32, 4>& b) {
    return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}
}

PointShadow::PointShadow(const Instance& instance_, Scheduler& scheduler_,
                         VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                         SceneTargets& scene_targets_, CameraMotion& camera_motion_,
                         RtShadow& rt_shadow_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, scene_targets{scene_targets_}, rt_shadow{rt_shadow_},
      camera_motion{camera_motion_} {
    fade_start_m = fade_end_m * 0.6f;
    if (const char* env = std::getenv("LANTERN_RT_LAMPS_EXACT"); env && env[0] == '1') {
        lamp_pick = 0;
    }
    if (const char* env = std::getenv("LANTERN_RT_LAMP_REUSE"); env && env[0] == '0') {
        lamp_reuse = false;
    }
}

bool PointShadow::Ready() {
    if (tried) {
        return !failed;
    }
    tried = true;
    using enum vk::DescriptorType;
    const bool made =
        instance.GetPhysicalDevice().getFeatures().shaderStorageImageExtendedFormats &&
        EffectPass::StorageFormat(instance, TargetFormat) &&
        EffectPass::CreateSampler(instance, sampler, vk::Filter::eNearest) &&
        EffectPass::Create(instance, set_layout, layout, pipeline,
                           {eStorageBuffer, eCombinedImageSampler, eStorageImage, eStorageImage,
                            eStorageImage, eStorageImage},
                           sizeof(Push), POINT_SHADOW_COMP, "Point shadows", true);
    if (!made) {
        failed = true;
        std::printf("Point shadows: off - this GPU cannot run the pass\n");
        return false;
    }
    if (rt_shadow.Capturing() &&
        !EffectPass::Create(instance, ray_set_layout, ray_layout, ray_pipeline,
                            {eStorageBuffer, eCombinedImageSampler, eStorageImage, eStorageImage,
                             eStorageImage, eStorageImage, eAccelerationStructureKHR, eStorageImage,
                             eStorageImage},
                            sizeof(Push), POINT_SHADOW_RAY_COMP, "Ray traced lamps", true)) {
        std::printf("Ray traced lamps: off - the pass could not be made\n");
    }
    light_data = std::make_unique<VideoCore::Buffer>(
        instance, 0, DataWords * sizeof(u32), VideoCore::MemoryType::DeviceLocal, "Point shadow lights");
    return true;
}

PointShadow::~PointShadow() = default;

bool PointShadow::EnsureImage(OwnImage& own, u32 width, u32 height, vk::Format format,
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
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, vk::Image(own.image), name);
    own.view = Check<"create point shadow snapshot view">(device.createImageViewUnique({
        .image = vk::Image(own.image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = ColorRange,
    }));
    own.width = width;
    own.height = height;
    return true;
}

void PointShadow::OnFrameEnd() {
    ++frame_index;
    if (history_written) {
        history_index ^= 1u;
        last_view = pending_view;
        last_proj = pending_proj;
        last_valid = pending_valid;
        history_fresh = true;
    } else {
        history_fresh = false;
    }
    history_written = false;
    if (batches != 0 && (!reported || frame_index % 1800 == 0)) {
        reported = true;
        std::printf("Point shadows: %u light tiles in %u batches this frame; %u draws between tiles "
                    "kept the batch, %u ended it (%08llx %08llx %08llx %08llx)\n",
                    frame_tiles, batches, kept_draws, ended_by_draws,
                    (unsigned long long)ending_hashes[0], (unsigned long long)ending_hashes[1],
                    (unsigned long long)ending_hashes[2], (unsigned long long)ending_hashes[3]);
    }
    batches = 0;
    frame_tiles = 0;
    kept_draws = 0;
    ended_by_draws = 0;
    ending_hashes = {};
}

std::array<VAddr, 2> PointShadow::PendingAddresses() const {
    std::array<VAddr, 2> out{};
    for (int i = 0; i < 2; ++i) {
        if (pending[i]) {
            if (const auto* image = texture_cache.TryGetImage(pending[i], pending_uid[i])) {
                out[i] = image->info.guest_address;
            }
        }
    }
    return out;
}

void PointShadow::Ended(u64 hash) {
    if (tiles.empty()) {
        return;
    }
    ++ended_by_draws;
    for (u64& slot : ending_hashes) {
        if (slot == hash) {
            return;
        }
        if (slot == 0) {
            slot = hash;
            return;
        }
    }
}

void PointShadow::Drop() {
    tiles.clear();
    pending[0] = pending[1] = {};
    merged = false;
    history_fresh = false;
}

void PointShadow::OnLightDraw(VideoCore::ImageId color0, VideoCore::ImageId color1,
                              const Inputs& inputs) {
    if (failed || !color0 || !color1 || !inputs.constants || !inputs.lights || inputs.count == 0 ||
        (!BbSettings::Get().effects[BbSettings::EffectPointShadows].load(
             std::memory_order_relaxed) &&
         !rt_shadow.Wanted()) ||
        !Ready()) {
        return;
    }
    const auto depth_id = camera_motion.Depth();
    if (!depth_id) {
        return;
    }
    const bool reduced = scene_targets.Reduced() &&
                         scene_targets.Eligible(texture_cache.GetImage(depth_id));
    const std::array ids{color0, color1};
    std::array<VideoCore::Image*, 2> targets{};
    for (int i = 0; i < 2; ++i) {
        targets[i] = &texture_cache.GetImage(ids[i]);
        const auto& info = targets[i]->info;
        if (info.pixel_format != TargetFormat ||
            (!reduced && !(targets[i]->usage_flags & vk::ImageUsageFlagBits::eStorage)) ||
            (!reduced && !(targets[i]->usage_flags & vk::ImageUsageFlagBits::eTransferSrc)) ||
            (reduced && !scene_targets.Eligible(*targets[i])) ||
            info.size.width != targets[0]->info.size.width ||
            info.size.height != targets[0]->info.size.height) {
            failed = true;
            std::printf("Point shadows: the light targets are not matching B10G11R11 storage "
                        "images; off\n");
            return;
        }
    }
    u32 width = targets[0]->info.size.width, height = targets[0]->info.size.height;
    if (reduced) {
        const auto size = scene_targets.ProxySize(*targets[0]);
        width = size.width;
        height = size.height;
    }
    const s32 x0 = std::clamp(inputs.tile.offset.x, 0, s32(width));
    const s32 y0 = std::clamp(inputs.tile.offset.y, 0, s32(height));
    const s32 x1 = std::clamp(inputs.tile.offset.x + s32(inputs.tile.extent.width), x0, s32(width));
    const s32 y1 = std::clamp(inputs.tile.offset.y + s32(inputs.tile.extent.height), y0, s32(height));
    if (x0 == x1 || y0 == y1) {
        return;
    }
    const std::array<s32, 4> rect{x0, y0, x1, y1};
    const u32 words = inputs.count * LightWords;
    const auto add_lights = [&] {
        data.insert(data.end(), inputs.lights, inputs.lights + words);
        ++frame_tiles;
    };
    if (!tiles.empty()) {
        const bool same_targets = pending[0] == color0 && pending[1] == color1 &&
                                  pending_uid[0] == targets[0]->image_uid &&
                                  pending_uid[1] == targets[1]->image_uid;
        const bool room = data.size() + words + TileWords * (tiles.size() + 1) <= DataWords;
        if (same_targets && (merged || !one_tile_batches) && room) {
            if (tiles.back().rect == rect) {
                tiles.back().count += inputs.count;
                add_lights();
                return;
            }
            if (merged ||
                std::ranges::none_of(tiles, [&](const Tile& t) { return Overlaps(t.rect, rect); })) {
                tiles.push_back({rect, u32(data.size()), inputs.count});
                add_lights();
                return;
            }
        }
        Flush(false);
        if (failed) {
            return;
        }
    }
    static bool printed = false;
    if (!printed) {
        printed = true;
        std::printf("Point shadows: %ux%u, length %.2f m, thickness %.2f m, strength %.2f, "
                    "%u steps, first tile %dx%d\n",
                    width, height, length_m, thickness_m, strength, steps, x1 - x0, y1 - y0);
    }

    if ((!snapshot[0].image || snapshot[0].width != width || snapshot[0].height != height) &&
        !EffectPass::HasRoom(instance, 2ull * 4 * width * height)) {
        failed = true;
        std::printf("Point shadows: off - not enough video memory\n");
        return;
    }
    scheduler.EndRendering();
    for (auto& own : snapshot) {
        EnsureImage(own, width, height, TargetFormat, "Point shadow snapshot");
    }
    VideoCore::ImageViewInfo target_info;
    target_info.format = TargetFormat;
    std::array<vk::Image, 2> src{};
    vk::ImageLayout src_layout = vk::ImageLayout::eTransferSrcOptimal;
    if (reduced) {
        src_layout = vk::ImageLayout::eGeneral;
        for (int i = 0; i < 2; ++i) {
            const auto proxy = scene_targets.Read(ids[i], target_info, vk::PipelineStageFlagBits2::eCopy,
                                                  vk::AccessFlagBits2::eTransferRead);
            if (!(proxy.usage & vk::ImageUsageFlagBits::eStorage) ||
                !(proxy.usage & vk::ImageUsageFlagBits::eTransferSrc)) {
                failed = true;
                scene_targets.Attachment(ids[0], target_info);
                scene_targets.Attachment(ids[1], target_info);
                std::printf("Point shadows: the reduced light targets cannot be used; off\n");
                return;
            }
            src[i] = proxy.image;
        }
    } else {
        for (auto* target : targets) {
            runtime.Transit(target, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
        }
        runtime.FlushBarriers();
        src = {targets[0]->GetImage(), targets[1]->GetImage()};
    }
    merged = ray_pipeline && rt_shadow.Wanted();
    one_tile_batches = ++batches > WholeImageBatches;
    const std::array<s32, 4> copied = one_tile_batches && !merged
                                          ? rect
                                          : std::array<s32, 4>{0, 0, s32(width), s32(height)};
    const std::array<vk::Image, 2> dst{vk::Image(snapshot[0].image),
                                       vk::Image(snapshot[1].image)};
    EffectPass::Mark(5, "point shadow snapshots");
    scheduler.RecordCrumb({.name = "Point shadows snapshot"},
                          [src, dst, copied, src_layout](vk::CommandBuffer cmdbuf) {
        std::array<vk::ImageMemoryBarrier2, 2> to_dst;
        for (int i = 0; i < 2; ++i) {
            to_dst[i] = {
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                 vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst[i],
                .subresourceRange = ColorRange,
            };
        }
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = to_dst.data()});
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffset = {copied[0], copied[1], 0},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffset = {copied[0], copied[1], 0},
            .extent = {u32(copied[2] - copied[0]), u32(copied[3] - copied[1]), 1},
        };
        for (int i = 0; i < 2; ++i) {
            cmdbuf.copyImage(src[i], src_layout, dst[i],
                             vk::ImageLayout::eTransferDstOptimal, region);
        }
        std::array<vk::ImageMemoryBarrier2, 2> to_general;
        for (int i = 0; i < 2; ++i) {
            to_general[i] = to_dst[i];
            to_general[i].srcStageMask = vk::PipelineStageFlagBits2::eCopy;
            to_general[i].srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            to_general[i].dstStageMask = Compute;
            to_general[i].dstAccessMask = vk::AccessFlagBits2::eShaderRead;
            to_general[i].oldLayout = vk::ImageLayout::eTransferDstOptimal;
            to_general[i].newLayout = vk::ImageLayout::eGeneral;
        }
        cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = to_general.data()});
    });
    if (reduced) {
        scene_targets.Attachment(ids[0], target_info);
        scene_targets.Attachment(ids[1], target_info);
    } else {
        for (auto* target : targets) {
            runtime.Transit(target, vk::ImageLayout::eColorAttachmentOptimal,
                            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                            vk::AccessFlagBits2::eColorAttachmentWrite |
                                vk::AccessFlagBits2::eColorAttachmentRead);
        }
        runtime.FlushBarriers();
    }

    pending[0] = color0;
    pending[1] = color1;
    pending_uid = {targets[0]->image_uid, targets[1]->image_uid};
    data.assign(inputs.constants, inputs.constants + ConstantWords);
    tiles.push_back({rect, u32(data.size()), inputs.count});
    add_lights();
}

void PointShadow::Flush(bool animate_noise) {
    if (tiles.empty()) {
        return;
    }
    const auto ids = pending;
    const auto uids = pending_uid;
    auto batch = std::move(tiles);
    tiles.clear();
    pending[0] = pending[1] = {};
    const auto depth_id = camera_motion.Depth();
    if (!depth_id) {
        return;
    }
    auto* depth_ptr = texture_cache.TryGetImage(depth_id, 0);
    auto* target0 = texture_cache.TryGetImage(ids[0], uids[0]);
    auto* target1 = texture_cache.TryGetImage(ids[1], uids[1]);
    if (!depth_ptr || !target0 || !target1) {
        return;
    }
    auto& depth = *depth_ptr;
    std::array<VideoCore::Image*, 2> targets{target0, target1};
    const bool reduced = scene_targets.Reduced() && scene_targets.Eligible(depth) &&
                         scene_targets.Eligible(*targets[0]);
    u32 width = targets[0]->info.size.width, height = targets[0]->info.size.height;
    if (reduced) {
        const auto size = scene_targets.ProxySize(*targets[0]);
        width = size.width;
        height = size.height;
    }
    if (snapshot[0].width != width || snapshot[0].height != height) {
        return;
    }

    scheduler.EndRendering();
    const bool batch_merged = merged;
    merged = false;
    const bool use_rays = ray_pipeline && rt_shadow.PrepareScene();
    if (!use_rays &&
        (batch_merged ||
         !BbSettings::Get().effects[BbSettings::EffectPointShadows].load(std::memory_order_relaxed))) {
        return;
    }
    std::array<s32, 4> covered{s32(width), s32(height), 0, 0};
    u32 table = 0;
    if (use_rays) {
        table = u32(data.size());
        for (const Tile& tile : batch) {
            covered = {std::min(covered[0], tile.rect[0]), std::min(covered[1], tile.rect[1]),
                       std::max(covered[2], tile.rect[2]), std::max(covered[3], tile.rect[3])};
            data.insert(data.end(), {u32(tile.rect[0]), u32(tile.rect[1]), u32(tile.rect[2]),
                                     u32(tile.rect[3]), tile.first_light, tile.count, 0u, 0u});
        }
        if (data.size() > DataWords || covered[0] >= covered[2] || covered[1] >= covered[3]) {
            data.clear();
            return;
        }
        bool made = false;
        if (!history[0].image || history[0].width != width || history[0].height != height) {
            if (!EffectPass::HasRoom(instance, 2ull * 8 * width * height)) {
                data.clear();
                return;
            }
            for (int i = 0; i < 2; ++i) {
                EnsureImage(history[i], width, height, HistoryFormat, "Ray traced lamp history");
                history_laid_out[i] = false;
            }
            made = true;
        }
        if (made) {
            history_fresh = false;
        }
    }
    u32 history_at = 0;
    bool clear_history = false;
    std::array<bool, 2> lay_out{};
    if (use_rays && !history_written) {
        const auto camera = camera_motion.PassMatrices();
        bool ok = false;
        const auto proj = Invert(data.data(), ok);
        u32 flags = lamp_reuse ? HistoryWrite : 0u;
        if (lamp_reuse && history_fresh && last_valid && camera.valid && ok) {
            flags |= HistoryRead;
        }
        history_at = u32(data.size());
        data.push_back(flags);
        data.push_back(frame_index);
        constexpr std::array<float, 12> Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        const auto reproject = (flags & HistoryRead) ? Multiply(last_view, camera.inv_view) : Identity;
        for (const float v : reproject) {
            data.push_back(std::bit_cast<u32>(v));
        }
        for (const float v : last_proj) {
            data.push_back(std::bit_cast<u32>(v));
        }
        if (data.size() > DataWords) {
            data.clear();
            return;
        }
        pending_view = camera.view;
        pending_proj = proj;
        pending_valid = camera.valid && ok;
        history_written = lamp_reuse;
        clear_history = lamp_reuse;
    }
    if (use_rays) {
        lay_out = {!history_laid_out[0], !history_laid_out[1]};
        history_laid_out = {true, true};
    }
    const vk::ImageView history_prev = use_rays ? *history[history_index ^ 1u].view : vk::ImageView{};
    const vk::ImageView history_cur = use_rays ? *history[history_index].view : vk::ImageView{};
    const vk::Image history_cur_image = use_rays ? vk::Image(history[history_index].image) : vk::Image{};
    const std::array<vk::Image, 2> history_images{
        use_rays ? vk::Image(history[0].image) : vk::Image{},
        use_rays ? vk::Image(history[1].image) : vk::Image{}};
    const auto device = instance.GetDevice();
    std::array<vk::ImageView, 2> target_views;
    vk::ImageView depth_view{};
    if (reduced) {
        VideoCore::ImageViewInfo target_info, depth_info;
        target_info.format = TargetFormat;
        depth_info.format = depth.info.pixel_format;
        for (int i = 0; i < 2; ++i) {
            target_views[i] = scene_targets
                                  .Read(ids[i], target_info, Compute,
                                        vk::AccessFlagBits2::eShaderRead |
                                            vk::AccessFlagBits2::eShaderWrite)
                                  .view;
        }
        depth_view = scene_targets.Read(depth_id, depth_info).view;
    } else {
        for (int i = 0; i < 2; ++i) {
            target_views[i] = Check<"create point shadow target view">(device.createImageView({
                .image = targets[i]->GetImage(),
                .viewType = vk::ImageViewType::e2D,
                .format = TargetFormat,
                .subresourceRange = ColorRange,
            }));
        }
        depth_view = Check<"create point shadow depth view">(device.createImageView({
            .image = depth.GetImage(),
            .viewType = vk::ImageViewType::e2D,
            .format = depth.info.pixel_format,
            .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
        }));
        scheduler.DeferOperation([device, target_views, depth_view] {
            device.destroyImageView(target_views[0]);
            device.destroyImageView(target_views[1]);
            device.destroyImageView(depth_view);
        });
        for (auto* target : targets) {
            runtime.Transit(target, vk::ImageLayout::eGeneral, Compute,
                            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        }
        runtime.Transit(&depth, vk::ImageLayout::eGeneral, Compute,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }

    Push push{
        .first_light = 0,
        .frame_index = animate_noise ? frame_index : 0u,
        .size = {float(width), float(height)},
        .length_m = use_rays ? std::bit_cast<float>(history_at) : length_m,
        .thickness_m = thickness_m,
        .strength = use_rays ? 1.0f : strength,
        .fade_start_m = use_rays ? 60.0f : fade_start_m,
        .fade_end_m = use_rays ? 100.0f : fade_end_m,
        .light_count = 0,
        .steps = use_rays ? lamp_pick : steps,
        .bias_m = bias_m,
        .rect = {},
    };
    EffectPass::Mark(6, "point shadows");
    scheduler.RecordCrumb(
        {.name = "Point shadows"},
        [push, target_views, depth_view, sampler = *sampler, snap0 = *snapshot[0].view,
         snap1 = *snapshot[1].view, batch = std::move(batch), words = std::move(data),
         buffer = light_data->Handle(), pipeline = use_rays ? *ray_pipeline : *pipeline,
         layout = use_rays ? *ray_layout : *layout, use_rays, scene = rt_shadow.Scene(), covered,
         table, tile_count = u32(batch.size()), history_prev, history_cur, history_cur_image,
         history_images, lay_out, clear_history](vk::CommandBuffer cmdbuf) mutable {
            for (int i = 0; i < 2; ++i) {
                if (!lay_out[i]) {
                    continue;
                }
                const vk::ImageMemoryBarrier2 to_general{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .oldLayout = vk::ImageLayout::eUndefined,
                    .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = history_images[i],
                    .subresourceRange = ColorRange,
                };
                cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general});
            }
            const vk::MemoryBarrier2 to_transfer{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_transfer});
            if (clear_history) {
                const vk::MemoryBarrier2 history_free{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferWrite};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &history_free});
                const vk::ClearColorValue zero{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
                cmdbuf.clearColorImage(history_cur_image, vk::ImageLayout::eGeneral, zero, ColorRange);
            }
            for (u32 at = 0; at < words.size(); at += UpdateWords) {
                const u32 count = std::min<u32>(UpdateWords, u32(words.size()) - at);
                cmdbuf.updateBuffer(buffer, at * sizeof(u32), count * sizeof(u32), words.data() + at);
            }
            const vk::MemoryBarrier2 before{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = Compute,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                                 vk::AccessFlagBits2::eShaderWrite};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
            const vk::DescriptorBufferInfo lights{buffer, 0, words.size() * sizeof(u32)};
            const vk::DescriptorImageInfo depth_info{sampler, depth_view,
                                                     vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo cur0{{}, target_views[0], vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo old0{{}, snap0, vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo cur1{{}, target_views[1], vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo old1{{}, snap1, vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 6> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &lights},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                 .pImageInfo = &depth_info},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &cur0},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &old0},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &cur1},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &old1},
            }};
            const vk::WriteDescriptorSetAccelerationStructureKHR scene_info{
                .accelerationStructureCount = 1, .pAccelerationStructures = &scene};
            const vk::DescriptorImageInfo prev_info{{}, history_prev, vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo cur_info{{}, history_cur, vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 9> ray_writes{
                writes[0], writes[1], writes[2], writes[3], writes[4], writes[5],
                vk::WriteDescriptorSet{.pNext = &scene_info, .dstBinding = 6, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eAccelerationStructureKHR},
                vk::WriteDescriptorSet{.dstBinding = 7, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eStorageImage,
                                       .pImageInfo = &prev_info},
                vk::WriteDescriptorSet{.dstBinding = 8, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eStorageImage,
                                       .pImageInfo = &cur_info}};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            if (use_rays) {
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, ray_writes);
            } else {
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            }
            if (use_rays) {
                push.first_light = table;
                push.light_count = tile_count;
                push.rect = covered;
                cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push), &push);
                const u32 gx0 = u32(covered[0]) / 8, gy0 = u32(covered[1]) / 8;
                const u32 gx1 = (u32(covered[2]) + 7) / 8, gy1 = (u32(covered[3]) + 7) / 8;
                cmdbuf.dispatchBase(gx0, gy0, 0, gx1 - gx0, gy1 - gy0, 1);
                batch.clear();
            }
            for (const Tile& tile : batch) {
                push.first_light = tile.first_light;
                push.light_count = tile.count;
                push.rect = tile.rect;
                cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push),
                                     &push);
                const u32 gx0 = u32(tile.rect[0]) / 8, gy0 = u32(tile.rect[1]) / 8;
                const u32 gx1 = (u32(tile.rect[2]) + 7) / 8, gy1 = (u32(tile.rect[3]) + 7) / 8;
                cmdbuf.dispatchBase(gx0, gy0, 0, gx1 - gx0, gy1 - gy0, 1);
            }
            const vk::MemoryBarrier2 after{
                .srcStageMask = Compute,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                 vk::AccessFlagBits2::eMemoryWrite};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        });
}

}
