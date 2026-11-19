// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_streamline.h"

namespace Vulkan {

class Instance;

class FsrFrameGen {
public:
    static constexpr const char* DllName = "amd_fidelityfx_framegeneration_vk.dll";
    static constexpr const char* LoaderName = "amd_fidelityfx_loader_vk.dll";

    static bool SelectedAtStart();
    static bool FilesPresent();
    static void Fallback(const char* reason);

    explicit FsrFrameGen(const Instance& instance);
    ~FsrFrameGen();

    [[nodiscard]] bool Usable() const;
    [[nodiscard]] bool Active() const;
    void Fail(const char* reason);

    bool CreateSwapchain(const vk::SwapchainCreateInfoKHR& info, vk::SwapchainKHR& out);
    void EndSwapchain();
    vk::Result GetImages(std::vector<vk::Image>& images);
    vk::Result Acquire(u64 timeout_ns, vk::Semaphore semaphore, u32& index);
    vk::Result Present(vk::Queue queue, const vk::PresentInfoKHR& info);
    void SetHdrMetadata(const vk::HdrMetadataEXT& metadata);

    void EnsureSlot(u32 slot, u32 width, u32 height);
    void RecordInputs(vk::CommandBuffer cmdbuf, u32 slot, vk::Image depth, vk::Image motion, u32 width,
                      u32 height);
    void RecordPresent(vk::CommandBuffer cmdbuf, const DlssFrameInfo* frame, vk::Image hudless,
                       vk::Extent2D frame_extent, vk::Format format, vk::Extent2D extent, vk::Rect2D fit,
                       bool menu_open, bool hdr);
    void AfterPresent(bool game_frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
