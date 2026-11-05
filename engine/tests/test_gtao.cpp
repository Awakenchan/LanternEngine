// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/gtao_frag.h"
#include "video_core/host_shaders/gtao_hq_frag.h"
#include "video_core/host_shaders/gtao_single_hq_frag.h"
#include "video_core/host_shaders/gtao_reference_frag.h"
#include "video_core/host_shaders/gtao_denoise_comp.h"
#include "video_core/host_shaders/gtao_copy_comp.h"

using u32 = uint32_t;
using Pixel = std::array<float, 4>;
using Packed = std::array<uint8_t, 4>;
u32 W = 257, H = 193, N = W * H, FloatBytes = N * sizeof(Pixel);
constexpr vk::ImageSubresourceRange Range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
struct Params {
    std::array<int32_t, 2> origin{0, 0};
    std::array<u32, 2> extent{W, H};
    u32 scene_size = 0;
    u32 buf_off = 0;
};
static_assert(sizeof(Params) == 24);

template <class T> T Check(vk::ResultValue<T> result) {
    if (result.result != vk::Result::eSuccess) {
        std::fprintf(stderr, "Vulkan test failed: %s\n", vk::to_string(result.result).c_str());
        std::abort();
    }
    return std::move(result.value);
}

struct Scene {
    std::vector<Pixel> depth = std::vector<Pixel>(N);
    std::vector<Pixel> normals = std::vector<Pixel>(N);
};

Scene MakeScene(u32 mode) {
    Scene scene;
    for (u32 y = 0; y < H; ++y) for (u32 x = 0; x < W; ++x) {
        const float dx = (x + 0.5f) / W * 2 - 1;
        const float dy = 1 - (y + 0.5f) / H * 2;
        float z = 2.0f;
        std::array<float, 3> normal{0, 0, -1};
        if (mode == 1 && dx < -0.125f) {
            z = -0.25f / dx;
            normal = {1, 0, 0};
        }
        if (mode == 2 || mode == 5) {
            const std::array<float, 3> center{mode == 5 ? 0.20f : -0.10f, 0.05f, 1.87f};
            constexpr float radius = 0.18f;
            const float a = dx * dx + dy * dy + 1;
            const float b = dx * center[0] + dy * center[1] + center[2];
            const float c = center[0] * center[0] + center[1] * center[1] +
                            center[2] * center[2] - radius * radius;
            const float discriminant = b * b - a * c;
            if (discriminant >= 0) {
                const float hit = (b - std::sqrt(discriminant)) / a;
                if (hit > 0.1f && hit < z) {
                    z = hit;
                    normal = {(dx * z - center[0]) / radius,
                              (dy * z - center[1]) / radius, (z - center[2]) / radius};
                }
            }
        }
        if (mode == 3 && (x % 23 < 2 || y % 37 < 2)) z = 1.75f;
        if (mode == 4) {
            const int cx = int(x % 29) - 14, cy = int(y % 31) - 15;
            if (cx * cx + cy * cy < 60 && (x + y) % 5 != 0) z = 1.82f;
        }
        scene.depth[y * W + x] = {1.0f - 0.1f / z, 0, 0, 0};
        scene.normals[y * W + x] = {(normal[0] + 1) / 2, (normal[1] + 1) / 2,
                                   (normal[2] + 1) / 2, 1};
    }
    return scene;
}

