// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include "lifter/lift_core.h"

namespace Lifter {

namespace {

constexpr u16 O(spv::Op op) {
    return u16(op);
}

constexpr u32 AttrParam = 32;
constexpr u32 AttrPos0 = 12;

enum BaryKind : u32 { PerspSample, PerspCenter, PerspCentroid, PerspPull, LinearSample, LinearCenter, LinearCentroid, BaryNone = 255 };

enum Qualifier : u8 { QualNone, QualSmooth, QualNoPersp, QualPerVertex, QualFlat };
enum Aux : u8 { AuxNone, AuxCentroid, AuxSample };

void Qualify(u32 kind, u8& primary, u8& aux) {
    switch (kind) {
    case PerspSample:
        primary = QualSmooth;
        aux = AuxSample;
        break;
    case PerspCenter:
    case PerspPull:
        primary = QualSmooth;
        aux = AuxNone;
        break;
    case PerspCentroid:
        primary = QualSmooth;
        aux = AuxCentroid;
        break;
    case LinearSample:
        primary = QualNoPersp;
        aux = AuxSample;
        break;
    case LinearCenter:
        primary = QualNoPersp;
        aux = AuxNone;
        break;
    default:
        primary = QualNoPersp;
        aux = AuxCentroid;
        break;
    }
}

} // namespace

void Lift_::DefineBaseTypes() {
    t_void = m.AddType(O(spv::Op::OpTypeVoid), {});
    t_bool = m.AddType(O(spv::Op::OpTypeBool), {});
    t_u32 = m.AddType(O(spv::Op::OpTypeInt), {32, 0});
    t_s32 = m.AddType(O(spv::Op::OpTypeInt), {32, 1});
    t_f32 = m.AddType(O(spv::Op::OpTypeFloat), {32});
    t_v2f = m.AddType(O(spv::Op::OpTypeVector), {t_f32, 2});
    t_v3f = m.AddType(O(spv::Op::OpTypeVector), {t_f32, 3});
    t_v4f = m.AddType(O(spv::Op::OpTypeVector), {t_f32, 4});
    t_v2u = m.AddType(O(spv::Op::OpTypeVector), {t_u32, 2});
    t_v3u = m.AddType(O(spv::Op::OpTypeVector), {t_u32, 3});
    t_v4u = m.AddType(O(spv::Op::OpTypeVector), {t_u32, 4});
    t_v2s = m.AddType(O(spv::Op::OpTypeVector), {t_s32, 2});
    t_v3s = m.AddType(O(spv::Op::OpTypeVector), {t_s32, 3});
    t_v4s = m.AddType(O(spv::Op::OpTypeVector), {t_s32, 4});
    true_id = m.ConstBool(t_bool, true);
    false_id = m.ConstBool(t_bool, false);
    mask_hi = CU(0);
    glsl = m.ExtInst("GLSL.std.450");
    ptr_input_f32 = PtrType(u32(spv::StorageClass::Input), t_f32);
    ptr_input_u32 = PtrType(u32(spv::StorageClass::Input), t_u32);
    ptr_output_f32 = PtrType(u32(spv::StorageClass::Output), t_f32);
    ptr_output_u32 = PtrType(u32(spv::StorageClass::Output), t_u32);
    ptr_output_s32 = PtrType(u32(spv::StorageClass::Output), t_s32);
    ptr_push_u32 = PtrType(u32(spv::StorageClass::PushConstant), t_u32);
    ptr_push_f32 = PtrType(u32(spv::StorageClass::PushConstant), t_f32);
    const std::array<u32, 14> members{t_f32, t_f32, t_f32, t_f32, t_v4u, t_v4u, t_v4u,
                                      t_v4u, t_v4u, t_v4u, t_v2u, t_u32, t_u32, t_v4f};
    const u32 member_count = in.tone_apply ? 14 : 13;
    push_type = m.AddUniqueType(O(spv::Op::OpTypeStruct), std::span<const u32>(members.data(), member_count));
    m.Decorate(push_type, u32(spv::Decoration::Block));
    const std::array<u32, 14> offsets{0, 4, 8, 12, 16, 32, 48, 64, 80, 96, 112, 120, 124, 128};
    for (u32 i = 0; i < member_count; ++i) {
        m.MemberDecorate(push_type, i, u32(spv::Decoration::Offset), offsets[i]);
    }
    push_block = m.Variable(PtrType(u32(spv::StorageClass::PushConstant), push_type),
                            u32(spv::StorageClass::PushConstant));
    m.Interface(push_block);
    main_id = m.NewId();
}

u32 Lift_::FragCoordRead(u32 comp) {
    if (!var_frag_coord) {
        var_frag_coord = InputVar(t_v4f, std::nullopt, spv::BuiltIn::FragCoord);
    }
    const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var_frag_coord, CU(comp)});
    const u32 value = Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
    if (comp >= 2) {
        return value;
    }
    const u32 packed = PushU32(12, -1);
    const u32 dimension = comp == 0 ? IOp(O(spv::Op::OpBitwiseAnd), packed, CU(65535))
                                    : IOp(O(spv::Op::OpShiftRightLogical), packed, CU(16));
    const u32 native = CU(comp == 0 ? 1920 : 1080);
    const u32 safe = Select(ICmp(O(spv::Op::OpIEqual), dimension, CU(0)), native, dimension);
    const u32 ratio = Emit(O(spv::Op::OpFDiv), t_f32, Ty::F32,
                           {Emit(O(spv::Op::OpConvertUToF), t_f32, Ty::F32, {native}),
                            Emit(O(spv::Op::OpConvertUToF), t_f32, Ty::F32, {safe})});
    return Emit(O(spv::Op::OpFMul), t_f32, Ty::F32, {value, ratio});
}

u32 Lift_::BaryRead(u32 kind, u32 comp) {
    const u32 id = m.NewId(Ty::F32);
    Node node;
    node.op = O(spv::Op::OpUndef);
    node.pseudo = 4;
    node.type = t_f32;
    node.result = id;
    node.nargs = 2;
    node.first = u32(args.size());
    node.lits = 3;
    args.push_back(kind);
    args.push_back(comp);
    const u32 index = u32(nodes.size());
    nodes.push_back(node);
    m.BindNode(id, index);
    out_blocks[cur].nodes.push_back(index);
    return id;
}

