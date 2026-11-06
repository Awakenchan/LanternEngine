// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#include "video_core/host_shaders/contact_shadow_comp.h"
#include "video_core/host_shaders/gtao_frag.h"
#include "video_core/host_shaders/native_motion_blur_frag.h"
#include "video_core/host_shaders/point_shadow_comp.h"
#include "video_core/host_shaders/point_volumetric_comp.h"
#include "video_core/host_shaders/ssgi_composite_comp.h"
#include "video_core/host_shaders/ssgi_denoise_comp.h"
#include "video_core/host_shaders/ssgi_gather_comp.h"
#include "video_core/host_shaders/ssgi_prep_comp.h"
#include "video_core/host_shaders/ssr_frag.h"
#include "video_core/host_shaders/ssr_temporal_copy_comp.h"
#include "video_core/host_shaders/ssr_temporal_resolve_comp.h"
#include "video_core/host_shaders/ssr_trace_comp.h"
#include "video_core/host_shaders/volumetric_light_composite_comp.h"
#include "video_core/host_shaders/volumetric_light_filter_comp.h"
#include "video_core/host_shaders/volumetric_light_integrate_comp.h"
#include "video_core/host_shaders/volumetric_light_repair_comp.h"
#include "video_core/host_shaders/volumetric_light_tint_comp.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

using u32 = uint32_t;

namespace {
u32 messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                       VkDebugUtilsMessageTypeFlagsEXT,
                                       const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++messages;
        std::printf("%s\n", data->pMessage);
    }
    return VK_FALSE;
}

struct Pass {
    const char* name;
    std::span<const u32> code;
    std::vector<vk::DescriptorType> types;
    u32 push;
};
}

