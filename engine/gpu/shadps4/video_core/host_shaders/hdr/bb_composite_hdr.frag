// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native+: the tone mapped scene for any of the 1380 standard variants of Bloodborne's final
// composite (bb_tonemap_variants.h). The game draws its composite unchanged; this shader is drawn
// right after it, with the same inputs, into the same (RGBA16F) target, with additive blending
// and color writes only: frag_color0 is the HDR output minus the game's (both encoded as the game
// encodes, before its grain), so the target ends up with the HDR color plus the game's grain and
// alpha. Its interface is the game shader's: HdrChannel copies the variant's descriptor bindings,
// input locations and constant-buffer slot onto the decorations named bb_* here, and sets the
// variant's options as specialization constants (vk_hdr_channel.cpp). The game's math is
// hdr/bb_composite.glsl.

#version 450
#ifndef BB_HW_INTERP
#extension GL_EXT_fragment_shader_barycentric : require
#endif
#ifdef BB_CB_UBO
#extension GL_EXT_scalar_block_layout : require
#endif
#extension GL_GOOGLE_include_directive : require

#include "bb_inputs.glsl"

layout(constant_id = 10) const int BB_CURVE = 0;
layout(constant_id = 11) const bool BB_LUMINANCE = false;
layout(constant_id = 12) const int BB_PRE = 0;
layout(constant_id = 13) const bool BB_PRE_LUM = false;
layout(constant_id = 14) const bool BB_COS4 = false;
layout(constant_id = 15) const bool BB_VIG888 = false;
layout(constant_id = 16) const bool BB_GAMMA = true;
layout(constant_id = 17) const bool BB_ADDITIVE = false;
layout(constant_id = 18) const uint BB_BUFFER_INDEX = 0u; // index of the constant buffer in buf_offsets
layout(constant_id = 20) const bool BB_GAIN = true;
layout(constant_id = 21) const bool BB_FLOOR_ON = true;
layout(constant_id = 22) const bool BB_SCENE_MAT = false;
layout(constant_id = 23) const bool BB_GLOW_MAT = false;
layout(constant_id = 24) const bool BB_GRADE = true;

// The game shader's constant buffer (decorations copied from the variant): a storage buffer from
// the legacy shader compiler (ssbo_N); BB_CB_UBO, a uniform buffer of 16384 dwords with a 4-byte
// stride from Lantern's native one (ubo_N, Profile::uniform_dword_layout).
#ifdef BB_CB_UBO
layout(set = 0, binding = 0, scalar) uniform bb_ssbo_block {
    uint data[16384];
} bb_ssbo;
#else
layout(set = 0, binding = 0, std430) readonly buffer bb_ssbo_block {
    uint data[];
} bb_ssbo;
#endif

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
    // bb_hdr.w: the menu's Bloom (bits plus one, 0: the game's). bb_hdr2a: x the display's peak
    // (game white units), y exposure, z highlights, w shadows; bb_hdr2b: x contrast,
    // y saturation (each a float's bits plus one, 0: not set), z the target gamut (PsychoV-30's
    // gamut_compression_mode: 0 BT.709, 1 BT.2020) + 4 for Vanilla+ or 16 for PsychoV + PsychoV's
    // Vanilla+ Highlights in percent from bit 8, its Vanilla+ Shadows from bit 16 and Keep Deeper
    // Blacks in bit 23, plus one; w PsychoV's Hue Correction, 0..1.
    layout(offset = 144) uvec4 bb_hdr2a;
    layout(offset = 160) uvec4 bb_hdr2b;
} push_data;

layout(set = 0, binding = 1) uniform texture2D bb_img_scene;
layout(set = 0, binding = 2) uniform texture2D bb_img_mult;
layout(set = 0, binding = 3) uniform texture2D bb_img_bloom;
layout(set = 0, binding = 4) uniform sampler bb_samp_scene;
layout(set = 0, binding = 5) uniform sampler bb_samp_mult;
layout(set = 0, binding = 6) uniform sampler bb_samp_bloom;

