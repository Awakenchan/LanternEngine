// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bbport_settings.h"
#include <xxhash.h>

#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_effect_shaders.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Vulkan {

namespace {
constexpr u64 ShadowResolveHash = 0xf55643de;
constexpr u64 ShadowBlurHash = 0x11ffdfb5;
constexpr u64 PointLightsFullHash = 0x9f9ef267;
constexpr u64 HdrPostHash = 0xccbf44a6;
constexpr u64 DirectionalTableHash = 0xd92266b9;
constexpr u64 SceneTargetHash = 0x32e47788;
constexpr u64 PointGridVertexHash = 0x8747a367;

constexpr u32 PointLightCount(u64 hash) {
    switch (hash) {
    case 0x370174f7:
    case 0x7cc2c044:
        return 1;
    case 0x186d69d5:
    case 0x600f86b0:
        return 2;
    case 0xd22cbfd1:
        return 3;
    case 0x3105494c:
        return 4;
    case 0xd75fe2ff:
        return 5;
    case 0x07d07cf3:
        return 6;
    case 0xf2531727:
        return 7;
    case 0xc6edb19a:
        return 8;
    case 0xb59b4c4e:
        return 9;
    case 0x505ebdbe:
        return 10;
    case 0x22b357f3:
        return 11;
    case PointLightsFullHash:
        return 12;
    default:
        return 0;
    }
}

u32 PackedOffsets(const Shader::PushData& push) {
    static_assert(offsetof(Shader::PushData, buf_offsets) == 80);
    u32 word = 0;
    std::memcpy(&word, push.buf_offsets.data(), sizeof(word));
    return word;
}

u64 FragmentHash(const GraphicsPipeline* pipeline) {
    const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    return ps ? ps->pgm_hash : 0;
}

bool AnyWanted() {
    const auto& s = BbSettings::Get();
    for (const int effect : {BbSettings::EffectGi, BbSettings::EffectContactShadows,
                             BbSettings::EffectPcss, BbSettings::EffectVolumetricLight,
                             BbSettings::EffectPointFog, BbSettings::EffectPointShadows,
                             BbSettings::EffectRayTracing}) {
        if (s.effects[effect].load(std::memory_order_relaxed)) {
            return true;
        }
    }
    return EffectShaders::GtaoActive() || EffectShaders::SsrActive();
}
}

void Rasterizer::EffectsOnFrameEnd() {
    effects_wanted = AnyWanted();
    if (!effects_wanted) {
        point_shadow->Drop();
    }
    point_shadow->OnFrameEnd();
    rt_shadow->OnFrameEnd();
    ssgi->OnFrameEnd();
    ssr_temporal->OnFrameEnd();
}

void Rasterizer::EffectsBeforeTargets(const GraphicsPipeline* pipeline) {
    if (!effects_wanted) {
        return;
    }
    const u64 hash = FragmentHash(pipeline);
    if (hash != EffectShaders::SsrHash) {
        ssr_temporal->Flush(upscaler->Jitter());
    }
    contact_shadow->Flush(hash == ShadowBlurHash);
    if (PointLightCount(hash) == 0) {
        const auto jitter = upscaler->Jitter();
        point_shadow->Flush(jitter[0] != 0.0f || jitter[1] != 0.0f);
    }
}

void Rasterizer::EffectsAfterTargets(const GraphicsPipeline* pipeline) {
    if (!effects_wanted) {
        return;
    }
    const auto& key = pipeline->GetGraphicsKey();
    if (std::popcount(key.mrt_mask) >= 5 && db_desc.first) {
        volumetric_light->Reset();
        ssgi->OnGBufferDraw(db_desc.first, cb_descs[2].first, cb_descs[0].first);
    }
    if (cb_descs[0].first && EffectShaders::GtaoActive() &&
        FragmentHash(pipeline) == EffectShaders::SsaoHash) {
        ssgi->OnAoDraw(cb_descs[0].first);
    }
}