class Gpu {
public:
    Gpu() {
        static vk::detail::DynamicLoader loader;
        d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
        const vk::ApplicationInfo app{.pApplicationName = "gtao-spatial-test",
                                     .apiVersion = VK_API_VERSION_1_3};
        instance = Check(vk::createInstanceUnique({.pApplicationInfo = &app}, nullptr, d));
        d.init(*instance);
        const auto gpus = Check(instance->enumeratePhysicalDevices(d));
        assert(!gpus.empty());
        physical = gpus[0];
        const auto families = physical.getQueueFamilyProperties(d);
        family = 0;
        while (family < families.size() &&
               (families[family].queueFlags &
                (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute)) !=
                (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute)) ++family;
        assert(family < families.size());
        timestamp_bits = families[family].timestampValidBits;
        const float priority = 1;
        const vk::DeviceQueueCreateInfo qi{.queueFamilyIndex = family, .queueCount = 1,
                                          .pQueuePriorities = &priority};
        const char* extension = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        vk::PhysicalDeviceVulkan12Features f12{.scalarBlockLayout = true};
        const vk::PhysicalDeviceVulkan13Features f13{
            .pNext = &f12, .synchronization2 = true, .dynamicRendering = true};
        device = Check(physical.createDeviceUnique({.pNext = &f13, .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &qi, .enabledExtensionCount = 1,
            .ppEnabledExtensionNames = &extension}, nullptr, d));
        d.init(*device);
        queue = device->getQueue(family, 0, d);
        pool = Check(device->createCommandPoolUnique({
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = family}, nullptr, d));
        command = Check(device->allocateCommandBuffers({.commandPool = *pool,
            .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}, d))[0];
        for (u32 i = 0; i < images.size(); ++i) {
            const bool noise = i == 2, packed = i >= 3;
            const u32 width = noise ? 64 : W, height = noise ? 64 : H;
            const auto format = packed ? vk::Format::eR8G8B8A8Unorm :
                                         vk::Format::eR32G32B32A32Sfloat;
            auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                         vk::ImageUsageFlagBits::eTransferSrc;
            if (packed) usage |= vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eColorAttachment;
            images[i] = Check(device->createImageUnique({.imageType = vk::ImageType::e2D,
                .format = format, .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
                .usage = usage}, nullptr, d));
            const auto r = device->getImageMemoryRequirements(*images[i], d);
            image_memory[i] = Check(device->allocateMemoryUnique({.allocationSize = r.size,
                .memoryTypeIndex = MemoryType(r.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)},
                nullptr, d));
            assert(device->bindImageMemory(*images[i], *image_memory[i], 0, d) == vk::Result::eSuccess);
            views[i] = Check(device->createImageViewUnique({.image = *images[i],
                .viewType = vk::ImageViewType::e2D, .format = format, .subresourceRange = Range},
                nullptr, d));
        }
        for (u32 i = 0; i < buffers.size(); ++i) {
            buffers[i] = Check(device->createBufferUnique({
                .size = i == 0 ? FloatBytes * 3 : 65536 * 3,
                .usage = i == 0 ? vk::BufferUsageFlagBits::eTransferSrc |
                                 vk::BufferUsageFlagBits::eTransferDst :
                                 vk::BufferUsageFlagBits::eUniformBuffer}, nullptr, d));
            const auto r = device->getBufferMemoryRequirements(*buffers[i], d);
            buffer_memory[i] = Check(device->allocateMemoryUnique({.allocationSize = r.size,
                .memoryTypeIndex = MemoryType(r.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible |
                                                               vk::MemoryPropertyFlagBits::eHostCoherent)},
                nullptr, d));
            assert(device->bindBufferMemory(*buffers[i], *buffer_memory[i], 0, d) == vk::Result::eSuccess);
            mapped[i] = Check(device->mapMemory(*buffer_memory[i], 0, VK_WHOLE_SIZE, {}, d));
        }
        auto* cb = static_cast<float*>(mapped[1]);
        std::fill_n(cb, 16384 * 3, 0.0f);
        cb[0] = cb[5] = cb[11] = 1;
        cb[14] = -10; cb[15] = 10;
        cb[16] = W; cb[17] = H;
        cb[18] = 28.0f / W; cb[19] = 28.0f / H;
        cb[20] = cb[25] = cb[30] = 1;
        cb[36] = cb[40] = cb[41] = cb[42] = 1;
        cb[16384 * 2 + 2] = 10;
        for (u32 i = 0; i < 2; ++i)
            samplers[i] = Check(device->createSamplerUnique({.magFilter = vk::Filter::eNearest,
                .minFilter = vk::Filter::eNearest, .mipmapMode = vk::SamplerMipmapMode::eNearest,
                .addressModeU = i ? vk::SamplerAddressMode::eRepeat : vk::SamplerAddressMode::eClampToEdge,
                .addressModeV = i ? vk::SamplerAddressMode::eRepeat : vk::SamplerAddressMode::eClampToEdge,
                .addressModeW = vk::SamplerAddressMode::eClampToEdge}, nullptr, d));
        std::array<vk::DescriptorSetLayoutBinding, 9> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i)
            bindings[i] = {i, i < 3 ? vk::DescriptorType::eUniformBuffer :
                             i < 6 ? vk::DescriptorType::eSampledImage : vk::DescriptorType::eSampler,
                           1, vk::ShaderStageFlagBits::eFragment};
        graphics_set = Check(device->createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}, nullptr, d));
        const vk::PushConstantRange gp{vk::ShaderStageFlagBits::eFragment, 80, 48};
        graphics_layout = Check(device->createPipelineLayoutUnique({.setLayoutCount = 1,
            .pSetLayouts = &*graphics_set, .pushConstantRangeCount = 1, .pPushConstantRanges = &gp},
            nullptr, d));
        const std::array<std::span<const u32>, 4> codes{
            GTAO_FRAG, GTAO_HQ_FRAG, GTAO_REFERENCE_FRAG, GTAO_SINGLE_HQ_FRAG};
        for (u32 i = 0; i < codes.size(); ++i) pipelines[i] = GraphicsPipeline(codes[i]);
        const std::array<vk::DescriptorSetLayoutBinding, 4> filter_bindings{{
            {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {3, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute}}};
        filter_set = Check(device->createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = 4, .pBindings = filter_bindings.data()}, nullptr, d));
        const vk::PushConstantRange fp{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Params)};
        filter_layout = Check(device->createPipelineLayoutUnique({.setLayoutCount = 1,
            .pSetLayouts = &*filter_set, .pushConstantRangeCount = 1, .pPushConstantRanges = &fp},
            nullptr, d));
        const std::array<std::span<const u32>, 2> filters{GTAO_DENOISE_COMP, GTAO_COPY_COMP};
        for (u32 i = 0; i < filters.size(); ++i) {
            const auto module = Check(device->createShaderModuleUnique({
                .codeSize = filters[i].size_bytes(), .pCode = filters[i].data()}, nullptr, d));
            filter_pipelines[i] = Check(device->createComputePipelineUnique({}, {
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
                .layout = *filter_layout}, nullptr, d));
        }
        Begin();
        std::array<vk::ImageMemoryBarrier2, 5> transitions{};
        for (u32 i = 0; i < transitions.size(); ++i)
            transitions[i] = {.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                .image = *images[i], .subresourceRange = Range};
        command.pipelineBarrier2({.imageMemoryBarrierCount = u32(transitions.size()),
                                  .pImageMemoryBarriers = transitions.data()}, d);
        Submit();
        std::printf("Device: %s\n", physical.getProperties(d).deviceName.data());
    }

    std::vector<Packed> Run(const Scene& scene, u32 mode, bool filter = false, float strength = 1) {
        Upload(scene);
        Begin();
        Draw(mode, filter, strength);
        return Readback();
    }

    std::vector<Packed> Filter(const Scene& scene, const std::vector<Packed>& pixels,
                               const Params& params = {},
                               std::array<float, 2> uv_size = {float(W), float(H)}) {
        auto* cb = static_cast<float*>(mapped[1]);
        cb[16] = uv_size[0]; cb[17] = uv_size[1];
        Upload(scene);
        std::memcpy(mapped[0], pixels.data(), pixels.size() * sizeof(Packed));
        Begin();
        Barrier(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead |
                vk::AccessFlagBits2::eMemoryWrite, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite);
        const vk::BufferImageCopy region{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                        .imageExtent = {W, H, 1}};
        command.copyBufferToImage(*buffers[0], *images[3], vk::ImageLayout::eGeneral, region, d);
        Denoise(params);
        auto result = Readback();
        cb[16] = W; cb[17] = H;
        return result;
    }

    void Benchmark(const Scene& scene) {
        assert(timestamp_bits > 0);
        Upload(scene);
        constexpr u32 Warmup = 32, Rounds = 128;
        auto queries = Check(device->createQueryPoolUnique({.queryType = vk::QueryType::eTimestamp,
                                                            .queryCount = Rounds * 4}, nullptr, d));
        Begin();
        for (u32 i = 0; i < Warmup; ++i) for (u32 mode : {0u, 1u}) Draw(mode, mode == 1);
        Submit();
        Begin();
        command.resetQueryPool(*queries, 0, Rounds * 4, d);
        for (u32 round = 0; round < Rounds; ++round) for (u32 item = 0; item < 2; ++item) {
            const u32 mode = (round + item) % 2, q = round * 4 + mode * 2;
            Barrier(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead |
                    vk::AccessFlagBits2::eMemoryWrite, vk::PipelineStageFlagBits2::eAllCommands,
                    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
            command.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, *queries, q, d);
            Draw(mode, mode == 1);
            command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, *queries, q + 1, d);
        }

        Submit();
        std::array<uint64_t, Rounds * 4> ticks{};
        assert(device->getQueryPoolResults(*queries, 0, ticks.size(), sizeof(ticks), ticks.data(),
            sizeof(uint64_t), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait, d) ==
            vk::Result::eSuccess);
        const uint64_t mask = timestamp_bits == 64 ? UINT64_MAX : (uint64_t(1) << timestamp_bits) - 1;
        const double period = physical.getProperties(d).limits.timestampPeriod;
        std::array<std::vector<double>, 2> costs;
        std::vector<double> ratios;
        for (u32 i = 0; i < Rounds; ++i) {
            std::array<double, 2> ms{};
            for (u32 mode = 0; mode < 2; ++mode) {
                const u32 q = i * 4 + mode * 2;
                const uint64_t delta = (ticks[q + 1] - ticks[q]) & mask;
                assert(delta > 0 && delta < (mask >> 1));
                ms[mode] = delta * period / 1e6;
                costs[mode].push_back(ms[mode]);
            }
            ratios.push_back(ms[1] / ms[0]);
        }
        const auto median = [](std::vector<double> values) {
            std::sort(values.begin(), values.end());
            return (values[values.size() / 2 - 1] + values[values.size() / 2]) / 2;
        };
        std::printf("Synthetic %ux%u AO, %u paired GPU samples: Standard %.6f ms, "
                    "HQ including filter %.6f ms, paired median %.3fx\n",
                    W, H, Rounds, median(costs[0]), median(costs[1]), median(ratios));
    }

    void FiniteFar(bool enabled) {
        static_cast<float*>(mapped[1])[14] = enabled ? -9.9f : -10.0f;
    }