// The game's inputs, per vertex or interpolated by the GPU (bb_inputs.glsl); their locations are
// the variant's (vk_hdr_channel.cpp).
BB_INPUT(0, bb_in_scene);
BB_INPUT(1, bb_in_mult);
BB_INPUT(2, bb_in_bloom);
BB_INPUT(3, bb_in_vig);

layout(location = 0) out vec4 frag_color0;

uint buf_off;

float bb_c(uint word) {
    return uintBitsToFloat(bb_ssbo.data[word + buf_off]);
}

#include "bb_composite.glsl"
#include "psychov30.glsl"

float bb_bits(uint bits, float fallback) {
    return bits == 0u ? fallback : uintBitsToFloat(bits - 1u);
}

// The chain's gamma encoding (`power`, the game's 1/2.2) with the sign kept for channels below 0
// (BT.2020 colors outside BT.709): exactly the unsigned encoding for the others.
vec3 bb_encode_signed(vec3 v, float power) {
    vec3 positive = exp2(power * log2(max(v, 0.0) + BB_EPS));
    vec3 negative = exp2(power * log2(max(-v, 0.0) + BB_EPS)) - exp2(power * log2(BB_EPS));
    return positive - negative;
}

// The value plus one in `word` (0: not set, `fallback`).
int bb_choice(uint word, int fallback) {
    return word == 0u ? fallback : int(word - 1u);
}

// The display's peak in units of the game's white (bb_hdr2a.x).
float bb_peak() {
    return max(bb_bits(push_data.bb_hdr2a.x, 1.0), 1.0);
}

// PsychoV-30 alone on the scene's linear color, in place of the game's curve, to the display's
// peak (the final pass then only encodes): PsychoV where the composite has no curve to extend
// (curve 3). Anchored where the game's curve gives mid grey (0.18), with the game's log-log slope there (x V'(x) / V(x)) as
// cone_response_exponent, as RenoDX's mods pass a vanilla slope (cp2077, orderofthesinkingstar):
// test30's cone power is contrast * cone_response_exponent but its purity purity_scale /
// contrast, so the game's slope passed as contrast also took purity down (by 1 / 1.05 to
// 1 / 1.17 in the scenes measured). The menu's exposure, highlights, shadows, contrast and
// saturation are PsychoV-30's own parameters (neutral at 1), its target gamut BT.2020, as
// RenoDX's HDR mods.
float bb_vanilla_plus_f(int curve, float v);

// The anchor (the scene's mid grey for the game's curve) and the cone (the game's log-log slope
// there) PsychoV-30 takes; false without a curve to read them from.
bool bb_psychov_anchor(BbComposite o, out float grey_in, out float cone) {
    float slope;
    grey_in = 0.18;
    cone = 1.0;
    if (!bb_knee(o.curve, 0.18, grey_in, slope)) {
        return false;
    }
    cone = clamp(grey_in * slope / 0.18, 0.3, 3.0);
    return true;
}

// PsychoV-30 with the menu's parameters on a scene color, anchored at grey_in with that cone.
vec3 bb_psychov(vec3 x, float grey_in, float game_contrast) {
    float peak = bb_peak();
    float exposure = bb_bits(push_data.bb_hdr2a.y, 1.0);
    float highlights = bb_bits(push_data.bb_hdr2a.z, 1.0);
    float shadows = bb_bits(push_data.bb_hdr2a.w, 1.0);
    float contrast = bb_bits(push_data.bb_hdr2b.x, 1.0);
    float purity = bb_bits(push_data.bb_hdr2b.y, 1.0);
    // bb_hdr2b.z: the gamut (1 BT.2020) plus the tone mapper's bits, + 1. Gamut compression and
    // compression: RenoDX's defaults, 1 and 1.
    int gamut = clamp(bb_choice(push_data.bb_hdr2b.z, 1) & 1, 0, 1);
    const float gamut_compression = 1.0, compression = 1.0;
    vec3 t = rd_psychov30(x, peak, exposure, highlights, shadows, max(contrast, 0.05), purity,
                          game_contrast, vec3(grey_in), vec3(0.18), gamut_compression, gamut,
                          compression, RD_PSY30_SOURCE_BOUNDARY_BT709);
    // BT.2020: a color outside BT.709 has BT.709 channels below 0. They are kept, signed, through
    // the 16-bit chain to the final pass, which converts to BT.2020 before it drops what is still
    // negative (as RenoDX's mods). BT.709: within the cube, as before.
    return gamut == 0 ? max(t, 0.0) : t;
}

