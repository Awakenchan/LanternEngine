// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native+ (the 16-bit chain): replaces the game's display copy
// (38d65b32), which copies its finished image into the display buffer through its brightness
// curve (a 1D table per channel). The table covers 0..1; above the game's white the value goes on
// as the table's value at white times the input (still in the chain's gamma encoding: the final
// pass decodes it, hdr_output.frag native16_nits). The names are the recompiled shader's:
// vk_hdr_channel.cpp copies its bindings and locations onto these.

#version 450
#extension GL_EXT_samplerless_texture_functions : require

// The recompiler's push constants (shader_recompiler/resource.h, PushData) at the C++ struct's
// offsets (checked in vk_rasterizer.cpp).
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
} push_data;

layout(set = 0, binding = 0) uniform texture2D fs_img0;
// The brightness table (one row per channel): a 1D array image. Lantern's recompiler keeps 1D
// image types (shadPS4 0.20.1, bbport's, binds it as a 2D array view instead): a 2D array
// declaration read 0 here and the whole frame was black.
layout(set = 0, binding = 1) uniform texture1DArray fs_img1;
layout(set = 0, binding = 2) uniform sampler fs_samp0;

layout(location = 0) out vec4 frag_color0;

float table(float v, float row) {
    if (v <= 1.0) {
        return texture(sampler1DArray(fs_img1, fs_samp0), vec2(v, row)).x;
    }
    return texture(sampler1DArray(fs_img1, fs_samp0), vec2(1.0, row)).x * v;
}

// Below 0 (PsychoV-30 with a BT.2020 target: a channel of a color outside BT.709), the curve
// continued point-symmetric about its value at 0: the table alone read its value at 0 there and
// the channel lost its sign.
float brightness(float v, float row) {
    return v >= 0.0 ? table(v, row) : 2.0 * table(0.0, row) - table(-v, row);
}

void main() {
    uint w = push_data.scene_size & 65535u;
    uint h = push_data.scene_size >> 16u;
    vec2 scale = vec2(1920.0 / float(w == 0u ? 1920u : w), 1080.0 / float(h == 0u ? 1080u : h));
    vec4 v = texelFetch(fs_img0, ivec2(floor(gl_FragCoord.xy * scale)), 0);
    frag_color0 = vec4(brightness(v.z, 2.0), brightness(v.y, 1.0), brightness(v.x, 0.0), v.w);
}