void Lift_::Prologue() {
    s.r[ExecReg] = true_id;
    s.r[RegScc] = false_id;
    for (u32 reg = 0; reg < in.num_user_sgprs; ++reg) {
        s.r[reg] = ReadUserData(reg);
    }
    for (u32 i = 0; i < in.buffers.size(); ++i) {
        const BufferSlot& buffer = in.buffers[i];
        if (!in.opt.buffer_offsets || buffer.kind != BufferKind::Guest || (buffer.uniform && buffer.uniform_exact)) {
            continue;
        }
        const u32 half = 8 + (buffer.offset_slot >> 4);
        const u32 comp = (buffer.offset_slot & 0xf) >> 2;
        const u32 shift = (buffer.offset_slot & 3) << 3;
        const u32 value = PushU32(half, s32(comp));
        buffer_off_id[i] = BfeU(value, CU(shift), CU(8));
        buffer_off_dwords[i] = IOp(O(spv::Op::OpShiftRightLogical), buffer_off_id[i], CU(2));
    }
    u32 v = SrcVgpr;
    switch (in.stage) {
    case Stage::Hull:
    case Stage::Domain:
    case Stage::Geometry:
        TessPrologue();
        break;
    case Stage::Vertex:
    case Stage::Local:
    case Stage::Export: {
        const auto load_vertex = [&] { return LoadBuiltinU32(var_vertex_index, spv::BuiltIn::VertexIndex); };
        const auto load_instance = [&] { return LoadBuiltinU32(var_instance_index, spv::BuiltIn::InstanceIndex); };
        u32 base_vertex = 0;
        u32 base_instance = 0;
        if (in.base_vertex_sgpr >= 0) {
            base_vertex = LoadBuiltinU32(var_base_vertex, spv::BuiltIn::BaseVertex);
            s.r[in.base_vertex_sgpr] = base_vertex;
        }
        if (in.base_instance_sgpr >= 0) {
            base_instance = LoadBuiltinU32(var_base_instance, spv::BuiltIn::BaseInstance);
            s.r[in.base_instance_sgpr] = base_instance;
        }
        u32 vertex_id = load_vertex();
        if (base_vertex && !in.fetch_applies_base_vertex) {
            vertex_id = IOp(O(spv::Op::OpISub), vertex_id, base_vertex);
        }
        s.r[v++] = vertex_id;
        if (in.stage == Stage::Local) {
            if (in.num_input_vgprs > 0) {
                s.r[v++] = CU(0);
            }
            if (in.num_input_vgprs > 1) {
                ++v;
            }
        } else if (in.num_input_vgprs > 0) {
            s.r[v++] = in.step_rate_0 ? Emit(O(spv::Op::OpUDiv), t_u32, Ty::U32, {load_instance(), CU(in.step_rate_0)}) : CU(0);
        }
        if (in.stage != Stage::Local && in.num_input_vgprs > 1) {
            s.r[v++] = in.step_rate_1 ? Emit(O(spv::Op::OpUDiv), t_u32, Ty::U32, {load_instance(), CU(in.step_rate_1)}) : CU(0);
        }
        if (in.num_input_vgprs > 2) {
            u32 instance = load_instance();
            if (base_instance && !in.fetch_applies_base_instance) {
                instance = IOp(O(spv::Op::OpISub), instance, base_instance);
            }
            s.r[v++] = instance;
        }
        break;
    }
    case Stage::Fragment: {
        const PsVgprs& addr = in.ps_addr;
        const PsVgprs& en = in.ps_en;
        const bool bary_values = in.opt.amd_explicit_vertex || in.opt.khr_barycentric;
        const auto bary = [&](u32 kind, u32 comps) {
            for (u32 c = 0; c < comps; ++c) {
                vgpr_bary[v - SrcVgpr] = u8(kind);
                if (bary_values) {
                    s.r[v] = BaryRead(kind, c);
                }
                ++v;
            }
        };
        vgpr_bary.fill(BaryNone);
        if (addr.persp_sample) {
            bary(PerspSample, 2);
        }
        if (addr.persp_center) {
            bary(PerspCenter, 2);
        }
        if (addr.persp_centroid) {
            bary(PerspCentroid, 2);
        }
        if (addr.persp_pull) {
            bary(PerspPull, 3);
        }
        if (addr.linear_sample) {
            bary(LinearSample, 2);
        }
        if (addr.linear_center) {
            bary(LinearCenter, 2);
        }
        if (addr.linear_centroid) {
            bary(LinearCentroid, 2);
        }
        if (addr.line_stipple) {
            ++v;
        }
        const auto pos = [&](bool has, bool on, u32 comp) {
            if (!has) {
                return;
            }
            if (!on) {
                s.r[v++] = CF(0.f);
                return;
            }
            const u32 value = FragCoordRead(comp);
            s.r[v++] = comp == 3 ? Emit(O(spv::Op::OpFDiv), t_f32, Ty::F32, {CF(1.f), value}) : value;
        };
        pos(addr.pos_x, en.pos_x, 0);
        pos(addr.pos_y, en.pos_y, 1);
        pos(addr.pos_z, en.pos_z, 2);
        pos(addr.pos_w, en.pos_w, 3);
        if (addr.front_face) {
            if (en.front_face) {
                if (!var_front_face) {
                    var_front_face = InputVar(t_bool, std::nullopt, spv::BuiltIn::FrontFacing);
                }
                const u32 face = Emit(O(spv::Op::OpLoad), t_bool, Ty::Bool, {var_front_face});
                s.r[v++] = in.front_face_all_bits ? Select(face, CU(1), CU(0)) : Select(face, CF(1.f), CF(-1.f));
            } else {
                s.r[v++] = CU(0);
            }
        }
        if (addr.ancillary) {
            if (en.ancillary) {
                const u32 id = Emit(O(spv::Op::OpUndef), t_u32, Ty::U32, {});
                undefined_reads.push_back(id);
                s.r[v++] = id;
            } else {
                s.r[v++] = CU(0);
            }
        }
        if (addr.sample_coverage) {
            if (en.sample_coverage) {
                const u32 id = Emit(O(spv::Op::OpUndef), t_u32, Ty::U32, {});
                undefined_reads.push_back(id);
                s.r[v++] = id;
            } else {
                s.r[v++] = CU(0);
            }
        }
        if (in.clip_emulation_input) {
            const u32 var = InputVar(t_v4f, 0u, std::nullopt);
            for (u32 c = 0; c < 4; ++c) {
                const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var, CU(c)});
                const u32 value = Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
                Discard(FCmp(O(spv::Op::OpFOrdLessThan), value, CF(0.f)));
            }
        }
        break;
    }
    case Stage::Compute: {
        const auto invocation = [&](u32 comp) {
            if (!var_local_id) {
                var_local_id = InputVar(t_v3u, std::nullopt, spv::BuiltIn::LocalInvocationId);
            }
            const u32 value = Emit(O(spv::Op::OpLoad), t_v3u, Ty::V3U, {var_local_id});
            return Extract(t_u32, Ty::U32, value, comp);
        };
        s.r[v++] = invocation(0);
        s.r[v++] = invocation(1);
        s.r[v++] = invocation(2);
        u32 sgpr = in.num_user_sgprs;
        for (u32 c = 0; c < 3; ++c) {
            if (in.tgid_enable[c]) {
                if (!var_work_id) {
                    var_work_id = InputVar(t_v3u, std::nullopt, spv::BuiltIn::WorkgroupId);
                }
                const u32 value = Emit(O(spv::Op::OpLoad), t_v3u, Ty::V3U, {var_work_id});
                s.r[sgpr++] = Extract(t_u32, Ty::U32, value, c);
            }
        }
        break;
    }
    default:
        Fail("stage");
    }
}

void Lift_::SendMsg(const Inst& inst) {
    const u32 simm = u32(inst.simm) & 0xffff;
    const u32 msg = simm & 15;
    const u32 op = (simm >> 4) & 7;
    if (msg == 3) {
        return;
    }
    if (msg != 2) {
        Fail("message " + std::to_string(msg));
    }
    if (op == 1) {
        EmitVoid(O(spv::Op::OpEndPrimitive), {});
    } else if (op == 2) {
        EmitVoid(O(spv::Op::OpEmitVertex), {});
    } else if (op != 0) {
        Fail("geometry message " + std::to_string(op));
    }
}

void Lift_::Discard(u32 cond) {
    if (IsFalse(cond)) {
        return;
    }
    uses_discard = true;
    if (IsTrue(cond)) {
        EmitVoid(O(spv::Op::OpDemoteToHelperInvocation), {});
        return;
    }
    const u32 kill = m.NewId();
    const u32 merge = m.NewId();
    EmitVoid(O(spv::Op::OpSelectionMerge), {merge, 0}, 2);
    EmitVoid(O(spv::Op::OpBranchConditional), {cond, kill, merge});
    Open(kill);
    EmitVoid(O(spv::Op::OpDemoteToHelperInvocation), {});
    EmitVoid(O(spv::Op::OpBranch), {merge});
    Open(merge);
    ++out.stats.ifs_for_exec;
}

void Lift_::LiftInterp(const Inst& inst) {
    if (in.stage != Stage::Fragment) {
        Fail("interpolation outside a pixel shader");
    }
    const u32 attr = inst.attr;
    if (attr >= in.ps_inputs.size()) {
        Fail("interpolation of an input that is not there");
    }
    if (in.discover && inst.op != Op::VINTRP_MOV && vgpr_bary[inst.src[0]] == BaryNone) {
        vgpr_bary[inst.src[0]] = PerspCenter;
    }
    const PsInput& input = in.ps_inputs[attr];
    const bool is_default = input.is_default && !input.is_flat;
    if (inst.op == Op::VINTRP_P1) {
        if (!in.opt.manual_interpolation || is_default) {
            return;
        }
    }
    if (inst.op == Op::VINTRP_P2 && is_default) {
        static constexpr float table[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 0}, {1, 1, 1, 1}};
        return SetVgpr(inst.dst, CF(table[input.default_value & 3][inst.chan]));
    }
    if (inst.op == Op::VINTRP_MOV && is_default) {
        Fail("flat read of a default input");
    }
    const u32 kind = inst.op == Op::VINTRP_MOV ? 0 : vgpr_bary[inst.src[0]];
    const u32 id = m.NewId(Ty::F32);
    Node node;
    node.op = O(spv::Op::OpUndef);
    node.type = t_f32;
    node.result = id;
    node.first = u32(args.size());
    node.lits = 0x7;
    if (inst.op == Op::VINTRP_P1) {
        node.pseudo = PseudoInterpP1;
        node.nargs = 4;
        args.insert(args.end(), {attr, inst.chan, kind, ToF(s.r[SrcVgpr + inst.src[0]])});
    } else if (inst.op == Op::VINTRP_P2) {
        node.pseudo = PseudoInterpP2;
        node.nargs = 5;
        const u32 previous = in.opt.manual_interpolation ? ToF(s.r[inst.dst]) : CF(0.f);
        args.insert(args.end(), {attr, inst.chan, kind, ToF(s.r[SrcVgpr + inst.src[0]]), previous});
        if (!in.opt.manual_interpolation) {
            u8 primary{};
            u8 aux{};
            Qualify(kind, primary, aux);
            interp_decl[attr] = primary;
            interp_aux[attr] = aux;
        } else {
            interp_decl[attr] = QualPerVertex;
        }
    } else {
        node.pseudo = PseudoInterpMov;
        node.nargs = 3;
        args.insert(args.end(), {attr, inst.chan, inst.src[0]});
        interp_decl[attr] = (in.opt.amd_explicit_vertex || in.opt.khr_barycentric) ? QualPerVertex : QualFlat;
        interp_other_read[attr] = 1;
    }
    const u32 index = u32(nodes.size());
    nodes.push_back(node);
    m.BindNode(id, index);
    out_blocks[cur].nodes.push_back(index);
    SetVgpr(inst.dst, id);
}

u32 Lift_::LoadAttrComp(u32 attr, u32 comp, s32 vertex) {
    if (!var_param_in[attr]) {
        const PsInput& input = in.ps_inputs[attr];
        const u32 location = input.param + (in.clip_emulation_input ? 1 : 0);
        const bool per_vertex = interp_decl[attr] == QualPerVertex;
        const u32 type = per_vertex && in.opt.khr_barycentric && !in.opt.amd_explicit_vertex
                             ? m.AddType(O(spv::Op::OpTypeArray), {t_v4f, CU(3)})
                             : t_v4f;
        const u32 var = InputVar(type, location, std::nullopt);
        if (per_vertex) {
            m.Decorate(var, in.opt.amd_explicit_vertex ? u32(spv::Decoration::ExplicitInterpAMD)
                                                       : u32(spv::Decoration::PerVertexKHR));
        } else if (interp_decl[attr] == QualFlat) {
            m.Decorate(var, u32(spv::Decoration::Flat));
        } else if (interp_decl[attr] == QualNoPersp) {
            m.Decorate(var, u32(spv::Decoration::NoPerspective));
        }
        if (interp_aux[attr] == AuxCentroid) {
            m.Decorate(var, u32(spv::Decoration::Centroid));
        } else if (interp_aux[attr] == AuxSample) {
            m.Decorate(var, u32(spv::Decoration::Sample));
            uses_sample_shading = true;
        }
        var_param_in[attr] = var;
    }
    const u32 var = var_param_in[attr];
    if (vertex >= 0 && in.opt.amd_explicit_vertex) {
        m.Extension("SPV_AMD_shader_explicit_vertex_parameter");
        const u32 set = m.ExtInst("SPV_AMD_shader_explicit_vertex_parameter");
        const u32 id = m.NewId(Ty::V4F);
        Node node;
        node.op = O(spv::Op::OpExtInst);
        node.type = t_v4f;
        node.result = id;
        node.nargs = 4;
        node.first = u32(args.size());
        node.lits = 2;
        args.insert(args.end(), {set, 1, var, CU(u32(vertex))});
        const u32 index = u32(nodes.size());
        nodes.push_back(node);
        m.BindNode(id, index);
        out_blocks[cur].nodes.push_back(index);
        return Extract(t_f32, Ty::F32, id, comp);
    }
    const u32 ptr = vertex >= 0
                        ? Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var, CU(u32(vertex)), CU(comp)})
                        : Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var, CU(comp)});
    return Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
}

