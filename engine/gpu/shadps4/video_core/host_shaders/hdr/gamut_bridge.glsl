// SPDX-FileCopyrightText: Copyright (C) 2026 Carlos Lopez (RenoDX), bbport
// SPDX-License-Identifier: MIT
//
// bbport: GLSL port of RenoDX's AdaptiveD65 BT.709 gamut compression (https://github.com/
// clshortfuse/renodx, src/shaders/color/gamut.hlsl and macleod_boynton.hlsl, MIT), the piece of
// RenoDX's SDR LUT bridge (.agents/skills/handle-sdr-tonemap-lut/snippets/sdr_lut_bridge.hlsl)
// that hdr/bb_color_grading.frag uses: colors outside BT.709 are
// brought inside it along the line to white in MacLeod-Boynton chromaticity (CIE 170-2
// weights, Stockman-Sharpe LMS relative to D65 white), then the same line taken back out after
// the LUT. Strength 1, not compressing in-gamut colors, as the snippet calls them. The constants
// are RenoDX's matrices multiplied out in double precision, checked against RenoDX's HLSL.

#ifndef BBPORT_GAMUT_BRIDGE_GLSL
#define BBPORT_GAMUT_BRIDGE_GLSL

const vec3 BB_GM_BT709_TO_LMS[3] = vec3[3](  // rows
    vec3(0.2896057766, 0.6972466442, 0.07637127862),
    vec3(0.09018375762, 0.7073490257, 0.1122031464),
    vec3(0.01552874012, 0.05360930731, 0.5097978305));
const vec3 BB_GM_LMS_TO_BT709[3] = vec3[3](  // rows
    vec3(4.96761965, -4.922379529, 0.3391991812),
    vec3(-0.6196828788, 2.051750652, -0.3587439844),
    vec3(-0.08615200402, -0.065819383, 1.988954483));
const vec3 BB_GM_WEIGHTS = vec3(0.68990272, 0.34832189, 0.03715970692);
const vec3 BB_GM_ADAPTIVE_LMS = vec3(1.063223699, 0.9097359297, 0.5789358779); // LMS of BT.709 (1, 1, 1)
const vec2 BB_GM_WHITE = vec2(0.6983281224, 0.02047613467);                    // D65 in MB
// The BT.709 primaries in adaptive-relative weighted MB.
const vec2 BB_GM_BOUND_R = vec2(0.8447741838, 0.004480724501);
const vec2 BB_GM_BOUND_G = vec2(0.6255404359, 0.004757598166);
const vec2 BB_GM_BOUND_B = vec2(0.5356426431, 0.3536890971);

const float BB_GM_EPSILON = 1e-20;
const float BB_GM_NEAR_WHITE = 1e-14;
const float BB_GM_RAY_T_MAX = 1e20;
const float BB_GM_INTERVAL_MAX = 3.402823466e38;
const vec2 BB_GM_CIE_NORMALS[7] = vec2[7](
    vec2(-0.043889, -0.006807), vec2(-0.007821, -0.008564), vec2(-0.000604, -0.007942),
    vec2(0.0, -0.080835), vec2(0.953597, 0.307020), vec2(-0.060969, 0.019752),
    vec2(-0.106895, 0.004035));
const float BB_GM_CIE_NUMERATORS[7] = float[7](
    0.0065035249, 0.00104900495, 0.000207697044, 0.00165556648, 0.252472349, 0.0241967351,
    0.0199621232);

vec3 bb_gm_rows(const vec3 m[3], vec3 v) {
    return vec3(dot(m[0], v), dot(m[1], v), dot(m[2], v));
}

// macleod_boynton::from::WeightedLMS: (l, s, l + m).
vec3 bb_gm_mb(vec3 w) {
    float y = max(w.x + w.y, 0.0);
    float inv = y == 0.0 ? 0.0 : 1.0 / y;
    return vec3(w.x * inv, w.z * inv, y);
}

vec3 bb_gm_weighted(vec2 ls, float y) {
    return vec3(ls.x, 1.0 - ls.x, ls.y) * y;
}

// RayExitTCIE1702D: from white along `direction` to the CIE 170-2 locus (its 7 half-spaces).
float bb_gm_ray_exit_cie(vec2 direction) {
    if (dot(direction, direction) <= BB_GM_NEAR_WHITE) {
        return BB_GM_RAY_T_MAX;
    }
    float t_best = BB_GM_RAY_T_MAX;
    bool hit_any = false;
    for (int i = 0; i < 7; ++i) {
        float denom = dot(BB_GM_CIE_NORMALS[i], direction);
        float t = denom > 1e-8 ? BB_GM_CIE_NUMERATORS[i] / denom : BB_GM_RAY_T_MAX;
        t_best = min(t_best, t);
        hit_any = hit_any || denom > 1e-8;
    }
    return hit_any ? max(t_best, 0.0) : BB_GM_RAY_T_MAX;
}

