// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>
#include <tuple>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "bbport_copy.h"
#include "engine/gx_objects.h"
#include "bbport_sections.h"
#include "bbport_toggles.h"
#include "bbport_free_check.h"
#include "engine/key_prompts.h"
#include "engine/hud_clean.h"
#include <zlib.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/overlap_diagnostics.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

namespace {
using Binding = TextureCache::BindingType;

void TraceOverlap(const ImageInfo& requested, Binding binding, ImageId id, const Image* candidate,
                  std::string_view action, std::string_view reason, int mip = -1, int slice = -1) {
    if (BbEngine::GxObjects::On()) {
        BbEngine::GxObjects::NoteOverlap(action.data(), action.size());
    }
    if (!ImageOverlapLogging()) {
        return;
    }

    // Detailed Logs is meant for short diagnostic captures. Skip the extremely common
    // exact-reuse path and rate-limit the rest so a loading screen cannot create a multi-GB log.
    if (action == "reuse") {
        return;
    }
    static std::atomic<u64> emitted{0};
    const u64 index = emitted.fetch_add(1, std::memory_order_relaxed);
    if (index >= 512 && (index % 1024) != 0) {
        return;
    }

    const ImageInfo empty{};
    const auto& cached = candidate ? candidate->info : empty;
    const auto requested_format = vk::to_string(requested.pixel_format);
    const auto cached_format = vk::to_string(cached.pixel_format);
    std::printf(
        "TextureDiag overlap[%llu]: action=%.*s reason=%.*s req_addr=0x%llx req_size=%llu "
        "req_fmt=%s req=%ux%ux%u mips=%u layers=%u type=%llu binding=%u "
        "candidate=%u cand_addr=0x%llx cand_size=%llu cand_fmt=%s cand=%ux%ux%u "
        "cand_mips=%u cand_layers=%u cand_type=%llu bound=%u target=%u view_mip=%d "
        "view_slice=%d\n",
        static_cast<unsigned long long>(index), static_cast<int>(action.size()), action.data(),
        static_cast<int>(reason.size()), reason.data(),
        static_cast<unsigned long long>(requested.guest_address),
        static_cast<unsigned long long>(requested.guest_size), requested_format.c_str(),
        requested.size.width, requested.size.height, requested.size.depth,
        requested.resources.levels, requested.resources.layers,
        static_cast<unsigned long long>(u64(requested.type)), u32(binding), id.index,
        static_cast<unsigned long long>(cached.guest_address),
        static_cast<unsigned long long>(cached.guest_size), cached_format.c_str(),
        cached.size.width, cached.size.height, cached.size.depth, cached.resources.levels,
        cached.resources.layers, static_cast<unsigned long long>(u64(cached.type)),
        candidate ? unsigned(candidate->binding.is_bound) : 0u,
        candidate ? unsigned(candidate->binding.is_target) : 0u, mip, slice);
    std::fflush(stdout);
}

bool ReusableFormat(const ImageInfo& requested, const ImageInfo& cached, Binding binding,
                    bool exact) {
    if (cached.pixel_format == vk::Format::eUndefined ||
        (exact && requested.pixel_format != cached.pixel_format)) {
        return false;
    }
    if (binding == Binding::DepthTarget) {
        return cached.props.is_depth && requested.props.has_stencil == cached.props.has_stencil &&
               requested.num_bits == cached.num_bits;
    }
    if (binding == Binding::Storage || binding == Binding::RenderTarget) {
        if (cached.props.is_depth) {
            return false;
        }
    }
    if (binding == Binding::Texture && cached.props.is_depth &&
        Vulkan::LiverpoolToVK::IsFormatDepthCompatible(requested.pixel_format)) {
        // ImageView promotes R16/R32 sampling to the cached depth image's actual format.
        return true;
    }
    return IsVulkanFormatCompatible(cached.pixel_format, requested.pixel_format);
}

std::string_view ReuseRejection(const ImageInfo& requested, const ImageInfo& cached, Binding binding,
                               bool exact, int& mip, int& slice) {
    mip = slice = 0;
    if (requested.guest_address != cached.guest_address) {
        mip = requested.MipOf(cached);
        slice = mip >= 0 ? requested.SliceOf(cached, mip) : -1;
        if (mip < 0 || slice < 0) {
            return "not an aligned, contained mip/slice";
        }
    }
    if (const auto reason = requested.ViewRejection(cached, {u32(mip), u32(slice)}); !reason.empty()) {
        return reason;
    }
    return ReusableFormat(requested, cached, binding, exact)
               ? std::string_view{} : "format or binding requires another backing image";
}
} // namespace

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
    static_pressure_gc_memory = pressure_gc_memory;
    static_critical_gc_memory = critical_gc_memory;
}

namespace {
/// bbport: discrete GPUs: the pressure mark as a share of the driver's current budget
/// (BB_GC_PRESSURE_PERCENT, default 80; the critical mark 10 points higher). 0: shadPS4's
/// marks only. Those keep 60% of an up to 8 GB budget vacant: on an 8 GB card the pressure
/// mark is ~2.5 GB while the game's VRAM sits at 3.4-4.3 GB (RTX 5060, Central Yharnam), so
/// the collector stayed in pressure mode, evicted hundreds of textures still in use (age 80
/// submissions, about one frame), uploaded them again and waited for the GPU to write tiled
/// render targets back. The marks never go below shadPS4's.
u64 DiscretePressurePercent() {
    static const u64 percent = [] {
        const char* env = std::getenv("BB_GC_PRESSURE_PERCENT");
        return env && env[0] ? std::min<u64>(std::strtoull(env, nullptr, 10), 85) : 80;
    }();
    return percent;
}
/// bbport: under pressure (below the critical mark) an image goes only once unused for this
/// many seconds (BB_GC_PRESSURE_SECONDS, default 1; 0: 80 submissions as before).
u64 PressureIdleSeconds() {
    static const u64 seconds = [] {
        const char* env = std::getenv("BB_GC_PRESSURE_SECONDS");
        return std::min<u64>(env && env[0] ? std::strtoull(env, nullptr, 10) : 1, 10);
    }();
    return seconds;
}
/// bbport (GPU hang guard): the collector never frees, nor writes back, an image used in the
/// last this many collector passes, one per guest submission and several a frame
/// (BB_GC_RECENT_FRAMES, default 48; 0: off). The memo hits that bind most
/// images (texture sets, binding and target memos: MarkFound) do not touch the LRU, so an image
/// sampled every frame looked idle: it was freed while in use, re-created and uploaded again,
/// and GPU-written render targets went through a synchronous write-back each time (stalls of
/// 0.3-2 s in area loads before a VK_ERROR_DEVICE_LOST in a full-screen blit).
u64 GcRecentFrames() {
    static const u64 frames = [] {
        const char* env = std::getenv("BB_GC_RECENT_FRAMES");
        return std::min<u64>(env && env[0] ? std::strtoull(env, nullptr, 10) : 48, 60);
    }();
    return frames;
}
/// bbport (GPU hang guard): a deleted image's Vulkan objects outlive its unregistration by this
/// many collector passes (submissions), then the GPU's completion of the tick of that pass
/// (BB_IMAGE_FREE_DELAY_FRAMES, default 32; 0: only the GPU tick, as shadPS4). A reference that outlived the registry change
/// (a memo, a proxy, the upscaler) then reads a still valid image instead of freed memory.
u64 ImageFreeDelayFrames() {
    static const u64 frames = [] {
        const char* env = std::getenv("BB_IMAGE_FREE_DELAY_FRAMES");
        return std::min<u64>(env && env[0] ? std::strtoull(env, nullptr, 10) : 32, 240);
    }();
    return frames;
}
} // namespace

TextureCache::~TextureCache() = default;

/// bbport BB_REFRESH_TRACE=1 (diagnostics): for images of 4 MiB and up, the writes that mark one
/// dirty (and their kind, InvalidateReason) and its refreshes, with the flags they found and what
/// they did: each combination once, 150 lines at most (loading screens would use up a plain
/// count before the frames that repeat one).
namespace RefreshTrace {
static bool On() {
    static const bool on = [] {
        const char* env = std::getenv("BB_REFRESH_TRACE");
        return env && env[0] == '1';
    }();
    return on;
}
static bool Large(const Image& image) {
    return On() && image.info.guest_size >= (4ull << 20);
}
static bool First(VAddr image, const char* kind, const char* what, u32 flags) {
    static std::mutex mutex;
    static std::vector<std::tuple<VAddr, const char*, const char*, u32>> seen;
    std::scoped_lock lock{mutex};
    const auto key = std::make_tuple(image, kind, what, flags);
    if (seen.size() >= 150 || std::find(seen.begin(), seen.end(), key) != seen.end()) {
        return false;
    }
    seen.push_back(key);
    return true;
}
static void Mark(const Image& image, const char* what, VAddr addr, u64 size) {
    if (!Large(image) ||
        !First(image.info.guest_address, invalidate_reason, what, u32(image.flags))) {
        return;
    }
    const auto format = vk::to_string(image.info.pixel_format);
    std::printf("Refresh trace: %s write %#llx (+%#llx, %#llx bytes) marks image %#llx %s "
                "%ux%u (%#llx bytes, flags %#x) %s\n",
                invalidate_reason, static_cast<unsigned long long>(addr),
                static_cast<unsigned long long>(addr - image.info.guest_address),
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(image.info.guest_address), format.c_str(),
                image.info.size.width, image.info.size.height,
                static_cast<unsigned long long>(image.info.guest_size), u32(image.flags), what);
}
static void Refresh(const Image& image, u32 flags, const char* outcome) {
    if (!Large(image) || !First(image.info.guest_address, "refresh", outcome, flags)) {
        return;
    }
    const auto format = vk::to_string(image.info.pixel_format);
    std::printf("Refresh trace: image %#llx %s %ux%u, %u mips, flags %#x: %s\n",
                static_cast<unsigned long long>(image.info.guest_address), format.c_str(),
                image.info.size.width, image.info.size.height, image.info.resources.levels, flags,
                outcome);
}
} // namespace RefreshTrace