void Rasterizer::EffectsBlendedScene() {
    if (effects_wanted) {
        ssgi->OnOpaqueScene(cb_descs[0].first, db_desc.first, upscaler->Jitter());
    }
}

void Rasterizer::EffectsBeforeUpscaler(u64 compute_hash) {
    if (effects_wanted && compute_hash == upscaler->TriggerHash()) {
        const auto camera = camera_motion->PassMatrices();
        volumetric_light->SetCamera({camera.view, camera.inv_view, camera.valid});
        volumetric_light->Composite(upscaler->SceneColor(), "the upscaler");
    }
}

void Rasterizer::EffectsBeforeResources(const GraphicsPipeline* pipeline) {
    if (effects_wanted && FragmentHash(pipeline) == HdrPostHash) {
        const auto camera = camera_motion->PassMatrices();
        volumetric_light->SetCamera({camera.view, camera.inv_view, camera.valid});
        volumetric_light->Composite();
    }
}

void Rasterizer::EffectsCaptureDraw(const AmdGpu::Regs& regs, const GraphicsPipeline* pipeline,
                                    bool is_indexed, u32 index_offset) {
    const u32 mode = pipeline->GetGraphicsKey().motion_vectors;
    if (mode == Shader::MotionVectors::VertexBoth) {
        push_data.motion_param &= 0xffffu;
    } else {
        push_data.motion_param = 0;
    }
    if (!rt_shadow->Wanted() || !motion_geometry) {
        return;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    const auto& fetch = pipeline->GetFetchShader();
    u32 first_vertex = regs.index_offset, first_instance = 0;
    if (fetch) {
        if (first_vertex == 0 && fetch->vertex_offset_sgpr != -1) {
            first_vertex = vs.UserData()[fetch->vertex_offset_sgpr];
        }
        if (fetch->instance_offset_sgpr != -1) {
            first_instance = vs.UserData()[fetch->instance_offset_sgpr];
        }
    }
    const u32 index_size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
    RtShadow::Draw draw{
        .shader = vs.pgm_hash,
        .geometry = motion_geometry,
        .index_size = index_size,
        .index_count = regs.num_indices,
        .first_vertex = first_vertex,
        .first_instance = first_instance,
        .instances = regs.num_instances.NumInstances(),
        .restart = regs.enable_primitive_restart != 0,
        .strip = regs.primitive_type == AmdGpu::PrimitiveType::TriangleStrip,
        .triangles = regs.primitive_type == AmdGpu::PrimitiveType::TriangleStrip ||
                     regs.primitive_type == AmdGpu::PrimitiveType::TriangleList,
        .shadow_pass = !gbuffer_draw &&
                       regs.depth_buffer.Pitch() == regs.depth_buffer.Height() &&
                       regs.depth_buffer.Height() >= 1024,
        .camera_pass = gbuffer_draw,
        .solid = regs.depth_control.depth_write_enable != 0 && !regs.blend_control[0].enable,
        .alpha = ps && ps->has_discard,
    };
    if (is_indexed) {
        const VAddr address = regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size;
        const u64 bytes = u64(regs.num_indices) * index_size;
        if (address && memory->IsValidGpuMapping(address, 0) &&
            memory->ClampRangeSize(address, bytes) == bytes) {
            draw.indices = address;
        }
    }
    for (const auto& resource : vs.buffers) {
        if (resource.IsSpecial()) {
            continue;
        }
        const auto buffer = resource.GetSharp(vs);
        const u32 size = buffer.GetSize();
        if (!buffer.Valid() || !memory->IsValidGpuMapping(buffer.base_address, 0) ||
            memory->ClampRangeSize(buffer.base_address, size) != size) {
            continue;
        }
        if (size == 864) {
            draw.constants = reinterpret_cast<const float*>(buffer.base_address);
        } else if (buffer.GetStride() == 16) {
            const auto role = Motion::ClassifyBuffer(size);
            if (role == Motion::BufferRole::Skeleton) {
                draw.skinned = true;
            } else if (role == Motion::BufferRole::SmallSkeleton) {
                draw.palette = XXH3_64bits_withSeed(reinterpret_cast<const void*>(buffer.base_address),
                                                    size, draw.palette);
            }
        }
    }
    const u32 param = rt_shadow->PrepareDraw(draw);
    if (mode == Shader::MotionVectors::VertexBoth) {
        push_data.motion_param |= param << 16;
    } else {
        push_data.motion_param = param;
    }
}

vk::Rect2D Rasterizer::EffectTile(const AmdGpu::Regs& regs, const GraphicsPipeline* pipeline,
                                   bool is_indexed, u32 index_offset, u32 first_vertex) const {
    const vk::Rect2D scissor = DrawScissor(0);
    const auto& fetch = pipeline->GetFetchShader();
    const auto& v = vertex_binds;
    const auto position =
        std::ranges::find(v.attributes, 0u, &vk::VertexInputAttributeDescription2EXT::location);
    if (!fetch || position == v.attributes.end() || position->binding >= v.num_buffers) {
        return scissor;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    VAddr base = 0;
    u32 stride = 0;
    for (const auto& attribute : fetch->attributes) {
        if (attribute.semantic == 0) {
            const auto sharp = attribute.GetSharp(vs);
            base = sharp.base_address;
            stride = sharp.GetStride();
        }
    }
    if (!base || stride < 16) {
        return scissor;
    }
    const u32 index_size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
    const VAddr indices =
        is_indexed ? regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size : 0;
    if (is_indexed && (!memory->IsValidGpuMapping(indices, 0) ||
                       memory->ClampRangeSize(indices, 4 * index_size) != 4 * index_size)) {
        return scissor;
    }
    const auto& vp = regs.viewports[0];
    float lo_x = 1e30f, lo_y = 1e30f, hi_x = -1e30f, hi_y = -1e30f;
    for (u32 i = 0; i < 4; ++i) {
        u32 index = i;
        if (is_indexed) {
            index = index_size == 2
                        ? reinterpret_cast<const u16*>(indices)[i]
                        : reinterpret_cast<const u32*>(indices)[i];
        }
        const VAddr at = base + u64(s64(index) + s64(s32(first_vertex))) * stride + position->offset;
        if (!memory->IsValidGpuMapping(at, 0) || memory->ClampRangeSize(at, 16) != 16) {
            return scissor;
        }
        const auto* p = reinterpret_cast<const float*>(at);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[3]) ||
            std::abs(p[3]) < 1e-8f) {
            return scissor;
        }
        const float x = p[0] / p[3] * (vp.xscale * target_scale[0]) +
                        (vp.xoffset * target_scale[0] + target_offset[0]);
        const float y = p[1] / p[3] * (vp.yscale * target_scale[1]) +
                        (vp.yoffset * target_scale[1] + target_offset[1]);
        lo_x = std::min(lo_x, x);
        lo_y = std::min(lo_y, y);
        hi_x = std::max(hi_x, x);
        hi_y = std::max(hi_y, y);
    }
    const s32 x0 = std::max(s32(std::floor(lo_x)), scissor.offset.x);
    const s32 y0 = std::max(s32(std::floor(lo_y)), scissor.offset.y);
    const s32 x1 = std::min(s32(std::ceil(hi_x)), scissor.offset.x + s32(scissor.extent.width));
    const s32 y1 = std::min(s32(std::ceil(hi_y)), scissor.offset.y + s32(scissor.extent.height));
    if (x1 <= x0 || y1 <= y0) {
        return scissor;
    }
    return {.offset = {x0, y0}, .extent = {u32(x1 - x0), u32(y1 - y0)}};
}

