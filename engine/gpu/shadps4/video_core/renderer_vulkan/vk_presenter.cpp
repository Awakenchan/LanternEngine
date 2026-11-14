// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_timeline.h"
#include "face_shot.h"
#include "lantern_anti_lag.h"
#include "video_core/renderer_vulkan/vk_fsr_framegen.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_hdr_channel.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>
#include <vk_mem_alloc.h>

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// bbport: screenshot capture and ImGui overlays removed; the port presents directly.

// Native+: a half float of the RGBA16F frame, clamped to 0..1 as an 8-bit channel.
static std::uint8_t FaceHalfToByte(std::uint16_t h) {
    const u32 exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    float value = exponent == 0 ? std::ldexp(float(mantissa), -24)
                                : exponent == 31 ? 1.0f : std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
    if (h & 0x8000) value = 0.0f;
    return std::uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

static void GrabFace(const Instance& instance, Scheduler& scheduler, vk::Image image, u32 width, u32 height,
                     vk::Format format) {
    float rect[4];
    if (!BbFaceShot::Due(rect) || !width || !height) {
        return;
    }
    // Native+ presents an RGBA16F frame (8 bytes a texel): read as such, then made 8-bit RGBA.
    // Any other format than 8-bit RGBA/BGRA is skipped, so the copy never outgrows the buffer.
    const bool half = format == vk::Format::eR16G16B16A16Sfloat;
    if (!half && format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eR8G8B8A8Srgb &&
        format != vk::Format::eB8G8R8A8Unorm && format != vk::Format::eB8G8R8A8Srgb) {
        return;
    }
    const u32 texel = half ? 8 : 4;
    const u32 x = u32(rect[0] * float(width)), y = u32(rect[1] * float(height));
    const u32 w = std::min(width - x, u32((rect[2] - rect[0]) * float(width)));
    const u32 h = std::min(height - y, u32((rect[3] - rect[1]) * float(height)));
    if (!w || !h) {
        return;
    }
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(w) * h * texel,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation, &info) != VK_SUCCESS) {
        return;
    }
    scheduler.EndRendering();
    const vk::CommandBuffer cmdbuf = scheduler.CommandBuffer();
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    const vk::BufferImageCopy region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {s32(x), s32(y), 0},
        .imageExtent = {w, h, 1},
    };
    cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eGeneral, buffer, region);
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info, w, h, bgra, half] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        const auto* pixels = static_cast<const std::uint8_t*>(info.pMappedData);
        std::vector<std::uint8_t> bytes;
        if (half) {
            const auto* halves = static_cast<const std::uint16_t*>(info.pMappedData);
            bytes.resize(std::size_t(w) * h * 4);
            for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = FaceHalfToByte(halves[i]);
            pixels = bytes.data();
        }
        BbFaceShot::Deliver(pixels, w, h, w * 4, bgra);
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }
    // bbport: frame generation's frames (images made at their first use).
    for (u32 i = 0; i < generated_frames.size(); ++i) {
        Frame& frame = generated_frames[i];
        frame.id = u8(num_images + i);
        frame.present_done = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
    }
    frame_gen = std::make_unique<FrameGenerator>(instance, draw_scheduler);
    if (FsrFrameGen::SelectedAtStart() && !BbSettings::Get().fsr_fg_running.load() &&
        !BbSettings::Get().fsr_fg_problem.load()) {
        FsrFrameGen::Fallback("AMD's swapchain was not made");
    }
    rasterizer->GetUpscaler().SetFrameGenerator(frame_gen.get());
    // bbport: DLSS Frame Generation (vk_streamline.h): a slot and a HUD-less frame per
    // presentation frame (images made at their first use).
    dlss_fg = std::make_unique<DlssFrameGen>(instance, num_images);
    dlss_hudless_frames.resize(num_images);
    for (u32 i = 0; i < num_images; ++i) {
        dlss_hudless_frames[i].id = u8(num_images + NumGeneratedFrames + i);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, HdrChannel::Native16() ? HdrChannel::Native16Format : swapchain.GetSurfaceFormat().format);
    // bbport: + 2: frame generation presents up to twice per game frame, and adds a swapchain
    // image; the menu's vertex buffers rotate over this many.
    BbOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images + 2);
    // bbport: HDR output (hdr in lantern.ini): frames, passes and the menu stay SDR; the encode
    // pass writes the HDR swapchain format chosen at start.
    if (const vk::Format hdr_format = swapchain.GetHdrFormat().format;
        hdr_format != vk::Format::eUndefined) {
        hdr_pass.Create(device, hdr_format);
    }

}

void Presenter::ReportState() {
    const vk::Device device = instance.GetDevice();
    const std::array<std::pair<const char*, Scheduler*>, 3> schedulers{{
        {"draw", &draw_scheduler}, {"present", &present_scheduler}, {"flip", &flip_scheduler}}};
    for (const auto& [name, scheduler] : schedulers) {
        Semaphore* semaphore = scheduler->GetWorkSemaphore();
        const auto [result, value] = device.getSemaphoreCounterValue(semaphore->Handle());
        std::printf("Freeze: %s scheduler: next tick %llu, submitted %llu, GPU done %llu%s\n", name,
                    static_cast<unsigned long long>(semaphore->CurrentTick()),
                    static_cast<unsigned long long>(scheduler->SubmittedTick()),
                    static_cast<unsigned long long>(value),
                    result == vk::Result::eSuccess ? "" : " (query failed)");
    }
    std::unique_lock lock{free_mutex, std::try_to_lock};
    if (lock.owns_lock()) {
        std::printf("Freeze: %zu free presentation frames\n", free_queue.size());
    } else {
        std::printf("Freeze: presentation frames locked\n");
    }
}

