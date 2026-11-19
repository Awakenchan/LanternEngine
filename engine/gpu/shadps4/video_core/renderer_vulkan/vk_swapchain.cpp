// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "bbport_settings.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_fsr_framegen.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

namespace Vulkan {

/// bbport: display events (a lost surface, a failed swapchain) are reported a few times and the
/// presenter skips frames until a new swapchain works; they ended the game (exit 23).
static void NoteDisplayEvent(const char* what, vk::Result result) {
    static std::atomic<u32> notes{0};
    if (notes.fetch_add(1, std::memory_order_relaxed) < 16) {
        std::printf("Display: %s: %s\n", what, vk::to_string(result).c_str());
    }
}

static constexpr vk::SurfaceFormatKHR SURFACE_FORMAT_HDR = {
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

/// bbport: HDR output formats, in order of preference: HDR10 (BT.2020 primaries, PQ), then
/// scRGB (linear BT.709 in FP16, 1.0 = 80 nits).
static constexpr std::array HDR_OUTPUT_FORMATS = {
    SURFACE_FORMAT_HDR,
    vk::SurfaceFormatKHR{
        .format = vk::Format::eR16G16B16A16Sfloat,
        .colorSpace = vk::ColorSpaceKHR::eExtendedSrgbLinearEXT,
    },
};

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_)
    : instance{instance_}, window{window_}, surface{CreateSurface(instance.GetInstance(), window)} {
    // bbport: the swapchain never becomes exclusive fullscreen. A borderless window covering
    // the screen was promoted to it by the driver, and Windows HDR switched off with it.
    full_screen_exclusive = instance.IsFullScreenExclusiveEnabled();
    if (full_screen_exclusive) {
        std::printf("Display: exclusive fullscreen disallowed (VK_EXT_full_screen_exclusive)\n");
    }
    policy.mode = PresentPolicy::PolicyModeFromSwitch(std::getenv(PresentPolicy::kPolicySwitch));
    std::printf("Display: present policy %s (%s=%d)\n",
                policy.mode == PresentPolicy::PolicyMode::New
                    ? "on: acquire and the idle redraw give up after 500 ms, a sticky suboptimal "
                      "swapchain is rechecked once a second"
                    : "off: the original waits and rebuilds",
                PresentPolicy::kPolicySwitch, policy.mode == PresentPolicy::PolicyMode::New ? 1 : 0);
    FindPresentFormat();
    FindPresentMode();
    if (instance.GetFsrFgQueues().present) {
        fsr_fg = std::make_unique<FsrFrameGen>(instance);
    }

    Create(window.GetWidth(), window.GetHeight());
    ImGui::Core::Initialize(instance, window, image_count, surface_format.format);
}

Swapchain::~Swapchain() {
    Destroy();
    fsr_fg.reset();
    instance.GetInstance().destroySurfaceKHR(surface);
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    needs_recreation = false;

    // bbport: a lost surface (VK_ERROR_SURFACE_LOST_KHR: a display or driver reset) is made again
    // on the same window before a swapchain can be created on it.
    if (surface_lost) {
        RecreateSurface();
    }

    // bbport: a minimised window has a 0x0 surface (issue #31); no swapchain can have that extent.
    // The old one stays (presents are skipped) until the window is back.
    if (!SetSurfaceProperties()) {
        needs_recreation = true;
        return;
    }
    if (extent.width == 0 || extent.height == 0) {
        zero_extent = true;
        needs_recreation = true;
        return;
    }
    zero_extent = false;

    Destroy();

    const std::array queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };

    const bool exclusive = queue_family_indices[0] == queue_family_indices[1];
    const u32 queue_family_indices_count = exclusive ? 1u : 2u;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    // bbport: HDR output while the display still offers the HDR format chosen at start
    // (switching Windows HDR off removes it; the swapchain is SDR until it is back).
    hdr_output = false;
    if (!needs_hdr && hdr_format.format != vk::Format::eUndefined) {
        const auto formats = QuerySurfaceFormats();
        hdr_output = std::ranges::find(formats, hdr_format) != formats.end();
        if (!hdr_output && !hdr_unavailable_logged) {
            std::printf("HDR output: the display offers no HDR format now (is Windows HDR on?); "
                        "SDR until it does\n");
        }
        hdr_unavailable_logged = !hdr_output;
    }
    const auto format = needs_hdr ? SURFACE_FORMAT_HDR : hdr_output ? hdr_format : surface_format;
    view_format = format.format;
    // bbport: a swapchain the NVIDIA boost hint is set on (Instance::IsLowLatencyBoostEnabled).
    const vk::SwapchainLatencyCreateInfoNV latency_info = {
        .pNext = FullScreenExclusiveDisallowed(full_screen_exclusive),
        .latencyModeEnable = true,
    };
    const bool fsr_fg_wanted = fsr_fg && fsr_fg->Usable();
    const bool boost = instance.IsLowLatencyBoostEnabled() && !fsr_fg_wanted &&
                       BbSettings::Get().reflex.load() != BbSettings::ReflexOff;
    const vk::SwapchainCreateInfoKHR swapchain_info = {
        .pNext = boost ? static_cast<const void*>(&latency_info)
                       : FullScreenExclusiveDisallowed(full_screen_exclusive),
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = nullptr,
    };

    vk::Result swapchain_result = vk::Result::eSuccess;
    vk::SwapchainKHR chain{};
    if (fsr_fg_wanted && fsr_fg->CreateSwapchain(swapchain_info, chain)) {
        fsr_fg_swapchain.store(true, std::memory_order_release);
    } else {
        auto created = instance.GetDevice().createSwapchainKHR(swapchain_info);
        swapchain_result = created.result;
        chain = created.value;
    }
    if (swapchain_result != vk::Result::eSuccess) {
        // bbport: no swapchain now (IsPresentable is false): presents are skipped and the next
        // one tries again.
        NoteDisplayEvent("creating the swapchain failed", swapchain_result);
        surface_lost = swapchain_result == vk::Result::eErrorSurfaceLostKHR;
        needs_recreation = true;
        return;
    }
    swapchain = chain;
    PresentPolicy::OnSwapchainCreated(policy);
    if (boost && VULKAN_HPP_DEFAULT_DISPATCHER.vkSetLatencySleepModeNV) {
        // bbport (0.9.37 beta 6): the driver keeps the GPU's clocks up (Reflex Boost) instead of
        // lowering them whenever the GPU waits for the engine: half the frame rate on an RTX 3070
        // without it. Low latency mode itself (its sleep and markers) is Streamline's.
        const VkLatencySleepModeInfoNV mode{
            .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
            .lowLatencyMode = VK_FALSE,
            .lowLatencyBoost = VK_TRUE,
            .minimumIntervalUs = 0,
        };
        const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkSetLatencySleepModeNV(
            instance.GetDevice(), swapchain, &mode);
        static bool logged = false;
        if (!logged || result != VK_SUCCESS) {
            logged = true;
            std::printf("GPU: NVIDIA boost clocks %s (Reflex setting; BB_NV_BOOST=0: off)\n",
                        result == VK_SUCCESS ? "on" : vk::to_string(vk::Result(result)).c_str());
        }
    }

    SetupImages();
    RefreshSemaphores();
    // The new chain may have fewer images (AwesomeObserver b3164c1): start the indices over.
    frame_index = image_index = 0;
    ApplyHdrMetadata();
}

void Swapchain::SetHdrMetadata(float max_nits, float content_nits, float frame_average_nits) {
    if (max_nits == hdr_max_nits && content_nits == hdr_content_nits &&
        frame_average_nits == hdr_average_nits) {
        return;
    }
    hdr_max_nits = max_nits;
    hdr_content_nits = content_nits;
    hdr_average_nits = frame_average_nits;
    ApplyHdrMetadata();
}