bool bb_curve_psychov_scene(BbComposite o, vec3 x, out vec3 t) {
    float grey_in, cone;
    if (!bb_psychov_anchor(o, grey_in, cone)) {
        t = bb_curve(o, x);
        return false;
    }
    if (o.curve != 3) {
        x = max(x, BB_FLOOR); // PsychoV-30 needs a positive input
    }
    t = bb_psychov(x, grey_in, cone);
    return true;
}

// Tone mapper PsychoV (+ 16). PsychoV-30 with this anchor and cone maps a grey as a display map (mid
// grey to mid grey, cone 1) maps the signal v = 0.18 (x / grey_in)^cone: PsychoV-30's own curve.
// Above grey_in, the menu's Vanilla+ Highlights (`highlights`, 0..1) mixes v in the log with
// Vanilla+'s extended curve (the game's tangent from mid grey; scaled to 0.18 at grey_in, where the
// two meet), as the FromSoftware RenoDX mod feeds PsychoV a vanilla curve continued above mid grey:
// at 1 the highlights have Vanilla+'s brightness, PsychoV-30 still compressing them to the peak.
// On the luminance, the color's ratios kept, then in Vanilla+'s order: the bloom added to v (no
// screen blend, whose per-channel clamp turned an orange glow yellow-green), the game's color
// matrix, then PsychoV-30 on the scene color whose v that is (bb_psychov_vplus_rows), then the
// menu's Hue Correction towards Vanilla+'s hues (bb_psychov_vplus_graded).
const vec3 BB_Y709 = vec3(0.2126390059, 0.7151686788, 0.0721923154);

bool bb_curve_psychov_vplus(BbComposite o, vec3 x, float highlights, out vec3 v) {
    float grey_in, cone;
    if (!bb_psychov_anchor(o, grey_in, cone)) {
        v = bb_curve(o, x);
        return false;
    }
    x = max(x, BB_FLOOR);
    float y = dot(x, BB_Y709);
    float signal = 0.18 * pow(y / grey_in, cone);
    if (y > grey_in && highlights > 0.0) {
        float vanilla_plus =
            bb_vanilla_plus_f(o.curve, y) * (0.18 / bb_vanilla_plus_f(o.curve, grey_in));
        signal = exp(mix(log(signal), log(max(vanilla_plus, BB_FLOOR)), highlights));
    }
    v = x * (signal / y);
    return true;
}

vec3 bb_psychov_vplus_rows(BbComposite o, vec3 rows) {
    float grey_in, cone;
    bb_psychov_anchor(o, grey_in, cone);
    rows = max(rows, BB_FLOOR);
    float y = dot(rows, BB_Y709);
    return bb_psychov(rows * (grey_in * pow(y / 0.18, 1.0 / cone) / y), grey_in, cone);
}

// Tone mapper Vanilla+ (+ 4), as RenoDX's Bloodborne mod (souperman9) sets it: the game's curve
// below an input of mid grey (0.18), its tangent above it (curve 0: the tangent of 1 - e^-kv, the
// toe after it, as the mod), the bloom added (no screen blend), then RenoDX's user color grading
// and CInfinityRollOff per channel to the display's peak (bb_vanilla_plus_rows).
float bb_vanilla_plus_f(int curve, float v) {
    const float anchor = 0.18;
    if (v < anchor) {
        return bb_f(curve, v);
    }
    if (curve == 0) {
        float k = bb_c(832u);
        float e = exp2(-(anchor * k) * 1.44269502162933349609375);
        float y = (1.0 - e) + (v - anchor) * k * e;
        float c = bb_c(833u) * (y - 1.0) + 1.0;
        return y * c * c;
    }
    const float h = 1e-3;
    float slope = (bb_f(curve, anchor + h) - bb_f(curve, anchor - h)) / (2.0 * h);
    return bb_f(curve, anchor) + (v - anchor) * max(slope, 0.0);
}