namespace {

void PrintPresentHealth(const PresentPolicy::State& policy) {
    const PresentPolicy::PresentHealth& health = policy.health;
    const auto read = [](const std::atomic<u64>& counter) {
        return static_cast<unsigned long long>(counter.load(std::memory_order_relaxed));
    };
    std::printf("Display (%s=%d): acquire timeouts %llu, frame wait timeouts %llu (redraw %llu), "
                "suboptimal reports %llu, rebuilds: surface changed %llu, surface unknown %llu, "
                "once per episode %llu; sticky rechecks %llu (failed %llu)\n",
                PresentPolicy::kPolicySwitch, policy.mode == PresentPolicy::PolicyMode::New ? 1 : 0,
                read(health.acquire_timeouts), read(health.render_frame_wait_timeouts),
                read(health.redraw_wait_timeouts), read(health.suboptimal_reports),
                read(health.suboptimal_changed_rebuilds), read(health.suboptimal_unknown_rebuilds),
                read(health.suboptimal_latch_rebuilds), read(health.suboptimal_rechecks),
                read(health.suboptimal_recheck_failures));
}

bool WaitFrameFence(vk::Device device, vk::Fence fence, u64 timeout_ns, std::atomic<u64>& timeouts,
                    const char* outcome) {
    const vk::Result result = device.waitForFences(fence, false, timeout_ns);
    if (result == vk::Result::eSuccess) {
        return true;
    }
    if (result == vk::Result::eErrorDeviceLost) {
        Breadcrumbs::ReportDeviceLost("waiting for a frame");
    }
    ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during waiting for a frame");
    if (result == vk::Result::eTimeout && PresentPolicy::CountFirst(timeouts)) {
        std::printf("Display: a presentation frame was not done within %llu ms; %s\n",
                    static_cast<unsigned long long>(timeout_ns / 1'000'000ull), outcome);
    }
    return false;
}

} // namespace