void Swapchain::ApplyHdrMetadata() {
    if (!hdr_output || !swapchain || hdr_max_nits <= 0.0f || !instance.IsHdrMetadataEnabled() ||
        !VULKAN_HPP_DEFAULT_DISPATCHER.vkSetHdrMetadataEXT) {
        return;
    }
    // The picture is the SDR frame: BT.709 primaries, D65 white, up to the peak set. bbport:
    // MaxCLL is what the HDR mode can reach (Presenter::PresentHdr): with expand's lower
    // ceiling the display has no reason to compress the highlights itself.
    const vk::HdrMetadataEXT metadata{
        .displayPrimaryRed = {0.640f, 0.330f},
        .displayPrimaryGreen = {0.300f, 0.600f},
        .displayPrimaryBlue = {0.150f, 0.060f},
        .whitePoint = {0.3127f, 0.3290f},
        .maxLuminance = hdr_max_nits,
        .minLuminance = 0.0f,
        .maxContentLightLevel = hdr_content_nits > 0.0f ? hdr_content_nits : hdr_max_nits,
        .maxFrameAverageLightLevel = hdr_average_nits,
    };
    if (fsr_fg_swapchain) {
        fsr_fg->SetHdrMetadata(metadata);
        return;
    }
    instance.GetDevice().setHdrMetadataEXT(1, &swapchain, &metadata);
}

std::vector<vk::SurfaceFormatKHR> Swapchain::QuerySurfaceFormats(vk::Result* quiet_result) const {
    const vk::PhysicalDevice physical = instance.GetPhysicalDevice();
    if (full_screen_exclusive) {
        // The same chain as the swapchain's creation (VK_EXT_full_screen_exclusive).
        const vk::PhysicalDeviceSurfaceInfo2KHR surface_info{
            .pNext = FullScreenExclusiveDisallowed(true),
            .surface = surface,
        };
        auto [formats2_result, formats2] = physical.getSurfaceFormats2KHR(surface_info);
        if (formats2_result == vk::Result::eSuccess) {
            std::vector<vk::SurfaceFormatKHR> formats;
            formats.reserve(formats2.size());
            for (const auto& format2 : formats2) {
                formats.push_back(format2.surfaceFormat);
            }
            if (quiet_result) {
                *quiet_result = formats2_result;
            }
            return formats;
        }
        if (!quiet_result) {
            LOG_WARNING(Render_Vulkan, "Failed to query surface formats (2): {}",
                        vk::to_string(formats2_result));
        }
    }
    auto [formats_result, formats] = physical.getSurfaceFormatsKHR(surface);
    if (quiet_result) {
        *quiet_result = formats_result;
    }
    if (formats_result != vk::Result::eSuccess) {
        if (!quiet_result) {
            LOG_ERROR(Render_Vulkan, "Failed to query surface formats: {}",
                      vk::to_string(formats_result));
        }
        return {};
    }
    return std::move(formats);
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the swapchain: width={} height={} HDR={}", width_, height_,
              needs_hdr);
    Create(width_, height_);
}

void Swapchain::SetHDR(bool hdr) {
    if (needs_hdr == hdr) {
        return;
    }

    WaitIdle();

    needs_hdr = hdr;
    Recreate(width, height);
    ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                 : surface_format.format);
}

static PresentPolicy::SurfaceExtent ToSurfaceExtent(vk::Extent2D extent) {
    return {.width = extent.width, .height = extent.height};
}

static u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

PresentPolicy::SurfaceResult Swapchain::AcquireImage(u64 timeout_ns, u32& index) {
    last_acquire_result =
        fsr_fg_swapchain
            ? fsr_fg->Acquire(timeout_ns, image_acquired[frame_index], index)
            : instance.GetDevice().acquireNextImageKHR(swapchain, timeout_ns,
                                                       image_acquired[frame_index], VK_NULL_HANDLE,
                                                       &index);
    return PresentPolicy::ToSurfaceResult(static_cast<VkResult>(last_acquire_result));
}

bool Swapchain::HdrFormatWatched() const {
    return !needs_hdr && hdr_format.format != vk::Format::eUndefined;
}

PresentPolicy::SurfaceState Swapchain::BuiltSurface() const {
    PresentPolicy::SurfaceState state;
    state.extent = ToSurfaceExtent(extent);
    state.hdr_offered = HdrFormatWatched() && hdr_output;
    return state;
}

