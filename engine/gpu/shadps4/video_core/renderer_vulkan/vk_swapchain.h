// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_present_policy.h"

namespace Frontend {
class WindowSDL;
}

namespace Vulkan {

class Instance;
class Scheduler;
class FsrFrameGen;

class Swapchain : private PresentPolicy::SwapchainProbe {
public:
    explicit Swapchain(const Instance& instance, const Frontend::WindowSDL& window);
    ~Swapchain() override;

    /// Creates (or recreates) the swapchain with a given size.
    void Create(u32 width, u32 height);

    /// Recreates the swapchain with a given size and current surface.
    void Recreate(u32 width, u32 height);

    /// Acquires the next image in the swapchain.
    PresentPolicy::AcquireStep AcquireNextImage();

    /// Presents the current image and move to the next one
    bool Present();

    vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    vk::Image Image() const {
        return images[image_index];
    }

    vk::ImageView ImageView() const {
        return images_view[image_index];
    }

    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    /// bbport: false while the window is minimised (0x0 surface): frames are not presented.
    bool IsPresentable() const {
        return !zero_extent && swapchain;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    u32 GetImageCount() const {
        return image_count;
    }

    /// bbport: frame generation presents two frames per game frame: one more image while it is
    /// on. True when the count changed (the caller recreates the swapchain).
    bool SetExtraImages(u32 count) {
        if (extra_images == count) {
            return false;
        }
        extra_images = count;
        return true;
    }

    vk::PresentModeKHR GetPresentMode() const {
        return present_mode;
    }

    u32 GetFrameIndex() const {
        return frame_index;
    }

    vk::Extent2D GetExtent() const {
        return extent;
    }

    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

    bool HasHDR() const {
        return supports_hdr;
    }

    void SetHDR(bool hdr);

    bool GetHDR() const {
        return needs_hdr;
    }

    /// bbport: HDR output (hdr in lantern.ini): the HDR format chosen at start (format
    /// eUndefined: none, SDR only). The frame images, passes and overlay keep the SDR format
    /// (GetSurfaceFormat); the presenter encodes into this one.
    vk::SurfaceFormatKHR GetHdrFormat() const {
        return hdr_format;
    }

    /// bbport: the current swapchain has the HDR format (it falls back to SDR while the
    /// display offers no HDR format, e.g. Windows HDR switched off).
    bool IsHdrOutput() const {
        return hdr_output;
    }

    /// bbport: VK_EXT_hdr_metadata for the HDR swapchain (kept for the swapchains created later):
    /// the display's peak, the brightest content (MaxCLL) and the frame average (MaxFALL).
    void SetHdrMetadata(float max_nits, float content_nits, float frame_average_nits);

    PresentPolicy::State& Policy() {
        return policy;
    }

    PresentPolicy::PresentMode PacingPresentMode() const {
        return pacing_mode.load(std::memory_order_relaxed);
    }

    FsrFrameGen* GetFsrFrameGen() const {
        return fsr_fg.get();
    }

    bool UsesFsrFrameGen() const {
        return fsr_fg_swapchain.load(std::memory_order_acquire);
    }

private:
    [[nodiscard]] PresentPolicy::SurfaceResult AcquireImage(u64 timeout_ns, u32& index) override;
    [[nodiscard]] std::optional<PresentPolicy::SurfaceState> CurrentSurface() const override;

    bool HdrFormatWatched() const;
    PresentPolicy::SurfaceState BuiltSurface() const;
    vk::Result QueryCapabilities(vk::SurfaceCapabilitiesKHR& capabilities) const;
    vk::Extent2D ExtentFor(const vk::SurfaceCapabilitiesKHR& capabilities, u32 width_,
                           u32 height_) const;

    /// bbport: the surface's formats, queried with the full-screen-exclusive chain when enabled.
    std::vector<vk::SurfaceFormatKHR> QuerySurfaceFormats(vk::Result* quiet_result = nullptr) const;

    /// bbport: applies the HDR metadata to the current swapchain.
    void ApplyHdrMetadata();

    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Selects the best available present mode
    void FindPresentMode();

    /// Sets the surface properties according to device capabilities
    /// (bbport: false when they cannot be queried, e.g. a lost surface)
    bool SetSurfaceProperties();

    /// bbport: vkDeviceWaitIdle under Scheduler::submit_mutex (the callers must not hold it).
    void WaitIdle();

    void EndFsrSwapchain();

    /// bbport: destroys the swapchain and the lost surface, and creates a new surface.
    void RecreateSurface();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

    /// Creates the image acquired and present ready semaphores
    void RefreshSemaphores();

private:
    const Instance& instance;
    const Frontend::WindowSDL& window;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceKHR surface{};
    vk::SurfaceFormatKHR surface_format;
    vk::Format view_format;
    vk::PresentModeKHR present_mode;
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> images_view;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0;
    u32 extra_images = 0; ///< bbport: SetExtraImages
    u32 image_index = 0;
    u32 frame_index = 0;
    bool needs_recreation = true;
    bool zero_extent = false; ///< bbport: the surface was 0x0 (minimised) at the last Create
    bool surface_lost = false; ///< bbport: VK_ERROR_SURFACE_LOST_KHR seen: Create makes a new one
    bool needs_hdr = false;    // The game requested HDR swapchain
    bool supports_hdr = false; // SC supports HDR output
    // bbport: HDR output (GetHdrFormat, IsHdrOutput).
    vk::SurfaceFormatKHR hdr_format{vk::Format::eUndefined, vk::ColorSpaceKHR::eSrgbNonlinear};
    bool hdr_output = false;
    bool hdr_unavailable_logged = false;
    bool full_screen_exclusive = false; ///< VK_EXT_full_screen_exclusive: DISALLOWED chained
    PresentPolicy::State policy;
    std::unique_ptr<FsrFrameGen> fsr_fg;
    std::atomic<bool> fsr_fg_swapchain{false};
    std::atomic<PresentPolicy::PresentMode> pacing_mode{PresentPolicy::PresentMode::Unknown};
    vk::Result last_acquire_result = vk::Result::eSuccess;
    float hdr_max_nits = 0.0f, hdr_content_nits = 0.0f,
          hdr_average_nits = 0.0f; ///< SetHdrMetadata; 0: not set
};

} // namespace Vulkan
