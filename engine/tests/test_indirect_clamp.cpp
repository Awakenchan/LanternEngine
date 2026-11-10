// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "video_core/host_shaders/indirect_clamp_comp.h"

namespace {

#define CHECK(call)                                                                               \
    do {                                                                                          \
        const VkResult result_ = (call);                                                          \
        if (result_ != VK_SUCCESS) {                                                              \
            std::fprintf(stderr, "%s failed: %d\n", #call, int(result_));                         \
            std::exit(1);                                                                         \
        }                                                                                         \
    } while (0)

struct Vk {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = 0;
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceLimits limits{};
    PFN_vkCmdPushDescriptorSetKHR push_descriptors{};
};

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    uint32_t* mapped = nullptr;
};

Buffer MakeBuffer(const Vk& vk, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    CHECK(vkCreateBuffer(vk.device, &info, nullptr, &b.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk.device, b.buffer, &requirements);
    const VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount && type == ~0u; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (vk.memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
        }
    }
    if (type == ~0u) {
        std::fprintf(stderr, "no host-visible memory type\n");
        std::exit(1);
    }
    const VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = requirements.size,
                                     .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(vk.device, &alloc, nullptr, &b.memory));
    CHECK(vkBindBufferMemory(vk.device, b.buffer, b.memory, 0));
    void* mapped = nullptr;
    CHECK(vkMapMemory(vk.device, b.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    b.mapped = static_cast<uint32_t*>(mapped);
    std::memset(mapped, 0, size);
    return b;
}

Vk Init() {
    Vk vk;
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pApplicationName = "indirect-clamp-test",
                                .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                             .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&instance_info, nullptr, &vk.instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(vk.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(vk.instance, &count, devices.data()));
    const char* wanted = std::getenv("BB_TEST_GPU");
    for (uint32_t i = 0; i < count && !vk.physical; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if ((wanted && uint32_t(std::atoi(wanted)) != i) || properties.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }
        vk.physical = devices[i];
        vk.limits = properties.limits;
        std::printf("Indirect clamp: %s, max %u x %u x %u groups, storage alignment %llu\n",
                    properties.deviceName, vk.limits.maxComputeWorkGroupCount[0],
                    vk.limits.maxComputeWorkGroupCount[1], vk.limits.maxComputeWorkGroupCount[2],
                    (unsigned long long)vk.limits.minStorageBufferOffsetAlignment);
    }
    if (!vk.physical) {
        std::fprintf(stderr, "no Vulkan 1.3 device\n");
        std::exit(1);
    }
    vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_properties(families);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, family_properties.data());
    while (!(family_properties[vk.family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        ++vk.family;
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                             .queueFamilyIndex = vk.family,
                                             .queueCount = 1,
                                             .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .synchronization2 = VK_TRUE};
    const char* extensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    const VkDeviceCreateInfo device_info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                         .pNext = &features13,
                                         .queueCreateInfoCount = 1,
                                         .pQueueCreateInfos = &queue_info,
                                         .enabledExtensionCount = 1,
                                         .ppEnabledExtensionNames = extensions};
    CHECK(vkCreateDevice(vk.physical, &device_info, nullptr, &vk.device));
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    vk.push_descriptors = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(vk.device, "vkCmdPushDescriptorSetKHR"));
    if (!vk.push_descriptors) {
        std::fprintf(stderr, "no vkCmdPushDescriptorSetKHR\n");
        std::exit(1);
    }
    const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                            .queueFamilyIndex = vk.family};
    CHECK(vkCreateCommandPool(vk.device, &pool_info, nullptr, &vk.pool));
    const VkCommandBufferAllocateInfo cmd_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                               .commandPool = vk.pool,
                                               .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(vk.device, &cmd_info, &vk.cmd));
    return vk;
}

}