void Presenter::WaitForSubmittedWork(u64 timeout_ns) {
    PrintPresentHealth(swapchain.Policy());
    // bbport: exit only. One deadline for the whole wait (it was timeout_ns per scheduler, up to
    // 3x). Every queue submission and present goes out under Scheduler::submit_mutex: taking it
    // (and never giving it back - the process is ending) stops the guest from queueing more
    // frames while the GPU drains, so the process does not end with fresh work in flight. Only
    // ticks already in the queue are waited for (CurrentTick-1 may never be submitted once the
    // lock is held; waiting for it just ran into the timeout). BB_EXIT_BLOCK_SUBMIT=0: old wait.
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::nanoseconds(timeout_ns);
    const char* block_env = std::getenv("BB_EXIT_BLOCK_SUBMIT");
    if (!(block_env && block_env[0] == '0')) {
        while (!Scheduler::submit_mutex.try_lock()) {
            if (Clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const vk::Device device = instance.GetDevice();
    for (Scheduler* scheduler : {&draw_scheduler, &present_scheduler, &flip_scheduler}) {
        const u64 value = scheduler->SubmittedTick();
        if (value == 0) {
            continue;
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            return;
        }
        const vk::Semaphore handle = scheduler->GetWorkSemaphore()->Handle();
        const vk::SemaphoreWaitInfo wait_info{
            .semaphoreCount = 1,
            .pSemaphores = &handle,
            .pValues = &value,
        };
        const u64 remaining = u64(
            std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count());
        (void)device.waitSemaphores(wait_info, remaining);
    }
}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
    frame_gen.reset();
    for (auto& frame : generated_frames) {
        if (frame.image) {
            vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
            device.destroyImageView(frame.image_view);
        }
        device.destroyFence(frame.present_done);
    }
    dlss_fg.reset();
    for (auto& frame : dlss_hudless_frames) {
        if (frame.image) {
            vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
            device.destroyImageView(frame.image_view);
        }
    }
    DestroyHdrTarget(hdr_game);
    DestroyHdrTarget(hdr_overlay);
    DestroyHdrTarget(dlss_hdr_composite);
    DestroyHdrTarget(dlss_hdr_hudless);
}

void Presenter::CreateHdrTarget(HdrTarget& target, vk::Extent2D extent, const char* name,
                                vk::Format format) {
    const vk::Device device = instance.GetDevice();
    // The SDR format: the blit's and the menu pipeline's (BbOverlay::Init); sRGB encoded values,
    // decoded by the encode pass. Format support is checked by Swapchain::FindPresentFormat.
    // Another format (DLSS-G's HDR HUD-less frame) is the swapchain's HDR one, read by DLSS-G.
    const bool sdr = format == vk::Format::eUndefined;
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        // Native+: the composite keeps the 16-bit chain's values above white.
        .format = !sdr ? format
                  : HdrChannel::Native16() && std::strcmp(name, "composite") == 0
                      ? HdrChannel::Native16Format
                      : swapchain.GetSurfaceFormat().format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eSampled |
                 (sdr ? vk::ImageUsageFlags{} : vk::ImageUsageFlagBits::eTransferSrc),
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    const VmaAllocationCreateInfo alloc_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage image{};
    const VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);
    const VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info,
                                           &alloc_info, &image, &target.allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating the HDR output image with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    target.image = vk::Image{image};
    SetObjectName(device, target.image, "HDR output {}", name);
    target.view = Check<"create HDR output image view">(device.createImageView({
        .image = target.image,
        .viewType = vk::ImageViewType::e2D,
        .format = image_info.format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
}

void Presenter::DestroyHdrTarget(const HdrTarget& target) {
    if (target.view) {
        instance.GetDevice().destroyImageView(target.view);
    }
    if (target.image) {
        vmaDestroyImage(instance.GetAllocator(), target.image, target.allocation);
    }
}

void Presenter::EnsureHdrTargets(vk::Extent2D extent) {
    if (hdr_game.image && hdr_extent == extent) {
        return;
    }
    if (hdr_game.image) {
        // Presents still in flight may read the old size's images: they go once the GPU has
        // finished this present's submission (later than every earlier one, same queue).
        present_scheduler.DeferOperation(
            [this, game = hdr_game, overlay = hdr_overlay] {
                DestroyHdrTarget(game);
                DestroyHdrTarget(overlay);
            });
    }
    CreateHdrTarget(hdr_game, extent, "composite");
    CreateHdrTarget(hdr_overlay, extent, "menu layer");
    hdr_extent = extent;
}

void Presenter::EnsureDlssHdrTargets(vk::Extent2D extent) {
    if (dlss_hdr_hudless.image && dlss_hdr_extent == extent) {
        return;
    }
    if (dlss_hdr_hudless.image) {
        present_scheduler.DeferOperation(
            [this, composite = dlss_hdr_composite, hudless = dlss_hdr_hudless] {
                DestroyHdrTarget(composite);
                DestroyHdrTarget(hudless);
            });
    }
    // Native+: the 16-bit chain's frame without its UI, as the back buffer's composite.
    CreateHdrTarget(dlss_hdr_composite, extent, "DLSS-G HUD-less composite",
                    HdrChannel::Native16() ? HdrChannel::Native16Format : vk::Format::eUndefined);
    CreateHdrTarget(dlss_hdr_hudless, extent, "DLSS-G HUD-less", swapchain.GetHdrFormat().format);
    dlss_hdr_extent = extent;
}

void Presenter::PresentHdr(vk::CommandBuffer cmdbuf, Frame* frame, vk::Extent2D extent,
                           bool dlss_inputs) {
    EnsureHdrTargets(extent);
    const vk::Image swapchain_image = swapchain.Image();
    const bool overlay = BbOverlay::Visible();
    const vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const auto barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                             vk::ImageLayout new_layout, vk::PipelineStageFlags2 src_stage,
                             vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                             vk::AccessFlags2 dst_access) {
        return vk::ImageMemoryBarrier2{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = color_range,
        };
    };
    const auto barriers = [&](std::span<const vk::ImageMemoryBarrier2> image_barriers) {
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
            .pImageMemoryBarriers = image_barriers.data(),
        });
    };
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    using Layout = vk::ImageLayout;

    // The composite and the menu layer are rewritten whole: their old contents (read by the
    // previous present's encode, same queue: the all-commands source scope orders it) go.
    // The frame as in the SDR path: General -> transfer source -> General.
    std::array<vk::ImageMemoryBarrier2, 3> pre{
        barrier(hdr_game.image, Layout::eUndefined, Layout::eTransferDstOptimal,
                Stage::eAllCommands, Access::eNone, Stage::eTransfer, Access::eTransferWrite),
        barrier(frame->image, Layout::eGeneral, Layout::eTransferSrcOptimal, Stage::eAllCommands,
                Access::eColorAttachmentWrite, Stage::eTransfer, Access::eTransferRead),
        barrier(hdr_overlay.image, Layout::eUndefined, Layout::eTransferDstOptimal,
                Stage::eAllCommands, Access::eNone, Stage::eTransfer, Access::eTransferWrite),
    };
    barriers(std::span{pre.data(), overlay ? 3u : 2u});
    const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
    cmdbuf.clearColorImage(hdr_game.image, Layout::eTransferDstOptimal, black, color_range);
    if (overlay) {
        const vk::ClearColorValue transparent{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
        cmdbuf.clearColorImage(hdr_overlay.image, Layout::eTransferDstOptimal, transparent,
                               color_range);
    }
    const vk::MemoryBarrier2 clear_done{
        .srcStageMask = Stage::eTransfer,
        .srcAccessMask = Access::eTransferWrite,
        .dstStageMask = Stage::eTransfer,
        .dstAccessMask = Access::eTransferWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &clear_done,
    });
    cmdbuf.blitImage(frame->image, Layout::eTransferSrcOptimal, hdr_game.image,
                     Layout::eTransferDstOptimal,
                     MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                     vk::Filter::eLinear);

    std::array<vk::ImageMemoryBarrier2, 4> post{
        barrier(hdr_game.image, Layout::eTransferDstOptimal, Layout::eShaderReadOnlyOptimal,
                Stage::eTransfer, Access::eTransferWrite, Stage::eFragmentShader,
                Access::eShaderSampledRead),
        barrier(frame->image, Layout::eTransferSrcOptimal, Layout::eGeneral, Stage::eTransfer,
                Access::eTransferRead, Stage::eAllCommands, Access::eColorAttachmentWrite),
        // The acquire semaphore is waited on at all commands: this transition comes after it.
        barrier(swapchain_image, Layout::eUndefined, Layout::eColorAttachmentOptimal,
                Stage::eColorAttachmentOutput, Access::eNone, Stage::eColorAttachmentOutput,
                Access::eColorAttachmentWrite),
        barrier(hdr_overlay.image, Layout::eTransferDstOptimal, Layout::eColorAttachmentOptimal,
                Stage::eTransfer, Access::eTransferWrite, Stage::eColorAttachmentOutput,
                Access::eColorAttachmentRead | Access::eColorAttachmentWrite),
    };
    barriers(std::span{post.data(), overlay ? 4u : 3u});

    // bbport: the settings menu / FPS counter, at display resolution, into its own layer: the
    // encode shows it at paper white over the (possibly expanded) frame.
    if (overlay) {
        BbOverlay::Render(cmdbuf, hdr_overlay.view, extent);
        const std::array menu_done{
            barrier(hdr_overlay.image, Layout::eColorAttachmentOptimal,
                    Layout::eShaderReadOnlyOptimal, Stage::eColorAttachmentOutput,
                    Access::eColorAttachmentWrite, Stage::eFragmentShader,
                    Access::eShaderSampledRead),
        };
        barriers(menu_done);
    }

    const auto& settings = BbSettings::Get();
    const bool scrgb =
        swapchain.GetHdrFormat().colorSpace == vk::ColorSpaceKHR::eExtendedSrgbLinearEXT;
    // bbport: native mode (hdr_mode, live): the frame's gain map when it has one (made for the
    // frame this display buffer holds, vk_hdr_native.h), else plain SDR at paper white for this
    // frame (title, menus, loading, movies: nothing in them is meant to be brighter).
    const int mode = std::clamp(settings.hdr_mode.load(), 0, BbSettings::HdrModeCount - 1);
    // A generated frame (FSR 3.1 frame generation) has no gain map of its own: it takes the
    // real frame's it was interpolated towards (PresentGenerated), or every other frame would
    // fall back to plain SDR.
    const Frame* gain = hdr_gain_source ? hdr_gain_source : frame;
    const bool native_plus = HdrChannel::Native16() && frame->nativeplus_frame;
    BbSettings::Get().hdr_native16_live.store(native_plus, std::memory_order_relaxed); // menu status
    const bool native = !HdrChannel::Native16() && mode == BbSettings::HdrModeNative &&
                        gain->hdr_native && gain->hdr_gain;
    LogHdrState(scrgb, mode, native);
    // BB_HDR_DEBUG=1: the gain as a heat map over a grey picture (hdr_output.frag).
    static const bool debug = [] {
        const char* env = std::getenv("BB_HDR_DEBUG");
        return env && env[0] == '1';
    }();
    // The gain map covers the frame, which the blit above letterboxed into this rectangle.
    const vk::Rect2D fit = FitImage(s32(frame->width), s32(frame->height), s32(extent.width),
                                    s32(extent.height));
    const HostPasses::HdrOutputPass::Settings hdr{
        .paper_white = std::clamp(settings.hdr_paper_white.load(), BbSettings::HdrPaperWhiteMin,
                                  BbSettings::HdrPaperWhiteMax),
        .peak = BbSettings::HdrPeakNits(settings),
        .boost = HdrChannel::Native16() || (mode == BbSettings::HdrModeNative && !native)
                     ? 0.0f
                     : std::clamp(settings.hdr_boost.load(), 0.0f, 1.0f),
        .flags = (scrgb ? HostPasses::HdrOutputPass::FlagScRgb : 0u) |
                 (overlay ? HostPasses::HdrOutputPass::FlagOverlay : 0u) |
                 (native ? HostPasses::HdrOutputPass::FlagNative : 0u) |
                 (debug ? HostPasses::HdrOutputPass::FlagDebug : 0u) |
                 (mode == BbSettings::HdrModeNative && !native ? HostPasses::HdrOutputPass::FlagUiFrame : 0u) |
                 // The split comparison (Native / Expand); Native+ has its own (hdr16_compare).
                 (!native_plus && settings.hdr_compare.load(std::memory_order_relaxed) ? HostPasses::HdrOutputPass::FlagCompare : 0u) |
                 (native_plus ? HostPasses::HdrOutputPass::FlagNativePlus : 0u) |
                 (native_plus && frame->nativeplus_encoded ? HostPasses::HdrOutputPass::FlagEncoded : 0u) |
                 (native_plus && frame->hud_snapshot ? HostPasses::HdrOutputPass::FlagSnapshot : 0u) |
                 (native_plus && HdrChannel::VanillaChain() ? HostPasses::HdrOutputPass::FlagGameLook : 0u),
        .gain_offset = {float(fit.offset.x), float(fit.offset.y)},
        .gain_scale = {1.0f / float(std::max(fit.extent.width, 1u)),
                       1.0f / float(std::max(fit.extent.height, 1u))},
        .ui_white = (mode == BbSettings::HdrModeNative || native_plus)
                        // UI Brightness in nits, as a factor of the paper white the pass gets.
                        ? std::clamp(settings.hdr_ui_nits.load(), BbSettings::HdrUiNitsMin,
                                     BbSettings::HdrUiNitsMax) /
                              std::clamp(settings.hdr_paper_white.load(), BbSettings::HdrPaperWhiteMin,
                                         BbSettings::HdrPaperWhiteMax)
                        : 1.0f,
        .ui_vivid = mode == BbSettings::HdrModeNative ? std::clamp(settings.hdr_ui_color.load(), 0.0f, 1.0f) : 0.0f,
        // The menu's HDR preset Off is the game's own look: no black floor either.
        .black_floor = settings.hdr_preset == 0
                           ? 0.0f
                           : settings.hdr_black_floor.load() / std::max(settings.hdr_paper_white.load(), 1.0f),
    };
    // bbport: MaxCLL, the brightest the content gets: native mode's highlights reach the peak;
    // expand mode's SDR white goes to boost's share of it. It changes with the settings, not
    // per frame: a display re-mapping its tone curve on every fallback would pump.
    const float expand_white =
        std::max(1.0f + (hdr.peak / hdr.paper_white - 1.0f) * hdr.boost, 1.0f);
    const float content_peak = HdrChannel::Native16() || mode == BbSettings::HdrModeNative
                                   ? hdr.peak
                                   : std::min(hdr.peak, hdr.paper_white * expand_white);
    swapchain.SetHdrMetadata(hdr.peak, content_peak, hdr.paper_white);
    if (native) {
        // Written on the draw scheduler's queue before this submission's wait (Present).
        const std::array gain_ready{
            barrier(vk::Image(gain->hdr_gain->image), Layout::eGeneral, Layout::eGeneral,
                    Stage::eAllCommands, Access::eMemoryWrite, Stage::eFragmentShader,
                    Access::eShaderSampledRead),
        };
        barriers(gain_ready);
    }
    hdr_pass.Render(cmdbuf, hdr_game.view, overlay ? hdr_overlay.view : hdr_game.view,
                    native ? *gain->hdr_gain->view : hdr_game.view,
                    native ? Layout::eGeneral : Layout::eShaderReadOnlyOptimal,
                    swapchain.ImageView(), extent, hdr,
                    native_plus && frame->hud_snapshot ? *frame->hud_snapshot->view : hdr_game.view);

    const std::array to_present{
        barrier(swapchain_image, Layout::eColorAttachmentOptimal, Layout::ePresentSrcKHR,
                Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite,
                Stage::eBottomOfPipe, Access::eNone),
    };
    barriers(to_present);

    // bbport: DLSS Frame Generation in HDR: the frame without its UI, placed and encoded as the
    // back buffer's frame was (same pass and settings, without the menu layer), so the two differ
    // only in the UI and the menu; DLSS-G takes it and the UI alpha made from the difference.
    if (!dlss_inputs || !DlssFrameGenHdr()) {
        return;
    }
    const Frame& hudless = dlss_hudless_frames[frame->id];
    if (!hudless.image) {
        return;
    }
    EnsureDlssHdrTargets(extent);
    const std::array to_copy{
        barrier(dlss_hdr_composite.image, Layout::eUndefined, Layout::eTransferDstOptimal,
                Stage::eAllCommands, Access::eNone, Stage::eTransfer, Access::eTransferWrite),
        barrier(hudless.image, Layout::eGeneral, Layout::eTransferSrcOptimal, Stage::eAllCommands,
                Access::eMemoryWrite, Stage::eTransfer, Access::eTransferRead),
    };
    barriers(to_copy);
    cmdbuf.clearColorImage(dlss_hdr_composite.image, Layout::eTransferDstOptimal, black, color_range);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &clear_done,
    });
    cmdbuf.blitImage(hudless.image, Layout::eTransferSrcOptimal, dlss_hdr_composite.image,
                     Layout::eTransferDstOptimal,
                     MakeImageBlitFit(hudless.width, hudless.height, extent.width, extent.height),
                     vk::Filter::eLinear);
    const std::array to_encode{
        barrier(dlss_hdr_composite.image, Layout::eTransferDstOptimal,
                Layout::eShaderReadOnlyOptimal, Stage::eTransfer, Access::eTransferWrite,
                Stage::eFragmentShader, Access::eShaderSampledRead),
        barrier(hudless.image, Layout::eTransferSrcOptimal, Layout::eGeneral, Stage::eTransfer,
                Access::eTransferRead, Stage::eAllCommands, Access::eMemoryRead),
        barrier(dlss_hdr_hudless.image, Layout::eUndefined, Layout::eColorAttachmentOptimal,
                Stage::eAllCommands, Access::eNone, Stage::eColorAttachmentOutput,
                Access::eColorAttachmentWrite),
    };
    barriers(to_encode);
    auto hudless_hdr = hdr;
    // Without the menu layer; Native+: no UI to tell from the scene in it (no pre-UI snapshot).
    hudless_hdr.flags &= ~(HostPasses::HdrOutputPass::FlagOverlay |
                           HostPasses::HdrOutputPass::FlagSnapshot);
    hdr_pass.Render(cmdbuf, dlss_hdr_composite.view, dlss_hdr_composite.view,
                    native ? *frame->hdr_gain->view : dlss_hdr_composite.view,
                    native ? Layout::eGeneral : Layout::eShaderReadOnlyOptimal,
                    dlss_hdr_hudless.view, extent, hudless_hdr);
    const std::array encoded{
        barrier(dlss_hdr_hudless.image, Layout::eColorAttachmentOptimal, Layout::eGeneral,
                Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite, Stage::eAllCommands,
                Access::eMemoryRead),
    };
    barriers(encoded);
    dlss_fg->RecordPresent(cmdbuf, frame->dlss, dlss_hdr_hudless.image, dlss_hdr_hudless.view,
                           extent, swapchain_image, swapchain.GetHdrFormat().format, extent,
                           vk::Rect2D{{0, 0}, extent});
}

