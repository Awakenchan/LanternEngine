// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader {

struct Profile {
    u32 max_viewport_width{};
    u32 max_viewport_height{};
    u32 max_shared_memory_size{};
    u32 supported_spirv{0x00010000};
    u32 subgroup_size{};
    u32 sparse_page_shift{};
    bool support_int8{};
    bool support_int16{};
    bool support_int64{};
    bool support_float16{};
    bool support_float64{};
    bool supports_denorm_behavior_independence{};
    bool supports_rounding_mode_independence{};
    bool support_fp16_denorm_preserve{};
    bool support_fp16_denorm_flush{};
    bool support_fp16_round_to_zero{};
    bool support_fp32_denorm_preserve{};
    bool support_fp32_denorm_flush{};
    bool support_fp32_round_to_zero{};
    bool support_fp64_denorm_preserve{};
    bool support_fp64_denorm_flush{};
    bool support_fp64_round_to_zero{};
    bool support_fp16_signed_zero_inf_nan_preserve{};
    bool support_fp32_signed_zero_inf_nan_preserve{};
    bool support_fp64_signed_zero_inf_nan_preserve{};
    bool supports_image_load_store_lod{};
    bool supports_native_cube_calc{};
    bool supports_trinary_minmax{};
    bool supports_buffer_fp32_atomic_min_max{};
    bool supports_image_fp32_atomic_min_max{};
    bool supports_buffer_int64_atomics{};
    bool supports_shared_int64_atomics{};
    bool supports_workgroup_explicit_memory_layout{};
    bool supports_amd_shader_explicit_vertex_parameter{};
    bool supports_fragment_shader_barycentric{};
    bool supports_shader_subgroup_clock{};
    bool has_broken_spirv_clamp{};
    bool lower_left_origin_mode{};
    bool needs_manual_interpolation{};
    /// bbport: NVIDIA without fragment shader barycentrics (Pascal and older).
    bool needs_integer_interpolation_fix{};
    bool needs_lds_barriers{};
    bool needs_buffer_offsets{};
    bool needs_unorm_fixup{};
    bool needs_clip_distance_emulation{};
    bool supports_shader_stencil_export{};
    /// Paleblood (Jgamrot/yharnam-pc #204, shadPS4 #5257/#5346): GCN NaN/Inf rules for min/max/med3, clamp,
    /// legacy multiply-adds, clamped rcp/rsq and negated compares.
    bool gcn_nan_semantics{};
    /// bbport (native shaders; BB_NATIVE_SHADERS=0 turns them off): guest constant buffers the
    /// shader only reads with scalar constant loads (s_buffer_load) are uniform buffers instead of
    /// storage buffers: NVIDIA reads them through its constant cache instead of the memory path.
    bool uniform_constant_buffers{};
    /// bbport: those uniform buffers are arrays of dwords (std430 rules, which
    /// uniformBufferStandardLayout allows for uniform blocks) instead of uvec4s, so a constant is
    /// one load at its own address rather than a vector load and a dynamic component pick.
    /// BB_UBO_DWORDS=0 keeps the uvec4s.
    bool uniform_dword_layout{};
    /// bbport: and they are bound at the exact address of the guest's V# (the rasterizer copies
    /// those it cannot bind there), so their reads add no offset from the push data and a
    /// constant index stays a constant: NVIDIA then reads it as an operand of the instruction
    /// that uses it. Off unless BB_UBO_EXACT=1: the copies of GPU-written constants end render
    /// passes, which cost more than the offsets at the test spot.
    bool uniform_exact_base{};
    /// bbport: a pixel shader drawn with DB_SHADER_CONTROL.DEPTH_BEFORE_SHADER runs after the
    /// depth and stencil tests, as on the console (EarlyFragmentTests). Off unless BB_EARLY_Z=1:
    /// at the test spot it cost frames (57.8 against 60.2 FPS) and the driver orders them itself.
    bool early_fragment_tests{};
    /// bbport: the logical stages (a bit per SwStage) whose constant buffers stay storage buffers
    /// while the rest are uniform buffers (BB_UBO_STAGES lists the stages that take them). Zero,
    /// every stage, unless set: a way to measure where uniform buffers cost GPU time.
    u8 uniform_stage_skip{};
    /// bbport: the flattened user data stays a storage buffer in native shaders
    /// (BB_UBO_FLATBUF=0); it is a uniform buffer by default.
    bool flatbuf_storage{};
    /// bbport: every float add, multiply and fma of a pixel shader keeps NoContraction, as GCN's
    /// unfused v_mad_f32 computes it (BB_FS_CONTRACT=0). By default a pixel shader's are left to
    /// the driver to fuse (NVIDIA: FFMA instead of FMUL and FADD, 10% less code over the game's
    /// pixel shaders, docs/native_shader_compiler.md); results differ in the last bit at most, and
    /// a shader that writes depth keeps them. False is the default so that caches made before
    /// this field (its byte was padding, zero) keep their profile.
    bool fragment_exact_fp{};
    /// bbport: with needs_manual_interpolation, every pixel shader input is interpolated by
    /// GCN's formula (BB_INTERP_HW=0). By default an input goes back to the hardware's
    /// interpolation when no use of it needs the exact bits (ir/passes/
    /// hardware_interpolation_pass.cpp). False is the default for the reason above.
    bool manual_interpolation_all{};

    bool operator==(const Profile&) const = default;
};

} // namespace Shader