std::optional<PresentPolicy::SurfaceState> Swapchain::CurrentSurface() const {
    vk::SurfaceCapabilitiesKHR capabilities{};
    if (QueryCapabilities(capabilities) != vk::Result::eSuccess) {
        return std::nullopt;
    }
    PresentPolicy::SurfaceState state;
    state.extent =
        ToSurfaceExtent(ExtentFor(capabilities, window.GetWidth(), window.GetHeight()));
    if (HdrFormatWatched()) {
        vk::Result formats_result = vk::Result::eSuccess;
        const std::vector<vk::SurfaceFormatKHR> formats = QuerySurfaceFormats(&formats_result);
        if (formats_result != vk::Result::eSuccess) {
            return std::nullopt;
        }
        state.hdr_offered = std::ranges::find(formats, hdr_format) != formats.end();
    }
    return state;
}

PresentPolicy::AcquireStep Swapchain::AcquireNextImage() {
    using PresentPolicy::AcquireStep;
    if (!swapchain) {
        needs_recreation = true; // the last Create failed
        return AcquireStep::Rebuild;
    }
    const PresentPolicy::AcquireOutcome outcome =
        PresentPolicy::AcquireImage(*this, BuiltSurface(), policy, NowNs(), image_index);
    switch (outcome.step) {
    case AcquireStep::Acquired:
        // A suboptimal acquire still owns an image and signals the semaphore: present it in the
        // normal submission. Dropping it left the semaphore signalled and never waited on. From
        // AwesomeObserver b3164c1 (E. Doronin).
        needs_recreation = needs_recreation || outcome.rebuild_after_present;
        return AcquireStep::Acquired;
    case AcquireStep::Skip:
        if (outcome.first_timeout) {
            std::printf("Display: no swapchain image within %llu ms (%s); the frame is skipped "
                        "and the next one tries again\n",
                        static_cast<unsigned long long>(PresentPolicy::kPresentWaitTimeoutMs),
                        vk::to_string(last_acquire_result).c_str());
        }
        return AcquireStep::Skip;
    case AcquireStep::Rebuild:
        break;
    }
    // bbport: a lost surface, out-of-date or another display event (a lost exclusive
    // fullscreen): recreated (this was UNREACHABLE, exit 23).
    if (last_acquire_result != vk::Result::eErrorOutOfDateKHR &&
        last_acquire_result != vk::Result::eErrorUnknown) {
        NoteDisplayEvent("acquiring a swapchain image", last_acquire_result);
    }
    if (last_acquire_result == vk::Result::eErrorSurfaceLostKHR) {
        surface_lost = true;
    }
    needs_recreation = true;
    return AcquireStep::Rebuild;
}

bool Swapchain::Present() {
    const vk::PresentInfoKHR present_info = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };

    auto result = fsr_fg_swapchain ? fsr_fg->Present(instance.GetPresentQueue(), present_info)
                                   : instance.GetPresentQueue().presentKHR(present_info);
    if (fsr_fg_swapchain && result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR &&
        result != vk::Result::eErrorOutOfDateKHR && result != vk::Result::eErrorSurfaceLostKHR &&
        result != vk::Result::eErrorDeviceLost) {
        fsr_fg->Fail("AMD's swapchain returned an error at present");
    }
    if (result == vk::Result::eErrorOutOfDateKHR) {
        needs_recreation = true;
    } else if (result == vk::Result::eSuboptimalKHR) {
        const bool rebuild = PresentPolicy::OnSuboptimal(*this, BuiltSurface(), policy, NowNs());
        needs_recreation = needs_recreation || rebuild;
    } else if (result != vk::Result::eSuccess && result != vk::Result::eErrorDeviceLost) {
        // bbport: a lost surface (display or driver reset) or another display event: recreated
        // like out-of-date (this was an assertion, exit 23). A lost device still stops below.
        NoteDisplayEvent("presenting", result);
        surface_lost = result == vk::Result::eErrorSurfaceLostKHR;
        needs_recreation = true;
    } else {
        if (result == vk::Result::eSuccess) {
            PresentPolicy::OnPresentSucceeded(policy);
        }
        if (result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("present");
        }
        ASSERT_MSG(result == vk::Result::eSuccess, "Swapchain presentation failed: {}",
                   vk::to_string(result));
    }

    frame_index = (frame_index + 1) % image_count;
    if (fsr_fg_swapchain && !fsr_fg->Usable()) {
        needs_recreation = true;
    }

    return !needs_recreation;
}

