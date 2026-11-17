// SPDX-FileCopyrightText: Copyright (C) 2026 Carlos Lopez (RenoDX), bbport
// SPDX-License-Identifier: MIT
//
// bbport: GLSL port of RenoDX's PsychoV30 tonemapper (https://github.com/clshortfuse/renodx,
// src/shaders/tonemap/psychov/test30.hlsl, MIT, commit 0ede35a of 15/10/2026): a perceptual
// tonemap in Stockman/CVRL cone (LMS) space with a finite per-cone response, Mean-A2 direction
// and an exact projection into the display's RGB cube. See the original file for the full
// research record; function names and structure follow it one to one (psycho30_X -> rd_psy30_x).
//
// Port notes:
//   - Derived constants (LMS matrices, Yf weights, D/T bases) are evaluated in double precision
//     from color/rgb.hlsl and written as literals.
//   - HLSL mul(M, v) is M * v here: the matrices are written by columns.
//   - The positional compatibility placeholders of psychotm_test30 (bleaching_intensity,
//     clip_point, hue_restore, encoded_response_power, white_curve_mode,
//     adaptive_normalization) are unused by the original and dropped.
//   - Checked against the HLSL original compiled through glslang (max difference 2e-4 of the
//     largest channel, 791 colors, 9 configurations).

#ifndef BBPORT_PSYCHOV30_GLSL
#define BBPORT_PSYCHOV30_GLSL

// ---------------------------------------------------------------- constants

const float RD_PSY30_EPSILON = 1e-6;
const float RD_PSY30_EPSILON2 = RD_PSY30_EPSILON * RD_PSY30_EPSILON;
const float RD_PSY30_AUTO_COMPRESSION_SENTINEL = 0.0;
const float RD_PSY30_LARGE_SUPPORT = 1e20;
const int RD_PSY30_SOURCE_BOUNDARY_NONE = 0;
const int RD_PSY30_SOURCE_BOUNDARY_BT709 = 1;
const int RD_PSY30_SOURCE_BOUNDARY_BT2020 = 2;
const int RD_PSY30_SOURCE_BOUNDARY_AP1 = 3;
const float RD_PSY30_REFERENCE_SIMULTANEOUS_RANGE_LOG10 = 3.7;
const float RD_PSY30_HIGHLIGHT_GRADE_REFERENCE_WHITE = 1.0;
const float RD_PSY30_SHADOW_GRADE_RANGE_STOPS = 4.0;

// Evaluated from RenoDX's color/rgb.hlsl (GLSL mat3 takes the HLSL matrix columns,
// so M * v here is mul(M, v) there).
const mat3 RD_PSY30_BT709_TO_LMS = mat3(  // columns
    vec3(0.2896057766, 0.09018375762, 0.01552874012),
    vec3(0.6972466442, 0.7073490257, 0.05360930731),
    vec3(0.07637127862, 0.1122031464, 0.5097978305));
const mat3 RD_PSY30_LMS_TO_BT709 = mat3(  // columns
    vec3(4.96761965, -0.6196828788, -0.08615200402),
    vec3(-4.922379529, 2.051750652, -0.065819383),
    vec3(0.3391991812, -0.3587439844, 1.988954483));
const mat3 RD_PSY30_LMS_TO_BT2020 = mat3(  // columns
    vec3(2.908921354, -0.227553273, -0.05027123211),
    vec3(-2.415564245, 1.545796661, 0.04094895167),
    vec3(0.1808342948, -0.2838427127, 1.755283912));
const mat3 RD_PSY30_BT2020_TO_LMS = mat3(  // columns
    vec3(0.3926591887, 0.05961208847, 0.009855048818),
    vec3(0.6120450441, 0.7370746835, 0.0003337481729),
    vec3(0.05851946653, 0.1130491576, 0.5687470809));
const mat3 RD_PSY30_AP1_TO_LMS = mat3(  // columns
    vec3(0.4011385871, 0.05893789913, 0.007257477879),
    vec3(0.6054981379, 0.7364008181, -0.01424600012),
    vec3(0.05658693641, 0.1143971872, 0.5859244256));
const mat3 RD_PSY30_BT709_TO_BT2020 = mat3(  // columns
    vec3(0.627403896, 0.06909728939, 0.01639143883),
    vec3(0.3292830384, 0.9195403951, 0.08801330783),
    vec3(0.04331306566, 0.01136231562, 0.8955952532));
const mat3 RD_PSY30_BT709_TO_AP1 = mat3(  // columns
    vec3(0.6130974024, 0.0701937225, 0.0206155929),
    vec3(0.3395231462, 0.9163538791, 0.1095697729),
    vec3(0.0473794514, 0.0134523985, 0.8698146342));