void Lift_::StoreOutput(u32 var, u32 type, u32 ptr_type, s32 comp, u32 value) {
    (void)type;
    if (comp < 0) {
        EmitVoid(O(spv::Op::OpStore), {var, value});
        return;
    }
    const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_type, Ty::None, {var, CU(u32(comp))});
    EmitVoid(O(spv::Op::OpStore), {ptr, value});
}

void Lift_::StoreClipDistance(u32 index, u32 value) {
    if (vs_clip_emulated) {
        if (!var_clip) {
            var_clip = OutputVar(t_v4f, 0u, std::nullopt);
        }
        if (index >= 4) {
            Fail("clip distance beyond the emulated four");
        }
    } else if (!var_clip) {
        const u32 array = m.AddType(O(spv::Op::OpTypeArray), {t_f32, CU(8)});
        const std::array<u32, 8> parts{CF(0), CF(0), CF(0), CF(0), CF(0), CF(0), CF(0), CF(0)};
        var_clip = OutputVar(array, std::nullopt, spv::BuiltIn::ClipDistance, m.ConstComposite(array, parts));
        m.Capability(u32(spv::Capability::ClipDistance));
    }
    StoreOutput(var_clip, t_f32, ptr_output_f32, s32(index), value);
}

void Lift_::UserClipPlanes() {
    if (in.clip_plane_slot < 0) {
        Fail("user clip planes without their buffer");
    }
    const u32 position = Emit(O(spv::Op::OpLoad), t_v4f, Ty::V4F, {var_position});
    for (u32 plane = 0; plane < 6; ++plane) {
        if (((in.user_clip_plane_mask >> plane) & 1) == 0) {
            continue;
        }
        const auto word = [&](u32 comp) { return ToF(BufferDword(u32(in.clip_plane_slot), CU(plane * 4 + comp))); };
        u32 distance = FMul(Extract(t_f32, Ty::F32, position, 0), word(0));
        for (u32 comp = 1; comp < 4; ++comp) {
            distance = FFma(Extract(t_f32, Ty::F32, position, comp), word(comp), distance);
        }
        StoreClipDistance(plane, distance);
    }
}

void Lift_::ExportPos(u32 target, u32 comp, u32 value) {
    if (!var_position) {
        var_position = OutputVar(t_v4f, std::nullopt, spv::BuiltIn::Position);
    }
    if (target == AttrPos0) {
        StoreOutput(var_position, t_f32, ptr_output_f32, s32(comp), value);
        return;
    }
    const u32 output = in.vs_outputs[target - AttrPos0 - 1][comp];
    if (output == 0) {
        return;
    }
    const auto zero_array = [&] {
        const u32 array = m.AddType(O(spv::Op::OpTypeArray), {t_f32, CU(8)});
        const std::array<u32, 8> parts{CF(0), CF(0), CF(0), CF(0), CF(0), CF(0), CF(0), CF(0)};
        return std::pair{array, m.ConstComposite(array, parts)};
    };
    if (output >= 15 && output <= 22) {
        StoreClipDistance(output - 15, value);
        return;
    }
    if (output >= 7 && output <= 14) {
        if (!var_cull) {
            const auto [array, init] = zero_array();
            var_cull = OutputVar(array, std::nullopt, spv::BuiltIn::CullDistance, init);
            m.Capability(u32(spv::Capability::CullDistance));
        }
        StoreOutput(var_cull, t_f32, ptr_output_f32, s32(output - 7), value);
        return;
    }
    if (output == 1) {
        if (!var_point_size) {
            var_point_size = OutputVar(t_f32, std::nullopt, spv::BuiltIn::PointSize);
        }
        StoreOutput(var_point_size, t_f32, 0, -1, value);
        return;
    }
    if (output == 5 || output == 6) {
        u32& var = output == 5 ? var_layer : var_viewport;
        if (!var) {
            var = OutputVar(t_u32, std::nullopt, output == 5 ? spv::BuiltIn::Layer : spv::BuiltIn::ViewportIndex);
            m.Capability(output == 5 ? u32(spv::Capability::ShaderLayer) : u32(spv::Capability::ShaderViewportIndex));
        }
        StoreOutput(var, t_u32, 0, -1, ToU(value));
        return;
    }
    Fail("position output " + std::to_string(output));
}

void Lift_::ExportAttr(u32 target, u32 comp, u32 value) {
    const u32 index = target - AttrParam;
    if (!var_param_out[index]) {
        var_param_out[index] = OutputVar(t_v4f, index + (vs_clip_emulated ? 1 : 0), std::nullopt);
    }
    StoreOutput(var_param_out[index], t_f32, ptr_output_f32, s32(comp), value);
}

u32 Lift_::ExportedValue(u32 id) {
    while (export_exec != 0 && id != 0 && !m.IsConst(id)) {
        const Node& node = nodes[m.NodeOf(id)];
        if (node.op != O(spv::Op::OpSelect) || m.Forward(args[node.first]) != export_exec) {
            break;
        }
        id = args[node.first + 1];
    }
    return id;
}

std::pair<u32, u32> Lift_::UnpackedHalves(u32 id) {
    id = m.Forward(id);
    if (m.IsConst(id)) {
        return {0, 0};
    }
    const u32 index = m.NodeOf(id);
    const Node node = nodes[index];
    if (node.op == O(spv::Op::OpExtInst) && args[node.first + 1] == GLSLstd450PackHalf2x16) {
        const Node& pair = nodes[m.NodeOf(args[node.first + 2])];
        return {args[pair.first], args[pair.first + 1]};
    }
    if (node.op == O(spv::Op::OpBitcast)) {
        return UnpackedHalves(args[node.first]);
    }
    if (node.op == O(spv::Op::OpSelect) && export_exec != 0 && m.Forward(args[node.first]) == export_exec) {
        return UnpackedHalves(args[node.first + 1]);
    }
    if (node.op != O(spv::Op::OpPhi)) {
        return {0, 0};
    }
    std::vector<std::array<u32, 2>> incoming;
    for (u32 a = 0; a + 1 < node.nargs; a += 2) {
        const auto [low, high] = UnpackedHalves(args[node.first + a]);
        if (!low) {
            return {0, 0};
        }
        incoming.push_back({low, high});
    }
    for (OutBlock& block : out_blocks) {
        const auto at = std::find(block.nodes.begin(), block.nodes.end(), index);
        if (at == block.nodes.end()) {
            continue;
        }
        std::array<u32, 2> made{};
        for (u32 part = 0; part < 2; ++part) {
            const u32 result = m.NewId(Ty::F32);
            Node phi;
            phi.op = O(spv::Op::OpPhi);
            phi.type = t_f32;
            phi.result = result;
            phi.nargs = node.nargs;
            phi.first = u32(args.size());
            for (u32 a = 0; a + 1 < node.nargs; a += 2) {
                const u32 label = args[node.first + a + 1];
                args.push_back(incoming[a / 2][part]);
                args.push_back(label);
            }
            const u32 phi_index = u32(nodes.size());
            nodes.push_back(phi);
            m.BindNode(result, phi_index);
            const auto spot = std::find(block.nodes.begin(), block.nodes.end(), index);
            block.nodes.insert(spot, phi_index);
            made[part] = result;
        }
        return {made[0], made[1]};
    }
    return {0, 0};
}

void Lift_::ExportMrt(const Inst& inst) {
    const u32 mrt = inst.target;
    u32 color_index = mrt;
    if (in.dual_source && mrt == 1) {
        color_index = 0;
    }
    ColorOut color = in.color[color_index];
    if (in.discover) {
        color.export_format = inst.compr ? 4 : 9;
    }
    if (color.export_format == 0 || inst.en == 0) {
        return;
    }
    std::array<u32, 4> comps{};
    if (inst.compr) {
        const auto unpack = [&](u32 reg, u32 first) {
            const u32 raw = ToU(s.r[SrcVgpr + reg]);
            u32 x = 0;
            u32 y = 0;
            switch (color.export_format) {
            case 4: {
                const auto [low, high] = UnpackedHalves(raw);
                if (low) {
                    comps[first] = low;
                    comps[first + 1] = high;
                    return;
                }
                const u32 pair = Ext(GLSLstd450UnpackHalf2x16, t_v2f, Ty::V2F, {raw});
                x = Extract(t_f32, Ty::F32, pair, 0);
                y = Extract(t_f32, Ty::F32, pair, 1);
                break;
            }
            case 5: {
                const u32 pair = Ext(GLSLstd450UnpackUnorm2x16, t_v2f, Ty::V2F, {raw});
                x = Extract(t_f32, Ty::F32, pair, 0);
                y = Extract(t_f32, Ty::F32, pair, 1);
                break;
            }
            case 6: {
                const u32 pair = Ext(GLSLstd450UnpackSnorm2x16, t_v2f, Ty::V2F, {raw});
                x = Extract(t_f32, Ty::F32, pair, 0);
                y = Extract(t_f32, Ty::F32, pair, 1);
                break;
            }
            case 7:
                x = ToF(BfeU(raw, CU(0), CU(16)));
                y = ToF(BfeU(raw, CU(16), CU(16)));
                break;
            case 8:
                x = ToF(BfeS(raw, CU(0), CU(16)));
                y = ToF(BfeS(raw, CU(16), CU(16)));
                break;
            default:
                Fail("compressed export format");
            }
            comps[first] = x;
            comps[first + 1] = y;
        };
        if (inst.en & 1) {
            unpack(inst.src[0], 0);
        }
        if ((inst.en >> 2) & 1) {
            unpack(inst.src[1], 2);
        }
    } else {
        u32 mask = inst.en;
        switch (color.export_format) {
        case 1:
            mask &= 1;
            break;
        case 2:
            mask &= 3;
            break;
        case 3:
            mask &= 9;
            break;
        case 9:
            break;
        default:
            Fail("export format " + std::to_string(color.export_format));
        }
        for (u32 c = 0; c < 4; ++c) {
            if ((mask >> c) & 1) {
                comps[c] = ToF(ExportedValue(s.r[SrcVgpr + inst.src[c]]));
            }
        }
    }
    if (!var_color[mrt]) {
        const u32 type = color.cls == NumClass::Float ? t_v4f : color.cls == NumClass::Uint ? t_v4u : t_v4s;
        u32 var;
        if (in.dual_source) {
            var = OutputVar(type, 0u, std::nullopt);
            m.Decorate(var, u32(spv::Decoration::Index), mrt);
        } else {
            var = OutputVar(type, mrt, std::nullopt);
        }
        var_color[mrt] = var;
    }
    const u32 ptr_type = color.cls == NumClass::Float ? ptr_output_f32 : color.cls == NumClass::Uint ? ptr_output_u32 : ptr_output_s32;
    const u32 scalar = color.cls == NumClass::Float ? t_f32 : color.cls == NumClass::Uint ? t_u32 : t_s32;
    for (u32 c = 0; c < 4; ++c) {
        const u8 sel = color.swizzle[c];
        const u32 from = sel >= 4 ? sel - 4u : c;
        u32 value = comps[from];
        if (value == 0) {
            continue;
        }
        value = ApplyWriteConv(value, color.conv);
        if (c < 3 && color.self_scale) {
            value = Emit(O(spv::Op::OpFMul), t_f32, Ty::F32, {value, value});
        }
        if (color.cls != NumClass::Float) {
            value = Emit(O(spv::Op::OpBitcast), scalar, Ty::None, {value});
        }
        StoreOutput(var_color[mrt], scalar, ptr_type, s32(c), value);
    }
}

