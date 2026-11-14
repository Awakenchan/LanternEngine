// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native+ (the 16-bit chain): replaces the game's ColorGrading pass (ed4c7668), a 16x16x16
// color table applied to the image. The table only covers BT.709 0..1: up to white it is applied
// exactly as the game does; above white the color is divided by its largest channel, graded and
// scaled back; a color outside BT.709 goes through RenoDX's SDR LUT bridge
// (.agents/skills/handle-sdr-tonemap-lut/snippets/sdr_lut_bridge.hlsl): brought inside it
// (AdaptiveD65 gamut compression, gamut_bridge.glsl), its largest channel into the table's range,
// graded, then both taken back out, so the HDR part keeps its hue and saturation and is graded
// too. The strength of the grading (the in-game menu's Color Grade Strength, 0 none .. 1 the
// game's) comes in bb_hdr.w, set for this draw only. The names are the recompiled shader's:
// vk_hdr_channel.cpp copies its bindings and locations onto these.

#version 450
#ifndef BB_HW_INTERP
#extension GL_EXT_fragment_shader_barycentric : require
#endif
#extension GL_GOOGLE_include_directive : require

#include "bb_inputs.glsl"
#include "gamut_bridge.glsl"

// The recompiler's push constants (shader_recompiler/resource.h, PushData) at the C++ struct's
// offsets (checked in vk_rasterizer.cpp); bb_hdr is bbport's, set per draw (vk_rasterizer.cpp).
layout(push_constant, std430) uniform AuxData {
    layout(offset = 0) float xoffset;
    layout(offset = 4) float yoffset;
    layout(offset = 8) float xscale;
    layout(offset = 12) float yscale;
    layout(offset = 80) uvec4 buf_offsets0;
    layout(offset = 96) uvec4 buf_offsets1;
    layout(offset = 112) uvec2 buf_offsets2;
    layout(offset = 120) uint motion_param;
    layout(offset = 124) uint scene_size;
    layout(offset = 128) uvec4 bb_hdr;
} push_data;

layout(set = 0, binding = 0) uniform texture2D fs_img0;
layout(set = 0, binding = 1) uniform texture3D fs_img1;
layout(set = 0, binding = 2) uniform sampler fs_samp0;
layout(set = 0, binding = 3) uniform sampler fs_samp1;

BB_INPUT(0, fs_in_attr0);
layout(location = 0) out vec4 frag_color0;

// The chain's encoding (gamma 2.2, the final pass decodes it so), with the sign kept below 0.
vec3 signed_pow(vec3 v, float e) {
    return sign(v) * pow(abs(v), vec3(e));
}

// The game's table at an encoded color in 0..1, blended with it by the strength.
vec3 grade(vec3 sdr, float strength) {
    vec3 graded = max(texture(sampler3D(fs_img1, fs_samp1), sdr * 0.9375 + 0.03125).xyz, 0.0);
    return mix(sdr, graded, strength);
}

void main() {
    vec4 c = textureLod(sampler2D(fs_img0, fs_samp0), BB_UV(fs_in_attr0), 0.0);
    // bb_hdr.w: the strength's bits plus one (0: not set by bbport, the game's grading).
    uint bits = push_data.bb_hdr.w;
    float strength = bits == 0u ? 1.0 : clamp(uintBitsToFloat(bits - 1u), 0.0, 1.0);
    float top = max(max(c.x, c.y), c.z);
    vec3 o;
    if (min(min(c.x, c.y), c.z) >= 0.0) {
        // Inside BT.709: up to white the table as the game applies it; above, the color divided
        // by its largest channel (hue and saturation kept), graded, and scaled back up.
        float scale = max(top, 1.0);
        o = grade(c.xyz / scale, strength) * scale;
    } else {
        // The bridge, in linear light: into BT.709, into the table's range, graded, back out.
        vec3 linear = signed_pow(c.xyz, 2.2);
        float gamut_scale = bb_gamut_compression_scale(linear);
        vec3 inside = gamut_scale < 1.0 ? bb_gamut_compress(linear, gamut_scale) : linear;
        inside = max(inside, 0.0);
        float range_scale = 1.0 / max(max(max(inside.x, inside.y), inside.z), 1.0);
        vec3 graded = signed_pow(grade(signed_pow(inside * range_scale, 1.0 / 2.2), strength), 2.2) /
                      range_scale;
        if (gamut_scale < 1.0) {
            graded = bb_gamut_decompress(graded, gamut_scale);
        }
        o = signed_pow(graded, 1.0 / 2.2);
    }
    // The game's channel order: its target holds them swapped.
    frag_color0 = vec4(o.zyx, max(c.w, 0.0));
}
