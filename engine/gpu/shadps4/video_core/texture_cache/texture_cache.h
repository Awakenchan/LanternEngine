// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <atomic>
#include <chrono>
#include "bbport_toggles.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <deque>
#include <queue>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/multi_level_page_table.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/tile_manager.h"
#include "video_core/texture_cache/watched_pages.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class Runtime;
}

namespace VideoCore {

class BufferCache;
class PageManager;

/// bbport BB_REFRESH_TRACE=1 (diagnostics): the kind of write behind an invalidation, named by
/// the rasterizer around its calls, printed with the large images it marks dirty.
inline thread_local const char* invalidate_reason = "other";
struct InvalidateReason {
    const char* previous;
    explicit InvalidateReason(const char* reason) : previous{invalidate_reason} {
        invalidate_reason = reason;
    }
    ~InvalidateReason() {
        invalidate_reason = previous;
    }
};

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 10;
        static constexpr size_t PageBits = 20;
    };
    using PageTable = MultiLevelPageTable<Traits>;

public:
    enum class BindingType : u32 {
        Texture,
        Storage,
        RenderTarget,
        DepthTarget,
        VideoOut,
    };

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {}
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

public:
    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache,
                 PageManager& tracker);
    ~TextureCache();

    /// bbport (diagnostics): the images over [addr, addr + size), described briefly.
    std::string DescribeImagesIn(VAddr addr, u64 size);

    /// bbport: changes whenever an image is registered or unregistered.
    [[nodiscard]] u64 RegistryGeneration() const noexcept {
        return registry_generation.load(std::memory_order_acquire);
    }

    /// bbport: FindImage's access tick for an image found by a memoized lookup. The LRU touch
    /// FindImage also does happens in UpdateImage (FindTexture), which every binding reaches.
    void MarkFound(ImageId image_id) {
        slot_images[image_id].tick_accessed_last = scheduler.CurrentTick();
    }

    /// bbport: whether FindImage's result `image_id` for `info` is an exact match: the
    /// registered image with the same address, size, extent, format, type, resources and
    /// layout (FindImage's own memo test, with equal resources). Images register once, their
    /// descriptions never change and later images come after them in the page table, so a new
    /// search finds that image first again for as long as it stays registered: a lookup
    /// remembered with it outlives other images' (un)registration (IsRegisteredImage).
    [[nodiscard]] bool IsExactMatch(ImageId image_id, const ImageInfo& info) const;

    /// bbport: whether `image_id` is still the registered image `uid` (see IsExactMatch).
    [[nodiscard]] bool IsRegisteredImage(ImageId image_id, u64 uid) const {
        if (!image_id || !slot_images.is_allocated(image_id)) {
            return false;
        }
        const Image& image = slot_images[image_id];
        return image.image_uid == uid && True(image.flags & ImageFlagBits::Registered);
    }

    /// bbport: the unique id of an image (slot ids are reused, these are not).
    [[nodiscard]] u64 ImageUid(ImageId image_id) const {
        return slot_images[image_id].image_uid;
    }

    /// bbport (BB_DRAWREC_WORKERS): where the image with this id would be, for a prefetch from
    /// the lookahead thread; the slot may be free or reused, nothing is read through it.
    [[nodiscard]] const void* ImageSlotAddress(ImageId image_id) const noexcept {
        return slot_images.SlotAddress(image_id);
    }

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    /// Invalidates any image in the logical page range.
    void InvalidateMemory(VAddr addr, size_t size);

    /// bbport: uploads the game's button icons again when what they show changed (keyboard and
    /// mouse prompts, engine/key_prompts.h). Once a frame, without the lock.
    void UpdateKeyPrompts();

    /// bbport: whether a tracked image protects a page of the range; without the lock, from any
    /// thread (Rasterizer::OnWriteFault: a write fault elsewhere leaves this cache alone).
    [[nodiscard]] bool WatchesPages(VAddr addr, size_t size) const noexcept {
        return watched_pages.Any(addr, size);
    }

    /// Marks an image as dirty if it exists at the provided address.
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size);

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// Schedules a copy of pending images for download back to CPU memory.
    void ProcessDownloadImages();

    /// bbport debugging: writes mip 0 / layer 0 of every image registered at `address` to
    /// <dir>/img_<address>_<n>_<w>x<h>_<format>.raw and prints what was found.
    void DumpImagesAt(VAddr address, const char* dir);

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// bbport: a FindView result remembered by the caller for the same image and view info.
    struct ViewMemo {
        ImageId image_id{};
        const void* backing = nullptr;
        ImageViewId view_id{};
    };

    /// Retrieves an image view with the properties of the specified image id.
    /// `refresh` false: an image found out of date is used as is (the texture binding helper
    /// checked it beforehand; only a guest write racing with the draw can make it so).
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc,
                                         ViewMemo* memo = nullptr, bool refresh = true);

    /// Retrieves the render target with specified properties
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// bbport: whether UpdateImage has nothing to do for this image (its fast path).
    [[nodiscard]] bool IsUpToDate(ImageId image_id) const {
        const Image& image = slot_images[image_id];
#ifdef _LIBCPP_VERSION
        // libc++ has no std::atomic_ref<const T> yet.
        const u32 flags = __atomic_load_n(reinterpret_cast<const u32*>(&image.flags), __ATOMIC_ACQUIRE);
#else
        const u32 flags = std::atomic_ref<const u32>(reinterpret_cast<const u32&>(image.flags))
                              .load(std::memory_order_acquire);
#endif
        constexpr u32 Dirty = static_cast<u32>(ImageFlagBits::Dirty);
        constexpr u32 Registered = static_cast<u32>(ImageFlagBits::Registered);
        return (flags & (Dirty | Registered)) == Registered &&
               image.track_addr == image.guest_begin && image.track_addr_end == image.guest_end &&
               image.lru_touched_tick == gc_tick;
    }

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id) {
        // bbport: a clean image already tracked and touched in this GC period needs nothing.
        // Every texture binding comes here; the mutex (shared with the fault handlers of the
        // guest threads) was ~3% of the GPU thread. Flags are read atomically: an invalidation
        // racing with this check races the same way with the locked path.
        if (!BbToggle::Disabled(BbToggle::UpdateImageFastPath)) {
            const Image& image = slot_images[image_id];
#ifdef _LIBCPP_VERSION
            const u32 flags =
                __atomic_load_n(reinterpret_cast<const u32*>(&image.flags), __ATOMIC_ACQUIRE);
#else
            const u32 flags = std::atomic_ref<const u32>(reinterpret_cast<const u32&>(image.flags))
                                  .load(std::memory_order_acquire);
#endif
            constexpr u32 Dirty = static_cast<u32>(ImageFlagBits::Dirty);
            constexpr u32 Registered = static_cast<u32>(ImageFlagBits::Registered);
            if ((flags & (Dirty | Registered)) == Registered &&
                image.track_addr == image.guest_begin && image.track_addr_end == image.guest_end &&
                image.lru_touched_tick == gc_tick) {
                return;
            }
        }
        std::scoped_lock lock{mutex};
        Image& image = slot_images[image_id];
        TrackImage(image_id);
        TouchImage(image);
        RefreshImage(image);
    }

    /// Resolves overlap between existing cache image and pending merged image
    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id,
                                                               bool exact_fmt = false);

    /// Resolves depth overlap and either re-creates the image or returns existing one
    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id, bool exact_fmt);

    /// Creates a new image with provided image info and copies subresources from image_id
    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId image_id);

    /// Reuploads image contents.
    void RefreshImage(Image& image);

    /// Retrieves the sampler that matches the provided S# descriptor.
    /// extra_lod_bias: bbport, added to the S#'s bias (reduced scene rendering).
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base,
                                         bool is_depth, float extra_lod_bias = 0.0f,
                                         size_t* lru_id = nullptr);

    /// bbport (sampler memo of the draw recording thread): a sampler GetSampler returned stays
    /// alive while this is unchanged (the collector frees samplers only in RunGarbageCollector,
    /// never while textures are bound: the GPU command thread with the draw pipe drained, or the
    /// recording thread between packets; it bumps it then), provided the caller touches it once
    /// per GC period (TouchSampler) as GetSampler does.
    [[nodiscard]] u64 SamplerGeneration() const noexcept {
        return sampler_generation;
    }
    [[nodiscard]] u64 GcTick() const noexcept {
        return gc_tick;
    }
    void TouchSampler(size_t lru_id) {
        std::scoped_lock lock{samplers_mutex};
        sampler_lru_cache.Touch(lru_id, gc_tick);
    }

    /// Retrieves the image with the specified id.
    Image* TryGetImage(ImageId id, u64 uid) {
        if (!slot_images.is_allocated(id)) return nullptr;
        auto& image = slot_images[id];
        return !uid || image.image_uid == uid ? &image : nullptr;
    }
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        TouchImage(image);
        return image;
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    /// Get the associated depth stencil image if it is still valid.
    ImageId GetAssociatedDepth(Image& image) {
        if (!image.depth_id) {
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        // The linked depth image is no longer valid, disassociate it.
        image.DisassociateDepth();
        return {};
    }

    enum class MetaType {
        CMask,
        FMask,
        HTile,
    };

    /// Returns meta type if the specified address is a metadata surface.
    std::optional<MetaType> IsMeta(VAddr address) const {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it->second.type;
        }
        return std::nullopt;
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            return true;
        }
        return false;
    }

    /// Updates the state of a slice of the specified metadata surface.
    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// Runs the garbage collector.
    void RunGarbageCollector();

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = slot_images[image_id];
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.flags |= ImageFlagBits::Picked;
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            slot_images[image_id].flags &= ~ImageFlagBits::Picked;
        }
    }