void Lift_::ExportZ(const Inst& inst) {
    if (inst.en == 0) {
        return;
    }
    std::array<u32, 4> comps{};
    if (inst.compr) {
        if (in.discover) {
            return;
        }
        Fail("compressed depth export");
    }
    u32 mask = inst.en & in.mrtz_mask;
    switch (in.z_export_format) {
    case 1:
        mask &= 1;
        break;
    case 2:
        mask &= 3;
        break;
    case 3:
        mask &= 9;
        break;
    case 9:
        break;
    default:
        Fail("depth export format " + std::to_string(in.z_export_format));
    }
    for (u32 c = 0; c < 4; ++c) {
        if ((mask >> c) & 1) {
            comps[c] = ToF(s.r[SrcVgpr + inst.src[c]]);
        }
    }
    if (comps[0]) {
        if (!var_frag_depth) {
            var_frag_depth = OutputVar(t_f32, std::nullopt, spv::BuiltIn::FragDepth);
        }
        EmitVoid(O(spv::Op::OpStore), {var_frag_depth, comps[0]});
    }
    if (comps[1] && in.opt.stencil_export) {
        if (!var_stencil) {
            var_stencil = OutputVar(t_s32, std::nullopt, spv::BuiltIn::FragStencilRefEXT);
        }
        stores_stencil = true;
        EmitVoid(O(spv::Op::OpStore), {var_stencil, Emit(O(spv::Op::OpBitcast), t_s32, Ty::None, {comps[1]})});
    }
    if (comps[2]) {
        if (!var_sample_mask) {
            const u32 array = m.AddType(O(spv::Op::OpTypeArray), {t_u32, CU(1)});
            var_sample_mask = OutputVar(array, std::nullopt, spv::BuiltIn::SampleMask);
        }
        stores_mask = true;
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_output_u32, Ty::None, {var_sample_mask, CU(0)});
        EmitVoid(O(spv::Op::OpStore), {ptr, ToU(comps[2])});
    }
}

void Lift_::LiftExport(const Inst& inst) {
    if (in.stage == Stage::Fragment && inst.vm) {
        Discard(Not(Exec()));
    }
    export_exec = in.stage == Stage::Fragment && !IsTrue(Exec()) ? Exec() : 0;
    const u32 target = inst.target;
    if (target < 8) {
        return ExportMrt(inst);
    }
    if (target == 8) {
        return ExportZ(inst);
    }
    if (target == 9) {
        return;
    }
    if (inst.compr) {
        Fail("compressed export of a vertex output");
    }
    for (u32 c = 0; c < 4; ++c) {
        if (!((inst.en >> c) & 1)) {
            continue;
        }
        const u32 value = ToF(s.r[SrcVgpr + inst.src[c]]);
        if (target >= AttrPos0 && target < AttrPos0 + 4) {
            ExportPos(target, c, value);
        } else if (target >= AttrParam && target < AttrParam + 32) {
            ExportAttr(target, c, value);
        } else {
            Fail("export target " + std::to_string(target));
        }
    }
}

void Lift_::Epilogue() {
    if (user_clip_planes) {
        UserClipPlanes();
    }
    if (in.vertex_motion) {
        VertexMotion();
    }
    if (in.vertex_capture) {
        VertexCapture();
    }
    if (in.fragment_motion && !var_color[7]) {
        FragmentMotion();
    }
    if ((in.stage == Stage::Vertex || in.stage == Stage::Domain) && (in.emulate_depth || in.clip_disable)) {
        if (!var_position) {
            Fail("position conversion without a position");
        }
        if (in.emulate_depth) {
            const u32 position = Emit(O(spv::Op::OpLoad), t_v4f, Ty::V4F, {var_position});
            const u32 z = Extract(t_f32, Ty::F32, position, 2);
            const u32 w = Extract(t_f32, Ty::F32, position, 3);
            const u32 depth = Emit(O(spv::Op::OpFMul), t_f32, Ty::F32, {Emit(O(spv::Op::OpFAdd), t_f32, Ty::F32, {z, w}), CF(0.5f)});
            const u32 vec = Emit(O(spv::Op::OpCompositeInsert), t_v4f, Ty::V4F, {depth, position, 2}, 4);
            EmitVoid(O(spv::Op::OpStore), {var_position, vec});
        }
        if (in.clip_disable) {
            if (var_viewport) {
                Fail("clip space conversion with viewports");
            }
            const u32 position = Emit(O(spv::Op::OpLoad), t_v4f, Ty::V4F, {var_position});
            const u32 x = Extract(t_f32, Ty::F32, position, 0);
            const u32 y = Extract(t_f32, Ty::F32, position, 1);
            const u32 z = Extract(t_f32, Ty::F32, position, 2);
            const u32 w = Extract(t_f32, Ty::F32, position, 3);
            const u32 xoffset = PushF32(0);
            const u32 yoffset = PushF32(1);
            const u32 xscale = PushF32(2);
            const u32 yscale = PushF32(3);
            const float vw = float(std::min<u32>(in.opt.max_viewport_width / 2, 8192));
            const float vh = float(std::min<u32>(in.opt.max_viewport_height / 2, 8192));
            const auto f = [&](u16 op, u32 a, u32 b) { return Emit(op, t_f32, Ty::F32, {a, b}); };
            const u32 wnd_x = f(O(spv::Op::OpFAdd), f(O(spv::Op::OpFMul), x, xscale), xoffset);
            const u32 ndc_x = f(O(spv::Op::OpFSub), f(O(spv::Op::OpFDiv), wnd_x, CF(vw)), CF(1.f));
            const u32 wnd_y = f(O(spv::Op::OpFAdd), f(O(spv::Op::OpFMul), y, yscale), yoffset);
            const u32 ndc_y = f(O(spv::Op::OpFSub), f(O(spv::Op::OpFDiv), wnd_y, CF(vh)), CF(1.f));
            EmitVoid(O(spv::Op::OpStore), {var_position, ComponentsF({ndc_x, ndc_y, z, w})});
        }
    }
}

void Lift_::MotionBlock(bool outputs) {
    if (!var_position) {
        Fail("motion vectors without a position");
    }
    if (!t_u64) {
        t_u64 = m.AddType(O(spv::Op::OpTypeInt), {64, 0});
        m.Capability(u32(spv::Capability::Int64));
    }
    if (!var_motion_addr) {
        const bool both = in.vertex_motion && in.vertex_capture;
        const std::array<u32, 2> members{t_u64, t_u64};
        const std::array<u32, 4> both_members{t_u64, t_u64, t_u64, t_u64};
        const u32 block = both ? m.AddUniqueType(O(spv::Op::OpTypeStruct), both_members)
                               : m.AddUniqueType(O(spv::Op::OpTypeStruct), members);
        m.Decorate(block, u32(spv::Decoration::Block));
        m.MemberDecorate(block, 0, u32(spv::Decoration::Offset), 0);
        m.MemberDecorate(block, 1, u32(spv::Decoration::Offset), 8);
        if (both) {
            m.MemberDecorate(block, 2, u32(spv::Decoration::Offset), 16);
            m.MemberDecorate(block, 3, u32(spv::Decoration::Offset), 24);
        }
        var_motion_addr = m.Variable(PtrType(u32(spv::StorageClass::Uniform), block), u32(spv::StorageClass::Uniform));
        m.Decorate(var_motion_addr, u32(spv::Decoration::DescriptorSet), 1);
        m.Decorate(var_motion_addr, u32(spv::Decoration::Binding), 0);
        m.Interface(var_motion_addr);
        if (outputs) {
            var_motion_cur_out = OutputVar(t_v4f, 30u, std::nullopt);
            var_motion_prev_out = OutputVar(t_v4f, 31u, std::nullopt);
        }
        m.Capability(u32(spv::Capability::PhysicalStorageBufferAddresses));
        m.Extension("SPV_KHR_physical_storage_buffer");
        physical_addressing = true;
    }
}