int main() {
    static vk::detail::DynamicLoader loader;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    const vk::ApplicationInfo app{.pApplicationName = "effect-pipelines-test",
                                  .apiVersion = VK_API_VERSION_1_3};
    const char* debug_utils = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    auto instance = vk::createInstanceUnique({.pApplicationInfo = &app,
                                              .enabledExtensionCount = 1,
                                              .ppEnabledExtensionNames = &debug_utils})
                        .value;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);
    auto messenger =
        instance
            ->createDebugUtilsMessengerEXTUnique(
                {.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                                    vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
                 .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                                vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral,
                 .pfnUserCallback =
                     reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(Message)})
            .value;
    const auto gpus = instance->enumeratePhysicalDevices().value;
    assert(!gpus.empty());
    const auto physical = gpus[0];
    std::printf("device: %s\n", physical.getProperties().deviceName.data());
    const auto supported = physical.getFeatures();
    const float priority = 1;
    const vk::DeviceQueueCreateInfo queue{.queueFamilyIndex = 0,
                                          .queueCount = 1,
                                          .pQueuePriorities = &priority};
    const char* extension = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    const vk::PhysicalDeviceFeatures features{
        .shaderStorageImageExtendedFormats = supported.shaderStorageImageExtendedFormats};
    vk::PhysicalDeviceVulkan12Features v12{.scalarBlockLayout = true,
                                           .uniformBufferStandardLayout = true};
    auto owned = physical
                     .createDeviceUnique({.pNext = &v12,
                                          .queueCreateInfoCount = 1,
                                          .pQueueCreateInfos = &queue,
                                          .enabledExtensionCount = 1,
                                          .ppEnabledExtensionNames = &extension,
                                          .pEnabledFeatures = &features})
                     .value;
    const auto device = *owned;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

    using enum vk::DescriptorType;
    const std::vector<Pass> passes = {
        {"ssgi prep", SSGI_PREP_COMP, {eSampledImage, eStorageImage, eSampledImage}, 128},
        {"ssgi gather",
         SSGI_GATHER_COMP,
         {eSampledImage, eSampledImage, eSampledImage, eStorageImage, eSampledImage},
         128},
        {"ssgi denoise", SSGI_DENOISE_COMP, {eSampledImage, eSampledImage, eStorageImage}, 128},
        {"ssgi composite",
         SSGI_COMPOSITE_COMP,
         {eSampledImage, eSampledImage, eStorageImage, eSampledImage, eSampledImage},
         128},
        {"ssr resolve",
         SSR_TEMPORAL_RESOLVE_COMP,
         {eSampledImage, eSampledImage, eSampledImage, eStorageImage},
         128},
        {"ssr copy", SSR_TEMPORAL_COPY_COMP, {eSampledImage, eStorageImage}, 128},
        {"ssr trace",
         SSR_TRACE_COMP,
         {eUniformBuffer, eCombinedImageSampler, eCombinedImageSampler, eCombinedImageSampler,
          eStorageImage},
         16},
        {"contact shadows",
         CONTACT_SHADOW_COMP,
         {eStorageBuffer, eCombinedImageSampler, eStorageImage, eCombinedImageSampler,
          eStorageBuffer},
         68},
        {"point shadows",
         POINT_SHADOW_COMP,
         {eStorageBuffer, eCombinedImageSampler, eStorageImage, eStorageImage, eStorageImage,
          eStorageImage},
         64},
        {"volumetric integrate",
         VOLUMETRIC_LIGHT_INTEGRATE_COMP,
         {eStorageBuffer, eStorageBuffer, eCombinedImageSampler, eCombinedImageSampler,
          eStorageImage, eStorageImage},
         32},
        {"volumetric repair",
         VOLUMETRIC_LIGHT_REPAIR_COMP,
         {eStorageBuffer, eStorageBuffer, eCombinedImageSampler, eCombinedImageSampler,
          eStorageImage, eStorageImage, eStorageImage},
         32},
        {"volumetric tint",
         VOLUMETRIC_LIGHT_TINT_COMP,
         {eStorageBuffer, eStorageBuffer},
         16},
        {"volumetric filter",
         VOLUMETRIC_LIGHT_FILTER_COMP,
         {eCombinedImageSampler, eStorageImage},
         4},
        {"volumetric composite",
         VOLUMETRIC_LIGHT_COMPOSITE_COMP,
         {eCombinedImageSampler, eCombinedImageSampler, eStorageImage, eStorageBuffer,
          eCombinedImageSampler, eCombinedImageSampler},
         4},
        {"point volumetric",
         POINT_VOLUMETRIC_COMP,
         {eStorageBuffer, eStorageBuffer, eCombinedImageSampler, eStorageImage},
         12},
    };
    u32 built = 0;
    for (const Pass& pass : passes) {
        std::vector<vk::DescriptorSetLayoutBinding> bindings;
        for (const auto type : pass.types) {
            bindings.push_back({.binding = u32(bindings.size()),
                                .descriptorType = type,
                                .descriptorCount = 1,
                                .stageFlags = vk::ShaderStageFlagBits::eCompute});
        }
        auto set = device
                       .createDescriptorSetLayoutUnique(
                           {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                            .bindingCount = u32(bindings.size()),
                            .pBindings = bindings.data()})
                       .value;
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = pass.push};
        auto layout = device
                          .createPipelineLayoutUnique({.setLayoutCount = 1,
                                                       .pSetLayouts = &*set,
                                                       .pushConstantRangeCount = 1,
                                                       .pPushConstantRanges = &push})
                          .value;
        auto module = device
                          .createShaderModuleUnique({.codeSize = pass.code.size_bytes(),
                                                     .pCode = pass.code.data()})
                          .value;
        auto pipeline = device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{.flags = vk::PipelineCreateFlagBits::eDispatchBase,
                                              .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                                                        .module = *module,
                                                        .pName = "main"},
                                              .layout = *layout});
        if (pipeline.result != vk::Result::eSuccess) {
            std::printf("%s: %s\n", pass.name, vk::to_string(pipeline.result).c_str());
            return 1;
        }
        ++built;
    }
    for (const auto code : {std::span<const u32>(GTAO_FRAG), std::span<const u32>(SSR_FRAG),
                            std::span<const u32>(NATIVE_MOTION_BLUR_FRAG)}) {
        auto module = device.createShaderModuleUnique(
            {.codeSize = code.size_bytes(), .pCode = code.data()});
        assert(module.result == vk::Result::eSuccess);
    }
    std::printf("effect pipelines: %u built, %u validation messages\n", built, messages);
    return messages == 0 ? 0 : 1;
}