void Swapchain::FindPresentFormat() {
    const auto formats = QuerySurfaceFormats();
    ASSERT_MSG(!formats.empty(), "Failed to query surface formats");

    // bbport: the game's own HDR request (sceVideoOutConfigureOutputMode with BT.2020 PQ, which
    // Bloodborne never makes) stays off: HDR output is the presenter's encode of the SDR
    // picture (hdr in lantern.ini, below), and the per-flip SetHDR(false) must not touch it.
    supports_hdr = false;

    // If there is a single undefined surface format, the device doesn't care, so we'll just use
    // RGBA sRGB.
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return;
    }

    // Try to find a suitable format. bbport: VK_EXT_swapchain_colorspace is always enabled now,
    // so the same formats can be listed with other color spaces too: SRGB_NONLINEAR first.
    const auto is_sdr_format = [](const vk::SurfaceFormatKHR& sformat) {
        return sformat.format == vk::Format::eR8G8B8A8Unorm ||
               sformat.format == vk::Format::eB8G8R8A8Unorm;
    };
    auto found = std::ranges::find_if(formats, [&](const vk::SurfaceFormatKHR& sformat) {
        return is_sdr_format(sformat) && sformat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
    });
    if (found == formats.end()) {
        found = std::ranges::find_if(formats, is_sdr_format);
    }
    if (found == formats.end()) {
        UNREACHABLE_MSG("Unable to find required swapchain format!");
    }
    surface_format.format = found->format;
    surface_format.colorSpace = found->colorSpace;

    // bbport: HDR output (hdr in lantern.ini, BB_HDR), chosen once.
    const auto& settings = BbSettings::Get();
    if (!settings.startup_hdr) {
        return;
    }
    for (const vk::SurfaceFormatKHR& candidate : HDR_OUTPUT_FORMATS) {
        if (std::ranges::find(formats, candidate) != formats.end()) {
            hdr_format = candidate;
            break;
        }
    }
    if (hdr_format.format == vk::Format::eUndefined) {
        std::printf("HDR output: the display offers no HDR format (is Windows HDR on?)\n");
        return;
    }
    // The presenter letterboxes the frame into an SDR image of the swapchain's size, draws the
    // menu over it, and samples it in the encode pass.
    constexpr vk::FormatFeatureFlags composite_features =
        vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eColorAttachment |
        vk::FormatFeatureFlagBits::eColorAttachmentBlend | vk::FormatFeatureFlagBits::eBlitDst |
        vk::FormatFeatureFlagBits::eTransferDst;
    const vk::FormatFeatureFlags features =
        instance.GetPhysicalDevice().getFormatProperties(surface_format.format).optimalTilingFeatures;
    if ((features & composite_features) != composite_features) {
        std::printf("HDR output: %s cannot hold the SDR picture for the encode; staying SDR\n",
                    vk::to_string(surface_format.format).c_str());
        hdr_format = {};
        return;
    }
    const float display_peak = settings.hdr_display_peak.load();
    std::printf("HDR output: %s, paper white %.0f nits, peak %.0f nits (%s), highlight boost %.2f\n",
                hdr_format == SURFACE_FORMAT_HDR ? "HDR10 PQ (BT.2020)" : "scRGB (FP16, linear BT.709)",
                settings.hdr_paper_white.load(), BbSettings::HdrPeakNits(settings),
                !settings.hdr_peak_auto.load()           ? "set"
                : display_peak >= BbSettings::HdrPeakMin ? "the display's"
                                                         : "the display reports none: the set value",
                settings.hdr_boost.load());
}