std::string TextureCache::DescribeImagesIn(VAddr addr, u64 size) {
    std::unique_lock lock{mutex, std::try_to_lock}; // the caller may hold it already
    if (!lock.owns_lock()) {
        return " [images: not looked up]";
    }
    std::string out;
    int shown = 0;
    ForEachImageInRegion(addr, size, [&](ImageId, Image& image) {
        if (shown++ < 3) {
            out += fmt::format(" [image {:#x}+{:#x} {}x{} fmt {} tile {}{}{}]", image.info.guest_address,
                               image.info.guest_size, image.info.size.width,
                               image.info.size.height, u32(image.info.pixel_format),
                               u32(image.info.tile_mode), image.usage.render_target ? " RT" : "",
                               True(image.flags & ImageFlagBits::GpuModified) ? " gpu" : "");
        }
    });
    if (shown > 3) {
        out += fmt::format(" +{} more", shown - 3);
    }
    return out;
}

void TextureCache::ProcessDownloadImages() {
    // bbport: called for every fence (~450 a frame): the lock only when something is queued.
    if (!downloads_queued.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lk{download_images_mutex};
    downloads_queued.store(false, std::memory_order_relaxed);
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, true);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    image.mip_hashes_current = false; // its data lands in guest memory
    vk::BufferImageCopy image_copy = {
        .bufferOffset = 0,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    // bbport BB_GUEST_IN_PLACE: the GPU copies the image into the game's memory itself, in stream
    // order: before the fences that follow, no wait here and no CPU copy afterwards.
    if (GuestInPlace() && image.info.guest_address != 0) {
        const auto [arena, offset] =
            buffer_cache.ObtainBuffer(image.info.guest_address, download_size, true);
        if (buffer_cache.IsInPlace(image.info.guest_address, download_size)) {
            image_copy.bufferOffset = offset;
            runtime.DownloadImage(&image, arena, std::span{&image_copy, 1});
            return;
        }
    }
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, !sync);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    if (sync) {
        scheduler.Finish();
        download.Invalidate();
        BbFreeCheck::Check(image.info.guest_address, download_size, download.mapped,
                           BbFreeCheck::ImageDownload);
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(image.info.guest_address),
                                                  download.mapped, download_size);
        if (!WriteTracking()) {
            // bbport: the caches' own thread (stage A drained, or the draw recording thread):
            // a readback the range needs runs here. Sent to the GPU command thread, it waited
            // for that thread, or for this one, which never came back to run it.
            buffer_cache.InvalidateMemory(image.info.guest_address, download_size, true);
        }
    } else {
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size] {
                download.Invalidate();
                BbFreeCheck::Check(device_addr, download_size, download.mapped,
                                   BbFreeCheck::ImageDownload);
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr),
                                                          download.mapped, download_size);
                if (!WriteTracking()) {
                    buffer_cache.InvalidateMemory(device_addr, download_size);
                }
                runtime.GetStagingPool().FreeDeferred(download);
            });
    }
}

// Lantern Engine: freeing a GPU-written tiled image under VRAM pressure. Its contents exist only
// in VRAM, in linear form; guest memory holds the surface tiled. The image is copied out linearly
// and tiled by the tiling shader into a host buffer that already holds the guest bytes, writing
// only texels inside the image's extent (TileImage's extent_only): padding rows and columns, and
// whatever lies beyond a render target's hinted size, keep the game's bytes. The buffer then
// replaces the guest surface.
bool TextureCache::StoreTiledImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return true; // guest memory already has it
    }
    const ImageInfo& info = image.info;
    const VAddr address = info.guest_address;
    const u64 size = info.guest_size;
    if (!info.props.is_tiled || address == 0 || size == 0 || !image.backing) {
        return false;
    }

    // What the tiling shader can express: one colour plane, single-sampled, thin tiling, no
    // compression metadata, whole texels of a size it has a pipeline for; not a scene proxy's
    // source (its newest contents may be in the reduced-resolution copy).
    const bool plain_colour =
        image.aspect_mask == vk::ImageAspectFlags{vk::ImageAspectFlagBits::eColor} &&
        !info.props.is_depth && !info.props.has_stencil && info.stencil_addr == 0 &&
        !info.props.is_block && !info.props.is_volume && info.num_samples == 1 &&
        AmdGpu::GetMicroTileThickness(info.array_mode) == 1 && !image.scene_proxy &&
        info.meta_info.cmask_addr == 0 && info.meta_info.fmask_addr == 0 &&
        info.meta_info.htile_addr == 0;
    const u32 bits = info.num_bits;
    const bool texel_size =
        bits == 8 || bits == 16 || bits == 32 || bits == 64 || bits == 96 || bits == 128;
    if (!plain_colour || !texel_size) {
        return false;
    }
    const u32 texel_bytes = bits / 8;
    // The shader runs in groups of 64 texels over the whole surface, and finds a texel's mip by
    // walking the mip sizes: the surface must be whole groups, its mips back to back, each
    // pitch x height x layers texels.
    const u32 levels = info.resources.levels;
    const u32 layers = info.resources.layers;
    if (size % (u64{texel_bytes} * 64) != 0 || levels == 0 || layers == 0 ||
        levels > info.mips_layout.size()) {
        return false;
    }
    u64 next_offset = 0;
    for (u32 level = 0; level < levels; ++level) {
        const auto& mip = info.mips_layout[level];
        const u64 expected = u64{mip.pitch} * mip.height * texel_bytes * layers;
        if (mip.pitch == 0 || mip.height == 0 || (mip.pitch | mip.height) % 8 != 0 ||
            mip.offset != next_offset || mip.size != expected) {
            return false;
        }
        next_offset += mip.size;
    }
    if (next_offset != size) {
        return false;
    }

    // Guest memory must take all of it, and nothing else may have GPU data there that storing
    // this image would overwrite (the buffer cache's, or another image's).
    auto* const memory = Core::Memory::Instance();
    if (memory->ClampRangeSize(address, size) != size ||
        buffer_cache.IsRegionGpuModified(address, size)) {
        return false;
    }
    ImageIds others;
    bool others_hold_gpu_data = false;
    ForEachImageInRegion(address, size, [&](ImageId other_id, Image& other) {
        if (other_id != image_id) {
            others.push_back(other_id);
            others_hold_gpu_data |= True(other.flags & ImageFlagBits::GpuModified);
        }
    });
    if (others_hold_gpu_data) {
        return false;
    }

    // The guest bytes first (the shader leaves the padding as it finds it), then the image.
    const auto host = runtime.GetStagingPool().Request(
        size, MemoryType::HostCached, std::max<u64>(instance.StorageMinAlignment(), 16));
    if (!host.mapped) {
        return false;
    }
    memory->CopySparseMemory(address, host.mapped, size);
    host.Flush();

    boost::container::small_vector<vk::BufferImageCopy, 14> copies;
    for (u32 level = 0; level < levels; ++level) {
        const auto& mip = info.mips_layout[level];
        copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip.offset,
            .bufferRowLength = mip.pitch,
            .bufferImageHeight = mip.height,
            .imageSubresource{vk::ImageAspectFlagBits::eColor, level, 0, layers},
            .imageOffset = {0, 0, 0},
            .imageExtent = {std::min(mip.pitch, std::max(info.size.width >> level, 1u)),
                            std::min(mip.height, std::max(info.size.height >> level, 1u)), 1},
        });
    }
    scheduler.EndRendering();
    tile_manager.TileImage(image, copies, host.buffer, host.offset, /*extent_only=*/true);
    runtime.FlushBarriers();
    // Waited for here: the image still watches its pages until the store has landed (a store
    // after FreeImage would land over whatever the game put there meanwhile).
    scheduler.Finish();
    host.Invalidate();
    BbFreeCheck::Check(address, size, host.mapped, BbFreeCheck::ImageDownload);
    const bool stored = memory->TryWriteBacking(std::bit_cast<u8*>(address), host.mapped, size);
    image.mip_hashes_current = false;
    // Guest memory changed under the copies made of it (also by a partial store).
    if (!WriteTracking()) {
        buffer_cache.InvalidateMemory(address, size, true);
    }
    // The other images over the range read guest memory again at their next use.
    for (const ImageId other_id : others) {
        Image& other = slot_images[other_id];
        RefreshTrace::Mark(other, "CPU dirty (tiled image stored)", address, size);
        other.flags |= ImageFlagBits::CpuDirty;
        UntrackImage(other_id);
    }
    return stored;
}