vec3 bb_curve_vanilla_plus(BbComposite o, vec3 x) {
    if (o.curve == 3) {
        return max(x, 0.0);
    }
    x = max(x, o.floor ? BB_FLOOR : 0.0);
    if (o.luminance) {
        float l = bb_mean(x);
        return x * (bb_vanilla_plus_f(o.curve, l) / (l + BB_EPS));
    }
    return vec3(bb_vanilla_plus_f(o.curve, x.r), bb_vanilla_plus_f(o.curve, x.g),
                bb_vanilla_plus_f(o.curve, x.b));
}

// RenoDX's CInfinityRollOff (tonemap/c_infinity_rolloff.hlsl): the value below `anchor`,
// tending to `peak` above it.
vec3 bb_c_infinity_rolloff(vec3 c, float peak) {
    const float anchor = 0.18, strength = 1.5;
    float range = peak - anchor;
    vec3 t;
    for (int i = 0; i < 3; ++i) {
        float d = max(c[i] - anchor, 0.0);
        if (d <= 0.0) {
            t[i] = c[i];
            continue;
        }
        float inverse_position = range / (strength * d);
        t[i] = range / (strength * inverse_position + exp2(-inverse_position)) + c[i] - d;
    }
    return t;
}

vec3 bb_oklab(vec3 rgb) {
    vec3 lms = vec3(dot(rgb, vec3(0.4122214615, 0.5363325477, 0.0514459945)),
                    dot(rgb, vec3(0.2119034976, 0.6806995273, 0.1073969603)),
                    dot(rgb, vec3(0.0883024633, 0.2817188501, 0.6299787164)));
    lms = sign(lms) * pow(abs(lms), vec3(1.0 / 3.0));
    return vec3(dot(lms, vec3(0.2104542553, 0.7936177850, -0.0040720468)),
                dot(lms, vec3(1.9779984951, -2.4285922050, 0.4505937099)),
                dot(lms, vec3(0.0259040371, 0.7827717662, -0.8086757660)));
}