float bb_gm_cross(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

bool bb_gm_segment_hit(vec2 origin, vec2 direction, vec2 a, vec2 b, out float t_hit) {
    t_hit = 0.0;
    vec2 edge = b - a;
    float denom = bb_gm_cross(direction, edge);
    if (abs(denom) <= BB_GM_EPSILON) return false;
    vec2 a_origin = a - origin;
    float t = bb_gm_cross(a_origin, edge) / denom;
    float u = bb_gm_cross(a_origin, direction) / denom;
    if (t < 0.0 || u < 0.0 || u > 1.0) return false;
    t_hit = t;
    return true;
}

// RayMaxT_RGBTriangleInMB: from white along `direction` to the BT.709 triangle.
float bb_gm_ray_triangle(vec2 direction, out bool has_solution) {
    has_solution = false;
    if (dot(direction, direction) <= BB_GM_NEAR_WHITE) return 0.0;
    float t_best = BB_GM_INTERVAL_MAX, t_hit;
    bool hit_any = false;
    if (bb_gm_segment_hit(BB_GM_WHITE, direction, BB_GM_BOUND_R, BB_GM_BOUND_G, t_hit)) {
        t_best = min(t_best, t_hit);
        hit_any = true;
    }
    if (bb_gm_segment_hit(BB_GM_WHITE, direction, BB_GM_BOUND_G, BB_GM_BOUND_B, t_hit)) {
        t_best = min(t_best, t_hit);
        hit_any = true;
    }
    if (bb_gm_segment_hit(BB_GM_WHITE, direction, BB_GM_BOUND_B, BB_GM_BOUND_R, t_hit)) {
        t_best = min(t_best, t_hit);
        hit_any = true;
    }
    has_solution = hit_any;
    return hit_any ? max(t_best, 0.0) : 0.0;
}

// ClampWeightedLMSToCIE1702 (purity 1).
vec3 bb_gm_clamp_cie(vec3 w) {
    vec3 c = max(w, 0.0);
    vec3 mb = bb_gm_mb(c);
    if (!(mb.z > BB_GM_EPSILON)) return vec3(c.x, c.y, 0.0);
    vec2 direction = mb.xy - BB_GM_WHITE;
    if (dot(direction, direction) <= BB_GM_NEAR_WHITE) return c;
    float t_final = min(1.0, bb_gm_ray_exit_cie(direction));
    return bb_gm_weighted(BB_GM_WHITE + direction * t_final, mb.z);
}

// The adaptive-relative weighted LMS of a BT.709 color, and back.
vec3 bb_gm_relative(vec3 bt709) {
    vec3 w = BB_GM_WEIGHTS * bb_gm_rows(BB_GM_BT709_TO_LMS, bt709);
    return vec3(BB_GM_ADAPTIVE_LMS.x == 0.0 ? 0.0 : w.x / BB_GM_ADAPTIVE_LMS.x,
                BB_GM_ADAPTIVE_LMS.y == 0.0 ? 0.0 : w.y / BB_GM_ADAPTIVE_LMS.y,
                BB_GM_ADAPTIVE_LMS.z == 0.0 ? 0.0 : w.z / BB_GM_ADAPTIVE_LMS.z);
}

vec3 bb_gm_bt709(vec3 relative) {
    return bb_gm_rows(BB_GM_LMS_TO_BT709,
                      relative * max(BB_GM_ADAPTIVE_LMS, vec3(1e-6)) / BB_GM_WEIGHTS);
}

// ComputeGamutCompressionScaleBT709AdaptiveD65(color, D65, strength 1): 1 inside BT.709, the
// share of the line to white kept outside it.
float bb_gamut_compression_scale(vec3 bt709) {
    vec3 c = bb_gm_clamp_cie(max(bb_gm_relative(bt709), 0.0));
    vec3 mb = bb_gm_mb(c);
    if (!(mb.z > BB_GM_EPSILON)) return 1.0;
    vec2 direction = mb.xy - BB_GM_WHITE;
    if (dot(direction, direction) <= BB_GM_NEAR_WHITE) return 1.0;
    bool has_peak;
    float t_peak = bb_gm_ray_triangle(direction, has_peak);
    float t_clip = bb_gm_ray_exit_cie(direction);
    if (!has_peak) t_peak = t_clip;
    float t_hard = clamp(t_peak, 0.0, 1.0);
    // NeutwoScaleFromRayT(min(t_peak, t_clip), t_clip): Neutwo(1, peak, clip).
    float peak = max(min(t_peak, t_clip), 0.0);
    float clip = max(t_clip, peak);
    float neutwo = clip * peak * inversesqrt(max((clip * clip - peak * peak) + clip * clip * peak * peak, 1e-20));
    float t_soft = clamp(neutwo, 0.0, 1.0);
    // SoftCompressionActivationFromRayT(t_peak, 0.08).
    float outside = 1.0 - clamp(t_peak, 0.0, 1.0);
    float soft_mix = outside / (outside + 0.08);
    return mix(t_hard, t_soft, soft_mix);
}

// GamutCompressBT709AdaptiveD65.
vec3 bb_gamut_compress(vec3 bt709, float scale) {
    vec3 c = bb_gm_clamp_cie(max(bb_gm_relative(bt709), 0.0));
    vec3 mb = bb_gm_mb(c);
    vec3 out_rel;
    if (!(mb.z > BB_GM_EPSILON)) {
        out_rel = vec3(c.x, c.y, 0.0);
    } else {
        vec2 direction = mb.xy - BB_GM_WHITE;
        out_rel = dot(direction, direction) <= BB_GM_NEAR_WHITE
                      ? c
                      : bb_gm_weighted(BB_GM_WHITE + clamp(scale, 0.0, 1.0) * direction, mb.z);
    }
    return bb_gm_bt709(out_rel);
}

// GamutDecompressBT709AdaptiveD65.
vec3 bb_gamut_decompress(vec3 bt709, float scale) {
    vec3 c = max(bb_gm_relative(bt709), 0.0);
    vec3 mb = bb_gm_mb(c);
    vec3 out_rel;
    if (!(mb.z > BB_GM_EPSILON)) {
        out_rel = vec3(c.x, c.y, 0.0);
    } else {
        vec2 direction = mb.xy - BB_GM_WHITE;
        out_rel = dot(direction, direction) <= BB_GM_NEAR_WHITE
                      ? c
                      : bb_gm_weighted(BB_GM_WHITE + direction / max(scale, 1e-6), mb.z);
    }
    return bb_gm_bt709(out_rel);
}

#endif