void TextureCache::DumpImagesAt(VAddr address, const char* dir) {
    boost::container::small_vector<ImageId, 4> ids;
    {
        std::scoped_lock lock{mutex};
        ForEachImageInRegion(address, 1, [&](ImageId image_id, Image& image) {
            if (image.info.guest_address == address) {
                ids.push_back(image_id);
            }
        });
    }
    std::printf("Image dump %#llx: %zu images\n", static_cast<unsigned long long>(address),
                ids.size());
    u32 n = 0;
    for (const ImageId image_id : ids) {
        Image& image = slot_images[image_id];
        const auto format = vk::to_string(image.info.pixel_format);
        const u32 width = image.info.size.width, height = image.info.size.height;
        std::printf("  image %u: %s %ux%u tile %u, flags %#x, layers %u, mips %u, %s\n",
                    image_id.index, format.c_str(), width, height,
                    static_cast<u32>(image.info.tile_mode), static_cast<u32>(image.flags),
                    image.info.resources.layers, image.info.resources.levels,
                    image.info.props.is_depth ? "depth" : "color");
        if (image.info.props.is_block) {
            continue;
        }
        const u32 bytes_per_pixel = image.info.props.is_depth ? 4 : image.info.num_bits / 8;
        const u64 size = u64(width) * height * bytes_per_pixel;
        const auto download = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16);
        const vk::BufferImageCopy copy = {
            .bufferOffset = download.offset,
            .imageSubresource = {image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                           : vk::ImageAspectFlagBits::eColor,
                                 0, 0, 1},
            .imageExtent = {width, height, 1},
        };
        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
        scheduler.Finish();
        download.Invalidate();
        const std::string path = std::format("{}/img_{:x}_{}_{}x{}_{}.raw", dir, address, n++, width,
                                             height, format);
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(download.mapped, 1, size, f);
            std::fclose(f);
        }
    }
}

/// bbport: a hash of guest memory for the refresh checks. From 1 MiB on, 256 KiB chunks are
/// hashed on the copy threads and then their hashes: a 1080p R32F image the game refreshes every
/// frame (its own compute copy of the depth buffer) cost ~1 ms of hashing on the draw recording
/// thread. Only compared with hashes made here. BB_PARALLEL_IMAGE_HASH=0: one XXH3 as before.
static u64 GuestMemoryHash(const void* data, u64 size) {
    static const bool parallel = [] {
        const char* env = std::getenv("BB_PARALLEL_IMAGE_HASH");
        return !(env && env[0] == '0');
    }();
    constexpr u64 Chunk = 256 * 1024;
    if (!parallel || size < 4 * Chunk || !BbCopy::Enabled()) {
        return XXH3_64bits(data, size);
    }
    const auto* bytes = static_cast<const u8*>(data);
    const u64 count = (size + Chunk - 1) / Chunk;
    boost::container::small_vector<u64, 128> hashes(count);
    BbCopy::ParallelFor(count, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        hashes[i] = XXH3_64bits(bytes + offset, std::min(Chunk, size - offset));
    });
    return XXH3_64bits(hashes.data(), count * sizeof(u64));
}

/// bbport: whether a GPU-dirty refresh may keep the mip hashes (RefreshImage).
static bool KeepMipHashes() {
    static const bool keep = [] {
        const char* env = std::getenv("BB_KEEP_MIP_HASHES");
        return !(env && env[0] == '0');
    }();
    return keep;
}

/// bbport: the guest memory a MaybeCpuDirty check compares, the same at marking and at
/// refresh (they hashed different ranges, the whole image and its first 8x8 pixels, so the
/// first check never matched). Such an image lies within the faulting page: cheap to hash
/// whole, and a CPU write past its first pixels still counts.
u64 TextureCache::MaybeDirtyHash(const Image& image) {
    return GuestMemoryHash(std::bit_cast<const u8*>(image.info.guest_address), image.info.guest_size);
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash. bbport: from a copy read by CopySparseMemory (through the backing view
        // with BB_READBACKS=2), the same bytes MaybeDirtyHash reads. This runs in the fault
        // handlers with `mutex` held: a read fault on a GPU-written page of the image sent a
        // guest thread to wait for the GPU command thread, which drains the draw recording
        // thread first, which waited for `mutex`.
        std::vector<u8> bytes(image.info.guest_size);
        Core::Memory::Instance()->CopySparseMemory(image.info.guest_address, bytes.data(),
                                                   bytes.size());
        image.hash = GuestMemoryHash(bytes.data(), bytes.size());
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        image.mip_hashes_current = false;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            RefreshTrace::Mark(image, "CPU dirty", addr, size);
            image.flags |= ImageFlagBits::CpuDirty;
            PageManager::NoteImageFault(image.info.guest_address, image.info.guest_size,
                                        image.info.size.width, image.info.size.height,
                                        u32(image.info.pixel_format), u32(image.info.tile_mode));
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

namespace {
struct UiEntry {
    u32 width, height, size;
};

struct UiPack {
    std::filesystem::path dir;
    std::unordered_map<u64, UiEntry> keys;
    std::unordered_map<u64, std::vector<u32>> sizes;
};

const UiPack& Pack() {
    static const UiPack pack = [] {
        UiPack p;
        const char* dir = std::getenv("BB_UI_4K");
        if (!dir || !*dir) return p;
        std::error_code error;
        p.dir = dir;
        for (const auto& entry : std::filesystem::directory_iterator(p.dir, error)) {
            const std::string name = entry.path().stem().string();
            unsigned w = 0, h = 0, size = 0;
            unsigned long long hash = 0;
            if (entry.path().extension() != ".bbui" ||
                std::sscanf(name.c_str(), "%ux%u_%u_%llx", &w, &h, &size, &hash) != 4 || !size) {
                continue;
            }
            p.keys[hash] = {w, h, size};
            auto& known = p.sizes[u64(w) << 32 | h];
            if (std::find(known.begin(), known.end(), size) == known.end()) known.push_back(size);
        }
        std::printf("UI textures: %zu 4x replacements found in %s\n", p.keys.size(), dir);
        return p;
    }();
    return pack;
}

constexpr u32 kUiLargestSide = 4096;
constexpr u64 kUiBudget = 1024ull << 20;

vk::Format UiFormat(vk::Format original) {
    switch (original) {
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc7SrgbBlock:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    default:
        return vk::Format::eR8G8B8A8Unorm;
    }
}

bool ReadUiFile(const std::filesystem::path& path, std::vector<u8>& pixels, int& w, int& h) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<u8> file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (file.size() < 12 || std::memcmp(file.data(), "BBUI", 4) != 0) {
        return false;
    }
    u32 fw = 0, fh = 0;
    std::memcpy(&fw, file.data() + 4, 4);
    std::memcpy(&fh, file.data() + 8, 4);
    if (!fw || !fh || fw > 16384 || fh > 16384) {
        return false;
    }
    pixels.resize(std::size_t(fw) * fh * 4);
    uLongf size = uLongf(pixels.size());
    if (uncompress(pixels.data(), &size, file.data() + 12, uLong(file.size() - 12)) != Z_OK || size != pixels.size()) {
        return false;
    }
    w = int(fw);
    h = int(fh);
    return true;
}

// A 2x2 box reduction. Colour is weighted by alpha, so transparent texels (black in most sheets)
// don't darken the edges of what is drawn, and sRGB data is averaged as light, not as its encoding.
void Halve(std::vector<u8>& pixels, int& w, int& h, bool srgb) {
    static const auto decode = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = float(i) / 255.0f;
            t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    static const auto encode = [] {
        std::array<u8, 4096> t{};
        for (int i = 0; i < 4096; ++i) {
            const float c = float(i) / 4095.0f;
            const float e = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            t[i] = u8(std::clamp(e * 255.0f + 0.5f, 0.0f, 255.0f));
        }
        return t;
    }();
    const auto to_byte = [srgb](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        return srgb ? encode[int(c * 4095.0f + 0.5f)] : u8(c * 255.0f + 0.5f);
    };
    const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    std::vector<u8> out(std::size_t(nw) * nh * 4);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            float weighted[3] = {}, plain[3] = {}, alpha = 0.0f;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const u8* p = &pixels[(std::size_t(std::min(h - 1, y * 2 + dy)) * w + std::min(w - 1, x * 2 + dx)) * 4];
                    const float a = float(p[3]) / 255.0f;
                    for (int c = 0; c < 3; ++c) {
                        const float v = srgb ? decode[p[c]] : float(p[c]) / 255.0f;
                        weighted[c] += v * a;
                        plain[c] += v;
                    }
                    alpha += a;
                }
            u8* o = &out[(std::size_t(y) * nw + x) * 4];
            for (int c = 0; c < 3; ++c) {
                o[c] = to_byte(alpha > 0.0f ? weighted[c] / alpha : plain[c] / 4.0f);
            }
            o[3] = u8(alpha / 4.0f * 255.0f + 0.5f);
        }
    pixels.swap(out);
    w = nw;
    h = nh;
}
}

bool TextureCache::UiReplacementsLoaded() {
    return !Pack().keys.empty();
}

void TextureCache::NoteUiTexture(Image& image) {
    image.ui_key = 0;
    const UiPack& pack = Pack();
    const ImageInfo& info = image.info;
    if (pack.keys.empty() || info.props.is_depth || info.resources.layers != 1 || info.size.depth != 1 ||
        info.size.width < 16 || info.size.height < 16 || !info.guest_address || !info.guest_size) {
        return;
    }
    const bool icon = info.guest_size == KeyPrompts::kIconBytes && info.size.width == 32;
    const bool hud = info.guest_size == KeyPrompts::kHudBytes && info.size.width == 1024;
    if (icon || hud) {
        return;
    }
    const auto sizes = pack.sizes.find(u64(info.size.width) << 32 | info.size.height);
    if (sizes == pack.sizes.end()) {
        return;
    }
    const u8* guest = std::bit_cast<const u8*>(info.guest_address);
    for (const u32 size : sizes->second) {
        if (size > info.guest_size) {
            continue;
        }
        const u64 hash = XXH3_64bits(guest, size);
        const auto it = pack.keys.find(hash);
        if (it != pack.keys.end() && it->second.width == info.size.width && it->second.height == info.size.height &&
            it->second.size == size) {
            image.ui_key = hash;
            return;
        }
    }
}