vec3 bb_oklab_to_rgb(vec3 lab) {
    vec3 lms = vec3(lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
                    lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
                    lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
    lms = lms * lms * lms;
    return vec3(dot(lms, vec3(4.0767416621, -3.3077115913, 0.2309699292)),
                dot(lms, vec3(-1.2684380046, 2.6097574011, -0.3413193965)),
                dot(lms, vec3(-0.0041960863, -0.7034186147, 1.7076147010)));
}

// RenoDX's user color grading as the Bloodborne mod applies it before its roll-off (exposure,
// contrast, highlights, shadows on the luminance around mid grey; the new lightness with the
// original hue in OKLab; saturation), then no color below 0 in BT.2020.
vec3 bb_renodx_user_grade(vec3 c, float exposure, float highlights, float shadows,
                          float contrast, float saturation) {
    vec3 exposed = c * exposure;
    float y = dot(exposed, vec3(0.2126390059, 0.7151686788, 0.0721923154));
    float scale = 0.0;
    if (y > 0.0) {
        float a = pow(y / 0.18, contrast);
        float b = mix(a, pow(a, highlights), clamp(a, 0.0, 1.0));
        scale = mix(pow(b, 2.0 - shadows), b, clamp(b, 0.0, 1.0)) * 0.18 / y;
    }
    vec3 graded = bb_oklab(exposed * scale);
    vec3 original = bb_oklab(c);
    float chroma = length(original.yz);
    vec2 ab = chroma > 0.0 ? original.yz * (length(graded.yz) / chroma) : graded.yz;
    vec3 rgb = bb_oklab_to_rgb(vec3(graded.x, ab * saturation));
    vec3 bt2020 = max(vec3(dot(rgb, vec3(0.6130974024, 0.3395231462, 0.0473794514)),
                           dot(rgb, vec3(0.0701937225, 0.9163538791, 0.0134523985)),
                           dot(rgb, vec3(0.0206155929, 0.1095697729, 0.8698146343))),
                      0.0);
    return vec3(dot(bt2020, vec3(1.7050509927, -0.6217921207, -0.0832588720)),
                dot(bt2020, vec3(-0.1302564175, 1.1408047366, -0.0105483191)),
                dot(bt2020, vec3(-0.0240033568, -0.1289689761, 1.1529723430)));
}

// Vanilla+ after the game's color matrix (rows in RGB order): the menu's grading, then the
// roll-off to the display's peak (in units of the game's white).
vec3 bb_vanilla_plus_rows(vec3 rows) {
    rows = max(rows, 0.0);
    float exposure = bb_bits(push_data.bb_hdr2a.y, 1.0);
    float highlights = bb_bits(push_data.bb_hdr2a.z, 1.0);
    float shadows = bb_bits(push_data.bb_hdr2a.w, 1.0);
    float contrast = bb_bits(push_data.bb_hdr2b.x, 1.0);
    float saturation = bb_bits(push_data.bb_hdr2b.y, 1.0);
    if (exposure != 1.0 || highlights != 1.0 || shadows != 1.0 || contrast != 1.0 ||
        saturation != 1.0) {
        rows = bb_renodx_user_grade(rows, exposure, highlights, shadows, contrast, saturation);
    }
    return bb_c_infinity_rolloff(rows, bb_peak());
}

// RenoDX's hue correction (colorcorrect.hlsl, HueOKLab): the color's lightness, its (a, b) moved
// towards the reference's by `strength`, then scaled back to the color's chroma.
vec3 bb_correct_hue(vec3 c, vec3 reference, float strength) {
    vec3 lab = bb_oklab(c);
    float chroma = length(lab.yz);
    vec2 ab = mix(lab.yz, bb_oklab(reference).yz, strength);
    float blended = length(ab);
    if (blended > 0.0) {
        lab.yz = ab * (chroma / blended);
    }
    return bb_oklab_to_rgb(lab);
}

// PsychoV's Hue Correction (the in-game menu's, 0..1; bb_hdr2b.w).
float bb_hue_correction() {
    return clamp(bb_bits(push_data.bb_hdr2b.w, 0.0), 0.0, 1.0);
}

// PsychoV's Vanilla+ Shadows (the in-game menu's, 0..1; bits 16 to 22 of bb_hdr2b.z).
float bb_vplus_shadows() {
    return clamp(float((bb_choice(push_data.bb_hdr2b.z, 1) >> 16) & 0x7F) / 100.0, 0.0, 1.0);
}

// Its Keep Deeper Blacks (bit 23): Vanilla+ Shadows only takes the shadows down.
bool bb_vplus_shadows_deeper() {
    return ((bb_choice(push_data.bb_hdr2b.z, 1) >> 23) & 1) != 0;
}

// PsychoV after the game's matrix (rows in RGB order), its hue moved towards
// Vanilla+'s by the menu's Hue Correction (0 by default): PsychoV-30's
// compression of each cone moves a bright orange towards yellow and a violet towards pink (as the
// eye sees a strong light); the game's curve, per channel, keeps the artists' hues. Lightness,
// chroma and peak stay PsychoV-30's; the reference is Vanilla+'s output for the same pixel (its
// curve and bloom, matrix, roll-off without the menu's grading, whose sliders are PsychoV-30's here).
// Below mid grey, the menu's Vanilla+ Shadows (0 by default) takes the luminance towards that
// reference's, the game's own curve there, in the log, the color's ratios kept (PsychoV's hue and
// chroma); with Keep Deeper Blacks only where the reference is darker (PsychoV's own blacks where
// they are deeper). It works on the output: PsychoV-30 is darker there than the signal it is given
// (its own toe), so taking the signal towards Vanilla+'s would only move part of the way.
vec3 bb_psychov_vplus_graded(BbComposite o, vec3 hdr_screen, vec3 vanilla_plus_screen) {
    vec3 t = bb_psychov_vplus_rows(o, bb_grade(o, hdr_screen));
    float hue = bb_hue_correction(), shadows = bb_vplus_shadows();
    if (hue > 0.0 || shadows > 0.0) {
        vec3 reference =
            bb_c_infinity_rolloff(max(bb_grade(o, vanilla_plus_screen), 0.0), bb_peak());
        if (hue > 0.0) {
            t = bb_correct_hue(t, reference, hue);
        }
        float y = dot(t, BB_Y709), y_reference = dot(reference, BB_Y709);
        if (shadows > 0.0 && y > BB_FLOOR && y_reference > BB_FLOOR) {
            float weight = shadows * (1.0 - smoothstep(0.09, 0.18, max(y, y_reference)));
            float ratio = y_reference / y;
            t *= exp2(weight * log2(bb_vplus_shadows_deeper() ? min(ratio, 1.0) : ratio));
        }
    }
    return (bb_choice(push_data.bb_hdr2b.z, 1) & 1) == 0 ? max(t, 0.0) : t;
}

#ifndef BB_HW_INTERP
// The game's interpolation, as recompiled: attribute with the barycentrics.
vec2 interp(vec4 a0, vec4 a1, vec4 a2) {
    vec3 b = gl_BaryCoordEXT;
    return vec2(fma(a2.x - a0.x, b.z, fma(a1.x - a0.x, b.y, a0.x)),
                fma(a2.y - a0.y, b.z, fma(a1.y - a0.y, b.y, a0.y)));
}
#define BB_IN_UV(NAME) interp(NAME[0], NAME[1], NAME[2])
#else
#define BB_IN_UV(NAME) BB_UV(NAME)
#endif

void main() {
    // buf_offsets holds one byte per buffer binding, as the recompiler reads it:
    // buf_offsets0 = buffers 0..15, buf_offsets1 = 16..31.
    uvec4 offsets = BB_BUFFER_INDEX < 16u ? push_data.buf_offsets0 : push_data.buf_offsets1;
    uint slot = BB_BUFFER_INDEX & 15u;
    buf_off = bitfieldExtract(offsets[slot >> 2u], int((slot & 3u) * 8u), 8) >> 2u;

    BbComposite o = BbComposite(BB_CURVE, BB_LUMINANCE, BB_PRE, BB_PRE_LUM, BB_COS4, BB_VIG888,
                                BB_GAMMA, BB_ADDITIVE, BB_GAIN, BB_FLOOR_ON, BB_SCENE_MAT,
                                BB_GLOW_MAT, BB_GRADE);

    vec4 scene = textureLod(sampler2D(bb_img_scene, bb_samp_scene), BB_IN_UV(bb_in_scene), 0.0);
    vec3 mult = texture(sampler2D(bb_img_mult, bb_samp_mult), BB_IN_UV(bb_in_mult)).rgb;
    vec3 bloom = textureLod(sampler2D(bb_img_bloom, bb_samp_bloom), BB_IN_UV(bb_in_bloom), 0.0).rgb;
    vec2 vig_uv = (BB_COS4 || BB_VIG888) ? BB_IN_UV(bb_in_vig) : vec2(0.0);
    // The game's output, and the same with the tone mapper's curve.
    vec3 ms = bb_scene_tint(mult);
    vec3 x = bb_exposed(o, scene, ms, vig_uv);
    vec3 base = bb_bloom_base(mult);
    vec3 sdr_t = bb_curve(o, x);
    vec3 hdr_t;
    int tone_mapper = bb_choice(push_data.bb_hdr2b.z, 1);
    bool vanilla_plus = (tone_mapper & 4) != 0;
    // PsychoV (16), its Vanilla+ Highlights in percent from bit 8.
    bool psychov_vplus = (tone_mapper & 16) != 0 && o.curve != 3;
    float psychov_highlights = clamp(float((tone_mapper >> 8) & 0x7F) / 100.0, 0.0, 1.0);
    // Vanilla+ and PsychoV add the bloom to their curve, before the game's matrix.
    bool added_bloom = vanilla_plus || psychov_vplus;
    bool has_hdr = false;
    if (!BB_ADDITIVE && vanilla_plus) {
        hdr_t = bb_curve_vanilla_plus(o, x);
        has_hdr = true;
    } else if (!BB_ADDITIVE && psychov_vplus) {
        has_hdr = bb_curve_psychov_vplus(o, x, psychov_highlights, hdr_t);
    } else if (!BB_ADDITIVE) {
        has_hdr = bb_curve_psychov_scene(o, x, hdr_t);
    }
    vec3 g = bb_glow(o, base, bloom, ms);
    vec3 glow = clamp(g, 0.0, 1.0);
    vec3 sdr_screen = BB_ADDITIVE ? x + g : sdr_t + glow - sdr_t * glow;
    // Screen blend on the SDR part only (above 1 it would subtract); Vanilla+ adds the bloom.
    vec3 hdr_screen = !has_hdr     ? sdr_screen
                      : added_bloom ? max(hdr_t + g, 0.0)
                                    : hdr_t + glow * (1.0 - min(hdr_t, 1.0));

    // Export order is rows 840, 844, 848; the game's target holds them as BGR (row 848 first),
    // hence .zyx.
    vec3 sdr_graded = bb_grade(o, sdr_screen).zyx;
    // The reference of Hue Correction and Vanilla+ Shadows: Vanilla+'s curve for the same pixel.
    bool vplus_reference =
        psychov_vplus && (bb_hue_correction() > 0.0 || bb_vplus_shadows() > 0.0);
    vec3 vplus_t = vplus_reference ? bb_curve_vanilla_plus(o, x) : vec3(0.0);
    vec3 hdr_graded = psychov_vplus  ? bb_psychov_vplus_graded(o, hdr_screen, max(vplus_t + g, 0.0)).zyx
                      : vanilla_plus ? bb_vanilla_plus_rows(bb_grade(o, hdr_screen)).zyx
                                     : bb_grade(o, hdr_screen).zyx;
    // The game's bloom, live (the menu's Bloom; bb_hdr.w: the strength's bits plus one, 0 = the
    // game's): the target is the composite with its glow scaled, so the difference can also take
    // light away.
    uint bloom_bits = push_data.bb_hdr.w;
    float k = (bloom_bits == 0u || BB_ADDITIVE) ? 1.0
                                                : clamp(uintBitsToFloat(bloom_bits - 1u), 0.0, 4.0);
    bool scaled = abs(k - 1.0) > 1e-3;
    vec3 target = has_hdr ? hdr_graded : clamp(sdr_graded, 0.0, 1.0);
    if (scaled) {
        vec3 glow_k = clamp(g * k, 0.0, 1.0);
        vec3 sdr_screen_k = sdr_t + glow_k - sdr_t * glow_k;
        vec3 hdr_screen_k = !has_hdr     ? sdr_screen_k
                            : added_bloom ? max(hdr_t + g * k, 0.0)
                                          : hdr_t + glow_k * (1.0 - min(hdr_t, 1.0));
        target = !has_hdr      ? clamp(bb_grade(o, sdr_screen_k).zyx, 0.0, 1.0)
                 : psychov_vplus ? bb_psychov_vplus_graded(o, hdr_screen_k,
                                                           max(vplus_t + g * k, 0.0)).zyx
                 : vanilla_plus ? bb_vanilla_plus_rows(bb_grade(o, hdr_screen_k)).zyx
                                : bb_grade(o, hdr_screen_k).zyx;
    }
    // The game's own output in the target (the delta goes over it), and the HDR one. A BT.2020
    // target leaves channels below 0: kept, signed (RGBA16F, additive blending).
    vec3 game_out = BB_GAMMA ? exp2(bb_c(808u) * log2(clamp(sdr_graded + BB_EPS, 0.0, 1.0)))
                             : clamp(sdr_graded, 0.0, 1.0);
    vec3 delta = vec3(0.0);
    if (has_hdr || scaled) {
        vec3 hdr_out = BB_GAMMA ? bb_encode_signed(target, bb_c(808u)) : target;
        delta = hdr_out - game_out;
    }
    // The tone mappers replace the game's curve, so the delta may also darken.
    frag_color0 = vec4(delta, 0.0);
    if (max(max(abs(delta.x), abs(delta.y)), abs(delta.z)) < 1.0 / 4096.0) {
        // Nothing to add (below a sixteenth of an 8-bit step, encoded): no blend, so no read and
        // write of the 16-bit target for this pixel (the shadows, most of a frame).
        discard;
    }
}