private:
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> Traits::PageBits;
        for (u64 page = addr >> Traits::PageBits; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Copies image memory back to CPU.
    void DownloadImageMemory(ImageId image_id, bool sync = false);

    /// Lantern Engine: stores a GPU-written tiled image into guest memory in its tiled layout,
    /// synchronously, so the collector may free it. False when this image cannot be stored (its
    /// layout, other GPU data over it, a mapping lost meanwhile): it then stays resident.
    bool StoreTiledImage(ImageId image_id);

    /// Thread function for copying downloaded images out to CPU memory.
    void DownloadedImagesThread(const std::stop_token& token);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, VAddr cpu_addr);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);

    /// bbport: the page manager's write watchers of an image's pages, and watched_pages with
    /// them: counted before the pages are protected, uncounted after they are unprotected.
    void WatchPages(VAddr addr, u64 size);
    void UnwatchPages(VAddr addr, u64 size);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);
    static u64 MaybeDirtyHash(const Image& image);

    /// bbport: for an image with the game's button icons, its data with keycaps over them in
    /// `data`; false when there is none to draw (engine/key_prompts.h).
    bool KeyPromptData(const Image& image, std::vector<u8>& data);
    void NoteUiTexture(Image& image);
public:
    Image* UiReplacement(const Image& image);
    /// The HUD's sheet: its cleaned copy, as a replacement (UiReplacement). `sheet`: the bytes
    /// KeyPromptData left (the sheet with any key prompts drawn in).
    void CleanHud(Image& image, const std::vector<u8>& sheet);
    /// Whether 4K menus are loaded (BB_UI_4K): the UI draws then sample smoothly
    /// (Rasterizer::SmoothUiSampler).
    static bool UiReplacementsLoaded();