Image* TextureCache::UiReplacement(const Image& image) {
    if (!image.ui_key) {
        return nullptr;
    }
    if (const auto it = ui_images.find(image.ui_key); it != ui_images.end()) {
        return it->second.get();
    }
    if (ui_missing.count(image.ui_key) || ui_bytes >= kUiBudget) {
        return nullptr;
    }
    const UiPack& pack = Pack();
    const auto entry = pack.keys.find(image.ui_key);
    if (entry == pack.keys.end()) {
        return nullptr;
    }
    char name[96];
    std::snprintf(name, sizeof name, "%ux%u_%u_%016llx.bbui", entry->second.width, entry->second.height, entry->second.size,
                  static_cast<unsigned long long>(image.ui_key));
    int w = 0, h = 0;
    std::vector<u8> pixels;
    if (!ReadUiFile(pack.dir / name, pixels, w, h)) {
        ui_missing.insert(image.ui_key);
        return nullptr;
    }
    const vk::Format format = UiFormat(image.info.pixel_format);
    // A BC1 the game reads as RGB has no alpha: its three-colour blocks' fourth texel is opaque
    // black there, where the replacement (made from the RGBA reading) holds alpha 0.
    if (image.info.pixel_format == vk::Format::eBc1RgbUnormBlock ||
        image.info.pixel_format == vk::Format::eBc1RgbSrgbBlock) {
        for (std::size_t i = 3; i < pixels.size(); i += 4) {
            pixels[i] = 255;
        }
    }
    while (u32(w) > kUiLargestSide || u32(h) > kUiLargestSide) {
        Halve(pixels, w, h, format == vk::Format::eR8G8B8A8Srgb);
    }
    Image* out = UploadUi(image.ui_key, pixels, w, h, format);
    static int told = 0;
    if (told++ < 8) {
        std::printf("UI textures: %ux%u drawn from its %dx%d replacement (%llu MiB in use)\n", image.info.size.width,
                    image.info.size.height, w, h, static_cast<unsigned long long>(ui_bytes >> 20));
    }
    return out;
}

// Scales the rectangle of `src` (`src_w` wide) up `s` times into the same rectangle of `dst`
// (`dst_w` wide): bilinear, weighted by alpha, within the rectangle.
static void UpscaleRect(const std::vector<u8>& src, int src_w, int rx, int ry, int rw, int rh, std::vector<u8>& dst,
                 int dst_w, int s) {
    for (int y = ry * s; y < (ry + rh) * s; ++y) {
        for (int x = rx * s; x < (rx + rw) * s; ++x) {
            const float u = std::clamp((float(x) + 0.5f) / float(s) - 0.5f, float(rx), float(rx + rw - 1));
            const float v = std::clamp((float(y) + 0.5f) / float(s) - 0.5f, float(ry), float(ry + rh - 1));
            const int x0 = int(u), y0 = int(v);
            const int x1 = std::min(x0 + 1, rx + rw - 1), y1 = std::min(y0 + 1, ry + rh - 1);
            const float fx = u - float(x0), fy = v - float(y0);
            const int tx[4] = {x0, x1, x0, x1}, ty[4] = {y0, y0, y1, y1};
            const float weight[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
            float acc[4] = {};
            for (int k = 0; k < 4; ++k) {
                const u8* t = &src[(std::size_t(ty[k]) * src_w + tx[k]) * 4];
                const float a = weight[k] * float(t[3]);
                for (int c = 0; c < 3; ++c) {
                    acc[c] += a * float(t[c]);
                }
                acc[3] += a;
            }
            u8* out = &dst[(std::size_t(y) * dst_w + x) * 4];
            for (int c = 0; c < 3; ++c) {
                out[c] = acc[3] > 0.0f ? u8(std::clamp(acc[c] / acc[3] + 0.5f, 0.0f, 255.0f)) : 0;
            }
            out[3] = u8(std::clamp(acc[3] + 0.5f, 0.0f, 255.0f));
        }
    }
}

Image* TextureCache::UploadUi(u64 key, std::vector<u8>& pixels, int w, int h, vk::Format format) {
    const bool srgb = format == vk::Format::eR8G8B8A8Srgb;
    // The full mip chain: a menu draws its art at a quarter of the replacement's size (a 4x copy at
    // the game's own size), and a single level sampled that small skips three texels in four.
    ImageInfo info{};
    info.pixel_format = format;
    info.type = AmdGpu::ImageType::Color2D;
    info.size = {u32(w), u32(h), 1};
    info.resources.layers = 1;
    info.num_bits = 32;
    info.pitch = u32(w);
    u64 bytes = 0;
    for (u32 level = 0, lw = u32(w), lh = u32(h); level < info.mips_layout.size(); ++level) {
        info.mips_layout[level] = {lw * lh * 4, lw, lh, u32(bytes)};
        bytes += u64(lw) * lh * 4;
        info.resources.levels = level + 1;
        if (lw == 1 && lh == 1) {
            break;
        }
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
    }
    auto replacement = std::make_unique<Image>(instance, runtime, slot_image_views, info);
    scheduler.EndRendering();
    // A level at a time: a 4096x4096 chain (85 MB) at once is most of the stream buffer.
    auto& stream = buffer_cache.GetStreamBuffer();
    for (u32 level = 0; level < info.resources.levels; ++level) {
        if (level) {
            Halve(pixels, w, h, srgb);
        }
        const vk::BufferImageCopy copy{
            .bufferOffset = stream.Copy(pixels.data(), pixels.size(), 256),
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, level, 0, 1},
            .imageExtent = {u32(w), u32(h), 1},
        };
        runtime.UploadImage(replacement.get(), &stream, {&copy, 1});
    }
    ui_bytes += bytes;
    Image* out = replacement.get();
    ui_images.emplace(key, std::move(replacement));
    return out;
}

// The HUD's sheet is never replaced (key prompts are drawn into it), and the gaps between its
// sprites carry colour: the HUD bars' glows blend theirs in, and filtering read it past their edges
// as a faint box. While 4K menus are loaded it is drawn from a cleaned copy (HudClean), with mips,
// filtered like a replacement: the 4K art for the sheet, cleaned by bb-ui-4k pack (HudArt), when
// the drawn sheet is the one the art was made from, with the key prompts that aren't the game's
// own scaled up into it; else the sheet itself, cleaned here. A copy per drawing of the sheet (the
// key prompts' choices); the oldest beyond kHudCopies is freed once the GPU is past the draws
// recorded so far.
void TextureCache::CleanHud(Image& image, const std::vector<u8>& sheet) {
    // KeyPromptData leaves the bytes of the game's button textures; only the HUD's are this size.
    if (sheet.size() != KeyPrompts::kHudBytes || !UiReplacementsLoaded()) {
        return;
    }
    const u8* data = sheet.data();
    const u64 key = XXH3_64bits(data, sheet.size()) ^ 0x6875642d636c65ull; // "hud-cle"
    image.ui_key = key;
    if (ui_images.count(key)) {
        return;
    }
    const int sw = int(KeyPrompts::kHudWidth), sh = int(KeyPrompts::kHudHeight);
    std::vector<u8> small(std::size_t(sw) * sh * 4);
    KeyPrompts::DecodeHud(data, small.data());
    std::array<u64, 3> prompts{};
    const bool same_sheet = KeyPrompts::HudHashes(data, prompts) == HudClean::kSheetMasked;
    const bool art = same_sheet && HudArt();
    static bool told = false;
    if (!same_sheet && !told && HudArt()) {
        told = true;
        std::printf("UI textures: the HUD's sheet isn't the one its 4K art was made from; drawn from itself\n");
    }
    std::vector<u8> pixels;
    int w = sw, h = sh, scaled = 0;
    if (art) {
        pixels = hud_art.pixels;
        w = hud_art.w;
        h = hud_art.h;
        const int s = w / sw;
        const auto rects = KeyPrompts::HudPrompts();
        for (std::size_t i = 0; i < rects.size(); ++i) {
            if (prompts[i] == HudClean::kSheetPrompts[i]) {
                continue;
            }
            const auto& r = rects[i];
            UpscaleRect(small, sw, r.x, r.y, r.w, r.h, pixels, w, s);
            // The gaps around the prompt took the colour of the game's prompt (bb-ui-4k pack):
            // filled again, a few texels around it too.
            constexpr int kMargin = 4;
            const int x0 = std::max(0, (r.x - kMargin) * s), y0 = std::max(0, (r.y - kMargin) * s);
            const int x1 = std::min(w, (r.x + r.w + kMargin) * s), y1 = std::min(h, (r.y + r.h + kMargin) * s);
            HudClean::Fill(pixels, w, x0, y0, x1 - x0, y1 - y0);
            ++scaled;
        }
    } else {
        pixels = std::move(small);
        HudClean::Clean(pixels, w, h, 1, false);
    }
    const u64 before = ui_bytes;
    UploadUi(key, pixels, w, h, UiFormat(image.info.pixel_format));
    hud_copies.emplace_back(key, ui_bytes - before);
    constexpr std::size_t kHudCopies = 3;
    while (hud_copies.size() > kHudCopies) {
        const auto [old, bytes] = hud_copies.front();
        hud_copies.pop_front();
        const auto it = ui_images.find(old);
        if (it == ui_images.end()) {
            continue;
        }
        scheduler.DeferOperation([this, copy = std::move(it->second)]() mutable {
            for (auto& backing : copy->backing_images) {
                for (const ImageViewId view : backing.image_view_ids) {
                    slot_image_views.erase(view);
                }
            }
            copy.reset();
        });
        ui_images.erase(it);
        ui_bytes -= bytes;
    }
    if (art) {
        std::printf("UI textures: the HUD's sheet drawn from its 4K art (%d key prompts scaled up into it)\n", scaled);
    } else {
        std::printf("UI textures: the HUD's sheet drawn from a cleaned copy\n");
    }
}

