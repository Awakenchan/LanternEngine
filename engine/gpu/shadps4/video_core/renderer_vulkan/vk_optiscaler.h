// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: OptiScaler (a DLL players drop next to the executable, often as winmm.dll) hooks
// vkCreateDevice, vkGetDeviceProcAddr, vkQueueSubmit and takes over the NGX entry points: our
// DLSS runs through its own feature, which may call vkDeviceWaitIdle from the thread recording
// the evaluate (a release, or a backend change from its menu). It is allowed; while it is loaded
// a compatibility mode keeps the engine on the paths it handles. BB_OPTISCALER_COMPAT=0 turns
// the whole mode off; each part has its own switch as well. Unset, they come from lantern.ini
// (optiscaler_*, the in-game menu; BbSettings::Values::startup_optiscaler).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace Vulkan::OptiScaler {

struct Info {
    bool loaded = false;
    std::string module;  ///< the file name it was loaded as (winmm.dll, dxgi.dll, ...)
    std::string version; ///< from its version resource, empty when it has none
    std::string frame_generation; ///< its frame generation output from OptiScaler.ini, empty when off
    void* handle = nullptr; ///< its module (HMODULE), which exports the NGX entry points
};

/// Looks once for a loaded OptiScaler and prints one line about it and the compatibility mode.
const Info& Detect();
/// OptiScaler is loaded and BB_OPTISCALER_COMPAT is not 0.
bool Compat();
/// Compat() and the part's own switch (`name`, e.g. "BB_OPTISCALER_SYNC_SUBMIT") is not 0.
bool Part(const char* name);
/// One submit thread: Part("BB_OPTISCALER_SYNC_SUBMIT") with its frame generation on, or the
/// switch set to 1.
bool SyncSubmit();
/// OptiScaler's module as the NGX provider for DLSS when the driver has no NGX core (an AMD or
/// Intel GPU): its exports take the DLSS inputs and run its own upscaler (OptiScaler.ini's
/// VulkanUpscaler) without VK_NVX_binary_import / VK_NVX_image_view_handle. Null without
/// OptiScaler or with BB_OPTISCALER_NGX=0.
void* NgxProvider();
std::filesystem::path PrivateFfxLibrary(const std::filesystem::path& source, const char* name);
const char* FfxFailureHint();

/// OptiScaler keeps a per-thread "a D3D device is being created" flag and skips its vkCreateInstance
/// and vkCreateDevice hooks while it is set (for DXVK/vkd3d-proton). It was found set on the
/// thread that creates our Vulkan objects (2026-10-21 log: both hooks "Skipping because DXVK/VKD3D
/// is creating a D3D device"), so its frame generation never prepared our device or hooked its
/// swapchain, yet started its D3D12 side and lost the GPU. With OptiScaler loaded the instance and
/// the device are created on a thread of their own, where that flag starts clear
/// (BB_OPTISCALER_CLEAN_THREAD=0: on the calling thread).
template <typename F>
auto OnCleanThread(F&& create) {
    if (!Part("BB_OPTISCALER_CLEAN_THREAD")) {
        return create();
    }
    std::optional<decltype(create())> result;
    std::thread([&] { result.emplace(create()); }).join();
    return std::move(*result);
}

} // namespace Vulkan::OptiScaler