private:
    /// A replacement's image from RGBA texels, with its mip chain, as ui_images[key].
    Image* UploadUi(u64 key, std::vector<u8>& pixels, int w, int h, vk::Format format);
    std::unordered_map<u64, std::unique_ptr<Image>> ui_images;
    std::unordered_set<u64> ui_missing;
    u64 ui_bytes = 0;
    /// CleanHud's copies (key, bytes), oldest first: one per drawing of the key prompts.
    std::deque<std::pair<u64, u64>> hud_copies;
    /// The cleaned 4K art for the HUD's sheet (HudClean::kArtFile), read once; empty without it.
    bool HudArt();
    struct {
        std::vector<u8> pixels;
        int w = 0, h = 0;
        bool tried = false;
    } hud_art;

    /// Removes the image and any views/surface metas that reference it.
    void DeleteImage(ImageId image_id);

    /// bbport (GPU hang guard): destroys the image and its views once the GPU is past the
    /// current tick (DeleteImage, directly or after BB_IMAGE_FREE_DELAY_FRAMES).
    void ReclaimImage(ImageId image_id);

    /// bbport (GPU hang guard): hands images unregistered at least BB_IMAGE_FREE_DELAY_FRAMES
    /// frames ago to ReclaimImage (RunGarbageCollector, once a frame).
    void FlushImageGraveyard();

    /// Touch the image in the LRU cache.
    void TouchImage(const Image& image);

    void FreeImage(ImageId image_id) {
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }

    void GarbageCollectImages();
    void GarbageCollectSamplers();

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    std::unordered_set<ImageId> download_images;
    u64 total_used_memory = 0;
    u64 gc_evictions = 0, gc_downloads = 0; ///< bbport: pressure report
    /// bbport (image write-back GC): tiled images written back / kept (write-back refused or
    /// failed) since the last pressure report.
    u64 gc_tiled_written = 0, gc_tiled_kept = 0;
    std::chrono::steady_clock::time_point gc_report_time{};
    /// bbport: gc_tick at each of the last 64 seconds (GarbageCollectImages).
    std::array<u64, 64> gc_tick_at_second{};
    u64 gc_second = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    /// bbport: shadPS4's marks from the startup budget, the floor of the discrete-GPU rule
    /// (GarbageCollectImages), and the second the driver's budget was last asked for.
    u64 static_pressure_gc_memory = 0;
    u64 static_critical_gc_memory = 0;
    u64 gc_budget_second = ~0ull;
    u64 total_used_samplers = 0;
    u64 sampler_generation = 0; ///< bbport: samplers freed so far (SamplerGeneration)
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    /// bbport (GPU hang guard): scheduler.CurrentTick() at each of the last 64 collector passes
    /// (one a frame, indexed by gc_tick), for "used in the last N frames" in submission ticks.
    std::array<u64, 64> sched_tick_at_gc{};
    /// bbport (GPU hang guard): unregistered images whose Vulkan objects are kept for a few more
    /// frames (DeleteImage): the gc_tick they were deleted at. Slots are not reused meanwhile.
    struct GraveImage {
        u64 gc_tick;
        ImageId image_id;
    };
    std::deque<GraveImage> image_graveyard;
    std::mutex graveyard_mutex;
    u64 gc_recent_kept = 0; ///< bbport: images kept because used in the last frames
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    const bool readback_linear_images;
    PageTable page_table;
    std::mutex mutex;
    /// bbport: the pages tracked images protect, readable without `mutex` (WatchesPages);
    /// changed with `mutex` held, through WatchPages and UnwatchPages.
    WatchedPages watched_pages;
    // bbport: FindImage results for unchanged image registrations (guarded by `mutex`).
    struct FindImageCacheEntry {
        VAddr address = 0;
        u64 size = 0;
        Extent3D extent{};
        vk::Format format{};
        AmdGpu::ImageType type{};
        bool exact_fmt = false;
        BindingType binding{};
        u32 levels = 0;
        u32 layers = 0;
        std::array<u32, 6> layout_key{};
        u64 generation = ~0ULL;
        ImageId image_id{};
        int view_mip = -1;
        int view_slice = -1;
    };
    std::array<FindImageCacheEntry, 1024> find_image_cache{};
    std::atomic<u64> registry_generation{0};
    std::mutex samplers_mutex;
    /// bbport: where the game's button icons are (KeyPromptData), for UpdateKeyPrompts.
    std::mutex key_icons_mutex;
    std::vector<std::pair<VAddr, u32>> key_icons;
    std::mutex download_images_mutex;
    std::atomic<bool> downloads_queued{false}; ///< download_images may be non-empty (checked without the lock)
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
};

} // namespace VideoCore