bool TextureCache::HudArt() {
    if (!hud_art.tried) {
        hud_art.tried = true;
        const UiPack& pack = Pack();
        int w = 0, h = 0;
        const int sw = int(KeyPrompts::kHudWidth), sh = int(KeyPrompts::kHudHeight);
        if (!pack.keys.empty() && ReadUiFile(pack.dir / HudClean::kArtFile, hud_art.pixels, w, h) && w % sw == 0 &&
            h == sh * (w / sw) && u32(w) <= kUiLargestSide) {
            hud_art.w = w;
            hud_art.h = h;
        } else {
            hud_art.pixels = {};
            if (pack.keys.count(HudClean::kArtKey)) {
                std::printf("UI textures: the 4K menus have no cleaned HUD art (%s); rebuild them for a 4K HUD\n",
                            HudClean::kArtFile);
            }
        }
    }
    return !hud_art.pixels.empty();
}

bool TextureCache::KeyPromptData(const Image& image, std::vector<u8>& data) {
    const ImageInfo& info = image.info;
    const bool icon = info.guest_size == KeyPrompts::kIconBytes && info.size.width == 32;
    const bool hud = info.guest_size == KeyPrompts::kHudBytes && info.size.width == 1024;
    if ((!icon && !hud) || info.resources.levels != 1 || info.resources.layers != 1 ||
        (info.pixel_format != vk::Format::eBc7UnormBlock &&
         info.pixel_format != vk::Format::eBc7SrgbBlock)) {
        return false;
    }
    const u8* guest = std::bit_cast<const u8*>(info.guest_address);
    const int texture = KeyPrompts::Identify(guest, info.guest_size, info.size.width, info.size.height);
    if (texture < 0) {
        return false;
    }
    {
        std::scoped_lock lock{key_icons_mutex};
        const std::pair<VAddr, u32> where{info.guest_address, info.guest_size};
        if (std::find(key_icons.begin(), key_icons.end(), where) == key_icons.end()) {
            // The game loads its icons once; a few reloads elsewhere at most.
            if (key_icons.size() >= 256) {
                key_icons.erase(key_icons.begin());
            }
            key_icons.push_back(where);
        }
    }
    data.assign(guest, guest + info.guest_size);
    return KeyPrompts::Draw(texture, data.data(), data.size());
}