void Swapchain::FindPresentMode() {
    const std::string requested_mode = EmulatorSettings.GetPresentMode();
    const PresentPolicy::PresentMode requested_by_name =
        PresentPolicy::PresentModeFromName(requested_mode);
    const auto [modes_result, modes] = GetSurfacePresentModes(
        instance.GetInstance(), instance.GetPhysicalDevice(), surface, full_screen_exclusive);
    if (modes_result != vk::Result::eSuccess) {
        LOG_ERROR(Render, "Failed to query available present modes, falling back to Fifo as "
                          "guaranteed supported option.");
        present_mode = vk::PresentModeKHR::eFifo;
        pacing_mode.store(PresentPolicy::PacingPresentMode(policy.mode, requested_by_name,
                                                           PresentPolicy::PresentMode::Fifo),
                          std::memory_order_relaxed);
        return;
    }

    vk::PresentModeKHR requested = vk::PresentModeKHR::eMailbox;
    if (requested_mode == "Mailbox") {
        requested = vk::PresentModeKHR::eMailbox;
    } else if (requested_mode == "Fifo") {
        requested = vk::PresentModeKHR::eFifo;
    } else if (requested_mode == "FifoRelaxed") {
        // bbport: the launcher offers it (VSync that tears instead of waiting when a frame is late).
        requested = vk::PresentModeKHR::eFifoRelaxed;
    } else if (requested_mode == "Immediate") {
        requested = vk::PresentModeKHR::eImmediate;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown present mode {}, defaulting to Mailbox.",
                  EmulatorSettings.GetPresentMode());
    }

    std::vector<PresentPolicy::PresentMode> supported;
    supported.reserve(modes.size());
    for (const vk::PresentModeKHR mode : modes) {
        supported.push_back(PresentPolicy::ToPolicyMode(static_cast<VkPresentModeKHR>(mode)));
    }
    const PresentPolicy::PresentMode chosen = PresentPolicy::ChoosePresentMode(
        policy.mode, PresentPolicy::ToPolicyMode(static_cast<VkPresentModeKHR>(requested)),
        supported);
    present_mode = static_cast<vk::PresentModeKHR>(PresentPolicy::ToVkMode(chosen));
    if (present_mode != requested) {
        LOG_WARNING(Render, "Requested present mode {} is not supported, falling back to {}.",
                    vk::to_string(requested), vk::to_string(present_mode));
    }
    pacing_mode.store(PresentPolicy::PacingPresentMode(policy.mode, requested_by_name, chosen),
                      std::memory_order_relaxed);
    std::printf("Display: present mode %s (requested %s)\n", vk::to_string(present_mode).c_str(),
                vk::to_string(requested).c_str());
}

vk::Result Swapchain::QueryCapabilities(vk::SurfaceCapabilitiesKHR& capabilities) const {
    if (full_screen_exclusive) {
        // The same chain as the swapchain's creation (VK_EXT_full_screen_exclusive).
        const vk::PhysicalDeviceSurfaceInfo2KHR surface_info{
            .pNext = FullScreenExclusiveDisallowed(true),
            .surface = surface,
        };
        const auto [result2, capabilities2] =
            instance.GetPhysicalDevice().getSurfaceCapabilities2KHR(surface_info);
        capabilities = capabilities2.surfaceCapabilities;
        return result2;
    }
    const auto [result1, capabilities1] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    capabilities = capabilities1;
    return result1;
}

vk::Extent2D Swapchain::ExtentFor(const vk::SurfaceCapabilitiesKHR& capabilities, u32 width_,
                                  u32 height_) const {
    vk::Extent2D result = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        result.width = std::max(capabilities.minImageExtent.width,
                                std::min(capabilities.maxImageExtent.width, width_));
        result.height = std::max(capabilities.minImageExtent.height,
                                 std::min(capabilities.maxImageExtent.height, height_));
    }
    return result;
}

bool Swapchain::SetSurfaceProperties() {
    vk::SurfaceCapabilitiesKHR capabilities{};
    const vk::Result capabilities_result = QueryCapabilities(capabilities);
    if (capabilities_result != vk::Result::eSuccess) {
        // bbport: this was an assertion (exit 23); a lost surface is made again at the next try.
        NoteDisplayEvent("querying the surface capabilities failed", capabilities_result);
        surface_lost = capabilities_result == vk::Result::eErrorSurfaceLostKHR;
        return false;
    }
    extent = ExtentFor(capabilities, width, height);

    // Select number of images in swap chain, we prefer one buffer in the background to work on
    image_count = capabilities.minImageCount + 1 + extra_images; // bbport: + frame generation's
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }

    // Opaque is not supported everywhere.
    composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (!(capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)) {
        composite_alpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
    }
    return true;
}