void Lift_::VertexMotion() {
    MotionBlock(true);
    const u32 storage = u32(spv::StorageClass::PhysicalStorageBuffer);
    const u32 ptr_u64 = PtrType(u32(spv::StorageClass::Uniform), t_u64);
    const auto wide = [&](u32 value) { return Emit(O(spv::Op::OpUConvert), t_u64, Ty::U64, {value}); };
    const auto c64 = [&](u64 value) { return m.ConstU64(t_u64, value); };
    const auto iadd64 = [&](u32 a, u32 b) { return Emit(O(spv::Op::OpIAdd), t_u64, Ty::U64, {a, b}); };
    const auto address = [&](u32 base, u32 index, u32 stride) {
        return iadd64(base, Emit(O(spv::Op::OpIMul), t_u64, Ty::U64, {wide(index), c64(stride)}));
    };
    const u32 position = Emit(O(spv::Op::OpLoad), t_v4f, Ty::V4F, {var_position});
    const u32 raw_param = PushU32(11, -1);
    const u32 masked_param = in.vertex_capture ? IOp(O(spv::Op::OpBitwiseAnd), raw_param, CU(0xffffu)) : raw_param;
    const u32 param_index = Select(ICmp(O(spv::Op::OpULessThan), masked_param, CU(MotionParamsCapacity)), masked_param, CU(0));
    const u32 params_address = Emit(O(spv::Op::OpLoad), t_u64, Ty::U64, {Emit(O(spv::Op::OpAccessChain), ptr_u64, Ty::None, {var_motion_addr, CU(0)})});
    const u32 positions_address = Emit(O(spv::Op::OpLoad), t_u64, Ty::U64, {Emit(O(spv::Op::OpAccessChain), ptr_u64, Ty::None, {var_motion_addr, CU(1)})});
    const u32 ptr_v4u = PtrType(storage, t_v4u);
    const u32 ptr_v4f = PtrType(storage, t_v4f);
    const u32 aligned = u32(spv::MemoryAccessMask::Aligned);
    const u32 params_ptr = Emit(O(spv::Op::OpConvertUToPtr), ptr_v4u, Ty::None, {address(params_address, param_index, 32)});
    const u32 params = Emit(O(spv::Op::OpLoad), t_v4u, Ty::V4U, {params_ptr, aligned, 16}, 6);
    const u32 store_base = Extract(t_u32, Ty::U32, params, 0);
    const u32 load_base = Extract(t_u32, Ty::U32, params, 1);
    const u32 vertices = Extract(t_u32, Ty::U32, params, 2);
    const u32 flags = Extract(t_u32, Ty::U32, params, 3);
    const u32 offsets_ptr = Emit(O(spv::Op::OpConvertUToPtr), ptr_v4u, Ty::None, {iadd64(address(params_address, param_index, 32), c64(16))});
    const u32 offsets = Emit(O(spv::Op::OpLoad), t_v4u, Ty::V4U, {offsets_ptr, aligned, 16}, 6);
    const u32 first_vertex = Extract(t_u32, Ty::U32, offsets, 0);
    const u32 first_instance = Extract(t_u32, Ty::U32, offsets, 1);
    const u32 instances = Extract(t_u32, Ty::U32, offsets, 2);
    const u32 vertex = IOp(O(spv::Op::OpISub), LoadBuiltinU32(var_vertex_index, spv::BuiltIn::VertexIndex), first_vertex);
    const u32 instance = IOp(O(spv::Op::OpISub), LoadBuiltinU32(var_instance_index, spv::BuiltIn::InstanceIndex), first_instance);
    const u32 in_range = Emit(O(spv::Op::OpLogicalAnd), t_bool, Ty::Bool,
                              {ICmp(O(spv::Op::OpULessThan), vertex, vertices), ICmp(O(spv::Op::OpULessThan), instance, instances)});
    const u32 slot = IOp(O(spv::Op::OpIAdd), vertex, IOp(O(spv::Op::OpIMul), instance, vertices));
    const auto flag = [&](u32 bit) {
        return Emit(O(spv::Op::OpLogicalAnd), t_bool, Ty::Bool,
                    {in_range, ICmp(O(spv::Op::OpINotEqual), IOp(O(spv::Op::OpBitwiseAnd), flags, CU(bit)), CU(0))});
    };
    const u32 do_store = flag(1);
    const u32 do_load = flag(2);
    const u32 store_label = m.NewId();
    const u32 store_merge = m.NewId();
    EmitVoid(O(spv::Op::OpSelectionMerge), {store_merge, 0}, 2);
    EmitVoid(O(spv::Op::OpBranchConditional), {do_store, store_label, store_merge});
    Open(store_label);
    const u32 store_address = address(positions_address, IOp(O(spv::Op::OpIAdd), store_base, slot), 16);
    const u32 ptr_scalar = PtrType(storage, t_u32);
    for (u32 i = 0; i < 4; ++i) {
        const u32 ptr = Emit(O(spv::Op::OpConvertUToPtr), ptr_scalar, Ty::None, {iadd64(store_address, c64(i * 4))});
        const u32 bits = Emit(O(spv::Op::OpBitcast), t_u32, Ty::U32, {Extract(t_f32, Ty::F32, position, i)});
        const u32 prev = Emit(O(spv::Op::OpAtomicExchange), t_u32, Ty::U32, {ptr, CU(u32(spv::Scope::Device)), CU(0), bits});
        nodes[m.NodeOf(prev)].effect = true;
    }
    EmitVoid(O(spv::Op::OpBranch), {store_merge});
    Open(store_merge);
    const u32 load_label = m.NewId();
    const u32 load_merge = m.NewId();
    EmitVoid(O(spv::Op::OpSelectionMerge), {load_merge, 0}, 2);
    EmitVoid(O(spv::Op::OpBranchConditional), {do_load, load_label, load_merge});
    Open(load_label);
    const u32 load_ptr = Emit(O(spv::Op::OpConvertUToPtr), ptr_v4f, Ty::None, {address(positions_address, IOp(O(spv::Op::OpIAdd), load_base, slot), 16)});
    const u32 loaded = Emit(O(spv::Op::OpLoad), t_v4f, Ty::V4F, {load_ptr, aligned, 16}, 6);
    EmitVoid(O(spv::Op::OpBranch), {load_merge});
    Open(load_merge);
    u32 previous = Emit(O(spv::Op::OpPhi), t_v4f, Ty::V4F, {position, store_merge, loaded, load_label});
    const u32 valid = Select(do_load, CF(1.f), CF(0.f));
    previous = Emit(O(spv::Op::OpCompositeInsert), t_v4f, Ty::V4F, {valid, previous, 2}, 4);
    EmitVoid(O(spv::Op::OpStore), {var_motion_cur_out, position});
    EmitVoid(O(spv::Op::OpStore), {var_motion_prev_out, previous});
}

void Lift_::VertexCapture() {
    MotionBlock(false);
    const bool both = in.vertex_motion;
    const u32 storage = u32(spv::StorageClass::PhysicalStorageBuffer);
    const u32 ptr_u64 = PtrType(u32(spv::StorageClass::Uniform), t_u64);
    const auto wide = [&](u32 value) { return Emit(O(spv::Op::OpUConvert), t_u64, Ty::U64, {value}); };
    const auto c64 = [&](u64 value) { return m.ConstU64(t_u64, value); };
    const auto iadd64 = [&](u32 a, u32 b) { return Emit(O(spv::Op::OpIAdd), t_u64, Ty::U64, {a, b}); };
    const auto address = [&](u32 base, u32 index, u32 stride) {
        return iadd64(base, Emit(O(spv::Op::OpIMul), t_u64, Ty::U64, {wide(index), c64(stride)}));
    };
    const u32 position = Emit(O(spv::Op::OpLoad), t_v4f, Ty::V4F, {var_position});
    const u32 raw_param = PushU32(11, -1);
    const u32 capture_param = both ? IOp(O(spv::Op::OpShiftRightLogical), raw_param, CU(16u)) : raw_param;
    const u32 param_index = Select(ICmp(O(spv::Op::OpULessThan), capture_param, CU(CaptureParamsCapacity)), capture_param, CU(0));
    const auto table = [&](u32 member) {
        return Emit(O(spv::Op::OpLoad), t_u64, Ty::U64,
                    {Emit(O(spv::Op::OpAccessChain), ptr_u64, Ty::None, {var_motion_addr, CU(member)})});
    };
    const u32 params_address = table(both ? 2u : 0u);
    const u32 positions_address = table(both ? 3u : 1u);
    const u32 ptr_v4u = PtrType(storage, t_v4u);
    const u32 aligned = u32(spv::MemoryAccessMask::Aligned);
    const u32 params_ptr = Emit(O(spv::Op::OpConvertUToPtr), ptr_v4u, Ty::None, {address(params_address, param_index, 32)});
    const u32 params = Emit(O(spv::Op::OpLoad), t_v4u, Ty::V4U, {params_ptr, aligned, 16}, 6);
    const u32 store_base = Extract(t_u32, Ty::U32, params, 0);
    const u32 vertices = Extract(t_u32, Ty::U32, params, 2);
    const u32 offsets_ptr = Emit(O(spv::Op::OpConvertUToPtr), ptr_v4u, Ty::None,
                                 {iadd64(address(params_address, param_index, 32), c64(16))});
    const u32 offsets = Emit(O(spv::Op::OpLoad), t_v4u, Ty::V4U, {offsets_ptr, aligned, 16}, 6);
    const u32 first_vertex = Extract(t_u32, Ty::U32, offsets, 0);
    const u32 first_instance = Extract(t_u32, Ty::U32, offsets, 1);
    const u32 instances = Extract(t_u32, Ty::U32, offsets, 2);
    const u32 vertex = IOp(O(spv::Op::OpISub), LoadBuiltinU32(var_vertex_index, spv::BuiltIn::VertexIndex), first_vertex);
    const u32 instance = IOp(O(spv::Op::OpISub), LoadBuiltinU32(var_instance_index, spv::BuiltIn::InstanceIndex), first_instance);
    const u32 in_range = Emit(O(spv::Op::OpLogicalAnd), t_bool, Ty::Bool,
                              {ICmp(O(spv::Op::OpULessThan), vertex, vertices), ICmp(O(spv::Op::OpULessThan), instance, instances)});
    const u32 slot = IOp(O(spv::Op::OpIAdd), vertex, IOp(O(spv::Op::OpIMul), instance, vertices));
    const u32 store_label = m.NewId();
    const u32 store_merge = m.NewId();
    EmitVoid(O(spv::Op::OpSelectionMerge), {store_merge, 0}, 2);
    EmitVoid(O(spv::Op::OpBranchConditional), {in_range, store_label, store_merge});
    Open(store_label);
    const u32 store_address = address(positions_address, IOp(O(spv::Op::OpIAdd), store_base, slot), 16);
    const u32 ptr_scalar = PtrType(storage, t_u32);
    for (u32 i = 0; i < 4; ++i) {
        const u32 ptr = Emit(O(spv::Op::OpConvertUToPtr), ptr_scalar, Ty::None, {iadd64(store_address, c64(i * 4))});
        const u32 bits = Emit(O(spv::Op::OpBitcast), t_u32, Ty::U32, {Extract(t_f32, Ty::F32, position, i)});
        const u32 prev = Emit(O(spv::Op::OpAtomicExchange), t_u32, Ty::U32, {ptr, CU(u32(spv::Scope::Device)), CU(0), bits});
        nodes[m.NodeOf(prev)].effect = true;
    }
    EmitVoid(O(spv::Op::OpBranch), {store_merge});
    Open(store_merge);
}

