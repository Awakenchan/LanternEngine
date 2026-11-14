// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: HDR output encode pass (hdr_pass.h), modelled on the post-processing pass.

#include "video_core/renderer_vulkan/host_passes/hdr_pass.h"

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/hdr_output_frag.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include <array>

namespace Vulkan::HostPasses {

void HdrOutputPass::Create(vk::Device device, vk::Format output_format) {
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 3, .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment},
    };
    desc_set_layout = Check<"create HDR output descriptor set layout">(
        device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(Settings),
    };
    pipeline_layout = Check<"create HDR output pipeline layout">(
        device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
            .setLayoutCount = 1U,
            .pSetLayouts = &*desc_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        }));

    const auto vs_module = CompileSPV(FS_TRI_VERT, device);
    ASSERT(vs_module);
    SetObjectName(device, vs_module, "fs_tri.vert");
    const auto fs_module = CompileSPV(HDR_OUTPUT_FRAG, device);
    ASSERT(fs_module);
    SetObjectName(device, fs_module, "hdr_output.frag");

    const std::array shaders_ci{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vs_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fs_module,
            .pName = "main",
        },
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &output_format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::Viewport viewport{
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const vk::Rect2D scissor{.extent = {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState attachment{
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    const std::array dynamic_states{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shaders_ci.size()),
        .pStages = shaders_ci.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };
    pipeline = Check<"create HDR output pipeline">(
        device.createGraphicsPipelineUnique(/*pipeline_cache*/ {}, pipeline_info));

    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);

    // The shader reads texels (texelFetch); a combined image sampler still needs a sampler.
    sampler = Check<"create HDR output sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    }));
    // bbport: native mode's gain map has the display buffer's size, not the output's: filtered
    // like the letterbox blit filters the frame.
    gain_sampler = Check<"create HDR gain sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    }));
}

void HdrOutputPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView game, vk::ImageView overlay,
                           vk::ImageView gain, vk::ImageLayout gain_layout, vk::ImageView output,
                           vk::Extent2D extent, const Settings& settings, vk::ImageView snapshot) {
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/HDR output",
        });
    }

    const std::array image_infos{
        vk::DescriptorImageInfo{
            .sampler = *sampler,
            .imageView = game,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = *sampler,
            .imageView = overlay,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = *gain_sampler,
            .imageView = gain,
            .imageLayout = gain_layout,
        },
        vk::DescriptorImageInfo{.sampler = *gain_sampler, .imageView = snapshot ? snapshot : game,
            .imageLayout = snapshot && snapshot != game ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal},
    };
    const std::array set_writes{
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_infos[0],
        },
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_infos[1],
        },
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_infos[2],
        },
        vk::WriteDescriptorSet{.dstBinding = 3, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &image_infos[3]},
    };

    const vk::RenderingAttachmentInfo attachment{
        .imageView = output,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        // Every pixel is written by the full-screen triangle.
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info{
        .renderArea = {.extent = extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    cmdbuf.setViewport(0, vk::Viewport{
                              .width = static_cast<float>(extent.width),
                              .height = static_cast<float>(extent.height),
                              .minDepth = 0.0f,
                              .maxDepth = 1.0f,
                          });
    cmdbuf.setScissor(0, vk::Rect2D{.extent = extent});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, set_writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
                         sizeof(Settings), &settings);
    cmdbuf.beginRendering(rendering_info);
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
}

} // namespace Vulkan::HostPasses