void Swapchain::EndFsrSwapchain() {
    if (!fsr_fg_swapchain) {
        return;
    }
    const vk::Device device = instance.GetDevice();
    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();
    images.clear();
    fsr_fg->EndSwapchain();
    swapchain = nullptr;
    fsr_fg_swapchain.store(false, std::memory_order_release);
    needs_recreation = true;
}

void Swapchain::WaitIdle() {
    EndFsrSwapchain();
    // bbport: vkDeviceWaitIdle needs every queue of the device externally synchronized: the
    // recording threads submit to the same queue under this lock (Scheduler::submit_mutex).
    std::scoped_lock submit_lock{Scheduler::submit_mutex};
    const auto wait_result = instance.GetDevice().waitIdle();
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }
}

void Swapchain::RecreateSurface() {
    Destroy(); // a swapchain goes before its surface
    instance.GetInstance().destroySurfaceKHR(surface);
    surface = CreateSurface(instance.GetInstance(), window);
    surface_lost = false;
    std::printf("Display: the window's surface was lost; created again\n");
    // bbport: on Wayland the compositor usually ended the game's connection with it, which no new
    // surface brings back (0.9.37 beta 5 report: NVIDIA 615.78 with KDE Plasma; 580.178 ran clean).
    static bool hinted = false;
    if (!hinted && window.GetWindowInfo().type == Frontend::WindowSystemType::Wayland) {
        hinted = true;
        std::printf("Display: on Wayland a lost surface usually means the compositor closed the "
                    "game's connection (a driver and compositor mismatch: NVIDIA 615 drivers on KDE "
                    "Plasma did). Another NVIDIA driver (580 ran clean), or SDL_VIDEODRIVER=x11 "
                    "(XWayland), avoids it\n");
    }
}

void Swapchain::Destroy() {
    vk::Device device = instance.GetDevice();
    WaitIdle();

    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();
    images.clear();

    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
        swapchain = nullptr; // bbport: a failed Create leaves none (IsPresentable)
    }

    for (const auto& sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto& sem : present_ready) {
        device.destroySemaphore(sem);
    }

    image_acquired.clear();
    present_ready.clear();
}

void Swapchain::RefreshSemaphores() {
    const vk::Device device = instance.GetDevice();
    image_acquired.resize(image_count);
    present_ready.resize(image_count);

    for (vk::Semaphore& semaphore : image_acquired) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create image acquired semaphore: {}",
                   vk::to_string(semaphore_result));
        semaphore = sem;
    }
    for (vk::Semaphore& semaphore : present_ready) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create present ready semaphore: {}", vk::to_string(semaphore_result));
        semaphore = sem;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, image_acquired[i], "Swapchain Semaphore: image_acquired {}", i);
        SetObjectName(device, present_ready[i], "Swapchain Semaphore: present_ready {}", i);
    }
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    std::vector<vk::Image> imgs;
    vk::Result images_result = vk::Result::eSuccess;
    if (fsr_fg_swapchain) {
        images_result = fsr_fg->GetImages(imgs);
    } else {
        auto listed = device.getSwapchainImagesKHR(swapchain);
        images_result = listed.result;
        imgs = std::move(listed.value);
    }
    ASSERT_MSG(images_result == vk::Result::eSuccess, "Failed to create swapchain images: {}",
               vk::to_string(images_result));
    images = std::move(imgs);
    image_count = static_cast<u32>(images.size());
    images_view.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        if (images_view[i]) {
            device.destroyImageView(images_view[i]);
        }
        auto [im_view_result, im_view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = view_format, // bbport: the format Create chose (SDR, game HDR, HDR output)
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        });
        ASSERT_MSG(im_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(im_view_result));
        images_view[i] = im_view;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, images[i], "Swapchain Image {}", i);
        SetObjectName(device, images_view[i], "Swapchain ImageView {}", i);
    }
}

} // namespace Vulkan