const vec3 RD_PSY30_BT709_SOURCE_YF_WEIGHTS = vec3(0.2201184123, 0.6925135352, 0.08736805248);
const vec3 RD_PSY30_BT2020_SOURCE_YF_WEIGHTS = vec3(0.2776659568, 0.6464104959, 0.07592354734);
const vec3 RD_PSY30_AP1_SOURCE_YF_WEIGHTS = vec3(0.2830116585, 0.6418870575, 0.07510128397);
const vec3 RD_PSY30_D65_WHITE_LMS = vec3(1.063223699, 0.9097359297, 0.5789358779);
const vec3 RD_PSY30_BT709_D_RGB = vec3(4.681554141, -1.461030493, -0.2141843862);
const vec3 RD_PSY30_BT709_T_RGB = vec3(0.09818728788, -0.1038448818, 0.5757385549);
const vec3 RD_PSY30_BT2020_D_RGB = vec3(2.446855765, -1.022427487, -0.2436752377);
const vec3 RD_PSY30_BT2020_T_RGB = vec3(0.0523457306, -0.08216336502, 0.5080984163);
// Yf row of STOCKMAN_SHARP_LMS_TO_XFYFZF_MAT, and CIE1702_MB_CIE_WEIGHTS.
const vec3 RD_PSY30_YF_LMS = vec3(0.68990272, 0.34832189, 0.0);
const vec3 RD_PSY30_MB_WEIGHTS = vec3(0.68990272, 0.34832189, 0.03715970692);
const float RD_PSY30_D65_WHITE_YF = 1.050401861;
const float RD_PSY30_D65_ALPHA_L = 0.6983240888;
const float RD_PSY30_D65_ALPHA_M = 0.3016759112;
const float RD_PSY30_D65_ALPHA_DELTA = 0.3966481776;

// ---------------------------------------------------------------- math helpers

float rd_psy30_yf(vec3 lms) {  // renodx::color::yf::from::LMS
    return dot(RD_PSY30_YF_LMS, lms);
}
float rd_psy30_max3(vec3 v) {
    return max(max(v.x, v.y), v.z);
}
float rd_psy30_min3(vec3 v) {
    return min(min(v.x, v.y), v.z);
}
float rd_psy30_copysign(float mag, float sgn) {
    return uintBitsToFloat((floatBitsToUint(sgn) & 0x80000000u) |
                           (floatBitsToUint(mag) & 0x7FFFFFFFu));
}
vec3 rd_psy30_copysign(vec3 mag, vec3 sgn) {
    return uintBitsToFloat((floatBitsToUint(sgn) & uvec3(0x80000000u)) |
                           (floatBitsToUint(mag) & uvec3(0x7FFFFFFFu)));
}
float rd_psy30_saturate(float x) {
    return clamp(x, 0.0, 1.0);
}

// ---------------------------------------------------------------- PsychoV30

vec3 rd_psy30_anchor_source_boundary_to_yf(vec3 source_rgb, mat3 source_to_lms,
                                           vec3 source_yf_weights) {
    if (all(greaterThanEqual(source_rgb, vec3(0.0)))) {
        return source_to_lms * source_rgb;
    }
    float source_total = dot(max(source_rgb, vec3(0.0)), source_yf_weights);
    vec3 residual = source_rgb - source_total;
    vec3 lower_fraction = mix(vec3(RD_PSY30_LARGE_SUPPORT), source_total / -residual,
                              lessThan(residual, vec3(-RD_PSY30_EPSILON)));
    float boundary_fraction = min(1.0, rd_psy30_min3(lower_fraction));
    vec3 anchored_source_rgb = source_total + residual * boundary_fraction;
    vec3 bounded_lms = source_to_lms * anchored_source_rgb;
    float bounded_yf = rd_psy30_yf(bounded_lms);
    float scale = source_total * RD_PSY30_D65_WHITE_YF / bounded_yf;
    return bounded_lms * scale;
}

// F(x) = peak * [1 + (1/k - 1) * q^(-beta)]^(-1/h), q = x/anchor, k = (anchor/peak)^h.
float rd_psy30_finite_endpoint(float x, float anchor, float peak, float h) {
    bool uniform_response = h == 1.0;
    float anchor_power = anchor / peak;
    if (!uniform_response) {
        anchor_power = pow(anchor / peak, h);
    }
    float slope_normalization = 1.0 - anchor_power;
    float normalized_input = x / anchor;
    float response_drive = pow(normalized_input, -h / slope_normalization);
    float encoded = anchor_power / (anchor_power + slope_normalization * response_drive);
    float result = peak * encoded;
    if (!uniform_response) {
        result = peak * pow(encoded, 1.0 / h);
    }
    return result;
}

float rd_psy30_auto_compression_power(float anchor_yf, float peak_yf) {
    float above_adaptation_range = log2(peak_yf / anchor_yf) * 0.30102999566398120;  // log10
    return max((RD_PSY30_REFERENCE_SIMULTANEOUS_RANGE_LOG10 * 0.5) / above_adaptation_range, 1.0);
}

