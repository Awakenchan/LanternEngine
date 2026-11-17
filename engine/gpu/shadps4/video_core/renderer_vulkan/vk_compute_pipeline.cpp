// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include <memory>
#include <stdexcept>

#include "common/serdes.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_hdr_channel.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 SerializationSupport& sdata, bool preloading /*=false*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();

    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = instance.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = buffer.is_uniform ? vk::DescriptorType::eUniformBuffer
                                                : vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = HdrChannel::PushConstantSize(),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pipeline_layout,
    };
    // bbport (BB_ASYNC_PIPELINES): as graphics pipelines (vk_graphics_pipeline.cpp); the stage
    // info only points at its subgroup size and the module, which outlives the pipeline.
    // bbport: the start's cached pipelines are created in parallel (WarmupJobs).
    WarmupJobs* const warmup = preloading ? warmup_jobs : nullptr;
    if (((!preloading && AsyncPipelinesEnabled()) || warmup) && subgroup_size_ci.pNext == nullptr &&
        shader_ci.pSpecializationInfo == nullptr) {
        struct Pending {
            vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size;
            vk::ComputePipelineCreateInfo info;
        };
        auto job = std::make_shared<Pending>();
        job->subgroup_size = subgroup_size_ci;
        job->info = compute_pipeline_ci;
        job->info.stage.pNext = shader_ci.pNext ? &job->subgroup_size : nullptr;
        ready.store(false, std::memory_order_relaxed);
        if (warmup) {
            warmup->jobs.push_back([this, job, device, cache = pipeline_cache, name = debug_str] {
                auto [result, created] = device.createComputePipelineUnique(cache, job->info);
                if (result == vk::Result::eSuccess) {
                    pipeline = std::move(created);
                    SetObjectName(device, *pipeline, "Compute Pipeline {}", name);
                }
                MarkReady();
                return result == vk::Result::eSuccess;
            });
            return;
        }
        SubmitPipelineJob(this, [this, job, device, cache = pipeline_cache, name = debug_str] {
            auto [result, created] = device.createComputePipelineUnique(cache, job->info);
            ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
                       vk::to_string(result));
            pipeline = std::move(created);
            SetObjectName(device, *pipeline, "Compute Pipeline {}", name);
            MarkReady();
        });
        return;
    }
    auto [pipeline_result, pipe] =
        instance.GetDevice().createComputePipelineUnique(pipeline_cache, compute_pipeline_ci);
    if (preloading && pipeline_result != vk::Result::eSuccess) {
        // bbport: a pipeline from the cache the driver rejects is a damaged cache entry: the
        // cache is rebuilt (PipelineCache::WarmUp) instead of stopping the game at every start.
        throw Serialization::CorruptData{"cached compute pipeline rejected by the driver: " +
                                         vk::to_string(pipeline_result)};
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Compute Pipeline {}", debug_str);
}

ComputePipeline::~ComputePipeline() {
    WaitReady(); // bbport: a compile thread may still be creating it
}

} // namespace Vulkan