void Rasterizer::EffectsBeforeBind(const AmdGpu::Regs& regs, const GraphicsPipeline* pipeline,
                                   bool is_indexed, u32 index_offset, u32 first_vertex) {
    if (!pipeline->GetGraphicsKey().mrt_mask) {
        return;
    }
    const u64 hash = FragmentHash(pipeline);
    const u32 tile_lights = PointLightCount(hash);
    if (tile_lights != 0) {
        effect_tile = EffectTile(regs, pipeline, is_indexed, index_offset, first_vertex);
    }
    static_assert(offsetof(Shader::PushData, motion_param) == 120);
    static_assert(offsetof(Shader::PushData, scene_size) == 124);
    if (hash == EffectShaders::SsaoHash) {
        if (EffectShaders::GtaoActive()) {
            const float k = std::clamp(
                BbSettings::Get().graphics_ao_strength.load(std::memory_order_relaxed), 0.0f, 1.0f);
            push_data.motion_param = std::bit_cast<u32>(k);
        }
    } else if (hash == EffectShaders::SsrHash) {
        if (EffectShaders::SsrActive() && cb_descs[0].first) {
            push_data.motion_param = ssr_frame;
            SsrTemporal::TraceInputs inputs{.frame_index = ssr_frame};
            if (!buffer_infos.empty() && buffer_infos[0].buffer && image_infos.size() == 6) {
                inputs.constants = buffer_infos[0].buffer;
                inputs.constants_offset = buffer_infos[0].offset;
                inputs.constants_range = buffer_infos[0].range;
                inputs.constants_dword_offset = PackedOffsets(push_data);
                inputs.normals_view = image_infos[1].imageView;
                inputs.normals_layout = image_infos[1].imageLayout;
                inputs.color_view = image_infos[2].imageView;
                inputs.color_layout = image_infos[2].imageLayout;
            }
            ssr_temporal->OnSsrDraw(cb_descs[0].first, inputs);
            ++ssr_frame;
        }
    } else if (hash == ShadowBlurHash) {
        if (cb_descs[0].first) {
            const auto& blur = pipeline->GetStage(Shader::SwStage::Fragment);
            contact_shadow->OnBlurDraw(
                cb_descs[0].first, blur.images.empty() ? 0 : blur.images[0].GetSharp(blur).Address());
        }
    } else if (hash == ShadowResolveHash) {
        if (cb_descs[0].first && !buffer_infos.empty() && buffer_infos[0].buffer) {
            const bool has_ubo = buffer_infos.size() > 1 && buffer_infos[1].buffer;
            const bool has_map = image_infos.size() > 1 && image_infos[1].imageView;
            contact_shadow->OnShadowDraw(
                cb_descs[0].first,
                {.constants = buffer_infos[0].buffer,
                 .constants_offset = buffer_infos[0].offset,
                 .constants_range = buffer_infos[0].range,
                 .uniforms = has_ubo ? buffer_infos[1].buffer : vk::Buffer{},
                 .uniforms_offset = has_ubo ? buffer_infos[1].offset : 0,
                 .uniforms_range = has_ubo ? buffer_infos[1].range : 0,
                 .shadow_map = has_map ? image_infos[1].imageView : vk::ImageView{},
                 .shadow_layout = has_map ? image_infos[1].imageLayout : vk::ImageLayout::eUndefined,
                 .constants_offset_word = PackedOffsets(push_data),
                 .frame_index = shadow_frame++});
        }
    } else if (tile_lights != 0) {
        const auto& ps = pipeline->GetStage(Shader::SwStage::Fragment);
        if ((pipeline->GetGraphicsKey().mrt_mask & 3) == 3 && cb_descs[0].first &&
            cb_descs[1].first && ps.buffers.size() >= 2 && !ps.buffers[0].IsSpecial() &&
            !ps.buffers[1].IsSpecial()) {
            const VAddr constants = ps.buffers[0].GetSharp(ps).base_address;
            const VAddr lights = ps.buffers[1].GetSharp(ps).base_address;
            const u64 light_bytes = u64(tile_lights) * 64;
            if (memory->IsValidGpuMapping(constants, 0) && memory->ClampRangeSize(constants, 64) == 64 &&
                memory->IsValidGpuMapping(lights, 0) &&
                memory->ClampRangeSize(lights, light_bytes) == light_bytes) {
                point_shadow->OnLightDraw(cb_descs[0].first, cb_descs[1].first,
                                          {.constants = reinterpret_cast<const u32*>(constants),
                                           .lights = reinterpret_cast<const u32*>(lights),
                                           .count = tile_lights,
                                           .tile = effect_tile});
            }
        }
    } else if (hash == EffectShaders::SsrCompositeHash && EffectShaders::SsrCompositeActive()) {
        const float intensity =
            BbSettings::Get().graphics_ssr_intensity.load(std::memory_order_relaxed);
        push_data.motion_param =
            std::bit_cast<u32>(std::clamp(std::isfinite(intensity) ? intensity : 1.0f, 0.0f, 3.0f));
        if (const auto full = ssr_temporal->FullResult(); full && image_infos.size() == 12) {
            image_infos[6].imageView = full;
            image_infos[6].imageLayout = vk::ImageLayout::eGeneral;
        }
    }
}