void Presenter::LogHdrState(bool scrgb, int mode, bool native_frame) {
    if (mode != hdr_logged_mode) {
        hdr_logged_mode = mode;
        hdr_native_state = hdr_logged_native = -1;
        std::printf("HDR output: %s, mode %s (%s)\n",
                    scrgb ? "scRGB (FP16)" : "HDR10 (PQ, BT.2020)", BbSettings::HdrModeNames[mode],
                    HdrChannel::Native16()
                        ? "Native+, tone mapped on the game's scene, 16-bit chain"
                    : mode == BbSettings::HdrModeNative
                        ? "highlights from the game's HDR scene, plain SDR where a frame has none"
                        : "highlights expanded from the SDR picture");
    }
    if (mode != BbSettings::HdrModeNative) {
        return;
    }
    // Menus, loading screens and movies have no scene: frames come and go without a gain map.
    const int state = native_frame ? 1 : 0;
    if (state != hdr_native_state) {
        hdr_native_state = state;
        ++hdr_native_switches;
    }
    const auto now = std::chrono::steady_clock::now();
    if (hdr_native_state == hdr_logged_native || now < hdr_next_log) {
        return;
    }
    std::printf("HDR native: %s",
                state ? "gain maps from the game's scene"
                      : "no gain map (no scene: title, menu, loading, movie), SDR");
    if (hdr_native_switches > 1) {
        std::printf(" (%u switches since the last line)", hdr_native_switches);
    }
    std::printf("\n");
    hdr_logged_native = state;
    hdr_native_switches = 0;
    hdr_next_log = now + std::chrono::seconds(5);
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = HdrChannel::Native16() ? HdrChannel::Native16Format : swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    const PresentPolicy::PolicyMode policy_mode = swapchain.Policy().mode;
    while (!WaitFrameFence(instance.GetDevice(), frame->present_done,
                           PresentPolicy::RedrawWaitNs(policy_mode),
                           swapchain.Policy().health.redraw_wait_timeouts,
                           "the screen redraw is skipped")) {
        if (PresentPolicy::RedrawGivesUp(policy_mode)) {
            return nullptr;
        }
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    BbTimeline::Note(BbTimeline::PipeTask, 4, draw_scheduler.CurrentTick());
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    const bool nativeplus_frame =
        HdrChannel::Native16() && upscaled && display.format == HdrChannel::Native16Format;
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();
    // Native+: the 16-bit chain's frame, its SDR part encoded as the display buffer's format
    // says, and the scene before its HUD (the final pass tells the HUD from it).
    frame->nativeplus_frame = nativeplus_frame;
    frame->nativeplus_encoded =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb ||
        GetFrameViewFormat(attribute.attrib.pixel_format) == vk::Format::eB8G8R8A8Srgb ||
        GetFrameViewFormat(attribute.attrib.pixel_format) == vk::Format::eR8G8B8A8Srgb;
    if (nativeplus_frame) {
        rasterizer->GetUpscaler().TakeHudSnapshot(cpu_address, frame->hud_snapshot);
    } else {
        frame->hud_snapshot.reset();
    }

    // bbport: native HDR output: the gain map made at this display buffer's display pass (for
    // the frame it holds, a frame earlier in the command stream) goes with the frame to the
    // present thread; the frame's old one, presented (its fence was waited above), goes back.
    frame->hdr_native = rasterizer->GetHdrNative().TakeGainMap(cpu_address, frame->hdr_gain);

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    // bbport: recorded like draws, on the recording threads. CommandBuffer() would first wait
    // for them to record everything handed over before (~90 us a frame of the draw recording
    // thread) and then submit from this thread.
    draw_scheduler.EndRendering();

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        // Native+: the RGBA16F display image holds the game's channel order; the view swaps red
        // and blue where the game's display buffer and the frame's view order them differently.
        const auto bgra = [](vk::Format format) {
            return format == vk::Format::eB8G8R8A8Srgb || format == vk::Format::eB8G8R8A8Unorm;
        };
        const bool swap_rb = nativeplus_frame && bgra(view_info.format) != bgra(display.guest_format);
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = nativeplus_frame ? display.format : view_info.format,
            .components = {swap_rb ? vk::ComponentSwizzle::eB : vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity,
                           swap_rb ? vk::ComponentSwizzle::eR : vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        GrabFace(instance, draw_scheduler, display.image, display.width, display.height, display.format);
        draw_scheduler.Record([image = display.image](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier2 to_read{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
        });
    } else {
        auto& image = texture_cache.GetImage(image_id);
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        // bbport: BB_PRESENT_DUMP_TRIGGER for the game's own display buffer too.
        if (auto& upscaler = rasterizer->GetUpscaler(); upscaler.PresentDumpDue()) {
            runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            upscaler.DumpPresented(image.GetImage(), image_size.width, image_size.height,
                                   view_info.format);
        }
        if (BbFaceShot::Pending()) {
            runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            GrabFace(instance, draw_scheduler, image.GetImage(), image_size.width, image_size.height, view_info.format);
        }
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;

    // Keep the gamma-encoded HDR values above one through the post-process target.
    pp_settings.hdr = nativeplus_frame;
    // bbport: frame generation: this frame's generated one, shown before it. Without a free
    // generated frame the interpolation still runs (its history stays whole) but none is shown.
    frame->generated = nullptr;
    std::optional<FrameGenerator::Job> gen_job;
    Frame* generated = nullptr;
    {
        // Native+'s frames are HDR too (16-bit): FSR 3.1 interpolates them as they are.
        const bool hdr = BbSettings::Get().hdr_output.load(std::memory_order_relaxed) !=
                             BbSettings::HdrOutputOff ||
                         frame->is_hdr || nativeplus_frame;
        gen_job = frame_gen->Prepare(image_size, view_info.format, hdr, fsr_settings.enable,
                                     draw_scheduler.CurrentTick());
        if (gen_job && gen_job->present) {
            generated = TakeGeneratedFrame();
            if (generated) {
                if (generated->width != frame->width || generated->height != frame->height ||
                    generated->is_hdr != frame->is_hdr) {
                    RecreateFrame(generated, frame->width, frame->height);
                }
                std::atomic_ref<bool>(generated->generated_ok).store(false);
            } else {
                gen_job->present = false;
            }
        }
    }

    // bbport: DLSS Frame Generation: this game frame's number (its Reflex markers) and, with a
    // DLSS job, its inputs: the captures into the frame's slot and the frame without its UI
    // through the same post process as the frame (vk_streamline.h).
    frame->dlss = {};
    frame->dlss.index = dlss_fg->CurrentFrame();
    Frame* dlss_hudless = nullptr;
    FsrFrameGen* const fsr_fg =
        gen_job && gen_job->sdk && swapchain.UsesFsrFrameGen() ? swapchain.GetFsrFrameGen() : nullptr;
    if (gen_job && gen_job->dlss && (gen_job->sdk ? fsr_fg != nullptr : dlss_fg->Available())) {
        dlss_hudless = &dlss_hudless_frames[frame->id];
        if (dlss_hudless->width != frame->width || dlss_hudless->height != frame->height ||
            dlss_hudless->is_hdr != frame->is_hdr || !dlss_hudless->image) {
            RecreateFrame(dlss_hudless, frame->width, frame->height);
        }
        if (fsr_fg) {
            fsr_fg->EnsureSlot(frame->id, gen_job->render_width, gen_job->render_height);
        } else {
            dlss_fg->EnsureSlot(frame->id, gen_job->render_width, gen_job->render_height);
        }
        frame->dlss.inputs = true;
        frame->dlss.slot = frame->id;
        frame->dlss.render_width = gen_job->render_width;
        frame->dlss.render_height = gen_job->render_height;
        frame->dlss.jitter = gen_job->jitter;
        frame->dlss.near_plane = gen_job->near_plane;
        frame->dlss.vertical_fov = gen_job->vertical_fov;
        frame->dlss.reset = gen_job->reset;
        frame->dlss.camera = gen_job->camera;
    }
    const bool dlss_srgb = view_info.format == vk::Format::eR8G8B8A8Srgb ||
                           view_info.format == vk::Format::eB8G8R8A8Srgb;

    // Native+ (FSR 3.1 frame generation): the generated frame's scene before its HUD is FSR's
    // interpolated frame, copied after the composition into the generated frame's own snapshot
    // (its pool frame is not reused before it is shown): the final pass tells its HUD as on a
    // real frame, at UI Brightness.
    vk::Image generated_snapshot{};
    u32 generated_width = 0, generated_height = 0;
    if (generated && nativeplus_frame && !gen_job->dlss) {
        generated_width = gen_job->width;
        generated_height = gen_job->height;
        auto& snapshot = generated->hud_snapshot;
        if (!snapshot || snapshot->width != generated_width ||
            snapshot->height != generated_height) {
            const auto device = instance.GetDevice();
            snapshot = std::make_unique<HdrGainMap>();
            snapshot->image = VideoCore::UniqueImage(device, instance.GetAllocator());
            snapshot->image.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = HdrChannel::Native16Format,
                .extent = {generated_width, generated_height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
            snapshot->view = Check(device.createImageViewUnique({
                .image = vk::Image(snapshot->image),
                .viewType = vk::ImageViewType::e2D,
                .format = HdrChannel::Native16Format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
            snapshot->width = generated_width;
            snapshot->height = generated_height;
        }
        generated_snapshot = vk::Image(snapshot->image);
    } else if (generated) {
        generated->hud_snapshot.reset();
    }

    // Numbered here, in command order; marked where the commands are recorded.
    const u32 stream = draw_scheduler.CrumbStream();
    const bool crumbs = Breadcrumbs::Enabled();
    const u32 fsr_crumb = crumbs ? Breadcrumbs::Note(stream, {.name = "presenter FSR"}) : 0;
    const u32 pp_crumb = crumbs ? Breadcrumbs::Note(stream, {.name = "presenter post process"}) : 0;
    draw_scheduler.Record([this, frame, image_view, image_size, frame_subresources, stream,
                           fsr_crumb, pp_crumb, fsr = fsr_settings, pp = pp_settings, generated,
                           gen_job, dlss_hudless, dlss_srgb, dlss = dlss_fg.get(), fsr_fg,
                           fg = frame_gen.get(), generated_snapshot, generated_width,
                           generated_height](vk::CommandBuffer cmdbuf) {
        // Frames of different submissions may be recorded on two threads at once.
        std::scoped_lock lock{passes_mutex};
        const auto pre_barrier = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = frame->image,
            .subresourceRange{frame_subresources},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &pre_barrier,
        });
        if (fsr_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, fsr_crumb, false);
        }
        const vk::ImageView input = fsr_pass.Render(cmdbuf, image_view, image_size,
                                                    {frame->width, frame->height}, fsr,
                                                    frame->is_hdr || frame->nativeplus_frame);
        if (fsr_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, fsr_crumb, true);
            Breadcrumbs::Mark(cmdbuf, stream, pp_crumb, false);
        }
        pp_pass.Render(cmdbuf, input, image_size, *frame, pp);
        if (pp_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, pp_crumb, true);
        }
        // bbport: DLSS Frame Generation's inputs for this frame: copies of the captures, and the
        // frame without its UI through the same post process (its capture stays in General).
        if (dlss_hudless) {
            if (fsr_fg) {
                fsr_fg->RecordInputs(cmdbuf, frame->id, fg->DepthImage(), fg->MotionImage(),
                                     gen_job->render_width, gen_job->render_height);
            } else {
                dlss->RecordInputs(cmdbuf, frame->id, fg->DepthImage(), fg->MotionImage(),
                                   gen_job->render_width, gen_job->render_height);
            }
            const auto hudless_barrier = [&](vk::ImageLayout from, vk::ImageLayout to) {
                const vk::ImageMemoryBarrier2 barrier{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                     vk::AccessFlagBits2::eMemoryWrite,
                    .oldLayout = from,
                    .newLayout = to,
                    .image = fg->HudlessImage(),
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                };
                cmdbuf.pipelineBarrier2(
                    {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
            };
            const auto to_target = vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .image = dlss_hudless->image,
                .subresourceRange{frame_subresources},
            };
            cmdbuf.pipelineBarrier2(
                vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
            hudless_barrier(vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal);
            pp_pass.Render(cmdbuf, fg->HudlessView(dlss_srgb), image_size, *dlss_hudless, pp);
            hudless_barrier(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eGeneral);
        }
        // bbport: frame generation: interpolated and composed, then the same post process
        // into the generated frame's image.
        if (gen_job && frame_gen->Record(cmdbuf, *gen_job, image_view) && generated) {
            const auto to_target = vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .image = generated->image,
                .subresourceRange{frame_subresources},
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &to_target,
            });
            pp_pass.Render(cmdbuf, frame_gen->ComposedView(), image_size, *generated, pp);
            if (generated_snapshot) {
                const vk::ImageSubresourceRange color{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
                const std::array to_copy{
                    vk::ImageMemoryBarrier2{
                        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                        .oldLayout = vk::ImageLayout::eGeneral,
                        .newLayout = vk::ImageLayout::eGeneral,
                        .image = frame_gen->GeneratedImage(),
                        .subresourceRange = color},
                    vk::ImageMemoryBarrier2{
                        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
                        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                        .oldLayout = vk::ImageLayout::eUndefined,
                        .newLayout = vk::ImageLayout::eTransferDstOptimal,
                        .image = generated_snapshot,
                        .subresourceRange = color}};
                cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 2,
                                         .pImageMemoryBarriers = to_copy.data()});
                cmdbuf.copyImage(frame_gen->GeneratedImage(), vk::ImageLayout::eGeneral,
                                 generated_snapshot, vk::ImageLayout::eTransferDstOptimal,
                                 vk::ImageCopy{
                                     .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     .extent = {generated_width, generated_height, 1}});
                const vk::ImageMemoryBarrier2 to_read{
                    .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout = vk::ImageLayout::eGeneral,
                    .image = generated_snapshot,
                    .subresourceRange = color};
                cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1,
                                         .pImageMemoryBarriers = &to_read});
            }
            std::atomic_ref<bool>(generated->generated_ok).store(true, std::memory_order_release);
        }
    });

    // Flush frame creation commands.
    BbStats::frame_number.fetch_add(1, std::memory_order_relaxed); // bbport: game frames shown
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    if (generated) {
        generated->ready_semaphore = frame->ready_semaphore;
        generated->ready_tick = frame->ready_tick;
        generated->hdr_native = false;
        // Native+ (FSR 3.1 frame generation): the generated frame is the chain's 16-bit image
        // like the real one, encoded alike by the final pass, with its own pre-HUD snapshot
        // (FSR's interpolated frame, above): not the real frame's, the scene moved since.
        generated->nativeplus_frame = frame->nativeplus_frame;
        generated->nativeplus_encoded = frame->nativeplus_encoded;
        frame->generated = generated;
    }
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    BbTimeline::Note(BbTimeline::PipeTask, 5, frame->ready_tick);
    dlss_fg->OnFrameSubmitted(); // bbport: Reflex: this frame's render submit ended

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                BbStats::WaitTimer timer{BbStats::present_wait_ns};
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    frame->hdr_native = false; // bbport: no game frame, no gain map

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

