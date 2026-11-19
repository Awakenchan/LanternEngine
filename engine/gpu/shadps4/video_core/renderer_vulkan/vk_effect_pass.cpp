// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/vk_effect_pass.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <vector>

#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

#include <vk_mem_alloc.h>

namespace Vulkan::EffectPass {

namespace {
constexpr vk::DeviceSize KeepFree = 256ull << 20;

bool Good(vk::Result result, const char* name, const char* what) {
    if (result == vk::Result::eSuccess) {
        return true;
    }
    std::printf("%s: off - %s failed (%s)\n", name, what, vk::to_string(result).c_str());
    return false;
}
}

bool Create(const Instance& instance, vk::UniqueDescriptorSetLayout& set_layout,
            vk::UniquePipelineLayout& layout, vk::UniquePipeline& pipeline,
            std::initializer_list<vk::DescriptorType> types, u32 push_size,
            std::span<const u32> code, const char* name, bool dispatch_base,
            const vk::SpecializationInfo* specialization) {
    const auto device = instance.GetDevice();
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    bindings.reserve(types.size());
    for (const vk::DescriptorType type : types) {
        bindings.push_back({u32(bindings.size()), type, 1, vk::ShaderStageFlagBits::eCompute});
    }
    auto set = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    });
    if (!Good(set.result, name, "the descriptor set layout")) {
        return false;
    }
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, push_size};
    auto made = device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*set.value,
        .pushConstantRangeCount = push_size ? 1u : 0u,
        .pPushConstantRanges = push_size ? &push : nullptr,
    });
    if (!Good(made.result, name, "the pipeline layout")) {
        return false;
    }
    const auto [module_result, module] =
        device.createShaderModule({.codeSize = code.size_bytes(), .pCode = code.data()});
    if (!Good(module_result, name, "the shader module")) {
        return false;
    }
    SetObjectName(device, module, "{}", name);
    auto built = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .flags = dispatch_base ? vk::PipelineCreateFlagBits::eDispatchBase
                                       : vk::PipelineCreateFlags{},
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main",
                          .pSpecializationInfo = specialization},
                .layout = *made.value,
            });
    device.destroyShaderModule(module);
    if (!Good(built.result, name, "the pipeline")) {
        return false;
    }
    set_layout = std::move(set.value);
    layout = std::move(made.value);
    pipeline = std::move(built.value);
    return true;
}

bool CreateSampler(const Instance& instance, vk::UniqueSampler& sampler, vk::Filter filter) {
    auto made = instance.GetDevice().createSamplerUnique({
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 0.0f,
    });
    if (made.result != vk::Result::eSuccess) {
        std::printf("Effects: off - no sampler (%s)\n", vk::to_string(made.result).c_str());
        return false;
    }
    sampler = std::move(made.value);
    return true;
}

bool StorageFormat(const Instance& instance, vk::Format format) {
    const auto properties = instance.GetPhysicalDevice().getFormatProperties(format);
    return bool(properties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage);
}

void Mark(u64 key, const char* name) {
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0x4C49'0000ull | key, [name] { return std::string{"Lighting: "} + name; });
    }
}

bool HasRoom(const Instance& instance, vk::DeviceSize bytes) {
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(instance.GetAllocator(), budgets.data());
    const auto memory = instance.GetPhysicalDevice().getMemoryProperties();
    vk::DeviceSize best = 0;
    for (u32 heap = 0; heap < memory.memoryHeapCount; ++heap) {
        if (!(memory.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal)) {
            continue;
        }
        const vk::DeviceSize budget = budgets[heap].budget, usage = budgets[heap].usage;
        best = std::max(best, budget > usage ? budget - usage : 0);
    }
    return best >= bytes + KeepFree;
}

}
