// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <cstring>
#include "lifter/bridge.h"
#include "shader_recompiler/native/tone_curve.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Lifter {

static_assert(MotionParamsCapacity == Shader::MotionVectors::ParamsCapacity);
static_assert(CaptureParamsCapacity == Shader::MotionVectors::CaptureParamsCapacity);

namespace {

template <class T>
Sharp MakeSharp(const Shader::SharpFetch<T>& fetch) {
    Sharp sharp;
    for (size_t i = 0; i < fetch.offsets.size(); ++i) {
        if ((fetch.load_mask >> i) & 1) {
            sharp.flat[i] = fetch.offsets[i];
        } else {
            sharp.imm[i] = fetch.immediates[i];
        }
    }
    return sharp;
}

std::array<u8, 4> Swizzle(const AmdGpu::CompMapping& mapping) {
    return {u8(mapping.array[0]), u8(mapping.array[1]), u8(mapping.array[2]), u8(mapping.array[3])};
}

NumClass ClassOf(AmdGpu::NumberFormat format) {
    switch (AmdGpu::GetNumberClass(format)) {
    case AmdGpu::NumberClass::Sint:
        return NumClass::Sint;
    case AmdGpu::NumberClass::Uint:
        return NumClass::Uint;
    default:
        return NumClass::Float;
    }
}

View ViewOf(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return View::D1;
    case AmdGpu::ImageType::Color1DArray:
        return View::D1Array;
    case AmdGpu::ImageType::Color2D:
        return View::D2;
    case AmdGpu::ImageType::Color2DArray:
        return View::D2Array;
    case AmdGpu::ImageType::Color2DMsaa:
        return View::D2Msaa;
    case AmdGpu::ImageType::Color3D:
        return View::D3;
    default:
        return View::Other;
    }
}

PsVgprs VgprsOf(const AmdGpu::PsInput& flags) {
    PsVgprs out;
    out.persp_sample = flags.persp_sample_ena;
    out.persp_center = flags.persp_center_ena;
    out.persp_centroid = flags.persp_centroid_ena;
    out.persp_pull = flags.persp_pull_model_ena;
    out.linear_sample = flags.linear_sample_ena;
    out.linear_center = flags.linear_center_ena;
    out.linear_centroid = flags.linear_centroid_ena;
    out.line_stipple = flags.line_stipple_tex_ena;
    out.pos_x = flags.pos_x_float_ena;
    out.pos_y = flags.pos_y_float_ena;
    out.pos_z = flags.pos_z_float_ena;
    out.pos_w = flags.pos_w_float_ena;
    out.front_face = flags.front_face_ena;
    out.ancillary = flags.ancillary_ena;
    out.sample_coverage = flags.sample_coverage_ena;
    return out;
}

s32 UdFromShOffset(u16 offset) {
    if (!offset) {
        return -1;
    }
    return s32(offset) - 0x4c;
}

} // namespace

Mode GetMode() {
    static const Mode mode = [] {
        const char* env = std::getenv("LANTERN_LIFTER");
        if (!env || std::strcmp(env, "1") == 0) {
            return Mode::On;
        }
        if (std::strcmp(env, "compare") == 0) {
            return Mode::Compare;
        }
        return Mode::Off;
    }();
    return mode;
}

