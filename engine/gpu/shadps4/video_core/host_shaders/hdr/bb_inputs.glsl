// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport Native+: how a replacement reads the game shader's inputs. Each replacement is built twice
// (gpu/CMakeLists.txt); vk_hdr_channel.cpp picks one per game shader:
//   - per vertex (the default): every input of the game's pixel shader is read per vertex and
//     interpolated by GCN's own formula (the recompiler's manual interpolation on NVIDIA), so the
//     replacement reads them so too and computes the game's values bit for bit;
//   - BB_HW_INTERP: the GPU interpolates them (Lantern 0.9.37's hardware interpolation pass gave
//     them back, or a GPU without fragment-shader barycentrics: AMD, Intel). The game's vertex
//     shader outputs the same values at the same locations either way.
// The includer puts, right after its #version line:
//     #ifndef BB_HW_INTERP
//     #extension GL_EXT_fragment_shader_barycentric : require
//     #endif

#ifndef BBPORT_BB_INPUTS_GLSL
#define BBPORT_BB_INPUTS_GLSL

#ifdef BB_HW_INTERP
// An input as the GPU interpolates it: BB_UV(name) is its value's first two components.
#define BB_INPUT(L, NAME) layout(location = L) in vec4 NAME
#define BB_UV(NAME) (NAME).xy
#else
// An input read per vertex: BB_UV(name) interpolates it as GCN does (v_interp_p1/p2:
// P0 + I (P1 - P0) + J (P2 - P0)), with the hardware's barycentrics.
#define BB_INPUT(L, NAME) layout(location = L) pervertexEXT in vec4 NAME[3]
#define BB_UV(NAME) bb_interp_uv(NAME[0], NAME[1], NAME[2])
vec2 bb_interp_uv(vec4 a0, vec4 a1, vec4 a2) {
    precise float dx1 = a1.x - a0.x;
    precise float dx2 = a2.x - a0.x;
    precise float dy1 = a1.y - a0.y;
    precise float dy2 = a2.y - a0.y;
    return vec2(fma(dx2, gl_BaryCoordEXT.z, fma(dx1, gl_BaryCoordEXT.y, a0.x)),
                fma(dy2, gl_BaryCoordEXT.z, fma(dy1, gl_BaryCoordEXT.y, a0.y)));
}
#endif

#endif // BBPORT_BB_INPUTS_GLSL