Frame* Presenter::TakeGeneratedFrame() {
    const vk::Device device = instance.GetDevice();
    for (u32 i = 0; i < generated_frames.size(); ++i) {
        Frame& frame = generated_frames[i];
        if (generated_busy[i].load(std::memory_order_acquire) ||
            device.getFenceStatus(frame.present_done) != vk::Result::eSuccess) {
            continue;
        }
        generated_busy[i].store(true, std::memory_order_release);
        return &frame;
    }
    return nullptr;
}

bool Presenter::PresentGenerated(Frame* frame, Frame* generated, bool& reached) {
    // Its commands went out with the real frame's submission.
    if (frame->ready_semaphore == draw_scheduler.GetWorkSemaphore()->Handle()) {
        draw_scheduler.WaitSubmitted(frame->ready_tick);
    }
    bool shown = false;
    if (std::atomic_ref<bool>(generated->generated_ok).load(std::memory_order_acquire) &&
        frame_gen->ShouldPresentGenerated()) {
        const auto start = FrameGenerator::Clock::now();
        hdr_gain_source = frame; // HDR output: the real frame's gain map (PresentHdr)
        presenting_generated = true;
        reached = Present(generated, true, false);
        presenting_generated = false;
        hdr_gain_source = nullptr;
        shown = true;
        frame_gen->WaitForRealSlot(start);
    }
    const auto index = size_t(generated - generated_frames.data());
    generated_busy[index].store(false, std::memory_order_release);
    return shown;
}