private:
    u32 MemoryType(u32 bits, vk::MemoryPropertyFlags flags) const {
        const auto properties = physical.getMemoryProperties(d);
        for (u32 i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
        std::abort();
    }
    vk::UniquePipeline GraphicsPipeline(std::span<const u32> code) {
        const auto vertex = Check(device->createShaderModuleUnique({
            .codeSize = sizeof(FS_TRI_VERT), .pCode = FS_TRI_VERT}, nullptr, d));
        const auto fragment = Check(device->createShaderModuleUnique({
            .codeSize = code.size_bytes(), .pCode = code.data()}, nullptr, d));
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
            {.stage = vk::ShaderStageFlagBits::eVertex, .module = *vertex, .pName = "main"},
            {.stage = vk::ShaderStageFlagBits::eFragment, .module = *fragment, .pName = "main"}}};
        const vk::PipelineVertexInputStateCreateInfo vertex_input{};
        const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology = vk::PrimitiveTopology::eTriangleList};
        const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
        const vk::PipelineRasterizationStateCreateInfo raster{.polygonMode = vk::PolygonMode::eFill,
            .cullMode = vk::CullModeFlagBits::eNone, .lineWidth = 1};
        const vk::PipelineMultisampleStateCreateInfo samples{.rasterizationSamples = vk::SampleCountFlagBits::e1};
        const vk::PipelineColorBlendAttachmentState attachment{
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
        const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1, .pAttachments = &attachment};
        const std::array dynamic{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamic_state{.dynamicStateCount = 2,
                                                             .pDynamicStates = dynamic.data()};
        const auto format = vk::Format::eR8G8B8A8Unorm;
        const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1,
                                                        .pColorAttachmentFormats = &format};
        return Check(device->createGraphicsPipelineUnique({}, {.pNext = &rendering,
            .stageCount = 2, .pStages = stages.data(), .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &assembly, .pViewportState = &viewport,
            .pRasterizationState = &raster, .pMultisampleState = &samples,
            .pColorBlendState = &blend, .pDynamicState = &dynamic_state,
            .layout = *graphics_layout}, nullptr, d));
    }
    void Begin() {
        assert(command.reset({}, d) == vk::Result::eSuccess);
        assert(command.begin(vk::CommandBufferBeginInfo{}, d) == vk::Result::eSuccess);
    }
    void Submit() {
        assert(command.end(d) == vk::Result::eSuccess);
        const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &command};
        assert(queue.submit(submit, {}, d) == vk::Result::eSuccess);
        assert(queue.waitIdle(d) == vk::Result::eSuccess);
    }
    void Barrier(vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access,
                 vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
        const vk::MemoryBarrier2 barrier{.srcStageMask = src, .srcAccessMask = src_access,
            .dstStageMask = dst, .dstAccessMask = dst_access};
        command.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier}, d);
    }
    void Upload(const Scene& scene) {
        std::memcpy(mapped[0], scene.depth.data(), FloatBytes);
        std::memcpy(static_cast<char*>(mapped[0]) + FloatBytes, scene.normals.data(), FloatBytes);
        auto* noise = reinterpret_cast<Pixel*>(static_cast<char*>(mapped[0]) + FloatBytes * 2);
        for (u32 y = 0; y < 64; ++y) for (u32 x = 0; x < 64; ++x) {
            const float angle = float((x * 1664525u + y * 1013904223u) % 65536) / 65536 * 6.2831853f;
            noise[y * 64 + x] = {std::cos(angle) * 0.5f + 0.5f, std::sin(angle) * 0.5f + 0.5f, 0, 1};
        }
        Begin();
        Barrier(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead |
                vk::AccessFlagBits2::eMemoryWrite, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite);
        for (u32 i = 0; i < 3; ++i) {
            const vk::BufferImageCopy region{.bufferOffset = i * FloatBytes,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {i == 2 ? 64u : W, i == 2 ? 64u : H, 1}};
            command.copyBufferToImage(*buffers[0], *images[i], vk::ImageLayout::eGeneral, region, d);
        }
        Submit();
    }
    void Draw(u32 mode, bool filter, float strength = 1) {
        Barrier(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead |
                vk::AccessFlagBits2::eMemoryWrite, vk::PipelineStageFlagBits2::eFragmentShader |
                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eColorAttachmentWrite);
        std::array<vk::DescriptorBufferInfo, 3> cb{};
        std::array<vk::DescriptorImageInfo, 6> infos{};
        std::array<vk::WriteDescriptorSet, 9> writes{};
        for (u32 i = 0; i < 3; ++i) {
            cb[i] = {*buffers[1], i * 65536, 65536};
            writes[i] = {.dstBinding = i, .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eUniformBuffer, .pBufferInfo = &cb[i]};
        }
        for (u32 i = 0; i < 6; ++i) {
            infos[i] = {.sampler = i == 5 ? *samplers[1] : *samplers[0],
                .imageView = *views[i % 3], .imageLayout = vk::ImageLayout::eGeneral};
            writes[i + 3] = {.dstBinding = i + 3, .descriptorCount = 1,
                .descriptorType = i < 3 ? vk::DescriptorType::eSampledImage : vk::DescriptorType::eSampler,
                .pImageInfo = &infos[i]};
        }
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipelines[mode], d);
        command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *graphics_layout, 0, writes, d);
        // gtao.frag's push block: buf_off at 80 (constants at offset 0), ao_k at 120, scene_size at 124.
        struct Push { u32 buf_off; std::array<u32, 9> pad; float strength; u32 scene_size; };
        static_assert(sizeof(Push) == 48);
        const Push push{0, {}, strength, 0};
        command.pushConstants(*graphics_layout, vk::ShaderStageFlagBits::eFragment, 80,
                              sizeof(push), &push, d);
        const vk::RenderingAttachmentInfo attachment{.imageView = *views[3],
            .imageLayout = vk::ImageLayout::eGeneral, .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore};
        command.beginRendering({.renderArea = {{0, 0}, {W, H}}, .layerCount = 1,
            .colorAttachmentCount = 1, .pColorAttachments = &attachment}, d);
        command.setViewport(0, vk::Viewport{0, 0, float(W), float(H), 0, 1}, d);
        command.setScissor(0, vk::Rect2D{{0, 0}, {W, H}}, d);
        command.draw(3, 1, 0, 0, d);
        command.endRendering(d);
        if (filter) Denoise({});
    }
    void Denoise(const Params& params) {
        for (u32 i = 0; i < 2; ++i) {
            Barrier(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead |
                    vk::AccessFlagBits2::eMemoryWrite, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
            const std::array<vk::DescriptorImageInfo, 3> infos{{
                {.imageView = *views[i ? 4 : 3], .imageLayout = vk::ImageLayout::eGeneral},
                {.imageView = *views[1], .imageLayout = vk::ImageLayout::eGeneral},
                {.imageView = *views[i ? 3 : 4], .imageLayout = vk::ImageLayout::eGeneral}}};
            std::array<vk::WriteDescriptorSet, 4> writes{};
            for (u32 j = 0; j < infos.size(); ++j)
                writes[j] = {.dstBinding = j, .descriptorCount = 1,
                    .descriptorType = j == 2 ? vk::DescriptorType::eStorageImage :
                                              vk::DescriptorType::eSampledImage, .pImageInfo = &infos[j]};
            const vk::DescriptorBufferInfo cb{*buffers[1], 0, 65536};
            writes[3] = {.dstBinding = 3, .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eUniformBuffer, .pBufferInfo = &cb};
            command.bindPipeline(vk::PipelineBindPoint::eCompute, *filter_pipelines[i], d);
            command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *filter_layout, 0, writes, d);
            command.pushConstants(*filter_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                  sizeof(params), &params, d);
            command.dispatch((params.extent[0] + 7) / 8, (params.extent[1] + 7) / 8, 1, d);
        }
    }
    std::vector<Packed> Readback() {
        Barrier(vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite,
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        const vk::BufferImageCopy region{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                        .imageExtent = {W, H, 1}};
        command.copyImageToBuffer(*images[3], vk::ImageLayout::eGeneral, *buffers[0], region, d);
        Barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
        Submit();
        std::vector<Packed> result(N);
        std::memcpy(result.data(), mapped[0], result.size() * sizeof(Packed));
        return result;
    }
    vk::detail::DispatchLoaderDynamic d;
    vk::UniqueInstance instance;
    vk::PhysicalDevice physical;
    u32 family = 0, timestamp_bits = 0;
    vk::UniqueDevice device;
    vk::Queue queue;
    vk::UniqueCommandPool pool;
    vk::CommandBuffer command;
    std::array<vk::UniqueDeviceMemory, 5> image_memory;
    std::array<vk::UniqueImage, 5> images;
    std::array<vk::UniqueImageView, 5> views;
    std::array<vk::UniqueDeviceMemory, 2> buffer_memory;
    std::array<vk::UniqueBuffer, 2> buffers;
    std::array<void*, 2> mapped{};
    std::array<vk::UniqueSampler, 2> samplers;
    vk::UniqueDescriptorSetLayout graphics_set, filter_set;
    vk::UniquePipelineLayout graphics_layout, filter_layout;
    std::array<vk::UniquePipeline, 4> pipelines;
    std::array<vk::UniquePipeline, 2> filter_pipelines;
};

