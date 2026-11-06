// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "video_core/host_shaders/point_volumetric_comp.h"

namespace {
#define CHECK(call) do { if ((call) != VK_SUCCESS) { std::fprintf(stderr, "%s failed\n", #call); std::abort(); } } while (false)
constexpr unsigned W = 19, H = 11;
struct Push {
    unsigned count = 2;
    float intensity = 1.0f;
    float glow_size = 0.08f;
};
static_assert(sizeof(Push) == 12);
struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void* data{}; unsigned size{}; };
struct Image { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; };
struct Test {
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkDescriptorSetLayout set{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkSampler sampler{};
    PFN_vkCmdPushDescriptorSetKHR descriptors{};
    std::vector<Buffer> buffers;
    std::vector<Image> images;

    unsigned Memory(unsigned bits, VkMemoryPropertyFlags flags) {
        for (unsigned i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        std::abort();
    }
    Buffer MakeBuffer(unsigned size) {
        Buffer b{.size = size};
        VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT};
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
    Image MakeImage(VkFormat format) {
        Image i;
        VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {W, H, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateImage(device, &info, nullptr, &i.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, i.image, &req);
        VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size, .memoryTypeIndex = Memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(device, &alloc, nullptr, &i.memory));
        CHECK(vkBindImageMemory(device, i.image, i.memory, 0));
        VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = i.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(device, &view, nullptr, &i.view));
        images.push_back(i);
        return i;
    }
    Test() {
        VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "point-volumetric-test", .apiVersion = VK_API_VERSION_1_3};
        VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
        CHECK(vkCreateInstance(&ci, nullptr, &instance));
        unsigned count = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        assert(count);
        gpu = devices[0];
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
        VkDeviceCreateInfo di{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
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
        const std::array<VkDescriptorType, 4> types{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        for (unsigned i = 0; i < bindings.size(); ++i)
            bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
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
            .codeSize = sizeof(POINT_VOLUMETRIC_COMP), .pCode = POINT_VOLUMETRIC_COMP};
        CHECK(vkCreateShaderModule(device, &mi, nullptr, &module));
        VkComputePipelineCreateInfo pc{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = layout};
        CHECK(vkCreateComputePipelines(device, {}, 1, &pc, nullptr, &pipeline));
        vkDestroyShaderModule(device, module, nullptr);
        VkSamplerCreateInfo sm{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        CHECK(vkCreateSampler(device, &sm, nullptr, &sampler));
    }
    void Barrier(VkImage image, VkImageLayout old_layout = VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0u : VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }
    ~Test() {
        vkDeviceWaitIdle(device);
        for (auto i : images) {
            vkDestroyImageView(device, i.view, nullptr);
            vkDestroyImage(device, i.image, nullptr);
            vkFreeMemory(device, i.memory, nullptr);
        }
        for (auto b : buffers) {
            vkUnmapMemory(device, b.memory);
            vkDestroyBuffer(device, b.buffer, nullptr);
            vkFreeMemory(device, b.memory, nullptr);
        }
        vkDestroySampler(device, sampler, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};
}

int main() {
    Test t;
    const auto cb = t.MakeBuffer(256), lights = t.MakeBuffer(1024), result = t.MakeBuffer(W * H * 8);
    const auto depth = t.MakeImage(VK_FORMAT_R32_SFLOAT), fog = t.MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT);
    auto* constants = static_cast<float*>(cb.data);
    for (unsigned i = 0; i < 4; ++i) constants[i * 5] = 1;
    auto* light = static_cast<float*>(lights.data);
    light[2] = 1; light[3] = 0.01f; light[4] = 1; light[5] = 0.5f; light[6] = 0.25f;
    light[16] = 0.2f; light[18] = 0.3f; light[19] = 4.0f; light[20] = 8; light[21] = 8; light[22] = 8;
    Push push;
    bool initialized = false;
    const auto run = [&](float scene_depth) {
        CHECK(vkResetCommandBuffer(t.cmd, 0));
        VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(t.cmd, &begin));
        t.Barrier(depth.image, initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
        t.Barrier(fog.image, initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue scene{{scene_depth, 0, 0, 0}}, zero{};
        vkCmdClearColorImage(t.cmd, depth.image, VK_IMAGE_LAYOUT_GENERAL, &scene, 1, &range);
        vkCmdClearColorImage(t.cmd, fog.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        t.Barrier(depth.image); t.Barrier(fog.image);
        const VkDescriptorBufferInfo cbi{cb.buffer, 0, cb.size}, li{lights.buffer, 0, lights.size};
        const VkDescriptorImageInfo d{t.sampler, depth.view, VK_IMAGE_LAYOUT_GENERAL},
            o{{}, fog.view, VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 4> writes{{
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &cbi},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &li},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &d},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &o},
        }};
        vkCmdBindPipeline(t.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, t.pipeline);
        t.descriptors(t.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, t.layout, 0, 4, writes.data());
        vkCmdPushConstants(t.cmd, t.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdDispatch(t.cmd, (W + 7) / 8, (H + 7) / 8, 1);
        t.Barrier(fog.image);
        VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
        vkCmdCopyImageToBuffer(t.cmd, fog.image, VK_IMAGE_LAYOUT_GENERAL, result.buffer, 1, &copy);
        VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &host, 0, nullptr, 0, nullptr);
        CHECK(vkEndCommandBuffer(t.cmd));
        VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &t.cmd};
        CHECK(vkQueueSubmit(t.queue, 1, &submit, {}));
        CHECK(vkQueueWaitIdle(t.queue));
        initialized = true;
        const auto* pixels = static_cast<const uint16_t*>(result.data);
        return std::vector<uint16_t>(pixels, pixels + W * H * 4);
    };
    const auto half_to_float = [](uint16_t h) {
        const int exponent = (h >> 10) & 31, mantissa = h & 1023;
        const float value = exponent == 0 ? std::ldexp(float(mantissa), -24)
                                          : std::ldexp(float(mantissa + 1024), exponent - 25);
        return (h & 0x8000) ? -value : value;
    };
    const auto reference = [&](unsigned x, unsigned y, float scene_depth, unsigned channel) {
        const float u = (float(x) + 0.5f) / W * 2 - 1, v = 1 - (float(y) + 0.5f) / H * 2;
        const double z = scene_depth == 1.0f ? 0.5 : scene_depth;
        const double len = std::sqrt(double(u) * u + double(v) * v + z * z);
        const double ray[3]{u / len, v / len, z / len};
        const double limit = scene_depth == 1.0f ? 100.0 : std::min(len, 100.0);
        double total = 0;
        for (unsigned l = 0; l < 2; ++l) {
            const float* r = light + l * 16;
            double rgb[3]{r[4], r[5], r[6]};
            const double peak = std::max({rgb[0], rgb[1], rgb[2]});
            const double divide = std::max(1.0, peak / 4);
            for (auto& c : rgb) c /= divide;
            const double luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
            const double colour = luminance + (rgb[channel] - luminance) * 0.75;
            const double radius = 1 / std::sqrt(double(r[3]));
            const double core = std::max(double(push.glow_size) * radius, 0.05);
            constexpr unsigned Steps = 20000;
            double energy = 0;
            for (unsigned i = 0; i < Steps; ++i) {
                const double t = (i + 0.5) / Steps * limit;
                const double dx = ray[0] * t - r[0], dy = ray[1] * t - r[1], dz = ray[2] * t - r[2];
                const double d2 = dx * dx + dy * dy + dz * dz;
                const double x2 = d2 / (radius * radius);
                const double window = std::clamp(1 - x2 * x2, 0.0, 1.0);
                const double falloff = 2.5 * core * core / (d2 + core * core) * window * window;
                energy += falloff * std::exp(-0.025 * t) * 0.025 * limit / Steps;
            }
            total += colour * energy;
        }
        return total * 0.35 * push.intensity;
    };
    double worst = 0;
    unsigned checked = 0;
    std::vector<uint16_t> default_glow;
    for (const float glow : {0.08f, 0.03f, 0.2f}) {
        push.glow_size = glow;
        for (const float scene_depth : {0.5f, 0.9f, 1.0f}) {
            const auto out = run(scene_depth);
            if (glow == 0.08f && scene_depth == 0.5f) default_glow = out;
            if (glow != 0.08f && scene_depth == 0.5f) assert(out != default_glow);
            for (unsigned y = 0; y < H; ++y) {
                for (unsigned x = 0; x < W; ++x) {
                    for (unsigned c = 0; c < 3; ++c) {
                        const double want = reference(x, y, scene_depth, c);
                        const double got = half_to_float(out[(y * W + x) * 4 + c]);
                        assert(want > 0);
                        worst = std::max(worst, std::abs(got - want) / want);
                        ++checked;
                    }
                }
            }
        }
    }
    push.glow_size = 0.08f;
    std::printf("Lamp fog: largest error against a 20000-step reference %.3f%% over %u values\n",
                worst * 100, checked);
    assert(worst < 0.02);
    light[19] = 0;
    light[3] = 1e9f;
    const auto missed = run(0.5f);
    assert(std::ranges::all_of(missed, [](auto v) { return v == 0; }));
    light[3] = 0.01f;
    push.intensity = 0;
    const auto off = run(0.5f);
    assert(std::ranges::all_of(off, [](auto v) { return v == 0; }));
    std::puts("Lamp fog: PASS (inverse-square reference match at three glow sizes with two lights, "
              "surfaces and sky, lights the ray misses, zero intensity)");
}