bool Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // bbport: frame generation: one more swapchain image while it is on; the generated frame
    // goes first, the real one half a game frame later.
    const bool real_game_frame = !is_reusing_frame && is_game_frame;
    const auto present_start = FrameGenerator::Clock::now();
    bool generated_made = false, generated_shown = false, generated_reached = false;
    if (real_game_frame) {
        if (swapchain.SetExtraImages(frame_gen->WantsExtraImage() ? 1 : 0) &&
            window.GetWidth() != 0 && window.GetHeight() != 0) {
            dlss_fg->BeforeSwapchainChange();
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
            std::printf("Frame generation: swapchain of %u images\n", swapchain.GetImageCount());
        }
        if (Frame* generated = std::exchange(frame->generated, nullptr)) {
            generated_made = true;
            generated_shown = PresentGenerated(frame, generated, generated_reached);
        }
    }
    const bool record = (real_game_frame || presenting_generated) && frame_gen->Recording();
    const auto note_real = [&] {
        if (real_game_frame && frame_gen->Enabled()) {
            frame_gen->NoteSwapchain(swapchain.GetPresentMode(), swapchain.GetImageCount());
            frame_gen->OnRealPresented(present_start, generated_shown, generated_made,
                                       generated_reached);
        }
    };

    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };
    const auto bail = [&] {
        free_frame();
        if (record) {
            frame_gen->RecordPresent(presenting_generated, FrameGenerator::Clock::now(), 0, false);
        }
        return false;
    };

    // bbport: nothing to present to while the window is minimised (a 0x0 surface, issue #31).
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        return bail();
    }
    // Recreate the swapchain if the window was resized (or was minimised at the last try).
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||
        !swapchain.IsPresentable()) {
        dlss_fg->BeforeSwapchainChange();
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.IsPresentable()) {
            return bail();
        }
    }

    using Vulkan::PresentPolicy::AcquireStep;
    const auto acquire_start =
        record ? FrameGenerator::Clock::now() : FrameGenerator::Clock::time_point{};
    AcquireStep acquired = swapchain.AcquireNextImage();
    if (acquired == AcquireStep::Rebuild) {
        dlss_fg->BeforeSwapchainChange();
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        acquired = swapchain.AcquireNextImage();
        if (acquired == AcquireStep::Rebuild) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
        }
    }
    if (acquired != AcquireStep::Acquired) {
        return bail();
    }
    const u32 acquire_wait_us =
        record ? u32(std::chrono::duration_cast<std::chrono::microseconds>(
                         FrameGenerator::Clock::now() - acquire_start)
                         .count())
               : 0;

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // bbport: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    DumpFinalFrameIfDue(instance, scheduler, cmdbuf, frame->image, frame->width, frame->height,
                        swapchain.GetSurfaceFormat().format);

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    // bbport: HDR output (hdr in lantern.ini) while the swapchain has the HDR format.
    const bool hdr_output = swapchain.IsHdrOutput() && hdr_pass.IsCreated();
    BbSettings::Get().hdr_output.store(
        !hdr_output ? BbSettings::HdrOutputOff
        : swapchain.GetHdrFormat().colorSpace == vk::ColorSpaceKHR::eExtendedSrgbLinearEXT
            ? BbSettings::HdrOutputScRgb
            : BbSettings::HdrOutputHdr10,
        std::memory_order_relaxed);
    if (hdr_output) {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        PresentHdr(cmdbuf, frame, extent, real_game_frame && frame->dlss.inputs);
        if (swapchain.UsesFsrFrameGen()) {
            swapchain.GetFsrFrameGen()->RecordPresent(cmdbuf, nullptr, {}, {frame->width, frame->height},
                                                      swapchain.GetSurfaceFormat().format, extent,
                                                      {{0, 0}, extent}, BbOverlay::MenuOpen(), true);
        }
    } else {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        // bbport: the black bars are only needed where the fitted blit leaves part of the image
        // uncovered; a frame of the swapchain's aspect (the usual case: the game renders at the
        // window size) overwrites every pixel, and the full-screen clear plus its barrier were
        // one more full-resolution write each present. BB_PRESENT_ALWAYS_CLEAR=1: always clear.
        static const bool always_clear = [] {
            const char* env = std::getenv("BB_PRESENT_ALWAYS_CLEAR");
            return env && env[0] == '1';
        }();
        const vk::Rect2D fit = FitImage(s32(frame->width), s32(frame->height), s32(extent.width),
                                        s32(extent.height));
        const bool covers = fit.offset.x == 0 && fit.offset.y == 0 &&
                            fit.extent.width == extent.width && fit.extent.height == extent.height;
        if (always_clear || !covers) {
            const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
            cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black,
                                   color_range);
            const vk::MemoryBarrier clear_done{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        }
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        // bbport: the settings menu / FPS counter over the frame, at display resolution.
        const bool overlay = BbOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
        // bbport: DLSS Frame Generation: the frame without its UI at the back buffer's size and
        // the UI alpha image, from the finished back buffer (vk_streamline.h).
        if (real_game_frame && frame->dlss.inputs) {
            const Frame& hudless = dlss_hudless_frames[frame->id];
            dlss_fg->RecordPresent(cmdbuf, frame->dlss, hudless.image, hudless.image_view,
                                   {frame->width, frame->height}, swapchain_image,
                                   swapchain.GetSurfaceFormat().format, extent, fit);
        }
        if (swapchain.UsesFsrFrameGen()) {
            const bool inputs = real_game_frame && frame->dlss.inputs;
            swapchain.GetFsrFrameGen()->RecordPresent(
                cmdbuf, inputs ? &frame->dlss : nullptr,
                inputs ? dlss_hudless_frames[frame->id].image : vk::Image{}, {frame->width, frame->height},
                swapchain.GetSurfaceFormat().format, extent, fit, BbOverlay::MenuOpen(), false);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    // bbport: the frame's submission goes out from a recording thread (BB_ASYNC_SUBMIT); this
    // one, waiting for it on the same queue, must not get there first.
    if (frame->ready_semaphore == draw_scheduler.GetWorkSemaphore()->Handle()) {
        draw_scheduler.WaitSubmitted(frame->ready_tick);
    }
    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    // Present to swapchain. bbport: DLSS Frame Generation's constants, tags and markers for
    // this frame go out right before (none for a frame shown again).
    const DlssFrameInfo* dlss_info = real_game_frame ? &frame->dlss : nullptr;
    dlss_fg->BeforePresent(dlss_info, BbOverlay::MenuOpen(), hdr_output);
    if (real_game_frame) {
        LanternAntiLag::BeforePresent();
    }
    bool presented;
    if (swapchain.UsesFsrFrameGen()) {
        presented = swapchain.Present();
    } else {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        presented = swapchain.Present();
    }
    dlss_fg->AfterPresent(dlss_info);
    if (swapchain.UsesFsrFrameGen()) {
        swapchain.GetFsrFrameGen()->AfterPresent(real_game_frame);
    }
    // bbport: recreated after the lock: its device wait takes it (Swapchain::WaitIdle).
    if (!presented) {
        dlss_fg->BeforeSwapchainChange();
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
    if (record) {
        frame_gen->RecordPresent(presenting_generated, FrameGenerator::Clock::now(),
                                 acquire_wait_us, presented);
    }
    note_real();
    return presented;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    while (!WaitFrameFence(device, frame->present_done, PresentPolicy::RenderFrameWaitNs(swapchain.Policy().mode),
                           swapchain.Policy().health.render_frame_wait_timeouts,
                           "still waiting, the frame is in use")) {
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // minimised: keep the last size (a 0x0 frame image cannot be made, issue #31)
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
