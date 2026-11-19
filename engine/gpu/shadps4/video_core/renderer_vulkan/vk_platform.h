// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include <vector>
#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/string_literal.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"

namespace Frontend {
enum class WindowSystemType : u8;
class WindowSDL;
} // namespace Frontend

namespace Vulkan {

constexpr u32 TargetVulkanApiVersion = VK_API_VERSION_1_3;

vk::SurfaceKHR CreateSurface(vk::Instance instance, const Frontend::WindowSDL& emu_window);

vk::UniqueInstance CreateInstance(Frontend::WindowSystemType window_type, bool enable_validation,
                                  bool enable_crash_diagnostic);

vk::UniqueDebugUtilsMessengerEXT CreateDebugCallback(vk::Instance instance);

/// bbport: whether CreateInstance enabled this instance extension (the last instance created).
bool IsInstanceExtensionEnabled(std::string_view name);

bool QueueFamilyCanPresent(vk::Instance instance, vk::PhysicalDevice physical, u32 family);

/// bbport: VK_EXT_full_screen_exclusive on Windows (the swapchain stays a normal window
/// swapchain: drivers otherwise switch a screen-covering window to exclusive fullscreen, which
/// turned Windows HDR off); nullptr on the other platforms.
const char* FullScreenExclusiveExtensionName();

/// bbport: a VkSurfaceFullScreenExclusiveInfoEXT {DISALLOWED} (its pNext null) for the pNext of
/// the swapchain's creation and of the *2 surface queries; nullptr when `enabled` is false or
/// on the other platforms.
const void* FullScreenExclusiveDisallowed(bool enabled);

/// bbport: the surface's present modes; with `full_screen_exclusive`, as reported for the
/// FullScreenExclusiveDisallowed chain (vkGetPhysicalDeviceSurfacePresentModes2EXT).
vk::ResultValue<std::vector<vk::PresentModeKHR>> GetSurfacePresentModes(
    vk::Instance instance, vk::PhysicalDevice physical, vk::SurfaceKHR surface,
    bool full_screen_exclusive);

template <typename T>
concept VulkanHandleType = vk::isVulkanHandleType<T>::value;

template <VulkanHandleType HandleType>
void SetObjectName(vk::Device device, const HandleType& handle, std::string_view debug_name) {
    const vk::DebugUtilsObjectNameInfoEXT name_info = {
        .objectType = HandleType::objectType,
        .objectHandle = reinterpret_cast<u64>(static_cast<typename HandleType::NativeType>(handle)),
        .pObjectName = debug_name.data(),
    };
    auto result = device.setDebugUtilsObjectNameEXT(name_info);
    if (result != vk::Result::eSuccess) {
        LOG_DEBUG(Render_Vulkan, "Could not set object debug name: {}", vk::to_string(result));
    }
}

template <VulkanHandleType HandleType, typename... Args>
void SetObjectName(vk::Device device, const HandleType& handle, const char* format,
                   const Args&... args) {
    const std::string debug_name = fmt::vformat(format, fmt::make_format_args(args...));
    SetObjectName(device, handle, debug_name);
}

template <StringLiteral msg = "">
static void Check(vk::Result r) {
    if (r == vk::Result::eErrorDeviceLost) {
        Breadcrumbs::ReportDeviceLost("a Vulkan call");
    }
    if constexpr (msg.len <= 1) {
        ASSERT_MSG(r == vk::Result::eSuccess, "vk::Result={}", vk::to_string(r));
    } else {
        ASSERT_MSG(r == vk::Result::eSuccess, "Failed to {}: vk::Result={}", msg.value,
                   vk::to_string(r));
    }
}

template <StringLiteral msg = "", typename T>
static T Check(vk::ResultValue<T> r) {
    if (r.result == vk::Result::eErrorDeviceLost) {
        Breadcrumbs::ReportDeviceLost("a Vulkan call");
    }
    if constexpr (msg.len <= 1) {
        ASSERT_MSG(r.result == vk::Result::eSuccess, "vk::Result={}", vk::to_string(r.result));
    } else {
        ASSERT_MSG(r.result == vk::Result::eSuccess, "Failed to {}: vk::Result={}", msg.value,
                   vk::to_string(r.result));
    }
    return std::move(r.value);
}

} // namespace Vulkan