void TextureCache::UpdateKeyPrompts() {
    if (!KeyPrompts::Changed()) {
        return;
    }
    std::vector<std::pair<VAddr, u32>> icons;
    {
        std::scoped_lock lock{key_icons_mutex};
        icons = key_icons;
    }
    // As a CPU write to them would: uploaded again where next drawn.
    for (const auto& [addr, size] : icons) {
        InvalidateMemory(addr, size);
    }
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        RefreshTrace::Mark(image, "GPU dirty", address, max_size);
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id, bool exact_fmt) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = !requested_info.ViewRejection(cache_image.info).empty() ||
                    (exact_fmt && requested_info.pixel_format != cache_image.info.pixel_format);

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        // Use a real covering allocation's layout. Combining independently larger mip/layer
        // counts or recomputing attachment padding can invent guest memory/shift later mips.
        // The existing color-channel -> MS depth conversion has different samples/bpp but the
        // same byte footprint; normalize only these two fields for the geometry check.
        auto source_layout = cache_image.info;
        source_layout.num_bits = requested_info.num_bits;
        source_layout.num_samples = requested_info.num_samples;
        const bool request_covers = source_layout.ViewRejection(requested_info).empty();
        const bool cache_covers = requested_info.ViewRejection(source_layout).empty();
        if (!request_covers && !cache_covers) {
            TraceOverlap(requested_info, binding, cache_image_id, &cache_image, "reject",
                         "depth conversion has incompatible guest subresource layout");
            return {};
        }
        auto new_info = request_covers ? requested_info : source_layout;
        new_info.pixel_format = requested_info.pixel_format;
        new_info.props.is_depth = requested_info.props.is_depth;
        new_info.props.has_stencil = requested_info.props.has_stencil;
        new_info.meta_info = requested_info.meta_info;
        new_info.stencil_addr = requested_info.stencil_addr;
        new_info.stencil_size = requested_info.stencil_size;
        int mip{}, slice{};
        if (!ReuseRejection(requested_info, new_info, binding, exact_fmt, mip, slice).empty()) {
            return {};
        }
        const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // SlotVector insertion may relocate images. Reacquire the source after allocation.
        auto& source_image = slot_images[cache_image_id];
        auto& new_image = slot_images[new_image_id];
        new_image.usage = source_image.usage;
        RefreshImage(new_image);
        RefreshImage(source_image);
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        runtime.CopyColorAndDepth(&source_image, &new_image);

        // Keep bound attachments valid until the renderer notices the replacement.
        if (source_image.binding.is_bound || source_image.binding.is_target) {
            source_image.binding.needs_rebind = 1u;
        }
        new_image.binding.is_target = source_image.binding.is_target;
        FreeImage(cache_image_id);
        TrackImage(new_image_id);
        return new_image_id;
    }

    int mip{}, slice{};
    return ReuseRejection(requested_info, cache_image.info, binding, exact_fmt, mip, slice).empty()
               ? cache_image_id : ImageId{};
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                         BindingType binding,
                                                         ImageId cache_image_id,
                                                         ImageId merged_image_id,
                                                         bool exact_fmt) {
    auto& cached = slot_images[cache_image_id];
    if (cache_image_id == merged_image_id || False(cached.flags & ImageFlagBits::Registered) ||
        cached.info.pixel_format == vk::Format::eUndefined) {
        // Undefined images are stencil/depth association records, not reusable Vulkan images.
        return {{}, -1, -1};
    }
    const auto reject = [&](std::string_view reason) -> std::tuple<ImageId, int, int> {
        TraceOverlap(image_info, binding, cache_image_id, &cached, "reject", reason);
        // Retain the old cheap retirement of cold aliases, but leave bound/GPU-modified
        // images to normal GC (which can write them back). Rejection must not lose GPU data.
        if (!cached.binding.is_bound && !cached.binding.is_target &&
            False(cached.flags & ImageFlagBits::GpuModified) &&
            scheduler.CurrentTick() - cached.tick_accessed_last > NumFramesBeforeRemoval) {
            FreeImage(cache_image_id);
        }
        return {{}, -1, -1};
    };

    // Once a covering image has been selected, only merge its actual child subresources.
    // Do not replace it with a later, smaller alias and lose the previously selected view.
    if (!merged_image_id && image_info.guest_address == cached.info.guest_address) {
        if (image_info.BlockDim() != cached.info.BlockDim() ||
            image_info.num_bits * image_info.num_samples !=
                cached.info.num_bits * cached.info.num_samples) {
            return reject("different block dimensions or sample footprint");
        }
        if (image_info.array_mode != cached.info.array_mode ||
            image_info.tile_mode != cached.info.tile_mode ||
            image_info.alt_tile != cached.info.alt_tile) {
            return reject("different guest tiling layout");
        }
        if (image_info.type != cached.info.type) {
            const bool volume_copy =
                (image_info.type == AmdGpu::ImageType::Color3D &&
                 cached.info.type == AmdGpu::ImageType::Color2D &&
                 image_info.size.depth == cached.info.resources.layers) ||
                (cached.info.type == AmdGpu::ImageType::Color3D &&
                 image_info.type == AmdGpu::ImageType::Color2D &&
                 cached.info.size.depth == image_info.resources.layers);
            if (volume_copy && image_info.guest_size == cached.info.guest_size &&
                image_info.resources.levels == cached.info.resources.levels &&
                image_info.IsCompatible(cached.info)) {
                TraceOverlap(image_info, binding, cache_image_id, &cached, "expand",
                             "2D/3D backing conversion");
                return {ExpandImage(image_info, cache_image_id), -1, -1};
            }
            return reject("different image type");
        }
        const bool cached_covers = cached.info.resources.Contains(image_info.resources);
        const bool request_covers = image_info.resources.Contains(cached.info.resources);
        if (!cached_covers && !request_covers) {
            // Taking a component-wise maximum here could invent guest memory beyond BOTH
            // allocations (e.g. more mips versus more layers). Preserve this legitimate alias.
            return reject("crossed mip/layer extents; neither image contains the other");
        }
        if (const auto depth = ResolveDepthOverlap(image_info, binding, cache_image_id, exact_fmt)) {
            TraceOverlap(image_info, binding, depth, &slot_images[depth],
                         depth == cache_image_id ? "view" : "expand", "depth/color binding");
            return {depth, -1, -1};
        }

        int mip{}, slice{};
        const auto reason = ReuseRejection(image_info, cached.info, binding, exact_fmt, mip, slice);
        if (reason.empty()) {
            TraceOverlap(image_info, binding, cache_image_id, &cached, "view", "covering backing");
            return {cache_image_id, mip, slice};
        }
        if (request_covers && image_info.IsCompatible(cached.info) &&
            cached.info.ViewRejection(image_info).empty() &&
            // Changing the number of layers relocates later guest mips. Only a one-mip
            // source, or an unchanged layer stride, can be preserved by CopyImage.
            (cached.info.resources.levels == 1 ||
             image_info.resources.layers == cached.info.resources.layers)) {
            TraceOverlap(image_info, binding, cache_image_id, &cached, "expand", reason);
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }
        return reject(reason);
    }

    if (!merged_image_id && image_info.guest_address > cached.info.guest_address) {
        int mip{}, slice{};
        const auto reason = ReuseRejection(image_info, cached.info, binding, exact_fmt, mip, slice);
        if (!reason.empty()) {
            return reject(reason);
        }
        TraceOverlap(image_info, binding, cache_image_id, &cached, "view",
                     "contained subresource", mip, slice);
        return {cache_image_id, mip, slice};
    }

    // Preserve a cached mip/array slice, including a currently bound render target, before
    // retiring it. Previously the target branch freed it before any larger image/copy existed.
    const int mip = cached.info.MipOf(image_info);
    const int slice = mip >= 0 ? cached.info.SliceOf(image_info, mip) : -1;
    if (mip < 0 || slice < 0 ||
        !cached.info.ViewRejection(image_info, {u32(mip), u32(slice)}).empty() ||
        !IsVulkanFormatCompatible(image_info.pixel_format, cached.info.pixel_format)) {
        return reject("cached image is not a covered child subresource");
    }
    if (!merged_image_id) {
        merged_image_id = slot_images.insert(instance, runtime, slot_image_views, image_info);
        RegisterImage(merged_image_id);
        RefreshImage(slot_images[merged_image_id]);
    }
    // Inserting the parent may relocate SlotVector storage.
    auto& source = slot_images[cache_image_id];
    auto& merged = slot_images[merged_image_id];
    RefreshImage(source);
    runtime.CopyMip(&source, &merged, mip, slice);
    if (source.binding.is_bound || source.binding.is_target) {
        source.binding.needs_rebind = 1u;
    }
    merged.binding.is_target |= source.binding.is_target;
    TraceOverlap(image_info, binding, cache_image_id, &source, "merge",
                 "copy child before retiring backing", mip, slice);
    FreeImage(cache_image_id);
    TrackImage(merged_image_id);
    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_info = info;
    const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, new_info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    RefreshImage(src_image);
    runtime.CopyImage(&src_image, &new_image);
    new_image.binding.is_target = src_image.binding.is_target;

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);
    TrackImage(new_image_id);
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    const bool owned = BbEngine::GxObjects::TexturesOwned();
    if (owned && desc.type <= BindingType::DepthTarget && info.num_samples == 1 &&
        info.pixel_format != vk::Format::eUndefined) {
        BbEngine::GxObjects::TextureExtent whole;
        const bool known = BbEngine::GxObjects::TextureAt(info.guest_address, &whole);
        if (known && (whole.width != info.size.width || whole.height != info.size.height)) {
            BbEngine::GxObjects::NoteWiden(BbEngine::GxObjects::WidenOtherSize);
        } else if (known &&
            whole.levels <= info.mips_layout.size() && whole.levels >= info.resources.levels &&
            whole.layers >= info.resources.layers &&
            (whole.levels > info.resources.levels || whole.layers > info.resources.layers)) {
            ImageInfo wide = info;
            wide.resources.levels = whole.levels;
            wide.resources.layers = whole.layers;
            wide.UpdateSize();
            const u32 before = info.mips_layout[0].size / std::max<u32>(info.resources.layers, 1);
            const u32 after = wide.mips_layout[0].size / std::max<u32>(wide.resources.layers, 1);
            if (before != after || wide.mips_layout[0].pitch != info.mips_layout[0].pitch) {
                BbEngine::GxObjects::NoteWiden(BbEngine::GxObjects::WidenOtherLayout);
            } else if (wide.guest_size > whole.room) {
                BbEngine::GxObjects::NoteWiden(BbEngine::GxObjects::WidenTooLarge);
            } else {
                desc.info = wide;
                BbEngine::GxObjects::NoteWiden(BbEngine::GxObjects::Widened);
            }
        }
    }

    std::scoped_lock lock{mutex};

    u64 key_hash = info.guest_address ^ info.guest_size << 17 ^ u64(info.pixel_format) << 40 ^
                   u64(info.size.width) << 24 ^ info.size.height ^ u64(desc.type) << 58 ^
                   u64(exact_fmt) << 63;
    key_hash = (key_hash ^ key_hash >> 33) * 0xFF51AFD7ED558CCDull;
    key_hash = (key_hash ^ key_hash >> 33) * 0xC4CEB9FE1A85EC53ull;
    key_hash ^= key_hash >> 33;
    auto& cached = find_image_cache[key_hash % find_image_cache.size()];
    // An exact match stays valid while that image is registered with the same description;
    // resolved overlaps (views into other images) only until any image registration changes.
    const auto still_exact = [&] {
        if (cached.view_mip >= 0 || cached.view_slice >= 0 || !cached.image_id ||
            !slot_images.is_allocated(cached.image_id)) {
            return false;
        }
        const Image& image = slot_images[cached.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format && image.info.type == info.type &&
               image.info.resources.Contains(info.resources) &&
               image.info.LayoutKey() == info.LayoutKey();
    };
    if ((cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()) &&
        cached.address == info.guest_address &&
        cached.size == info.guest_size && cached.extent == info.size &&
        cached.format == info.pixel_format && cached.type == info.type &&
        cached.exact_fmt == exact_fmt && cached.binding == desc.type &&
        cached.levels == info.resources.levels && cached.layers == info.resources.layers &&
        cached.layout_key == info.LayoutKey() &&
        !BbToggle::Disabled(BbToggle::FindImageCache)) {
        Image& image = slot_images[cached.image_id];
        image.tick_accessed_last = scheduler.CurrentTick();
        TouchImage(image);
        if (cached.view_mip > 0) {
            desc.view_info.range.base.level += cached.view_mip;
        }
        if (cached.view_slice > 0) {
            desc.view_info.range.base.layer += cached.view_slice;
        }
        if (BbEngine::GxObjects::On()) {
            BbEngine::GxObjects::NoteImage(info.guest_address, info.guest_size, ImageUid(cached.image_id), info.size.width, info.size.height,
                                           static_cast<u32>(info.pixel_format));
        }
        return cached.image_id;
    }

    ImageId image_id{};

    int view_mip{-1};
    int view_slice{-1};
    bool held_served = false;
    const bool at_object = owned && [&] {
        using namespace BbEngine::GxObjects;
        const HeldImage held = HeldAt(info.guest_address);
        if (!held.object) {
            NoteObjectPath(PathNoObject);
            return false;
        }
        if (!held.uid) {
            NoteObjectPath(PathNothingHeld);
            return true;
        }
        const ImageId held_id{held.index};
        if (!IsRegisteredImage(held_id, held.uid)) {
            NoteObjectPath(PathHeldGone);
            return true;
        }
        const Image& candidate = slot_images[held_id];
        int mip{}, slice{};
        std::string_view reason = ReuseRejection(info, candidate.info, desc.type, exact_fmt, mip, slice);
        if (reason.empty() && info.props.is_depth && !candidate.info.props.is_depth) {
            reason = "depth requested of a color image";
        }
        if (!reason.empty()) {
            NoteObjectPath(PathHeldRefused, reason.data(), reason.size());
            return true;
        }
        NoteObjectPath(PathServed);
        image_id = held_id;
        held_served = true;
        return true;
    }();
    ImageIds image_ids;
    if (!held_served || BbEngine::GxObjects::TexturesChecked()) {
        ForEachImageInRegion(info.guest_address, info.guest_size,
                             [&](ImageId id, Image&) { image_ids.push_back(id); });
    }
    if (at_object && !held_served) {
        image_ids.erase(std::remove_if(image_ids.begin(), image_ids.end(), [&](ImageId id) {
            const ImageInfo& other = slot_images[id].info;
            return other.guest_address >= info.guest_address + info.guest_size ||
                   info.guest_address >= other.guest_address + other.guest_size;
        }), image_ids.end());
        BbEngine::GxObjects::NoteObjectPath(image_ids.empty() ? BbEngine::GxObjects::PathMadeDirectly
                                                              : BbEngine::GxObjects::PathOverlapped);
    }
    if (held_served && BbEngine::GxObjects::TexturesChecked()) {
        using namespace BbEngine::GxObjects;
        ImageId exact{};
        u32 others = 0;
        for (const auto id : image_ids) {
            const Image& candidate = slot_images[id];
            if (id != image_id && candidate.info.pixel_format != vk::Format::eUndefined) {
                ++others;
            }
            int mip{}, slice{};
            if (!exact && candidate.info.pixel_format != vk::Format::eUndefined &&
                candidate.info.guest_address == info.guest_address &&
                candidate.info.guest_size == info.guest_size && candidate.info.size == info.size &&
                candidate.info.resources == info.resources &&
                ReuseRejection(info, candidate.info, desc.type, exact_fmt, mip, slice).empty()) {
                exact = id;
            }
        }
        NoteHeldCheck(exact == image_id ? CheckSame : !exact ? CheckResolver : CheckOther, others);
        image_ids.clear();
    }
    // Preserve the exact-match fast path, but validate independent mip/layer coverage too.
    // Other covering parents go through resolution so modified child targets can be merged.
    for (const auto cache_id : image_ids) {
        const auto& candidate = slot_images[cache_id];
        if (candidate.info.pixel_format == vk::Format::eUndefined ||
            candidate.info.guest_address != info.guest_address ||
            candidate.info.guest_size != info.guest_size || candidate.info.size != info.size ||
            candidate.info.resources != info.resources) {
            continue;
        }
        int mip{}, slice{};
        const auto reason = ReuseRejection(info, candidate.info, desc.type, exact_fmt, mip, slice);
        if (reason.empty()) {
            image_id = cache_id;
            view_mip = mip ? mip : -1;
            view_slice = slice ? slice : -1;
            TraceOverlap(info, desc.type, cache_id, &candidate,
                         mip || slice ? "view" : "reuse", "covers requested resources", mip, slice);
            break;
        }
        TraceOverlap(info, desc.type, cache_id, &candidate, "reject reuse", reason, mip, slice);
    }

    if (!image_id) {
        for (const auto cache_id : image_ids) {
            const auto& candidate = slot_images[cache_id];
            if (cache_id == image_id || False(candidate.flags & ImageFlagBits::Registered)) {
                continue;
            }
            // Exact-format callers cannot end up with a differently formatted covering view.
            // A conversion/expansion may still create the requested format, so let it proceed.
            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            const auto [resolved, mip, slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id, exact_fmt);
            if (!resolved || resolved == image_id) {
                continue; // Keep the original subresource location on non-matches/child merges.
            }
            int original_mip{}, original_slice{};
            const auto reason = ReuseRejection(info, slot_images[resolved].info, desc.type,
                                               exact_fmt, original_mip, original_slice);
            if (!reason.empty()) {
                TraceOverlap(info, desc.type, resolved, &slot_images[resolved], "reject result", reason);
                continue;
            }
            image_id = resolved;
            view_mip = original_mip ? original_mip : -1;
            view_slice = original_slice ? original_slice : -1;
        }
    }
    // Create and register a new image
    if (!image_id) {
        view_mip = view_slice = -1;
        TraceOverlap(info, desc.type, {}, nullptr, "create", "no candidate covers requested view");
        image_id = slot_images.insert(instance, runtime, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level += view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer += view_slice;
    }

    cached = FindImageCacheEntry{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .layout_key = info.LayoutKey(),
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    if (at_object && !held_served && view_mip <= 0 && view_slice <= 0 &&
        slot_images[image_id].info.guest_address == info.guest_address) {
        BbEngine::GxObjects::HoldImage(info.guest_address, image_id.index, slot_images[image_id].image_uid);
    }
    if (BbEngine::GxObjects::On()) {
        BbEngine::GxObjects::NoteImage(info.guest_address, info.guest_size, ImageUid(image_id), info.size.width, info.size.height,
                                       static_cast<u32>(info.pixel_format));
    }
    return image_id;
}

bool TextureCache::IsExactMatch(ImageId image_id, const ImageInfo& info) const {
    if (!image_id || !slot_images.is_allocated(image_id)) {
        return false;
    }
    const Image& image = slot_images[image_id];
    return True(image.flags & ImageFlagBits::Registered) &&
           image.info.guest_address == info.guest_address &&
           image.info.guest_size == info.guest_size && image.info.size == info.size &&
           image.info.pixel_format == info.pixel_format && image.info.type == info.type &&
           image.info.resources == info.resources && image.info.LayoutKey() == info.LayoutKey();
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
            downloads_queued.store(true, std::memory_order_release);
        }
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (memo && !BbToggle::Disabled(BbToggle::TextureViewMemo)) {
        if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
            return slot_image_views[memo->view_id];
        }
        ImageView& view = image.FindView(desc.view_info);
        const auto& ids = image.backing->image_view_ids;
        const u32 last = image.backing->last_view;
        memo->image_id = image_id;
        memo->backing = image.backing;
        memo->view_id = last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last]
                                                                                    : ids.back();
        return view;
    }
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
        downloads_queued.store(true, std::memory_order_release);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaType::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaType::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaType::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = slot_images.insert(instance, runtime, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        const auto depth_uid = slot_images[image_id].image_uid;
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, depth_uid);
    }

    return slot_images[image_id].FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    BbStats::Timer timer{BbStats::t_refresh};

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));
    const u32 entry_flags = u32(image.flags);

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        const u64 hash = MaybeDirtyHash(image);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            RefreshTrace::Refresh(image, entry_flags, "unchanged (maybe dirty check)");
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);
    // bbport: a GPU-dirty refresh uploads the buffer cache's data, and the guest memory the mip
    // hashes record is unchanged unless something wrote it since they were taken (which clears
    // mip_hashes_current): they are kept instead of hashing it again. The game's compute copy of
    // the depth buffer into a 1080p R32F image made that ~1 ms of every frame on the draw
    // recording thread. BB_KEEP_MIP_HASHES=0: hashed every time.
    constexpr u32 CpuDirtyBits = u32(ImageFlagBits::MaybeCpuDirty) | u32(ImageFlagBits::CpuDirty);
    const bool keep_hashes = is_gpu_dirty && image.mip_hashes_current &&
                             (entry_flags & CpuDirtyBits) == 0 && KeepMipHashes();

    BbStats::image_upload_bytes.fetch_add(image.info.guest_size, std::memory_order_relaxed);
    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        // bbport: every upload records the guest memory it saw, not only uploads of images
        // the GPU had already written. An image the GPU writes after an upload from the buffer
        // cache (GpuDirty: a storage image's first binding) or from a plain texture otherwise
        // had no reference, and the first CPU write anywhere in its page replaced the GPU's
        // contents with stale guest memory (a 1x1 exposure texture computed once: the
        // character creation preview went black after one frame).
        const u8* mip_addr = std::bit_cast<u8*>(image.info.guest_address) + mip_offset;
        u64 mip_hash = image.mip_hashes[m];
        if (!keep_hashes) {
            BB_SECTION(RefreshHash);
            BbStats::Timer hash_timer{BbStats::t_refresh_hash};
            mip_hash = GuestMemoryHash(mip_addr, mip_size);
        }
        if (is_gpu_modified && !is_gpu_dirty && image.mip_hashes[m] == mip_hash) {
            continue;
        }
        image.mip_hashes[m] = mip_hash;

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    // Only a tracked image hears of the writes that would outdate them.
    image.mip_hashes_current =
        image.track_addr == image.guest_begin && image.track_addr_end == image.guest_end;

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        RefreshTrace::Refresh(image, entry_flags, "unchanged guest memory, the GPU's contents kept");
        return;
    }
    RefreshTrace::Refresh(image, entry_flags,
                          !is_gpu_dirty ? "uploaded"
                          : keep_hashes ? "uploaded from the buffer cache, hashes kept"
                                        : "uploaded from the buffer cache");

    scheduler.EndRendering();

    std::optional<BbStats::Timer> part_timer{std::in_place, BbStats::t_refresh_obtain};
    BbSections::Scope obtain_scope{BbSections::RefreshObtain};
    std::pair<const Buffer*, u64> source;
    // bbport: the game's button icons, as the keys they are bound to (engine/key_prompts.h).
    std::vector<u8> keycaps;
    if (KeyPromptData(image, keycaps)) {
        auto& stream = buffer_cache.GetStreamBuffer();
        source = {&stream, stream.Copy(keycaps.data(), keycaps.size(), 256)};
    } else {
        source = buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    }
    const auto [in_buffer, in_offset] = source;
    obtain_scope.End();
    part_timer.emplace(BbStats::t_refresh_detile);
    BbSections::Scope detile_scope{BbSections::RefreshDetile};
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    detile_scope.End();
    part_timer.reset();
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    BB_SECTION(RefreshUpload);
    runtime.UploadImage(&image, buffer, image_copies);
    NoteUiTexture(image);
    CleanHud(image, keycaps);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth, float extra_lod_bias,
                                     size_t* lru_id) {
    // Compare and plain uses of one S# need separate samplers; so do extra LOD biases.
    const u64 hash = HashCombine(HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), is_depth),
                                 u64(std::bit_cast<u32>(extra_lod_bias)));

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base,
                                                        is_depth, extra_lod_bias);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, gc_tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, gc_tick);
    }
    if (lru_id) {
        *lru_id = it->second.lru_id;
    }

    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    BbStats::images_registered.fetch_add(1, std::memory_order_relaxed);
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registry_generation;
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    BbStats::live_image_bytes.fetch_add(Common::AlignUp(image.info.guest_size, 1024),
                                        std::memory_order_relaxed);
    BbStats::live_images.fetch_add(1, std::memory_order_relaxed);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    image.lru_touched_tick = gc_tick;
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_generation;
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    BbStats::live_image_bytes.fetch_sub(Common::AlignUp(image.info.guest_size, 1024),
                                        std::memory_order_relaxed);
    BbStats::live_images.fetch_sub(1, std::memory_order_relaxed);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::WatchPages(VAddr addr, u64 size) {
    watched_pages.Add(addr, size, 1);
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UnwatchPages(VAddr addr, u64 size) {
    tracker.UpdatePageWatchers<false>(addr, size);
    watched_pages.Add(addr, size, -1);
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        WatchPages(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    WatchPages(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    WatchPages(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    image.mip_hashes_current = false; // writes go unseen until it is tracked again
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        UnwatchPages(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    image.mip_hashes_current = false;
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    UnwatchPages(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    image.mip_hashes_current = false;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    UnwatchPages(addr, size);
}

void TextureCache::GarbageCollectImages() {
    const u64 second = u64(std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
        static const bool low_vram_gc = [this] {
            const char* env = std::getenv("BB_LOW_VRAM_GC");
            return instance.IsLowVramAmd() && !(env && env[0] == '0');
        }();
        if (low_vram_gc) {
            total_used_memory -= std::min(total_used_memory, instance.GetVmaFreeInBlocks());
        }
        // bbport: on integrated GPUs (Steam Deck) the usage covers system-memory heaps holding
        // much more than images (buffers backing guest memory), and the startup budget left
        // ~1 GB after its 8 GB system reserve: usage stayed above the critical mark, so the
        // collector evicted images used two or three frames ago on every submission and wrote
        // GPU-written ones back. Compare with the driver's current budget instead.
        // BB_GC_BUDGET_MB=N: this rule with a fixed budget on any GPU (tests on a desktop).
        static const u64 forced_budget = [] {
            const char* env = std::getenv("BB_GC_BUDGET_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
        }();
        if (instance.IsIntegrated() || forced_budget) {
            const u64 budget = forced_budget ? forced_budget : instance.GetDeviceMemoryBudgetNow();
            if (budget != 0) {
                trigger_gc_memory = budget / 10 * 7;
                pressure_gc_memory = budget / 100 * 85;
                critical_gc_memory = budget / 100 * 95;
            }
        } else if (const u64 percent = DiscretePressurePercent();
                   percent != 0 && second != gc_budget_second) {
            // bbport: discrete GPUs (see DiscretePressurePercent); the budget once a second.
            gc_budget_second = second;
            if (const u64 budget = instance.GetDeviceMemoryBudgetNow(); budget != 0) {
                pressure_gc_memory = low_vram_gc
                                         ? budget / 100 * percent
                                         : std::max(static_pressure_gc_memory, budget / 100 * percent);
                critical_gc_memory =
                    low_vram_gc ? budget / 100 * (percent + 10)
                                : std::max({static_critical_gc_memory, budget / 100 * (percent + 10),
                                            pressure_gc_memory});
            }
        }
    }
    BbStats::gc_used_bytes.store(total_used_memory, std::memory_order_relaxed);
    BbStats::gc_trigger_bytes.store(trigger_gc_memory, std::memory_order_relaxed);
    // bbport: gc_tick (one per guest submission, hundreds a second) at each of the last 64
    // seconds, for ages in seconds. The usage compared is all of our VRAM, not only images: on a
    // discrete GPU it stays over the trigger, where an age of 16 ticks (~3 frames) evicted every
    // texture briefly off screen and uploaded it again. Below the pressure mark an image goes
    // once unused for BB_GC_IDLE_SECONDS (20): the textures of an area left behind.
    if (second != gc_second) {
        gc_second = second;
        BbStats::coarse_second.store(u32(second), std::memory_order_relaxed);
        gc_tick_at_second[second % gc_tick_at_second.size()] = gc_tick;
    }
    if (BbToggle::Disabled(BbToggle::TextureCollector)) {
        return;
    }

    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    static const u64 idle_seconds = [] {
        const char* env = std::getenv("BB_GC_IDLE_SECONDS");
        return std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 20, 1, 63);
    }();
    // The tick at the start of that second (an older one where no submission came then).
    const u64 idle_tick =
        gc_tick_at_second[(second - idle_seconds) % gc_tick_at_second.size()];
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 below_tick = 0;
    size_t num_deletions = 0;
    u32 visited = 0;
    // Lantern Engine: GPU-written tiled images are stored into guest memory (StoreTiledImage)
    // and freed under pressure. BB_IMAGE_WRITEBACK_GC=0: kept resident instead.
    static const bool store_tiled = [] {
        const char* env = std::getenv("BB_IMAGE_WRITEBACK_GC");
        return !(env && env[0] == '0');
    }();
    // Each store waits for the GPU (scheduler.Finish): at most this many a pass. An image that
    // could not be stored waits 60 ticks before the next try, twice as long after each further
    // failure (up to 1920): one that failed by chance comes back soon, one that never fits stops
    // costing GPU waits.
    static constexpr size_t MaxStoresPerPass = 4;
    size_t stores = 0;
    // bbport (GPU hang guard): the submission tick GcRecentFrames() frames ago; images accessed
    // at or after it are in use. 0: the guard is off. Until that many frames have passed, every
    // image counts as recent.
    const u64 recent_frames = GcRecentFrames();
    const u64 recent_tick =
        recent_frames == 0
            ? 0
            : (gc_tick >= recent_frames
                   ? std::max<u64>(sched_tick_at_gc[(gc_tick - recent_frames) %
                                                    sched_tick_at_gc.size()],
                                   1)
                   : ~0ull);

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        const u64 ticks_to_destroy = std::min<u64>(aggresive ? 160 : pressured ? 80 : 16, gc_tick);
        below_tick = gc_tick - ticks_to_destroy;
        if (!pressured && !aggresive) {
            below_tick = std::min(below_tick, idle_tick);
        } else if (!aggresive && PressureIdleSeconds() != 0) {
            below_tick = std::min(below_tick, gc_tick_at_second[(second - PressureIdleSeconds()) %
                                                                gc_tick_at_second.size()]);
        }
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
        visited = 0;
    };

    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0 || ++visited > 256) {
            return true;
        }
        auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        // bbport: images that cannot go now (GPU-written: their contents exist only here, and
        // tiled ones only through a write-back that succeeded) neither use up the deletions nor
        // stay first in line. The oldest ten being such render targets stopped the collector for
        // good, and VRAM grew with every area visited up to the pressure mark (~10 GB on 16 GB).
        const auto keep = [&] {
            lru_cache.Touch(image.lru_id, gc_tick);
            BbStats::gc_kept_images.fetch_add(1, std::memory_order_relaxed);
            return false;
        };
        if (download && !pressured) {
            return keep();
        }
        // bbport (GPU hang guard): in use in the last frames (see GcRecentFrames), or by GPU
        // work not finished yet: kept, whatever its LRU age says.
        if (recent_tick != 0 && (image.tick_accessed_last >= recent_tick ||
                                 !scheduler.IsFree(image.tick_accessed_last))) {
            ++gc_recent_kept;
            return keep();
        }
        bool written_back = false;
        if (tiled && download) {
            // Freed only once its contents are in guest memory, tiled (StoreTiledImage).
            if (!store_tiled || gc_tick < image.store_retry_tick) {
                ++gc_tiled_kept;
                return keep();
            }
            if (stores >= MaxStoresPerPass) {
                return false; // each waits for the GPU: the next pass, still first in line
            }
            ++stores;
            if (!StoreTiledImage(image_id)) {
                image.store_retry_tick = gc_tick + (u64{60} << std::min<u8>(image.store_failures, 5));
                image.store_failures = static_cast<u8>(std::min(image.store_failures + 1, 255));
                ++gc_tiled_kept;
                return keep();
            }
            image.store_failures = 0;
            written_back = true;
            ++gc_tiled_written;
        }
        --num_deletions;
        if (download && !written_back) {
            // bbport: synchronously, while the image still protects its pages. A deferred
            // write-back landed after FreeImage had unprotected them, over whatever the game
            // had meanwhile stored there (e.g. its heap after unloading an area).
            DownloadImageMemory(image_id, true);
            ++gc_downloads;
        }
        ++gc_evictions;
        BbStats::gc_freed_images.fetch_add(1, std::memory_order_relaxed);
        FreeImage(image_id);

        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    configure(false);
    lru_cache.ForEachItemBelow(below_tick, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        configure(true);
        lru_cache.ForEachItemBelow(below_tick, clean_up);
    }
    // bbport: evictions under memory pressure, at most every 5 s (BB_FRAME_STATS or not).
    if (pressured || gc_downloads != 0 || gc_tiled_written != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - gc_report_time >= std::chrono::seconds(5)) {
            std::printf("Texture cache: memory pressure, %llu of %llu MiB (critical %llu): "
                        "%llu images evicted, %llu written back, %llu tiled written back, "
                        "%llu tiled kept, %llu in use kept since the last report\n",
                        (unsigned long long)(total_used_memory >> 20),
                        (unsigned long long)(pressure_gc_memory >> 20),
                        (unsigned long long)(critical_gc_memory >> 20),
                        (unsigned long long)gc_evictions, (unsigned long long)gc_downloads,
                        (unsigned long long)gc_tiled_written, (unsigned long long)gc_tiled_kept,
                        (unsigned long long)gc_recent_kept);
            gc_report_time = now;
            gc_evictions = gc_downloads = gc_tiled_written = gc_tiled_kept = gc_recent_kept = 0;
        }
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const size_t lru_id = samplers.at(hash).lru_id;
        samplers.erase(hash);
        sampler_lru_cache.Free(lru_id);
        ++sampler_generation; // remembered handles may be gone (SamplerGeneration)
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };
    // bbport (GPU hang guard): this frame's submission tick (GcRecentFrames), and the images
    // deleted long enough ago (ImageFreeDelayFrames).
    sched_tick_at_gc[gc_tick % sched_tick_at_gc.size()] = scheduler.CurrentTick();
    FlushImageGraveyard();

    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    if (image.lru_touched_tick == gc_tick) {
        return;
    }
    image.lru_touched_tick = gc_tick;
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // bbport (GPU hang guard): kept for ImageFreeDelayFrames() frames first (FlushImageGraveyard).
    if (ImageFreeDelayFrames() != 0) {
        std::scoped_lock lk{graveyard_mutex};
        image_graveyard.push_back({gc_tick, image_id});
        return;
    }
    ReclaimImage(image_id);
}

void TextureCache::ReclaimImage(ImageId image_id) {
    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

void TextureCache::FlushImageGraveyard() {
    const u64 delay = ImageFreeDelayFrames();
    std::scoped_lock lk{graveyard_mutex};
    while (!image_graveyard.empty() && image_graveyard.front().gc_tick + delay <= gc_tick) {
        // Deferred again: freed once the GPU has finished everything recorded until now.
        ReclaimImage(image_graveyard.front().image_id);
        image_graveyard.pop_front();
    }
}

} // namespace VideoCore