void Lift_::FragmentMotion() {
    if (!var_motion_frag) {
        var_motion_frag = OutputVar(t_v4f, 7u, std::nullopt);
        var_motion_cur_in = InputVar(t_v4f, 30u, std::nullopt);
        var_motion_prev_in = InputVar(t_v4f, 31u, std::nullopt);
        if (!var_frag_coord) {
            var_frag_coord = InputVar(t_v4f, std::nullopt, spv::BuiltIn::FragCoord);
        }
    }
    const auto f = [&](u16 op, u32 a, u32 b) { return Emit(op, t_f32, Ty::F32, {a, b}); };
    const u32 current = Emit(O(spv::Op::OpLoad), t_v4f, Ty::V4F, {var_motion_cur_in});
    const u32 previous = Emit(O(spv::Op::OpLoad), t_v4f, Ty::V4F, {var_motion_prev_in});
    const u32 xscale = PushF32(2);
    const u32 yscale = PushF32(3);
    const auto ndc = [&](u32 clip, u32 comp) {
        return f(O(spv::Op::OpFDiv), Extract(t_f32, Ty::F32, clip, comp), Extract(t_f32, Ty::F32, clip, 3));
    };
    const u32 dx = f(O(spv::Op::OpFMul), f(O(spv::Op::OpFSub), ndc(previous, 0), ndc(current, 0)), xscale);
    const u32 dy = f(O(spv::Op::OpFMul), f(O(spv::Op::OpFSub), ndc(previous, 1), ndc(current, 1)), yscale);
    const u32 zero = CF(0.f);
    const u32 epsilon = CF(1e-5f);
    const u32 front = Emit(O(spv::Op::OpLogicalAnd), t_bool, Ty::Bool,
                           {FCmp(O(spv::Op::OpFOrdGreaterThan), Extract(t_f32, Ty::F32, previous, 3), epsilon),
                            FCmp(O(spv::Op::OpFOrdGreaterThan), Extract(t_f32, Ty::F32, current, 3), epsilon)});
    const u32 valid = Select(front, Extract(t_f32, Ty::F32, previous, 2), zero);
    const u32 depth = Extract(t_f32, Ty::F32, Emit(O(spv::Op::OpLoad), t_v4f, Ty::V4F, {var_frag_coord}), 2);
    EmitVoid(O(spv::Op::OpStore), {var_motion_frag, ComponentsF({Select(front, dx, zero), Select(front, dy, zero), valid, depth})});
}

struct ExactWalk {
    Lift_& l;
    std::vector<std::vector<u32>> users;

    explicit ExactWalk(Lift_& lift) : l{lift}, users(lift.nodes.size()) {
        std::vector<u8> live(l.nodes.size(), 0);
        l.MarkLive(live);
        for (u32 n = 0; n < l.nodes.size(); ++n) {
            if (!live[n]) {
                continue;
            }
            const Node& node = l.nodes[n];
            for (u32 a = 0; a < node.nargs; ++a) {
                if (a < 32 && ((node.lits >> a) & 1)) {
                    continue;
                }
                const u32 id = l.m.Forward(l.args[node.first + a]);
                if (id == 0 || l.m.IsConst(id)) {
                    continue;
                }
                const u32 def = l.m.NodeOf(id);
                if (def < l.nodes.size() && l.nodes[def].result == id) {
                    users[def].push_back(n);
                }
            }
        }
    }

    bool IsAbs(u32 id) const {
        id = l.m.Forward(id);
        if (l.m.IsConst(id)) {
            return false;
        }
        const Node& node = l.nodes[l.m.NodeOf(id)];
        return node.op == u16(spv::Op::OpExtInst) && l.args[node.first + 1] == GLSLstd450FAbs;
    }

    enum class Use { Exact, Follow, End };

    Use Classify(const Node& user, bool bits) const {
        const u16 op = user.op;
        if (user.pseudo != 0) {
            return Use::Exact;
        }
        if (op == u16(spv::Op::OpPhi) || op == u16(spv::Op::OpBitcast) || op == u16(spv::Op::OpSelect)) {
            return Use::Follow;
        }
        if (bits) {
            if (op == u16(spv::Op::OpGroupNonUniformQuadBroadcast) ||
                op == u16(spv::Op::OpGroupNonUniformShuffle) ||
                op == u16(spv::Op::OpGroupNonUniformShuffleXor)) {
                return Use::Follow;
            }
            return user.result == 0 ? Use::End : Use::Exact;
        }
        switch (spv::Op(op)) {
        case spv::Op::OpFOrdEqual:
        case spv::Op::OpFUnordEqual:
        case spv::Op::OpFOrdNotEqual:
        case spv::Op::OpFUnordNotEqual:
        case spv::Op::OpFOrdLessThan:
        case spv::Op::OpFUnordLessThan:
        case spv::Op::OpFOrdGreaterThan:
        case spv::Op::OpFUnordGreaterThan:
        case spv::Op::OpFOrdLessThanEqual:
        case spv::Op::OpFUnordLessThanEqual:
        case spv::Op::OpFOrdGreaterThanEqual:
        case spv::Op::OpFUnordGreaterThanEqual: {
            for (u32 a = 0; a < 2; ++a) {
                const u32 id = l.args[user.first + a];
                if (l.m.IsConst(id) && l.m.TyOf(id) == Ty::F32) {
                    const float value = std::bit_cast<float>(l.m.ConstBits(id));
                    if (value == 0.f || value != std::trunc(value)) {
                        return Use::End;
                    }
                }
            }
            if (IsAbs(l.args[user.first]) && IsAbs(l.args[user.first + 1])) {
                return Use::End;
            }
            return Use::Exact;
        }
        case spv::Op::OpImageSampleImplicitLod:
        case spv::Op::OpImageSampleExplicitLod:
        case spv::Op::OpImageSampleDrefImplicitLod:
        case spv::Op::OpImageSampleDrefExplicitLod:
        case spv::Op::OpImageGather:
        case spv::Op::OpImageDrefGather:
        case spv::Op::OpImageQueryLod:
            return Use::End;
        case spv::Op::OpExtInst: {
            const u32 fn = l.args[user.first + 1];
            if (fn == GLSLstd450PackHalf2x16) {
                return Use::End;
            }
            if (fn == GLSLstd450Floor || fn == GLSLstd450Ceil || fn == GLSLstd450Trunc ||
                fn == GLSLstd450Fract || fn == GLSLstd450RoundEven) {
                return Use::Exact;
            }
            break;
        }
        default:
            break;
        }
        if (user.result == 0) {
            return Use::End;
        }
        const Ty ty = l.m.TyOf(user.result);
        return ty == Ty::F32 || ty == Ty::V2F || ty == Ty::V3F || ty == Ty::V4F ? Use::Follow : Use::Exact;
    }

    bool NeedsExact(const std::vector<u32>& starts) const {
        std::vector<std::pair<u32, bool>> stack;
        std::vector<u8> seen(l.nodes.size(), 0);
        u32 visited = 0;
        for (const u32 start : starts) {
            stack.push_back({start, false});
            seen[start] = 1;
        }
        while (!stack.empty()) {
            const auto [node, bits] = stack.back();
            stack.pop_back();
            for (const u32 user_index : users[node]) {
                const Node& user = l.nodes[user_index];
                switch (Classify(user, bits)) {
                case Use::Exact:
                    return true;
                case Use::Follow:
                    if (!seen[user_index]) {
                        seen[user_index] = 1;
                        if (++visited > 4096) {
                            return true;
                        }
                        const bool to_bits = user.op == u16(spv::Op::OpBitcast) && l.m.TyOf(user.result) == Ty::U32;
                        stack.push_back({user_index, to_bits || ((user.op == u16(spv::Op::OpPhi) || user.op == u16(spv::Op::OpSelect)) && bits)});
                    }
                    break;
                case Use::End:
                    break;
                }
            }
        }
        return false;
    }
};

