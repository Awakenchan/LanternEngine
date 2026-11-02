// SPDX-License-Identifier: GPL-3.0-or-later
// The volumetric sunlight's color: which lighting table record it follows, and its fade.
#include <vulkan/vulkan.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include "video_core/host_shaders/volumetric_light_tint_comp.h"

namespace {
#define CHECK(call) do { if ((call) != VK_SUCCESS) { std::fprintf(stderr, "%s failed\n", #call); std::abort(); } } while (false)
constexpr unsigned RecordFloats = 72, Records = 16, TableStart = 64;
struct Push {
    unsigned offset_bytes = TableStart;
    unsigned record_count = Records;
    float blend = 1.0f;
};
static_assert(sizeof(Push) == 12);
struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void* data{}; unsigned size{}; };
struct Rgb { float r, g, b; };

struct Test {
    VkInstance instance{};
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkDescriptorSetLayout set{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    PFN_vkCmdPushDescriptorSetKHR descriptors{};
    std::vector<Buffer> buffers;

    unsigned Memory(unsigned bits, VkMemoryPropertyFlags flags) {
        for (unsigned i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        std::abort();
    }
    Buffer MakeBuffer(unsigned size) {
        Buffer b{.size = size};
        VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        CHECK(vkCreateBuffer(device, &info, nullptr, &b.buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size, .memoryTypeIndex = Memory(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
        CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.data));
        std::memset(b.data, 0, size);
        buffers.push_back(b);
        return b;
    }
    Test() {
        VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "volumetric-tint-test", .apiVersion = VK_API_VERSION_1_3};
        VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
        CHECK(vkCreateInstance(&ci, nullptr, &instance));
        unsigned count = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        assert(count);
        const VkPhysicalDevice gpu = devices[0];
        vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
        unsigned families_count = 0, family = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(families_count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families_count, families.data());
        while (family < families_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
        assert(family < families_count);
        const float priority = 1;
        VkDeviceQueueCreateInfo qi{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
        const char* extension = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .scalarBlockLayout = VK_TRUE};
        VkDeviceCreateInfo di{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &v12,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
            .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
        CHECK(vkCreateDevice(gpu, &di, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        descriptors = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
        assert(descriptors);
        VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
        CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        for (unsigned i = 0; i < bindings.size(); ++i)
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo si{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = unsigned(bindings.size()), .pBindings = bindings.data()};
        CHECK(vkCreateDescriptorSetLayout(device, &si, nullptr, &set));
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &set, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        CHECK(vkCreatePipelineLayout(device, &li, nullptr, &layout));
        VkShaderModule module;
        VkShaderModuleCreateInfo mi{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(VOLUMETRIC_LIGHT_TINT_COMP), .pCode = VOLUMETRIC_LIGHT_TINT_COMP};
        CHECK(vkCreateShaderModule(device, &mi, nullptr, &module));
        VkComputePipelineCreateInfo pc{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = layout};
        CHECK(vkCreateComputePipelines(device, {}, 1, &pc, nullptr, &pipeline));
        vkDestroyShaderModule(device, module, nullptr);
    }
    ~Test() {
        vkDeviceWaitIdle(device);
        for (auto b : buffers) {
            vkUnmapMemory(device, b.memory);
            vkDestroyBuffer(device, b.buffer, nullptr);
            vkFreeMemory(device, b.memory, nullptr);
        }
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};

/// What the tint pass makes of a chosen record's color.
Rgb Target(Rgb c) {
    const float luminance = c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f;
    return {luminance + (c.r - luminance) * 0.75f, luminance + (c.g - luminance) * 0.75f,
            luminance + (c.b - luminance) * 0.75f};
}

Rgb Mix(Rgb a, Rgb b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
}

int main() {
    Test t;
    const auto table = t.MakeBuffer(TableStart + Records * RecordFloats * sizeof(float));
    const auto state = t.MakeBuffer(16);
    auto* floats = static_cast<float*>(table.data) + TableStart / sizeof(float);
    auto* color = static_cast<float*>(state.data);

    const auto clear = [&] { std::memset(table.data, 0, table.size); };
    const auto set_record = [&](unsigned id, Rgb c, Rgb second, Rgb direction) {
        float* r = floats + id * RecordFloats;
        r[0] = direction.r; r[1] = direction.g; r[2] = direction.b;
        r[16] = c.r; r[17] = c.g; r[18] = c.b;
        r[20] = second.r; r[21] = second.g; r[22] = second.b;
    };
    // The fallback record as the game lists it, seen with z 2.22e-07 and -3.091e-08.
    const auto set_fallback = [&](unsigned id) {
        set_record(id, {1, 1, 1}, {0, 0, 0}, {0.7071068f, 0.7071068f, -3.091e-08f});
    };
    const Rgb moon{0.556f, 0.730f, 0.965f}, moon_second{0.019f, 0.030f, 0.100f},
        moon_direction{0.672f, 0.707f, 0.219f}, dim{0.393f, 0.603f, 0.923f};
    const auto run = [&](float blend = 1.0f) {
        Push push;
        push.blend = blend;
        CHECK(vkResetCommandBuffer(t.cmd, 0));
        VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(t.cmd, &begin));
        const VkDescriptorBufferInfo ti{table.buffer, 0, table.size}, si{state.buffer, 0, state.size};
        const std::array<VkWriteDescriptorSet, 2> writes{{
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &ti},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &si},
        }};
        vkCmdBindPipeline(t.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, t.pipeline);
        t.descriptors(t.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, t.layout, 0, 2, writes.data());
        vkCmdPushConstants(t.cmd, t.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdDispatch(t.cmd, 1, 1, 1);
        VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &host, 0, nullptr, 0, nullptr);
        CHECK(vkEndCommandBuffer(t.cmd));
        VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &t.cmd};
        CHECK(vkQueueSubmit(t.queue, 1, &submit, {}));
        CHECK(vkQueueWaitIdle(t.queue));
    };
    const auto expect = [&](Rgb want) {
        const bool close = std::abs(color[0] - want.r) < 1e-4f && std::abs(color[1] - want.g) < 1e-4f &&
                           std::abs(color[2] - want.b) < 1e-4f && color[3] == 1.0f;
        if (!close) {
            std::printf("got %f %f %f %f, want %f %f %f\n", color[0], color[1], color[2], color[3],
                        want.r, want.g, want.b);
        }
        assert(close);
    };

    // The fallback record does not beat the area's lights, wherever it sits in the table.
    for (const unsigned slot : {0u, 2u, 5u, Records - 1}) {
        clear();
        set_record(1, moon, moon_second, moon_direction);
        set_record(3, dim, moon_second, moon_direction);
        set_fallback(slot);
        run();
        expect(Target(moon));
    }
    // The same table with and without the fallback gives the same color: the flip in game.
    clear();
    set_record(1, moon, moon_second, moon_direction);
    set_record(5, dim, moon_second, moon_direction);
    run();
    expect(Target(moon));
    set_fallback(5);
    run();
    expect(Target(moon));

    // With nothing else lit it is used, so the fog does not go black.
    clear();
    set_fallback(4);
    run();
    expect({1, 1, 1});
    // Nothing lit at all.
    clear();
    run();
    expect({0, 0, 0});

    // Only that exact record is skipped: other white lights still count.
    clear();
    set_record(1, moon, moon_second, moon_direction);
    set_record(2, {1, 1, 1}, moon_second, {0.7071068f, 0.7071068f, 0});
    run();
    expect({1, 1, 1});
    clear();
    set_record(1, moon, moon_second, moon_direction);
    set_record(2, {1, 1, 1}, {0, 0, 0}, moon_direction);
    run();
    expect({1, 1, 1});

    // Brightest still wins among the rest, and bright values are scaled down to at most 1.
    clear();
    set_record(1, dim, moon_second, moon_direction);
    set_record(2, {2.0f, 1.5f, 1.0f}, moon_second, moon_direction);
    run();
    expect(Target({1.0f, 0.75f, 0.5f}));

    // Fades: part of the way from the stored color, and the rest over later frames.
    clear();
    set_record(1, moon, moon_second, moon_direction);
    color[0] = color[1] = color[2] = 1.0f;
    run(0.25f);
    expect(Mix({1, 1, 1}, Target(moon), 0.25f));
    const Rgb step{color[0], color[1], color[2]};
    run(0.5f);
    expect(Mix(step, Target(moon), 0.5f));
    for (int i = 0; i < 200; ++i) {
        run(0.1f);
    }
    expect(Target(moon));

    // Blend 1 sets the color whatever the buffer holds (it starts out undefined).
    color[0] = color[1] = color[2] = std::numeric_limits<float>::quiet_NaN();
    run();
    expect(Target(moon));

    std::puts("Volumetric tint: PASS (fallback record skipped in any slot, used alone, other white "
              "lights kept, brightest wins, fade, first frame)");
}
