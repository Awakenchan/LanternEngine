// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <initializer_list>
#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace Vulkan::EffectPass {

bool Create(const Instance& instance, vk::UniqueDescriptorSetLayout& set_layout,
            vk::UniquePipelineLayout& layout, vk::UniquePipeline& pipeline,
            std::initializer_list<vk::DescriptorType> types, u32 push_size,
            std::span<const u32> code, const char* name, bool dispatch_base = false,
            const vk::SpecializationInfo* specialization = nullptr);

bool CreateSampler(const Instance& instance, vk::UniqueSampler& sampler, vk::Filter filter);

bool StorageFormat(const Instance& instance, vk::Format format);

bool HasRoom(const Instance& instance, vk::DeviceSize bytes);

void Mark(u64 key, const char* name);

}