void Lift_::ExpandInterpolation() {
    if (in.stage != Stage::Fragment) {
        return;
    }
    std::array<u8, 32> hardware{};
    if (in.opt.manual_interpolation && !in.opt.manual_interpolation_all) {
        ExactWalk walk{*this};
        std::array<std::vector<u32>, 32> results;
        std::array<u8, 32> keep{};
        std::array<u32, 32> kinds;
        kinds.fill(BaryNone);
        for (u32 n = 0; n < nodes.size(); ++n) {
            const Node& node = nodes[n];
            if (node.pseudo != PseudoInterpP2 && node.pseudo != PseudoInterpP1) {
                continue;
            }
            const u32 attr = args[node.first];
            const u32 kind = args[node.first + 2];
            if (node.pseudo == PseudoInterpP1) {
                if (walk.users[n].size() != 1 || nodes[walk.users[n][0]].pseudo != PseudoInterpP2) {
                    bool dead_only = walk.users[n].empty();
                    if (!dead_only) {
                        keep[attr] = 1;
                    }
                }
                continue;
            }
            const u32 previous = m.Forward(args[node.first + 4]);
            const Node* p1 = m.IsConst(previous) ? nullptr : &nodes[m.NodeOf(previous)];
            if (!p1 || p1->pseudo != PseudoInterpP1 || args[p1->first] != attr ||
                args[p1->first + 1] != args[node.first + 1] || args[p1->first + 2] != kind) {
                keep[attr] = 1;
            }
            if (kind == PerspPull || kind == BaryNone) {
                keep[attr] = 1;
            }
            if (kinds[attr] != BaryNone && kinds[attr] != kind) {
                keep[attr] = 1;
            }
            kinds[attr] = kind;
            results[attr].push_back(n);
        }
        const u32 count = u32(std::min<size_t>(in.ps_inputs.size(), 32));
        for (u32 a = 0; a < count; ++a) {
            for (u32 b = 0; b < count; ++b) {
                if (a != b && !(in.ps_inputs[b].is_default && !in.ps_inputs[b].is_flat) &&
                    in.ps_inputs[a].param == in.ps_inputs[b].param) {
                    keep[a] = 1;
                }
            }
        }
        for (u32 a = 0; a < 32; ++a) {
            if (keep[a] || interp_other_read[a] || results[a].empty() || walk.NeedsExact(results[a])) {
                continue;
            }
            hardware[a] = 1;
            u8 primary{};
            u8 aux{};
            Qualify(kinds[a], primary, aux);
            interp_decl[a] = primary;
            interp_aux[a] = aux;
        }
    }
    for (u32 n = 0; n < nodes.size(); ++n) {
        Node& node = nodes[n];
        if (node.pseudo == PseudoInterpP2 && (hardware[args[node.first]] || !in.opt.manual_interpolation)) {
            args[node.first + 3] = CF(0.f);
            args[node.first + 4] = CF(0.f);
        }
        if (node.pseudo == PseudoInterpP1 && hardware[args[node.first]]) {
            args[node.first + 3] = CF(0.f);
        }
    }
    std::vector<u8> live(nodes.size(), 0);
    MarkLive(live);
    for (u32 b = 0; b < out_blocks.size(); ++b) {
        std::vector<u32> old = std::move(out_blocks[b].nodes);
        out_blocks[b].nodes.clear();
        cur = b;
        for (const u32 index : old) {
            const Node node = index < live.size() && live[index] ? nodes[index] : Node{};
            if (index >= live.size() || !live[index]) {
                continue;
            }
            if (node.pseudo == 0) {
                if (in.opt.integer_interpolation_fix) {
                    const bool convert = node.op == O(spv::Op::OpConvertFToU) || node.op == O(spv::Op::OpConvertFToS);
                    const bool ext = node.op == O(spv::Op::OpExtInst) && (args[node.first + 1] == GLSLstd450Trunc || args[node.first + 1] == GLSLstd450Floor);
                    if (convert || ext) {
                        const u32 slot = node.first + (ext ? 2 : 0);
                        const u32 value = args[slot];
                        if (!m.IsConst(value) && nodes[m.NodeOf(value)].pseudo == PseudoInterpP2) {
                            args[slot] = Emit(O(spv::Op::OpFMul), t_f32, Ty::F32, {value, CF(1.f + 1.f / 65536.f)});
                        }
                    }
                }
                out_blocks[b].nodes.push_back(index);
                continue;
            }
            u32 value = 0;
            if (node.pseudo == 4) {
                const u32 kind = args[node.first];
                const u32 comp = args[node.first + 1];
                const bool amd = in.opt.amd_explicit_vertex;
                if (amd) {
                    static constexpr spv::BuiltIn builtin[7] = {
                        spv::BuiltIn::BaryCoordSmoothSampleAMD, spv::BuiltIn::BaryCoordSmoothAMD,
                        spv::BuiltIn::BaryCoordSmoothCentroidAMD, spv::BuiltIn::BaryCoordPullModelAMD,
                        spv::BuiltIn::BaryCoordNoPerspSampleAMD, spv::BuiltIn::BaryCoordNoPerspAMD,
                        spv::BuiltIn::BaryCoordNoPerspCentroidAMD};
                    if (!var_bary_amd_all[kind]) {
                        var_bary_amd_all[kind] = InputVar(kind == PerspPull ? t_v3f : t_v2f, std::nullopt, builtin[kind]);
                        m.Extension("SPV_AMD_shader_explicit_vertex_parameter");
                    }
                    const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var_bary_amd_all[kind], CU(comp)});
                    value = Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
                } else if (kind <= PerspPull) {
                    if (!var_bary) {
                        var_bary = InputVar(t_v3f, std::nullopt, spv::BuiltIn::BaryCoordKHR);
                    }
                    switch (kind) {
                    case PerspCenter: {
                        const u32 all = Emit(O(spv::Op::OpLoad), t_v3f, Ty::V3F, {var_bary});
                        value = Extract(t_f32, Ty::F32, all, comp + 1);
                        break;
                    }
                    case PerspCentroid: {
                        const u32 all = Ext(GLSLstd450InterpolateAtCentroid, t_v3f, Ty::V3F, {var_bary});
                        m.Capability(u32(spv::Capability::InterpolationFunction));
                        value = Extract(t_f32, Ty::F32, all, comp + 1);
                        break;
                    }
                    case PerspSample: {
                        const u32 sample = LoadBuiltinU32(var_sample_id, spv::BuiltIn::SampleId);
                        uses_sample_shading = true;
                        const u32 all = Ext(GLSLstd450InterpolateAtSample, t_v3f, Ty::V3F, {var_bary, sample});
                        m.Capability(u32(spv::Capability::InterpolationFunction));
                        value = Extract(t_f32, Ty::F32, all, comp + 1);
                        break;
                    }
                    default: {
                        if (!var_frag_coord) {
                            var_frag_coord = InputVar(t_v4f, std::nullopt, spv::BuiltIn::FragCoord);
                        }
                        const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var_frag_coord, CU(3)});
                        const u32 inv_w = Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
                        if (comp == 2) {
                            value = inv_w;
                        } else {
                            const u32 all = Emit(O(spv::Op::OpLoad), t_v3f, Ty::V3F, {var_bary});
                            value = Emit(O(spv::Op::OpFMul), t_f32, Ty::F32, {Extract(t_f32, Ty::F32, all, comp + 1), inv_w});
                        }
                        break;
                    }
                    }
                } else {
                    if (!var_bary_nopersp) {
                        var_bary_nopersp = InputVar(t_v3f, std::nullopt, spv::BuiltIn::BaryCoordNoPerspKHR);
                    }
                    const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var_bary_nopersp, CU(comp + 1)});
                    value = Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
                }
            } else if (node.pseudo == PseudoInterpP1 || node.pseudo == PseudoInterpP2) {
                const u32 attr = args[node.first];
                const u32 chan = args[node.first + 1];
                if (node.pseudo == PseudoInterpP1) {
                    const u32 i = args[node.first + 3];
                    const u32 p0 = LoadAttrComp(attr, chan, 0);
                    const u32 p1 = LoadAttrComp(attr, chan, 1);
                    value = FFma(FSub(p1, p0), i, p0);
                } else if (hardware[attr] || !in.opt.manual_interpolation) {
                    value = LoadAttrComp(attr, chan, -1);
                } else {
                    const u32 j = args[node.first + 3];
                    const u32 previous = args[node.first + 4];
                    const u32 p0 = LoadAttrComp(attr, chan, 0);
                    const u32 p2 = LoadAttrComp(attr, chan, 2);
                    value = FFma(FSub(p2, p0), j, previous);
                }
            } else if (node.pseudo == PseudoInterpMov) {
                const u32 attr = args[node.first];
                const u32 chan = args[node.first + 1];
                const u32 select = args[node.first + 2];
                if (select >= 3) {
                    Fail("v_interp_mov selector");
                }
                if (in.opt.amd_explicit_vertex || in.opt.khr_barycentric) {
                    value = LoadAttrComp(attr, chan, s32((select + 1) % 3));
                    const PsInput& input = in.ps_inputs[attr];
                    if (select != 2 && !(input.is_default && input.is_flat)) {
                        value = FSub(value, LoadAttrComp(attr, chan, 0));
                    }
                } else {
                    value = LoadAttrComp(attr, chan, -1);
                }
            }
            m.SetForward(node.result, value);
        }
    }
}