float rd_psy30_grade_quintic_unit_ramp(float t) {
    t = rd_psy30_saturate(t);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float rd_psy30_highlights_scalar(float x, float highlights, float adapted_anchor_yf) {
    if (highlights == 1.0) {
        return x;
    }
    float ratio = x / adapted_anchor_yf;
    float t;
    if (x > adapted_anchor_yf) {
        t = rd_psy30_saturate(log2(ratio) /
                              log2(RD_PSY30_HIGHLIGHT_GRADE_REFERENCE_WHITE / adapted_anchor_yf));
    } else {
        t = 0.0;
    }
    t = rd_psy30_grade_quintic_unit_ramp(t);
    if (highlights > 1.0) {
        float shaped = pow(ratio, highlights - 1.0);
        return x * mix(1.0, shaped, t);
    }
    float shaped = pow(ratio, 1.0 - highlights);
    return x / mix(1.0, shaped, t);
}

float rd_psy30_shadows_scalar(float x, float shadows, float adapted_anchor_yf) {
    if (shadows == 1.0) {
        return x;
    }
    float ratio = x / adapted_anchor_yf;
    float anchor_squared = adapted_anchor_yf * adapted_anchor_yf;
    float anchor_cubed = anchor_squared * adapted_anchor_yf;
    float shadow_floor = adapted_anchor_yf * exp2(-RD_PSY30_SHADOW_GRADE_RANGE_STOPS);
    float t;
    if (x > shadow_floor) {
        t = rd_psy30_saturate(log2(ratio) / log2(shadow_floor / adapted_anchor_yf));
    } else {
        t = 1.0;
    }
    t = rd_psy30_grade_quintic_unit_ramp(t);
    if (shadows > 1.0) {
        float shaped = pow(ratio, 2.0 - shadows);
        float target = x * (1.0 - anchor_squared) + anchor_cubed * shaped;
        return mix(x, target, t);
    }
    float shaped = pow(ratio, shadows);
    float target = x * (1.0 + anchor_squared) - anchor_cubed * shaped;
    return mix(x, target, t);
}

vec3 rd_psy30_apply_adaptive_relative_purity(vec3 source_lms, vec3 anchor_lms,
                                             float purity_delta) {
    vec3 source_q = source_lms / anchor_lms;
    if (purity_delta == 1.0) return source_q;
    float relative_yf = rd_psy30_yf(source_lms) / rd_psy30_yf(anchor_lms);
    return mix(vec3(relative_yf), source_q, purity_delta);
}

vec2 rd_psy30_adaptive_neutral_mb() {
    float lm_weight_sum = RD_PSY30_MB_WEIGHTS.x + RD_PSY30_MB_WEIGHTS.y;
    return vec2(RD_PSY30_MB_WEIGHTS.x, RD_PSY30_MB_WEIGHTS.z) / lm_weight_sum;
}

vec3 rd_psy30_target_rgb_from_lms(vec3 lms, int target_gamut_mode) {
    return target_gamut_mode == 0 ? RD_PSY30_LMS_TO_BT709 * lms : RD_PSY30_LMS_TO_BT2020 * lms;
}

float rd_psy30_target_neutral_yf_limit(float target_rgb_peak, vec3 anchor_lms,
                                       int target_gamut_mode) {
    float anchor_yf = rd_psy30_yf(anchor_lms);
    vec3 anchor_target_rgb = rd_psy30_target_rgb_from_lms(anchor_lms, target_gamut_mode);
    return target_rgb_peak * anchor_yf / rd_psy30_max3(anchor_target_rgb);
}

vec2 rd_psy30_mb_from_relative_lms(vec3 relative_lms) {
    float yf = rd_psy30_yf(relative_lms);
    return vec2(relative_lms.x * RD_PSY30_MB_WEIGHTS.x / yf,
                relative_lms.z * RD_PSY30_MB_WEIGHTS.z / yf);
}

vec3 rd_psy30_apply_signed_cone_response_fallback(vec3 source_relative_lms,
                                                  float response_power) {
    if (response_power == 1.0) {
        return source_relative_lms;
    }
    return rd_psy30_copysign(pow(abs(source_relative_lms), vec3(response_power)),
                             source_relative_lms);
}

float rd_psy30_scaled_a2_radius6(vec2 dt) {
    return 3.0 * dt.x * dt.x + dt.y * dt.y;
}

// Requires all(source_q > 0).
vec3 rd_psy30_finite_response_u_from_positive_q(vec3 source_q, vec3 adaptation_peak_ratio,
                                                float response_power, float response_h) {
    bool uniform_response = response_h == 1.0;
    vec3 anchor_power;
    if (uniform_response) {
        anchor_power = adaptation_peak_ratio;
    } else {
        anchor_power = pow(adaptation_peak_ratio, vec3(response_h));
    }
    vec3 slope_normalization = 1.0 - anchor_power;
    vec3 input_exponent = response_power * response_h / slope_normalization;
    vec3 response_drive = pow(source_q, -input_exponent);
    vec3 finite_encoded = anchor_power / (anchor_power + slope_normalization * response_drive);
    if (uniform_response) {
        return finite_encoded;
    }
    return pow(finite_encoded, vec3(1.0 / response_h));
}

// Requires all(source_q > 0); the caller selects the signed path otherwise.
vec3 rd_psy30_mean_a2_response_from_positive_q(vec3 source_q, vec3 adaptation_peak_ratio,
                                               float response_power, float response_h,
                                               out float normalized_response_yf) {
    vec3 response_u = rd_psy30_finite_response_u_from_positive_q(
        source_q, adaptation_peak_ratio, response_power, response_h);
    vec2 source_dt = vec2(source_q.x - source_q.y, 2.0 * source_q.z - source_q.x - source_q.y);
    float response_lms_difference = response_u.x - response_u.y;
    float response_lms_sum = response_u.x + response_u.y;
    vec2 response_dt = vec2(response_lms_difference, 2.0 * response_u.z - response_lms_sum);
    vec2 authored_dt = response_dt;
    float source_radius6 = rd_psy30_scaled_a2_radius6(source_dt);
    float response_radius6 = rd_psy30_scaled_a2_radius6(response_dt);

    if (source_radius6 > 6.0 * RD_PSY30_EPSILON2 && response_radius6 > 6.0 * RD_PSY30_EPSILON2) {
        float inverse_response_radius = inversesqrt(response_radius6);
        float response_radius = response_radius6 * inverse_response_radius;
        vec2 mean_direction =
            source_dt * inversesqrt(source_radius6) + response_dt * inverse_response_radius;
        float mean_radius6 = rd_psy30_scaled_a2_radius6(mean_direction);
        if (mean_radius6 > RD_PSY30_EPSILON2) {
            authored_dt = mean_direction * (response_radius * inversesqrt(mean_radius6));
        }
    }

    normalized_response_yf =
        RD_PSY30_D65_ALPHA_L * response_u.x + RD_PSY30_D65_ALPHA_M * response_u.y;
    return vec3(authored_dt.x, response_lms_sum + response_u.z, authored_dt.y);
}

vec2 rd_psy30_closest_point_on_scale_segment(float desired_c, float radial_metric,
                                             vec2 segment_start, vec2 segment_end) {
    vec2 segment = segment_end - segment_start;
    float denominator = 2.0 * segment.x * segment.x + radial_metric * segment.y * segment.y;
    if (denominator <= RD_PSY30_EPSILON2) return segment_start;
    float numerator = 2.0 * (desired_c - segment_start.x) * segment.x +
                      radial_metric * (1.0 - segment_start.y) * segment.y;
    float t = rd_psy30_saturate(numerator / denominator);
    return segment_start + segment * t;
}

float rd_psy30_scale_cost(vec2 candidate, float desired_c, float radial_metric) {
    vec2 delta = candidate - vec2(desired_c, 1.0);
    return 2.0 * delta.x * delta.x + radial_metric * delta.y * delta.y;
}

// Exact nearest point for the fixed authored A2 direction (four-edge polygon in (C, scale)).
vec3 rd_psy30_yf_ceiling_solve(vec3 desired_coord, float response_yf, int target_gamut_mode) {
    float max_a = response_yf;
    float radial_a_numerator = 3.0 * RD_PSY30_D65_ALPHA_DELTA * desired_coord.x - desired_coord.z;
    float desired_a = (2.0 * desired_coord.y + radial_a_numerator) / 6.0;
    vec3 radial_rgb;
    if (target_gamut_mode == 0) {
        radial_rgb = desired_coord.x * RD_PSY30_BT709_D_RGB + desired_coord.z * RD_PSY30_BT709_T_RGB;
    } else {
        radial_rgb =
            desired_coord.x * RD_PSY30_BT2020_D_RGB + desired_coord.z * RD_PSY30_BT2020_T_RGB;
    }
    vec3 desired_target_rgb = desired_a + radial_rgb;
    if (desired_a >= 0.0 && desired_a <= max_a &&
        all(greaterThanEqual(desired_target_rgb, vec3(-RD_PSY30_EPSILON))) &&
        all(lessThanEqual(desired_target_rgb, vec3(1.0 + RD_PSY30_EPSILON)))) {
        return desired_coord;
    }

    float radial_metric = rd_psy30_scaled_a2_radius6(desired_coord.xz);
    if (radial_metric <= 6.0 * RD_PSY30_EPSILON2) {
        return vec3(0.0, clamp(desired_coord.y, 0.0, 3.0 * max_a), 0.0);
    }

    float positive_pressure = rd_psy30_max3(radial_rgb);
    float negative_pressure = -rd_psy30_min3(radial_rgb);

    float total_pressure = positive_pressure + negative_pressure;
    bool is_triangle = max_a * total_pressure <= negative_pressure;
    float max_a_radial_scale;
    float upper_a;
    float upper_radial_scale;
    if (is_triangle) {
        max_a_radial_scale = max_a / negative_pressure;
        upper_a = max_a;
        upper_radial_scale = max_a_radial_scale;
    } else {
        max_a_radial_scale = (1.0 - max_a) / positive_pressure;
        upper_radial_scale = 1.0 / total_pressure;
        upper_a = negative_pressure * upper_radial_scale;
    }
    vec2 vertex0 = vec2(0.0);
    vec2 vertex1 = vec2(3.0 * max_a, 0.0);
    vec2 vertex2 =
        vec2(3.0 * max_a - 0.5 * radial_a_numerator * max_a_radial_scale, max_a_radial_scale);
    vec2 vertex3 =
        vec2(3.0 * upper_a - 0.5 * radial_a_numerator * upper_radial_scale, upper_radial_scale);
    // The first edge has constant zero radial scale, so no projection divide.
    vec2 best_c_scale = vec2(clamp(desired_coord.y, vertex0.x, vertex1.x), 0.0);
    float best_cost = rd_psy30_scale_cost(best_c_scale, desired_coord.y, radial_metric);

    vec2 edge_candidate = rd_psy30_closest_point_on_scale_segment(desired_coord.y, radial_metric,
                                                                  vertex1, vertex2);
    float edge_cost = rd_psy30_scale_cost(edge_candidate, desired_coord.y, radial_metric);
    if (edge_cost < best_cost) {
        best_c_scale = edge_candidate;
        best_cost = edge_cost;
    }

    if (!is_triangle) {
        edge_candidate = rd_psy30_closest_point_on_scale_segment(desired_coord.y, radial_metric,
                                                                 vertex2, vertex3);
        edge_cost = rd_psy30_scale_cost(edge_candidate, desired_coord.y, radial_metric);
        if (edge_cost < best_cost) {
            best_c_scale = edge_candidate;
            best_cost = edge_cost;
        }
    }

    edge_candidate = rd_psy30_closest_point_on_scale_segment(desired_coord.y, radial_metric,
                                                             vertex0, vertex3);
    edge_cost = rd_psy30_scale_cost(edge_candidate, desired_coord.y, radial_metric);
    if (edge_cost < best_cost) {
        best_c_scale = edge_candidate;
    }

    return vec3(desired_coord.x * best_c_scale.y, best_c_scale.x,
                desired_coord.z * best_c_scale.y);
}

vec2 rd_psy30_scaled_a2_from_q(vec3 q) {
    return vec2(q.x - q.y, 2.0 * q.z - q.x - q.y);
}

vec3 rd_psy30_lms_from_scaled_a2(vec2 scaled_a2, float physical_yf, vec3 anchor_lms) {
    float a_l = RD_PSY30_YF_LMS.x * anchor_lms.x;
    float a_m = RD_PSY30_YF_LMS.y * anchor_lms.y;
    float q_m = (physical_yf - a_l * scaled_a2.x) / (a_l + a_m);
    float q_l = q_m + scaled_a2.x;
    float q_s = 0.5 * (scaled_a2.y + q_l + q_m);
    return vec3(q_l, q_m, q_s) * anchor_lms;
}

float rd_psy30_scaled_a2_target_support(vec2 direction, float clip_magnitude, float physical_yf,
                                        vec3 anchor_lms, int target_gamut_mode,
                                        float target_rgb_peak) {
    if (clip_magnitude <= RD_PSY30_EPSILON) return 0.0;

    vec3 neutral_target = rd_psy30_target_rgb_from_lms(
        rd_psy30_lms_from_scaled_a2(vec2(0.0), physical_yf, anchor_lms), target_gamut_mode);
    vec3 unit_target = rd_psy30_target_rgb_from_lms(
        rd_psy30_lms_from_scaled_a2(direction, physical_yf, anchor_lms), target_gamut_mode);
    vec3 delta_target = unit_target - neutral_target;

    float support = clip_magnitude;
    for (int channel = 0; channel < 3; ++channel) {
        if (delta_target[channel] > RD_PSY30_EPSILON) {
            support = min(support,
                          (target_rgb_peak - neutral_target[channel]) / delta_target[channel]);
        } else if (delta_target[channel] < -RD_PSY30_EPSILON) {
            support = min(support, neutral_target[channel] / -delta_target[channel]);
        }
    }
    return max(support, 0.0);
}

float rd_psy30_cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

float rd_psy30_ray_segment_radius(vec2 origin, vec2 direction, vec2 a, vec2 b) {
    vec2 edge = b - a;
    float denominator = rd_psy30_cross2(direction, edge);
    if (abs(denominator) <= RD_PSY30_EPSILON) return RD_PSY30_LARGE_SUPPORT;
    vec2 ao = a - origin;
    float t = rd_psy30_cross2(ao, edge) / denominator;
    float u = rd_psy30_cross2(ao, direction) / denominator;
    return t >= 0.0 && u >= 0.0 && u <= 1.0 ? t : RD_PSY30_LARGE_SUPPORT;
}

float rd_psy30_transformed_source_clip_scaled_a2_magnitude(vec2 source_mb, float response_power,
                                                           vec3 response_anchor_ratio,
                                                           float physical_yf, vec3 anchor_lms) {
    vec2 neutral_mb = rd_psy30_adaptive_neutral_mb();
    vec2 source_offset = source_mb - neutral_mb;
    float source_radius2 = dot(source_offset, source_offset);
    if (source_radius2 <= RD_PSY30_EPSILON2) return 0.0;

    vec2 vertices[3];
    for (int channel = 0; channel < 3; ++channel) {
        // Column `channel` of BT709_TO_LMS: the LMS of that BT.709 primary.
        vertices[channel] = rd_psy30_mb_from_relative_lms(RD_PSY30_BT709_TO_LMS[channel] / anchor_lms);
    }
    vec2 source_direction = source_offset * inversesqrt(source_radius2);
    float source_boundary_radius = min(
        rd_psy30_ray_segment_radius(neutral_mb, source_direction, vertices[0], vertices[1]),
        min(rd_psy30_ray_segment_radius(neutral_mb, source_direction, vertices[1], vertices[2]),
            rd_psy30_ray_segment_radius(neutral_mb, source_direction, vertices[2], vertices[0])));
    if (source_boundary_radius >= RD_PSY30_LARGE_SUPPORT) return 0.0;

    vec2 boundary_mb = neutral_mb + source_direction * source_boundary_radius;
    float m_fraction = 1.0 - boundary_mb.x;
    if (boundary_mb.x <= RD_PSY30_EPSILON || m_fraction <= RD_PSY30_EPSILON ||
        boundary_mb.y <= RD_PSY30_EPSILON) {
        return 0.0;
    }
    vec2 boundary_response_ratio =
        vec2(boundary_mb.x * RD_PSY30_MB_WEIGHTS.y / (m_fraction * RD_PSY30_MB_WEIGHTS.x),
             boundary_mb.y * RD_PSY30_MB_WEIGHTS.y / (m_fraction * RD_PSY30_MB_WEIGHTS.z));
    vec2 response_ratio;
    if (response_power == 1.0) {
        response_ratio = boundary_response_ratio;
    } else {
        response_ratio = pow(boundary_response_ratio, vec2(response_power));
    }
    response_ratio *= vec2(response_anchor_ratio.x / response_anchor_ratio.y,
                           response_anchor_ratio.z / response_anchor_ratio.y);
    vec3 boundary_q_direction = vec3(response_ratio.x, 1.0, response_ratio.y);
    float boundary_q_m = physical_yf / rd_psy30_yf(anchor_lms * boundary_q_direction);
    vec3 boundary_q_at_yf = boundary_q_direction * boundary_q_m;
    return sqrt(rd_psy30_scaled_a2_radius6(rd_psy30_scaled_a2_from_q(boundary_q_at_yf)));
}

float rd_psy30_neutwo_with_clip(float x, float peak, float clip, float h) {
    if (peak <= RD_PSY30_EPSILON) return 0.0;
    if (clip <= peak * (1.0 + RD_PSY30_EPSILON)) return min(x, peak);
    float q = rd_psy30_saturate(x / clip);
    float k = rd_psy30_saturate(peak / clip);
    if (h == 1.0) {
        float denominator = q * (1.0 - k) + k;
        return peak * q / denominator;
    }
    float qh = pow(q, h);
    float kh = pow(k, h);
    float denominator = pow(qh * (1.0 - kh) + kh, 1.0 / h);
    return peak * q / denominator;
}

// Signed fallback for adaptation-relative LMS containing a zero or negative cone value.
vec3 rd_psy30_linear_a2_fallback(vec3 source_q, vec3 anchor_in_lms, vec3 anchor_out_lms,
                                 int target_gamut_mode, float target_rgb_peak,
                                 float response_power, float response_h,
                                 float target_compression_strength) {
    vec3 response_lms =
        anchor_out_lms * rd_psy30_apply_signed_cone_response_fallback(source_q, response_power);
    float response_yf = rd_psy30_yf(response_lms);

    vec2 source_scaled_a2 = rd_psy30_scaled_a2_from_q(source_q);
    vec2 response_scaled_a2 = rd_psy30_scaled_a2_from_q(response_lms / anchor_in_lms);
    float source_radius6 = rd_psy30_scaled_a2_radius6(source_scaled_a2);
    float response_radius6 = rd_psy30_scaled_a2_radius6(response_scaled_a2);
    vec3 authored_lms = response_lms;
    if (source_radius6 > 6.0 * RD_PSY30_EPSILON2 && response_radius6 > 6.0 * RD_PSY30_EPSILON2) {
        float inverse_source_radius = inversesqrt(source_radius6);
        float inverse_response_radius = inversesqrt(response_radius6);
        float response_radius = response_radius6 * inverse_response_radius;
        vec2 midpoint = source_scaled_a2 * inverse_source_radius +
                        response_scaled_a2 * inverse_response_radius;
        float midpoint_radius6 = rd_psy30_scaled_a2_radius6(midpoint);
        if (midpoint_radius6 > RD_PSY30_EPSILON2) {
            authored_lms = rd_psy30_lms_from_scaled_a2(
                midpoint * inversesqrt(midpoint_radius6) * response_radius, response_yf,
                anchor_in_lms);
        }
    }

    float neutral_yf_limit =
        rd_psy30_target_neutral_yf_limit(target_rgb_peak, anchor_in_lms, target_gamut_mode);
    float anchor_out_yf = rd_psy30_yf(anchor_out_lms);
    float target_yf = rd_psy30_copysign(
        rd_psy30_finite_endpoint(abs(response_yf), anchor_out_yf, neutral_yf_limit, response_h),
        response_yf);

    vec3 desired_lms;
    if (response_yf == 0.0 && target_yf == 0.0) {
        desired_lms = vec3(0.0);
    } else {
        desired_lms = authored_lms * (target_yf / response_yf);
    }
    if (target_compression_strength == 0.0) {
        return desired_lms;
    } else if (target_yf <= 0.0) {
        return desired_lms * (1.0 - target_compression_strength);
    }
    vec2 desired_scaled_a2 = rd_psy30_scaled_a2_from_q(desired_lms / anchor_in_lms);
    float desired_radius6 = rd_psy30_scaled_a2_radius6(desired_scaled_a2);
    if (desired_radius6 <= 6.0 * RD_PSY30_EPSILON2) {
        return anchor_in_lms * (target_yf / rd_psy30_yf(anchor_in_lms));
    }

    float inverse_desired_magnitude = inversesqrt(desired_radius6);
    float desired_magnitude = desired_radius6 * inverse_desired_magnitude;
    vec2 direction = desired_scaled_a2 * inverse_desired_magnitude;
    float source_clip_magnitude = max(
        rd_psy30_transformed_source_clip_scaled_a2_magnitude(
            rd_psy30_mb_from_relative_lms(source_q), response_power,
            anchor_out_lms / anchor_in_lms, target_yf, anchor_in_lms),
        desired_magnitude);
    float target_support = rd_psy30_scaled_a2_target_support(
        direction, source_clip_magnitude, target_yf, anchor_in_lms, target_gamut_mode,
        target_rgb_peak);
    float compressed_magnitude =
        min(rd_psy30_neutwo_with_clip(desired_magnitude, target_support,
                                      max(source_clip_magnitude, target_support), response_h),
            target_support);
    return rd_psy30_lms_from_scaled_a2(
        direction * mix(desired_magnitude, compressed_magnitude, target_compression_strength),
        target_yf, anchor_in_lms);
}

// psychotm_test30. Input and output: linear BT.709, 1.0 = reference (paper) white.
//   peak_value              display peak / reference white (1000/203 by default in RenoDX)
//   exposure                linear multiplier
//   highlights, shadows     scalar-Yf grades (1 = off)
//   contrast                factor in the common cone power p
//   purity_scale            adaptation-relative LMS purity (1 = unchanged)
//   cone_response_exponent  second factor in p
//   adaptive_state          input anchor (BT.709, 0.18 by default)
//   background_state        output anchor (BT.709, 0.18 by default)
//   gamut_compression       target-projection strength (1 = full)
//   gamut_compression_mode  0 = BT.709 cube, otherwise BT.2020 cube
//   compression             manual h > 0, or 0 for automatic
//   source_boundary         RD_PSY30_SOURCE_BOUNDARY_* (BT.709 by default)
vec3 rd_psychov30(vec3 bt709_linear_input, float peak_value, float exposure, float highlights,
                  float shadows, float contrast, float purity_scale, float cone_response_exponent,
                  vec3 adaptive_state, vec3 background_state, float gamut_compression,
                  int gamut_compression_mode, float compression, int source_boundary) {
    // Extended-range transport to observer basis.
    vec3 exposed_input = bt709_linear_input * exposure;
    vec3 input_lms;
    if (source_boundary == RD_PSY30_SOURCE_BOUNDARY_NONE) {
        input_lms = RD_PSY30_BT709_TO_LMS * exposed_input;
    } else if (source_boundary == RD_PSY30_SOURCE_BOUNDARY_BT2020) {
        vec3 source_rgb = RD_PSY30_BT709_TO_BT2020 * exposed_input;
        if (all(lessThanEqual(source_rgb, vec3(0.0))) && !any(isinf(source_rgb))) {
            return vec3(0.0);
        }
        input_lms = rd_psy30_anchor_source_boundary_to_yf(source_rgb, RD_PSY30_BT2020_TO_LMS,
                                                          RD_PSY30_BT2020_SOURCE_YF_WEIGHTS);
    } else if (source_boundary == RD_PSY30_SOURCE_BOUNDARY_AP1) {
        vec3 source_rgb = RD_PSY30_BT709_TO_AP1 * exposed_input;
        if (all(lessThanEqual(source_rgb, vec3(0.0))) && !any(isinf(source_rgb))) {
            return vec3(0.0);
        }
        input_lms = rd_psy30_anchor_source_boundary_to_yf(source_rgb, RD_PSY30_AP1_TO_LMS,
                                                          RD_PSY30_AP1_SOURCE_YF_WEIGHTS);
    } else {
        if (all(lessThanEqual(exposed_input, vec3(0.0))) && !any(isinf(exposed_input))) {
            return vec3(0.0);
        }
        input_lms = rd_psy30_anchor_source_boundary_to_yf(exposed_input, RD_PSY30_BT709_TO_LMS,
                                                          RD_PSY30_BT709_SOURCE_YF_WEIGHTS);
    }

    vec3 anchor_in_lms = RD_PSY30_BT709_TO_LMS * adaptive_state;
    vec3 anchor_out_lms;
    if (all(equal(background_state, adaptive_state))) {
        anchor_out_lms = anchor_in_lms;
    } else {
        anchor_out_lms = RD_PSY30_BT709_TO_LMS * background_state;
    }

    // Observer-basis controls: scalar physiological-Yf grading, then LMS purity.
    vec3 graded_lms;
    if (highlights != 1.0 || shadows != 1.0) {
        graded_lms = abs(input_lms);
        float graded_yf = rd_psy30_yf(graded_lms);
        float adapted_anchor_yf = rd_psy30_yf(anchor_in_lms);
        float graded_yf_out = rd_psy30_highlights_scalar(graded_yf, highlights, adapted_anchor_yf);
        graded_yf_out = rd_psy30_shadows_scalar(graded_yf_out, shadows, adapted_anchor_yf);
        graded_lms *= graded_yf_out / graded_yf;
        graded_lms = rd_psy30_copysign(graded_lms, input_lms);
    } else {
        graded_lms = input_lms;
    }

    float response_power = contrast * cone_response_exponent;
    float purity_delta = purity_scale / contrast;
    vec3 response_input_q =
        rd_psy30_apply_adaptive_relative_purity(graded_lms, anchor_in_lms, purity_delta);

    // Positive finite-G response and Mean-A2 direction authoring.
    float target_rgb_peak = peak_value;
    float response_h;
    if (compression == RD_PSY30_AUTO_COMPRESSION_SENTINEL) {
        response_h = rd_psy30_auto_compression_power(
            rd_psy30_yf(anchor_out_lms),
            rd_psy30_target_neutral_yf_limit(target_rgb_peak, anchor_in_lms,
                                             gamut_compression_mode));
    } else {
        response_h = compression;
    }

    if (any(lessThanEqual(response_input_q, vec3(0.0)))) {
        // An actual zero or negative cone component selects the signed path.
        vec3 fallback_lms = rd_psy30_linear_a2_fallback(
            response_input_q, anchor_in_lms, anchor_out_lms, gamut_compression_mode,
            target_rgb_peak, response_power, response_h, gamut_compression);
        return RD_PSY30_LMS_TO_BT709 * fallback_lms;
    }
    float response_yf;
    vec3 desired_coord = rd_psy30_mean_a2_response_from_positive_q(
        response_input_q, anchor_out_lms / (RD_PSY30_D65_WHITE_LMS * target_rgb_peak),
        response_power, response_h, response_yf);

    // Device mapping: exact fixed-direction projection into the selected RGB cube.
    vec3 selected_coord = desired_coord;
    if (gamut_compression != 0.0) {
        vec3 solved_coord =
            rd_psy30_yf_ceiling_solve(desired_coord, response_yf, gamut_compression_mode);
        selected_coord = gamut_compression == 1.0
                             ? solved_coord
                             : mix(desired_coord, solved_coord, gamut_compression);
    }

    // Direct inverse scaled-A2/Yf basis to linear BT.709.
    float output_a = (2.0 * selected_coord.y + 3.0 * RD_PSY30_D65_ALPHA_DELTA * selected_coord.x -
                      selected_coord.z) /
                     6.0;
    return peak_value * (output_a + selected_coord.x * RD_PSY30_BT709_D_RGB +
                         selected_coord.z * RD_PSY30_BT709_T_RGB);
}

#endif // BBPORT_PSYCHOV30_GLSL