double Error(const std::vector<Packed>& a, const std::vector<Packed>& b) {
    double sum = 0;
    for (u32 i = 0; i < N; ++i) {
        const double delta = (double(a[i][0]) - b[i][0]) / 255;
        sum += delta * delta;
    }
    return sum / N;
}
void SameDepth(const std::vector<Packed>& a, const std::vector<Packed>& b) {
    for (u32 i = 0; i < N; ++i) for (u32 c = 1; c < 4; ++c) assert(a[i][c] == b[i][c]);
}
void Write(const std::filesystem::path& path, const std::vector<Packed>& pixels) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size() * sizeof(Packed));
    assert(file.good());
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 3 || (argc == 3 && std::string(argv[2]) != "--benchmark" &&
                    std::string(argv[2]) != "--benchmark-native" && std::string(argv[2]) != "--native")) {
        std::fprintf(stderr, "Usage: gtao-test [output-directory [--native|--benchmark|--benchmark-native]]\n");
        return 1;
    }
    if (argc == 3 && (std::string(argv[2]) == "--benchmark-native" || std::string(argv[2]) == "--native")) {
        W = 960; H = 540; N = W * H; FloatBytes = N * sizeof(Pixel);
    }
    const std::filesystem::path output = argc > 1 ? argv[1] : "";
    if (!output.empty()) std::filesystem::create_directories(output);
    Gpu gpu;
    const std::array names{"flat", "corner", "curved", "rails", "foliage", "moving"};
    double standard_error = 0, single_error = 0, raw_error = 0, filtered_error = 0;
    for (u32 i = 0; i < names.size(); ++i) {
        const auto scene = MakeScene(i);
        const auto standard = gpu.Run(scene, 0), raw = gpu.Run(scene, 1),
                   hq = gpu.Run(scene, 1, true), reference = gpu.Run(scene, 2),
                   single = gpu.Run(scene, 3);
        SameDepth(standard, raw);
        SameDepth(raw, hq);
        SameDepth(raw, reference);
        SameDepth(raw, single);
        if (i == 0) {
            const auto darkest = std::min_element(hq.begin(), hq.end(),
                [](const Packed& a, const Packed& b) { return a[0] < b[0]; });
            const auto index = std::distance(hq.begin(), darkest);
            std::printf("Open plane minimum: HQ %u, raw %u, reference %u, Standard %u at %zu,%zu\n",
                        (*darkest)[0], raw[index][0], reference[index][0], standard[index][0],
                        size_t(index) % W, size_t(index) / W);
            if (!output.empty()) {
                Write(output / "flat-hq.rgba", hq);
                Write(output / "flat-reference.rgba", reference);
            }
            for (const auto& p : hq) assert(p[0] >= 253);
        }
        if (i == 5) {
            gpu.Run(MakeScene(0), 1, true);
            assert(gpu.Run(scene, 1, true) == hq);
        }
        const auto off = gpu.Run(scene, 1, true, 0);
        SameDepth(off, hq);
        for (const auto& p : off) assert(p[0] == 255);
        const auto standard_off = gpu.Run(scene, 0, false, 0);
        for (const auto& p : standard_off) assert(p[0] == 255);
        const auto half = gpu.Run(scene, 1, true, 0.5f);
        SameDepth(half, hq);
        for (u32 p = 0; p < N; ++p)
            assert(std::abs(int(half[p][0]) * 2 - (int(hq[p][0]) + 255)) <= 3);
        const double se = Error(standard, reference), ce = Error(single, reference),
                     re = Error(raw, reference), he = Error(hq, reference);
        standard_error += se;
        single_error += ce;
        raw_error += re;
        filtered_error += he;
        std::printf("%s: Standard MSE %.8f, corrected single slice %.8f, raw HQ %.8f, filtered HQ %.8f\n",
                    names[i], se, ce, re, he);
        constexpr double QuantizationMse = 1.0 / (255.0 * 255.0);
        assert(he <= ce + QuantizationMse);
        assert(he <= re * 1.05 + QuantizationMse);
        if (!output.empty()) {
            Write(output / (std::string(names[i]) + "-standard.rgba"), standard);
            Write(output / (std::string(names[i]) + "-single-hq.rgba"), single);
            Write(output / (std::string(names[i]) + "-raw-hq.rgba"), raw);
            Write(output / (std::string(names[i]) + "-hq.rgba"), hq);
            Write(output / (std::string(names[i]) + "-reference.rgba"), reference);
        }
    }
    std::printf("Aggregate MSE: Standard %.8f, corrected single slice %.8f, raw HQ %.8f, filtered HQ %.8f\n",
                standard_error, single_error, raw_error, filtered_error);
    assert(filtered_error < standard_error * 0.85);
    assert(filtered_error < single_error * 0.85);
    assert(filtered_error <= raw_error * 1.05);
    auto scene = MakeScene(0);
    std::vector<Packed> pixels(N);
    for (u32 y = 0; y < H; ++y) for (u32 x = 0; x < W; ++x)
        pixels[y * W + x] = {uint8_t((x + y) % 2 ? 102 : 153), 51, 0, 51};
    auto filtered = gpu.Filter(scene, pixels);
    SameDepth(pixels, filtered);
    auto constant = pixels;
    for (auto& p : constant) p[0] = 128;
    assert(Error(filtered, constant) < Error(pixels, constant) * 0.1);
    for (u32 y = 0; y < H; ++y) for (u32 x = 0; x < W; ++x)
        pixels[y * W + x] = x < W / 2 ? Packed{51, 0, 0, 51} : Packed{204, 0, 0, 102};
    assert(gpu.Filter(scene, pixels) == pixels);
    for (u32 y = 0; y < H; ++y) for (u32 x = W / 2; x < W; ++x) {
        pixels[y * W + x][3] = 51;
        scene.normals[y * W + x] = {1, 0.5f, 0.5f, 1};
    }
    assert(gpu.Filter(scene, pixels) == pixels);
    const Params region{{7, 9}, {W - 17, H - 21}, 0};
    filtered = gpu.Filter(scene, pixels, region);
    assert(filtered == pixels);
    constexpr u32 ViewW = 210, ViewH = 156;
    const Params padded{{0, 0}, {ViewW, ViewH}, 0};
    for (u32 y = 0; y < H; ++y) for (u32 x = 0; x < W; ++x) {
        scene.normals[y * W + x] = x < W / 2 ? Pixel{0.5f, 0.5f, 0, 1} : Pixel{1, 0.5f, 0.5f, 1};
        const u32 normal_x = u32((x + 0.5f) / ViewW * W);
        pixels[y * W + x] = {uint8_t(normal_x < W / 2 ? 51 : 204), 0, 0, 51};
    }
    assert(gpu.Filter(scene, pixels, padded, {ViewW, ViewH}) == pixels);
    const Params proxy{{0, 0}, {ViewW, ViewH}, (810u << 16) | 1440u};
    assert(gpu.Filter(scene, pixels, proxy, {ViewW / 0.75f, ViewH / 0.75f}) == pixels);
    for (u32 i = 0; i < N; ++i)
        pixels[i] = {128, uint8_t(i), uint8_t(i * 199u), uint8_t(i * 113u)};
    assert(gpu.Filter(scene, pixels) == pixels);
    gpu.FiniteFar(true);
    for (auto& p : scene.depth) p[0] = 1;
    const auto native_sky = gpu.Run(scene, 0);
    for (const auto& p : native_sky) {
        const double depth = p[3] / 255.0 + p[2] / 65025.0 + p[1] / 16581375.0;
        assert(p[0] == 255 && std::abs(depth - 1) < 1e-4);
    }
    for (u32 mode : {1u, 2u}) assert(gpu.Run(scene, mode, mode == 1) == native_sky);
    gpu.FiniteFar(false);
    std::printf("PASS: six scenes; open plane, packed depth, AO off, no history, "
                "linear strength, sky, all packed-byte values, noise reduction, "
                "depth/normal edges, odd-size/partial/padded/proxy UV guards\n");
    if (argc == 3 && std::string(argv[2]) != "--native") gpu.Benchmark(MakeScene(1));
}
