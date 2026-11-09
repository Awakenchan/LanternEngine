// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "lifter/gcn.h"

namespace Lifter {

enum class Stage : u8 { Fragment, Vertex, Compute, Local, Export, Hull, Domain, Geometry };
enum class NumClass : u8 { Float, Sint, Uint };
enum class Conv : u8 { None, UintToUscaled, SintToSscaled, UnormToUbnorm, Sint8ToSnormNz,
                       Sint16ToSnormNz, Uint32ToUnorm, SrgbToNorm, Other };
enum class View : u8 { D1, D1Array, D2, D2Array, D2Msaa, D3, Other };
enum class SwizzleSel : u8 { Zero = 0, One = 1, R = 4, G = 5, B = 6, A = 7 };

constexpr u16 NoFlat = 0xffff;
constexpr u32 MotionParamsCapacity = 1 + 4 * 8192;
constexpr u32 CaptureParamsCapacity = 1 + 4 * 8192;

struct Sharp {
    std::array<u16, 8> flat{NoFlat, NoFlat, NoFlat, NoFlat, NoFlat, NoFlat, NoFlat, NoFlat};
    std::array<u32, 8> imm{};
};

struct FlatEntry {
    u16 parent;
    u32 offset;
    u16 flat;
};

enum class BufferKind : u8 { Guest, Flatbuf, Gds, Other };

struct BufferSlot {
    Sharp sharp;
    BufferKind kind{};
    u32 binding{};
    u32 offset_slot{};
    bool uniform{};
    bool uniform_exact{};
    bool written{};
    bool coherent{};
    bool formatted{};
    bool swizzle_enable{};
    bool add_tid{};
    u32 stride{};
    u32 index_stride{};
    u32 element_size{};
    u8 data_format{};
    u8 num_format{};
    Conv conv{};
    std::array<u8, 4> swizzle{4, 5, 6, 7};
    bool used_u8{};
    bool used_u16{};
    bool used_u32{};
    bool used_f32{};
    bool not_only_constants{};
};

struct ImageSlot {
    Sharp sharp;
    u32 binding{};
    View view{};
    NumClass cls{};
    bool storage{};
    bool array{};
    bool cube{};
    bool srgb{};
    bool atomic{};
    u32 atomic_format{};
    Conv conv{};
    std::array<u8, 4> swizzle{4, 5, 6, 7};
    u32 width{};
    u32 height{};
};

struct SamplerSlot {
    Sharp sharp;
    u32 binding{};
    bool depth{};
    bool force_unnormalized{};
    bool force_degamma{};
};

struct VsAttribute {
    u8 semantic{};
    u8 vgpr{};
    NumClass cls{};
    Conv conv{};
    std::array<u8, 4> swizzle{4, 5, 6, 7};
};

struct PsInput {
    u8 param{};
    bool is_default{};
    bool is_flat{};
    u8 default_value{};
};

struct ColorOut {
    u8 export_format{};
    NumClass cls{};
    Conv conv{};
    std::array<u8, 4> swizzle{4, 5, 6, 7};
    bool self_scale{};
    bool unorm_fixup{};
};

struct PsVgprs {
    bool persp_sample{}, persp_center{}, persp_centroid{}, persp_pull{};
    bool linear_sample{}, linear_center{}, linear_centroid{}, line_stipple{};
    bool pos_x{}, pos_y{}, pos_z{}, pos_w{}, front_face{}, ancillary{}, sample_coverage{};
};

struct Options {
    bool amd_explicit_vertex{};
    bool khr_barycentric{};
    bool manual_interpolation{};
    bool manual_interpolation_all{};
    bool integer_interpolation_fix{};
    bool fragment_exact_fp{};
    bool trinary_minmax{};
    bool native_cube{};
    bool buffer_offsets{};
    bool uniform_dword_layout{};
    bool lower_left_origin{};
    bool stencil_export{};
    bool clip_distance_emulation{};
    bool denorm_flush_32{};
    bool denorm_flush_ok{};
    bool rtz_32{};
    bool rtz_ok{};
    bool szinp_ok{};
    bool image_lod_rw{};
    u32 max_viewport_width{};
    u32 max_viewport_height{};
};

struct TessInfo {
    Sharp sharp;
    u32 ls_stride{};
    u32 hs_cp_stride{};
    u32 input_vertices{};
    u32 output_vertices{};
    u32 domain{};
    u32 partitioning{};
    u32 topology{};
    bool passthrough{};
    bool offchip{};
};

struct GsAttr {
    u32 offset{};
    u32 attr{};
    u32 comp{};
};

struct GsInfo {
    u32 in_vertices{};
    u32 output_vertices{};
    u32 invocations{};
    u32 out_primitive{};
    u32 dwords_per_vertex{};
    u32 copy_output_vertices{};
    std::vector<GsAttr> attr_map;
};

struct Input {
    Stage stage{};
    std::span<const u32> code;
    u64 hash{};
    Options opt;
    bool discover{};

    std::array<s32, 16> ud_push{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::vector<FlatEntry> flat_table;
    std::vector<BufferSlot> buffers;
    std::vector<ImageSlot> images;
    std::vector<SamplerSlot> samplers;
    s32 flatbuf_slot = -1;
    u32 num_user_sgprs{};

    std::vector<VsAttribute> attributes;
    u32 num_input_vgprs{};
    s32 base_vertex_sgpr = -1;
    s32 base_instance_sgpr = -1;
    bool fetch_applies_base_vertex{};
    bool fetch_applies_base_instance{};
    u32 step_rate_0{};
    u32 step_rate_1{};
    std::array<std::array<u8, 4>, 3> vs_outputs{};
    bool emulate_depth{};
    bool clip_disable{};
    u32 user_clip_plane_mask{};
    bool tone_curve{};
    bool tone_apply{};
    s32 clip_plane_slot = -1;
    bool vertex_motion{};
    bool vertex_capture{};

    PsVgprs ps_addr;
    PsVgprs ps_en;
    std::vector<PsInput> ps_inputs;
    std::array<ColorOut, 8> color{};
    u8 z_export_format{};
    u8 num_samples = 1;
    u8 mrtz_mask{};
    bool front_face_all_bits{};
    bool dual_source{};
    bool clip_emulation_input{};
    bool fragment_motion{};
    bool depth_first{};

    std::array<u32, 3> workgroup_size{};
    std::array<bool, 3> tgid_enable{};
    u32 shared_memory_size{};

    TessInfo tess;
    GsInfo gs;
};

enum class ResourceKind : u8 { Buffer, Image, Sampler };

struct UsedResource {
    ResourceKind kind{};
    u32 slot{};
    s32 ud_reg{};
    s32 table_dword{};
};

struct Stats {
    u32 instructions{};
    u32 blocks{};
    u32 loops{};
    u32 selections{};
    u32 phis{};
    u32 phis_folded{};
    u32 nodes{};
    u32 nodes_live{};
    u32 selects_for_exec{};
    u32 ifs_for_exec{};
};

struct Output {
    bool tone_composite{};
    bool uses_tone_curve{};
    std::vector<u32> spirv;
    std::string decline;
    u32 decline_pc{};
    u32 last_pc{};
    std::vector<UsedResource> resources;
    std::vector<BufferSlot> buffers;
    std::vector<ImageSlot> images;
    std::vector<SamplerSlot> samplers;
    std::vector<FlatEntry> flat_table;
    Stats stats;
    bool uses_discard{};
    bool ok() const {
        return decline.empty() && !spirv.empty();
    }
};

Output Lift(const Input& input);

} // namespace Lifter