void Lift_::DeclareStageModes() {
    m.Capability(u32(spv::Capability::Shader));
    if (uses_quad) {
        m.Capability(u32(spv::Capability::GroupNonUniformQuad));
    }
    if (uses_shuffle) {
        m.Capability(u32(spv::Capability::GroupNonUniformShuffle));
    }
    if (uses_lane || uses_quad || uses_shuffle || uses_broadcast_first) {
        m.Capability(u32(spv::Capability::GroupNonUniform));
    }
    if (uses_broadcast_first) {
        m.Capability(u32(spv::Capability::GroupNonUniformBallot));
    }
    if (uses_image_query) {
        m.Capability(u32(spv::Capability::ImageQuery));
    }
    if (uses_gather) {
        m.Capability(u32(spv::Capability::ImageGatherExtended));
    }
    if (uses_storage_image) {
        m.Capability(u32(spv::Capability::StorageImageExtendedFormats));
        m.Capability(u32(spv::Capability::StorageImageReadWithoutFormat));
        m.Capability(u32(spv::Capability::StorageImageWriteWithoutFormat));
        if (in.opt.image_lod_rw) {
            m.Extension("SPV_AMD_shader_image_load_store_lod");
            m.Capability(u32(spv::Capability::ImageReadWriteLodAMD));
        }
    }
    if (in.stage != Stage::Fragment && in.stage != Stage::Compute && (var_base_vertex || var_base_instance)) {
        m.Extension("SPV_KHR_shader_draw_parameters");
        m.Capability(u32(spv::Capability::DrawParameters));
    }
    m.Extension("SPV_KHR_float_controls");
    if (in.opt.denorm_flush_32) {
        m.Capability(u32(spv::Capability::DenormFlushToZero));
        m.ExecutionMode(u32(spv::ExecutionMode::DenormFlushToZero), {32});
    }
    if (in.opt.rtz_32) {
        m.Capability(u32(spv::Capability::RoundingModeRTZ));
        m.ExecutionMode(u32(spv::ExecutionMode::RoundingModeRTZ), {32});
    }
    if (in.opt.szinp_ok) {
        m.Capability(u32(spv::Capability::SignedZeroInfNanPreserve));
        m.ExecutionMode(u32(spv::ExecutionMode::SignedZeroInfNanPreserve), {32});
    }
    if (var_primitive && in.stage == Stage::Geometry) {
        m.Capability(u32(spv::Capability::Geometry));
    }
    switch (in.stage) {
    case Stage::Hull:
        m.Capability(u32(spv::Capability::Tessellation));
        m.ExecutionMode(u32(spv::ExecutionMode::OutputVertices), {in.tess.output_vertices});
        break;
    case Stage::Domain: {
        m.Capability(u32(spv::Capability::Tessellation));
        m.ExecutionMode(in.tess.domain == 0   ? u32(spv::ExecutionMode::Isolines)
                        : in.tess.domain == 1 ? u32(spv::ExecutionMode::Triangles)
                                              : u32(spv::ExecutionMode::Quads));
        m.ExecutionMode(in.tess.partitioning == 2   ? u32(spv::ExecutionMode::SpacingFractionalOdd)
                        : in.tess.partitioning == 3 ? u32(spv::ExecutionMode::SpacingFractionalEven)
                                                    : u32(spv::ExecutionMode::SpacingEqual));
        m.ExecutionMode(in.tess.topology == 3 ? u32(spv::ExecutionMode::VertexOrderCcw) : u32(spv::ExecutionMode::VertexOrderCw));
        break;
    }
    case Stage::Geometry: {
        m.Capability(u32(spv::Capability::Geometry));
        const u32 n = in.gs.in_vertices;
        m.ExecutionMode(n == 1   ? u32(spv::ExecutionMode::InputPoints)
                        : n == 2 ? u32(spv::ExecutionMode::InputLines)
                        : n == 3 ? u32(spv::ExecutionMode::Triangles)
                        : n == 4 ? u32(spv::ExecutionMode::InputLinesAdjacency)
                                 : u32(spv::ExecutionMode::InputTrianglesAdjacency));
        m.ExecutionMode(in.gs.out_primitive == 0   ? u32(spv::ExecutionMode::OutputPoints)
                        : in.gs.out_primitive == 1 ? u32(spv::ExecutionMode::OutputLineStrip)
                                                   : u32(spv::ExecutionMode::OutputTriangleStrip));
        m.ExecutionMode(u32(spv::ExecutionMode::OutputVertices), {in.gs.output_vertices});
        m.ExecutionMode(u32(spv::ExecutionMode::Invocations), {in.gs.invocations});
        break;
    }
    case Stage::Compute:
        m.ExecutionMode(u32(spv::ExecutionMode::LocalSize), {in.workgroup_size[0], in.workgroup_size[1], in.workgroup_size[2]});
        break;
    case Stage::Fragment:
        m.ExecutionMode(in.opt.lower_left_origin ? u32(spv::ExecutionMode::OriginLowerLeft) : u32(spv::ExecutionMode::OriginUpperLeft));
        if (in.opt.khr_barycentric && !in.opt.amd_explicit_vertex) {
            m.Extension("SPV_KHR_fragment_shader_barycentric");
            m.Capability(u32(spv::Capability::FragmentBarycentricKHR));
        }
        if (uses_discard) {
            m.Capability(u32(spv::Capability::DemoteToHelperInvocation));
        }
        if (var_frag_depth) {
            m.ExecutionMode(u32(spv::ExecutionMode::DepthReplacing));
        }
        if (uses_sample_shading || in.ps_addr.persp_sample || in.ps_addr.linear_sample) {
            m.Capability(u32(spv::Capability::SampleRateShading));
        }
        if (stores_stencil) {
            m.Extension("SPV_EXT_shader_stencil_export");
            m.Capability(u32(spv::Capability::StencilExportEXT));
        }
        if (in.depth_first && !in.clip_emulation_input && !var_frag_depth && !stores_stencil && !stores_mask) {
            m.ExecutionMode(u32(spv::ExecutionMode::EarlyFragmentTests));
        }
        break;
    default:
        break;
    }
}

Output Lift_::Run() {
    try {
        const DecodeResult decoded = Decode(in.code);
        if (decoded.bad_at != 0xffffffff) {
            Fail("undecodable instruction");
        }
        insts = decoded.insts;
        out.stats.instructions = u32(insts.size());
        BuildCfg();
        Dominators();
        DefineBaseTypes();
        visited.assign(blocks.size(), 0);
        u32 position_written = 0;
        for (const Inst& inst : insts) {
            if (inst.enc == Enc::Exp && inst.target == AttrPos0) {
                position_written |= inst.en;
            }
        }
        user_clip_planes = in.user_clip_plane_mask != 0 && (in.stage == Stage::Vertex || in.stage == Stage::Domain) &&
                           position_written == 15;
        vs_clip_emulated = user_clip_planes && in.opt.clip_distance_emulation;
        for (const Inst& inst : insts) {
            if (inst.enc == Enc::Exp && inst.target > AttrPos0 && inst.target < AttrPos0 + 4 && in.opt.clip_distance_emulation) {
                for (u32 c = 0; c < 4; ++c) {
                    const u8 output = in.vs_outputs[inst.target - AttrPos0 - 1][c];
                    if (((inst.en >> c) & 1) && output >= 15 && output <= 22) {
                        vs_clip_emulated = true;
                    }
                }
            }
            if (inst.enc == Enc::Exp && inst.target == 8 && (inst.en & in.mrtz_mask & 1) && in.stage == Stage::Fragment) {
                stores_depth = true;
            }
        }
        tess_on = in.stage == Stage::Hull || in.stage == Stage::Domain;
        Open(m.NewId());
        Prologue();
        Walk(0, nullptr);
        ExpandInterpolation();
        {
            std::vector<u8> live(nodes.size(), 0);
            MarkLive(live);
            for (const u32 id : undefined_reads) {
                const u32 n = m.NodeOf(id);
                if (n < live.size() && live[n]) {
                    Fail("reads a value the resource plan does not hold");
                }
            }
        }
        out.uses_discard = uses_discard;
        DeclareStageModes();
        const std::vector<u32> function = Serialize();
        u32 model = u32(spv::ExecutionModel::Vertex);
        if (in.stage == Stage::Fragment) {
            model = u32(spv::ExecutionModel::Fragment);
        } else if (in.stage == Stage::Compute) {
            model = u32(spv::ExecutionModel::GLCompute);
        } else if (in.stage == Stage::Hull) {
            model = u32(spv::ExecutionModel::TessellationControl);
        } else if (in.stage == Stage::Domain) {
            model = u32(spv::ExecutionModel::TessellationEvaluation);
        } else if (in.stage == Stage::Geometry) {
            model = u32(spv::ExecutionModel::Geometry);
        }
        out.tone_composite = ToneComposite();
        out.uses_tone_curve = in.tone_apply && !tone_above.empty();
        out.spirv = m.Assemble(model, main_id,
                               physical_addressing ? u32(spv::AddressingModel::PhysicalStorageBuffer64)
                                                   : u32(spv::AddressingModel::Logical),
                               0x00010600, function);
    } catch (const Decline& decline) {
        out.decline = decline.why;
        out.spirv.clear();
    }
    return out;
}

static Output LiftOnce(const Input& input, Input* found) {
    const Input saved = found ? *found : Input{};
    bool pessimistic = false;
    std::vector<u32> no_loop_origin;
    for (u32 attempt = 0; attempt < 16; ++attempt) {
        Lift_ lift{input, found};
        if (pessimistic) {
            lift.SetPessimistic();
        }
        lift.SetNoLoopOrigin(no_loop_origin);
        try {
            return lift.Run();
        } catch (const RestartPessimistic&) {
            if (pessimistic) {
                break;
            }
            pessimistic = true;
        } catch (const RestartOrigins& restart) {
            no_loop_origin.insert(no_loop_origin.end(), restart.regs.begin(), restart.regs.end());
        }
        if (found) {
            *found = saved;
        }
    }
    Output failed;
    failed.decline = "exec in a loop";
    return failed;
}

static Output LiftPlain(const Input& input);

Output Lift(const Input& input) {
    Output out = LiftPlain(input);
    if (out.ok() && input.tone_curve && !input.tone_apply && out.tone_composite) {
        Input apply = input;
        apply.tone_apply = true;
        return LiftPlain(apply);
    }
    return out;
}

static Output LiftPlain(const Input& input) {
    if (!input.discover) {
        return LiftOnce(input, nullptr);
    }
    Input first = input;
    BufferSlot flat;
    flat.kind = BufferKind::Flatbuf;
    flat.uniform = true;
    flat.not_only_constants = false;
    first.buffers.push_back(flat);
    first.flatbuf_slot = 0;
    Output scan = LiftOnce(first, &first);
    if (!scan.decline.empty()) {
        return scan;
    }
    Input second = first;
    u32 binding = 0;
    for (BufferSlot& slot : second.buffers) {
        slot.binding = binding;
        slot.offset_slot = binding++;
        if (slot.kind == BufferKind::Guest) {
            slot.uniform = !slot.not_only_constants && !slot.written;
        }
    }
    for (ImageSlot& slot : second.images) {
        slot.binding = binding++;
    }
    for (SamplerSlot& slot : second.samplers) {
        slot.binding = binding++;
    }
    Output out = LiftOnce(second, nullptr);
    out.buffers = second.buffers;
    out.images = second.images;
    out.samplers = second.samplers;
    out.flat_table = second.flat_table;
    return out;
}

} // namespace Lifter