Input MakeInput(const Shader::Info& info, const Shader::RuntimeInfo& rt,
                const Shader::Profile& profile, const Shader::Backend::Bindings& start,
                std::span<const u32> code) {
    using Shader::HwStage;
    using Shader::SwStage;
    Input in;
    in.code = code;
    in.hash = info.pgm_hash;
    switch (info.hw_stage) {
    case HwStage::Fragment:
        in.stage = Stage::Fragment;
        break;
    case HwStage::Vertex:
        in.stage = info.sw_stage == SwStage::TessellationEval ? Stage::Domain : Stage::Vertex;
        break;
    case HwStage::Compute:
        in.stage = Stage::Compute;
        break;
    case HwStage::Local:
        in.stage = Stage::Local;
        break;
    case HwStage::Export:
        in.stage = info.sw_stage == SwStage::TessellationEval ? Stage::Domain : Stage::Export;
        break;
    case HwStage::Hull:
        in.stage = Stage::Hull;
        break;
    case HwStage::Geometry:
        in.stage = Stage::Geometry;
        break;
    }
    Options& opt = in.opt;
    opt.amd_explicit_vertex = profile.supports_amd_shader_explicit_vertex_parameter;
    opt.khr_barycentric = profile.supports_fragment_shader_barycentric;
    opt.manual_interpolation = profile.needs_manual_interpolation;
    opt.manual_interpolation_all = profile.manual_interpolation_all;
    opt.integer_interpolation_fix = info.sw_stage == SwStage::Fragment && profile.needs_integer_interpolation_fix;
    opt.fragment_exact_fp = profile.fragment_exact_fp;
    opt.trinary_minmax = profile.supports_trinary_minmax;
    opt.native_cube = profile.supports_native_cube_calc;
    opt.buffer_offsets = profile.needs_buffer_offsets;
    opt.uniform_dword_layout = profile.uniform_dword_layout;
    opt.lower_left_origin = profile.lower_left_origin_mode;
    opt.stencil_export = profile.supports_shader_stencil_export;
    opt.clip_distance_emulation = info.hw_stage == HwStage::Vertex && info.sw_stage == SwStage::Vertex && profile.needs_clip_distance_emulation;
    opt.denorm_flush_32 = rt.props.fp_denorm_mode32 == AmdGpu::FpDenormMode::InOutFlush && profile.support_fp32_denorm_flush;
    opt.rtz_32 = rt.props.fp_round_mode32 == AmdGpu::FpRoundMode::ToZero && profile.support_fp32_round_to_zero;
    opt.szinp_ok = profile.support_fp32_signed_zero_inf_nan_preserve;
    opt.image_lod_rw = profile.supports_image_load_store_lod;
    opt.max_viewport_width = profile.max_viewport_width;
    opt.max_viewport_height = profile.max_viewport_height;

    u32 ud_index = start.user_data;
    for (u32 reg = 0; reg < 16; ++reg) {
        if (info.ud_mask.mask & (1u << reg)) {
            in.ud_push[reg] = s32(ud_index++);
        }
    }
    const auto& plan = info.table_plan;
    if (plan.valid) {
        for (u32 i = 0; i < plan.entries.size(); ++i) {
            if (plan.entries[i].expr_len == 0) {
                in.flat_table.push_back({plan.entries[i].parent, plan.entries[i].offset,
                                         u16(Shader::Native::TablePlan::FirstFlat + i)});
            }
        }
    }
    u32 binding = start.unified;
    u32 offset_slot = start.buffer;
    for (u32 i = 0; i < info.buffers.size(); ++i) {
        const auto& desc = info.buffers[i];
        BufferSlot slot;
        slot.sharp = MakeSharp(desc.sharp_fetch);
        switch (desc.buffer_type) {
        case Shader::BufferType::Guest:
            slot.kind = BufferKind::Guest;
            break;
        case Shader::BufferType::Flatbuf:
            slot.kind = BufferKind::Flatbuf;
            in.flatbuf_slot = s32(i);
            break;
        case Shader::BufferType::GdsBuffer:
            slot.kind = BufferKind::Gds;
            break;
        case Shader::BufferType::ClipPlanes:
            slot.kind = BufferKind::Other;
            in.clip_plane_slot = s32(i);
            break;
        default:
            slot.kind = BufferKind::Other;
            break;
        }
        const auto sharp = desc.GetSharp(info);
        slot.binding = binding++;
        slot.offset_slot = offset_slot++;
        slot.uniform = desc.is_uniform;
        slot.uniform_exact = profile.uniform_exact_base;
        slot.written = desc.is_written;
        slot.coherent = desc.buffer_type == Shader::BufferType::SharedMemory || sharp.mtype == 3;
        slot.formatted = desc.is_formatted;
        slot.swizzle_enable = sharp.swizzle_enable;
        slot.add_tid = sharp.add_tid_enable;
        slot.stride = u32(sharp.stride);
        slot.index_stride = sharp.GetIndexStride();
        slot.element_size = sharp.GetElementSize();
        slot.data_format = u8(sharp.GetDataFmt());
        slot.num_format = u8(sharp.GetNumberFmt());
        slot.conv = Conv(u8(sharp.GetNumberConversion()));
        slot.swizzle = Swizzle(sharp.DstSelect());
        in.buffers.push_back(slot);
    }
    for (u32 i = 0; i < info.images.size(); ++i) {
        const auto& desc = info.images[i];
        const auto sharp = desc.GetSharp(info);
        ImageSlot slot;
        slot.sharp = MakeSharp(desc.sharp_fetch);
        slot.binding = binding;
        binding += desc.NumBindings(info);
        slot.view = ViewOf(sharp.GetViewType(desc.is_array));
        slot.cls = ClassOf(sharp.GetNumberFmt());
        slot.storage = desc.is_written;
        slot.array = desc.is_array;
        slot.cube = sharp.IsCube();
        slot.srgb = sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb;
        slot.atomic = desc.is_atomic;
        slot.conv = Conv(u8(sharp.GetNumberConversion()));
        slot.swizzle = Swizzle(sharp.DstSelect());
        in.images.push_back(slot);
    }
    for (u32 i = 0; i < info.samplers.size(); ++i) {
        const auto& desc = info.samplers[i];
        const auto sharp = desc.GetSharp(info);
        SamplerSlot slot;
        slot.sharp = MakeSharp(desc.sharp_fetch);
        slot.binding = binding++;
        slot.depth = desc.is_depth;
        slot.force_unnormalized = sharp.force_unnormalized;
        slot.force_degamma = sharp.force_degamma;
        in.samplers.push_back(slot);
    }
    u32 tess_output_base = 0;
    if (info.sw_stage == SwStage::TessellationControl || info.sw_stage == SwStage::TessellationEval) {
        const u32 ptr = u32(info.tess_consts_ptr_base);
        const u32 dword = u32(info.tess_consts_dword_offset);
        if (info.tess_consts_dword_offset >= 0) {
            if (info.tess_consts_ptr_base == Shader::IR::ScalarReg::Max) {
                in.tess.sharp.flat[0] = u16(dword);
            } else {
                for (const FlatEntry& entry : in.flat_table) {
                    if (entry.parent == ptr && entry.offset == dword) {
                        in.tess.sharp.flat[0] = entry.flat;
                    }
                }
            }
            Shader::TessellationDataConstantBuffer constants{};
            info.ReadTessConstantBuffer(constants);
            tess_output_base = constants.hs_output_base;
            in.tess.ls_stride = constants.ls_stride;
            in.tess.hs_cp_stride = constants.hs_cp_stride;
        }
        if (info.sw_stage == SwStage::TessellationControl) {
            const auto& tcs = rt.sw.tcs;
            auto copy = tcs;
            copy.hs_output_cp_stride = in.tess.hs_cp_stride;
            copy.ls_stride = in.tess.ls_stride;
            copy.hs_output_base = tess_output_base;
            in.tess.domain = u32(tcs.tess_type);
            in.tess.offchip = tcs.offchip_lds_enable;
            in.tess.input_vertices = tcs.num_input_control_points;
            in.tess.passthrough = copy.IsPassthrough();
            in.tess.output_vertices = copy.NumOutputControlPoints();
        } else {
            const auto& tes = rt.sw.tes;
            in.tess.domain = u32(tes.tess_type);
            in.tess.partitioning = u32(tes.tess_partitioning);
            in.tess.topology = u32(tes.tess_topology);
        }
    }
    if (info.hw_stage == HwStage::Local) {
        in.tess.ls_stride = rt.hw.ls.ls_stride;
    }
    if (info.hw_stage == HwStage::Geometry) {
        const auto& gs = rt.hw.gs;
        static constexpr auto count_of = [](AmdGpu::PrimitiveType type) -> u32 {
            switch (type) {
            case AmdGpu::PrimitiveType::PointList:
                return 1;
            case AmdGpu::PrimitiveType::LineList:
            case AmdGpu::PrimitiveType::LineStrip:
                return 2;
            case AmdGpu::PrimitiveType::AdjTriangleList:
                return 6;
            case AmdGpu::PrimitiveType::AdjLineList:
                return 4;
            default:
                return 3;
            }
        };
        in.gs.in_vertices = count_of(gs.in_primitive);
        in.gs.output_vertices = gs.output_vertices;
        in.gs.invocations = gs.num_invocations;
        in.gs.out_primitive = u32(gs.out_primitive[0]);
        in.gs.dwords_per_vertex = gs.out_vertex_data_size;
        const auto& copy = info.gs_copy_data;
        if (copy.num_comps && copy.num_comps > in.gs.dwords_per_vertex) {
            in.gs.dwords_per_vertex = copy.num_comps;
        }
        in.gs.copy_output_vertices = copy.output_vertices;
        for (const auto& [offset, target] : copy.attr_map) {
            in.gs.attr_map.push_back({offset, u32(target.first), target.second});
        }
        for (u32 a = 0; a < 3; ++a) {
            for (u32 c = 0; c < 4; ++c) {
                in.vs_outputs[a][c] = u8(gs.outputs[a][c]);
            }
        }
    }
    in.num_user_sgprs = rt.props.num_user_data;
    in.num_input_vgprs = rt.props.num_input_vgprs;
    if (info.sw_stage == SwStage::Vertex) {
        in.base_vertex_sgpr = UdFromShOffset(rt.sw.vs.vertex_sgpr_offset);
        in.base_instance_sgpr = UdFromShOffset(rt.sw.vs.instance_sgpr_offset);
        in.step_rate_0 = rt.sw.vs.step_rate_0;
        in.step_rate_1 = rt.sw.vs.step_rate_1;
        if (const auto fetch = Shader::Gcn::ParseFetchShader(info)) {
            in.fetch_applies_base_vertex = fetch->vertex_offset_sgpr != -1;
            in.fetch_applies_base_instance = fetch->instance_offset_sgpr != -1;
            for (const auto& attrib : fetch->attributes) {
                const auto sharp = attrib.GetSharp(info);
                VsAttribute attr;
                attr.semantic = attrib.semantic;
                attr.vgpr = attrib.dest_vgpr;
                attr.cls = ClassOf(sharp.GetNumberFmt());
                attr.conv = Conv(u8(sharp.GetNumberConversion()));
                attr.swizzle = Swizzle(sharp.DstSelect());
                in.attributes.push_back(attr);
            }
        }
    }
    if (info.hw_stage == HwStage::Vertex) {
        const auto& vs = rt.hw.vs;
        for (u32 a = 0; a < 3; ++a) {
            for (u32 c = 0; c < 4; ++c) {
                in.vs_outputs[a][c] = u8(vs.outputs[a][c]);
            }
        }
        in.emulate_depth = vs.emulate_depth_negative_one_to_one;
        in.clip_disable = vs.clip_disable;
        in.user_clip_plane_mask = vs.user_clip_plane_mask;
        for (u32 a = 0; a < vs.num_outputs; ++a) {
            for (const auto output : vs.outputs[a]) {
                if (output >= Shader::Output::ClipDist0 && output <= Shader::Output::ClipDist7) {
                    in.user_clip_plane_mask = 0;
                }
            }
        }
        in.vertex_motion = (vs.motion_vectors & Shader::MotionVectors::VertexOutputs) != 0;
        in.vertex_capture = (vs.motion_vectors & Shader::MotionVectors::VertexCapture) != 0;
    }
    if (info.hw_stage == HwStage::Fragment) {
        in.tone_curve = Shader::Native::ToneCurveEnabled();
        const auto& fs = rt.hw.fs;
        in.ps_addr = VgprsOf(fs.addr_flags);
        in.ps_en = VgprsOf(fs.en_flags);
        for (u32 i = 0; i < fs.num_inputs && i < fs.inputs.size(); ++i) {
            in.ps_inputs.push_back({fs.inputs[i].param_index, fs.inputs[i].is_default,
                                    fs.inputs[i].is_flat, fs.inputs[i].default_value});
        }
        for (u32 i = 0; i < 8; ++i) {
            const auto& cb = fs.color_buffers[i];
            ColorOut& color = in.color[i];
            color.export_format = u8(cb.export_format);
            color.cls = ClassOf(cb.num_format);
            color.conv = Conv(u8(cb.num_conversion));
            color.swizzle = Swizzle(cb.swizzle);
            color.self_scale = cb.blend_self_scale;
        }
        in.z_export_format = u8(fs.z_export_format);
        in.num_samples = fs.num_samples;
        in.mrtz_mask = fs.mrtz_mask;
        in.front_face_all_bits = fs.front_face_all_bits;
        in.dual_source = fs.dual_source_blending;
        in.clip_emulation_input = fs.clip_distance_emulation;
        in.fragment_motion = fs.motion_vectors;
        in.depth_first = fs.depth_first;
    }
    if (info.hw_stage == HwStage::Compute) {
        const auto& cs = rt.hw.cs;
        in.workgroup_size = cs.workgroup_size;
        in.tgid_enable = cs.tgid_enable;
        in.shared_memory_size = cs.shared_memory_size;
    }
    return in;
}

} // namespace Lifter
