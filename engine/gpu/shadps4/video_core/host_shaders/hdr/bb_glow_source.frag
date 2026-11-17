// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native+ (the 16-bit chain): replaces the game's EmissiveToGlowPS
// (57f3b8c2), a copy of the finished image into the glow's source. The copy is clamped to the
// game's white, so the glow (added back over the image after the tone mapping) stays as the
// game draws it while the image itself carries HDR light. The names are the recompiled
// shader's: vk_hdr_channel.cpp copies its bindings and locations onto these.

#version 450
#ifndef BB_HW_INTERP
#extension GL_EXT_fragment_shader_barycentric : require
#endif
#extension GL_GOOGLE_include_directive : require

#include "bb_inputs.glsl"

layout(set = 0, binding = 0) uniform texture2D fs_img0;
layout(set = 0, binding = 1) uniform sampler fs_samp0;

BB_INPUT(0, fs_in_attr0);
layout(location = 0) out vec4 frag_color0;

void main() {
    vec4 c = textureLod(sampler2D(fs_img0, fs_samp0), BB_UV(fs_in_attr0), 0.0);
    // The game's channel order: its target holds them swapped.
    frag_color0 = vec4(min(c.zyx, vec3(1.0)), c.w);
}