int main() {
    Vk vk = Init();
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    const VkDescriptorSetLayoutCreateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = 2,
        .pBindings = bindings.data()};
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(vk.device, &set_info, nullptr, &set_layout));
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    const VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                 .setLayoutCount = 1,
                                                 .pSetLayouts = &set_layout,
                                                 .pushConstantRangeCount = 1,
                                                 .pPushConstantRanges = &range};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &layout));
    const VkShaderModuleCreateInfo module_info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                               .codeSize = sizeof(INDIRECT_CLAMP_COMP),
                                               .pCode = INDIRECT_CLAMP_COMP};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(vk.device, &module_info, nullptr, &module));
    const VkComputePipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = layout};
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));
    vkDestroyShaderModule(vk.device, module, nullptr);

    const uint64_t align = vk.limits.minStorageBufferOffsetAlignment;
    Buffer game = MakeBuffer(vk, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer stream = MakeBuffer(vk, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    Buffer scratch = MakeBuffer(vk, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(scratch.mapped, 0, 4096);
    const uint64_t wrong_at = align + 12, sane_at = 8 * align + 4;
    const std::array<uint32_t, 3> wrong{3063677180u, 1, 1}, sane{5, 3, 2};
    std::memcpy(reinterpret_cast<uint8_t*>(game.mapped) + wrong_at, wrong.data(), 12);
    std::memcpy(reinterpret_cast<uint8_t*>(game.mapped) + sane_at, sane.data(), 12);
    const std::array<uint64_t, 2> slots{0, align};

    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(vk.cmd, &begin));
    const std::array<uint64_t, 2> args{wrong_at, sane_at};
    for (uint32_t n = 0; n < 2; ++n) {
        const uint64_t offset = args[n];
        const uint64_t src_offset = offset / align * align;
        const uint64_t src_size = offset - src_offset + 12;
        const std::array<VkDescriptorBufferInfo, 2> infos{{{game.buffer, src_offset, src_size},
                                                           {stream.buffer, slots[n], 16}}};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         .dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         .pBufferInfo = &infos[i]};
        }
        const std::array<uint32_t, 4> push{std::min(vk.limits.maxComputeWorkGroupCount[0], 65535u),
                                           std::min(vk.limits.maxComputeWorkGroupCount[1], 65535u),
                                           std::min(vk.limits.maxComputeWorkGroupCount[2], 65535u),
                                           uint32_t((offset - src_offset) / 4)};
        vkCmdBindPipeline(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vk.push_descriptors(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, writes.data());
        vkCmdPushConstants(vk.cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push.data());
        vkCmdDispatch(vk.cmd, 1, 1, 1);
        const VkMemoryBarrier2 clamped{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                                       .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .memoryBarrierCount = 1,
                                          .pMemoryBarriers = &clamped};
        vkCmdPipelineBarrier2(vk.cmd, &dependency);
        const std::array<VkDescriptorBufferInfo, 2> again{{{game.buffer, src_offset, src_size},
                                                           {scratch.buffer, slots[n], 16}}};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].pBufferInfo = &again[i];
        }
        vk.push_descriptors(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, writes.data());
        vkCmdDispatchIndirect(vk.cmd, stream.buffer, slots[n]);
    }
    const VkMemoryBarrier2 to_host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                   .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo host_dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                           .memoryBarrierCount = 1,
                                           .pMemoryBarriers = &to_host};
    vkCmdPipelineBarrier2(vk.cmd, &host_dependency);
    CHECK(vkEndCommandBuffer(vk.cmd));
    const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &vk.cmd};
    CHECK(vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(vk.queue));

    int failures = 0;
    const auto expect = [&](const char* what, const uint32_t* got, const std::array<uint32_t, 3>& want) {
        const bool ok = got[0] == want[0] && got[1] == want[1] && got[2] == want[2];
        std::printf("%s: %u x %u x %u (want %u x %u x %u) %s\n", what, got[0], got[1], got[2], want[0],
                    want[1], want[2], ok ? "ok" : "WRONG");
        failures += !ok;
    };
    const std::array<uint32_t, 3> none{0, 0, 0};
    expect("wrong counts run no groups", stream.mapped + slots[0] / 4, none);
    expect("sane counts kept", stream.mapped + slots[1] / 4, sane);
    expect("indirect dispatch of the wrong counts wrote nothing", scratch.mapped + slots[0] / 4, none);
    expect("indirect dispatch of the sane counts", scratch.mapped + slots[1] / 4, sane);
    std::printf("Indirect clamp: %s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