void Rasterizer::EffectsAfterDraw(const AmdGpu::Regs& regs, const GraphicsPipeline* pipeline, bool is_indexed,
                                  u32 num_indices, u32 num_instances, u32 first_vertex) {
    if (!effects_wanted) {
        return;
    }
    const auto* ps_info = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    if (!ps_info) {
        return;
    }
    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const u64 hash = ps_info->pgm_hash;
    if (hash == ShadowResolveHash && cb_descs[0].first && buffer_infos.size() >= 2 &&
        image_infos.size() >= 2) {
        volumetric_light->Integrate(cb_descs[0].first, image_infos[0],
                                    {.constants = buffer_infos[0].buffer,
                                     .constants_offset = buffer_infos[0].offset,
                                     .constants_range = buffer_infos[0].range,
                                     .uniforms = buffer_infos[1].buffer,
                                     .uniforms_offset = buffer_infos[1].offset,
                                     .uniforms_range = buffer_infos[1].range,
                                     .shadow_map = image_infos[1].imageView,
                                     .shadow_layout = image_infos[1].imageLayout,
                                     .constants_offset_word = PackedOffsets(push_data)});
    } else if (hash == DirectionalTableHash && !buffer_infos.empty()) {
        volumetric_light->Tint(buffer_infos[0], push_data.buf_offsets[0] & 255u);
    } else if (const u32 count = PointLightCount(hash);
               count != 0 && (pipeline->GetGraphicsKey().mrt_mask & 3) == 3 && cb_descs[0].first &&
               cb_descs[1].first && buffer_infos.size() >= 2) {
        if (vs_info.pgm_hash != PointGridVertexHash || ps_info->buffers.size() < 2 ||
            ps_info->buffers[0].IsSpecial() || ps_info->buffers[1].IsSpecial()) {
            return;
        }
        const VAddr constants = ps_info->buffers[0].GetSharp(*ps_info).base_address;
        const VAddr lights = ps_info->buffers[1].GetSharp(*ps_info).base_address;
        const u64 light_bytes = u64(count) * 64;
        if (memory->IsValidGpuMapping(constants, 0) && memory->ClampRangeSize(constants, 64) == 64 &&
            memory->IsValidGpuMapping(lights, 0) &&
            memory->ClampRangeSize(lights, light_bytes) == light_bytes) {
            volumetric_light->AddPointLights(reinterpret_cast<const u32*>(constants),
                                             reinterpret_cast<const u32*>(lights), count);
        }
    } else if (hash == SceneTargetHash && cb_descs[0].first) {
        volumetric_light->Scene(cb_descs[0].first);
    }
}

}
