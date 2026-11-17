// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport HDR: Bloodborne's final composite (bloom + tone curve + color matrix + gamma), all 1380
// standard variants of renderer_vulkan/bb_tonemap_variants.h, as Native+'s composite reads it
// (hdr/bb_composite_hdr.frag). Written from the game's code of every variant and checked against
// it.
//
// The includer defines float bb_c(uint word): the composite's constant buffer, by word.
//
// The game's math (x: the exposed scene, before the curve):
//   ms   = tint(mult; 752, 753, 756) * c754 + c755          scene tint (mult: 1x1 texture)
//   mb   = clamp(tint(mult; 744, 745, 748)) * c746 + c747   bloom base
//   x    = pre(scene) * c616 * vignettes * ms                (c616: 1 in variants without it)
//          or, scene matrix variants: (A (pre(scene) * ms) + a) * vignettes  (3x4, words 856..867)
//   T    = curve(x)                                         per channel or on the mean of RGB
//   g    = c617 * (mb + bloom * ms), or max(B (mb + bloom * ms) + b, 0)  (3x4, words 872..883)
//   U    = clamp(g);  S = T + U - T U   (screen blend; additive variants: S = x + g, no max)
//   out  = M S + offsets  (3x4, words 840..851; S itself in variants without it), then pow(clamp(out), c808) when gamma-encoded
// tint(t; s, b, sat) = mean + sat * (t * c[s] + c[b] - mean) around the mean of the scaled RGB.

#ifndef BBPORT_BB_COMPOSITE_GLSL
#define BBPORT_BB_COMPOSITE_GLSL

struct BbComposite {
    int curve;      // 0 exp, 1 extended Reinhard, 2 log, 3 none
    bool luminance; // curve on the mean of RGB
    int pre;        // pre-exposure: 0 none, 1 sqrt form (16-19), 2 exp form (8-11), 3 alpha (12-15)
    bool pre_lum;   // pre-exposure on the mean of RGB
    bool cos4;      // (1 - r^2)^4 vignette
    bool vig888;    // lens vignette (888-892)
    bool gamma;     // output encoded with c808
    bool additive;  // no curve, bloom added, unclamped (HDR output for a later pass)
    bool gain;      // scene gain c616 (else 1)
    bool floor;     // curve input raised to BB_FLOOR first
    bool scene_mat; // 3x4 matrix on the tinted scene (856..867), vignettes after it
    bool glow_mat;  // 3x4 matrix on the glow (872..883) instead of c617
    bool grade;     // color matrix 840..851 (else the screen blend is the output)
};

// A 3x4 matrix of the constant buffer (rows at base, base + 4, base + 8; offsets in column 3).
vec3 bb_mat34(uint base, vec3 v) {
    return vec3(bb_c(base) * v.r + bb_c(base + 1u) * v.g + bb_c(base + 2u) * v.b + bb_c(base + 3u),
                bb_c(base + 4u) * v.r + bb_c(base + 5u) * v.g + bb_c(base + 6u) * v.b + bb_c(base + 7u),
                bb_c(base + 8u) * v.r + bb_c(base + 9u) * v.g + bb_c(base + 10u) * v.b + bb_c(base + 11u));
}

const float BB_FLOOR = 6.103515625e-05;   // 2^-14: scene floor before the curves
const float BB_EPS = 2.384185791015625e-07; // 2^-22: added before log2 / rcp
const float BB_THIRD = 0.3333333432674407958984375;

float bb_mean(vec3 v) {
    return v.r * BB_THIRD + v.g * BB_THIRD + v.b * BB_THIRD;
}

vec3 bb_tint(vec3 t, uint scale, uint bias, uint saturation) {
    vec3 s = t * bb_c(scale) + bb_c(bias);
    float mean = bb_mean(s);
    return bb_c(saturation) * (s - mean) + mean;
}

vec3 bb_scene_tint(vec3 mult) {
    return bb_tint(mult, 752u, 753u, 756u) * bb_c(754u) + bb_c(755u);
}

vec3 bb_bloom_base(vec3 mult) {
    return clamp(bb_tint(mult, 744u, 745u, 748u), 0.0, 1.0) * bb_c(746u) + bb_c(747u);
}

float bb_vignettes(BbComposite o, vec2 uv) {
    float f = 1.0;
    float r2 = dot(uv, uv);
    if (o.cos4) {
        float a = clamp(1.0 - r2, 0.0, 1.0);
        a *= a;
        f *= a * a;
    }
    if (o.vig888) {
        float r = sqrt(r2);
        float a = clamp(bb_c(888u) * r + bb_c(889u), 0.0, 1.0);
        float s = max(min(bb_c(891u), 3.0 * a * a - 2.0 * a * a * a), 0.0);
        float d = bb_c(890u) * r;
        f *= s * exp2(bb_c(892u) * log2(1.0 / (d * d + 1.0)));
    }
    return f;
}

float bb_pre(int kind, float v) {
    if (kind == 1) {
        return bb_c(19u) * (v + sqrt(v * (v + bb_c(16u)) + 1.0) - 1.0);
    }
    return bb_c(11u) * (exp2(bb_c(8u) * v * 1.44269502162933349609375) - 1.0);
}

// The exposed scene x (before the curve).
vec3 bb_exposed(BbComposite o, vec4 scene, vec3 ms, vec2 vig_uv) {
    float gain = (o.gain ? bb_c(616u) : 1.0) * bb_vignettes(o, vig_uv);
    vec3 sc = scene.rgb;
    if (o.pre == 3) {
        float a = clamp(scene.a, 0.0, 1.0);
        sc = clamp(sc, 0.0, 1.0) * (bb_c(12u) * (bb_c(14u) * a * a + 1.0));
    } else if (o.pre != 0 && o.pre_lum) {
        float l = bb_mean(sc);
        sc *= bb_pre(o.pre, l) / (l + BB_EPS);
    } else if (o.pre != 0) {
        sc = vec3(bb_pre(o.pre, sc.r), bb_pre(o.pre, sc.g), bb_pre(o.pre, sc.b));
    }
    if (o.scene_mat) {
        return bb_mat34(856u, sc * ms) * gain;
    }
    return sc * gain * ms;
}

// The glow before its clamp (g above).
vec3 bb_glow(BbComposite o, vec3 base, vec3 bloom, vec3 ms) {
    vec3 g = base + bloom * ms;
    if (!o.glow_mat) {
        return bb_c(617u) * g;
    }
    g = bb_mat34(872u, g);
    return o.additive ? g : max(g, 0.0);
}

// The curve's function, unclamped (v >= BB_FLOOR).
float bb_f(int curve, float v) {
    if (curve == 0) {
        float y = 1.0 - exp2(-(v * bb_c(832u)) * 1.44269502162933349609375);
        float k = bb_c(833u) * (y - 1.0) + 1.0;
        return y * k * k;
    }
    if (curve == 1) {
        float a = bb_c(833u), b = bb_c(834u);
        float u = a * v;
        return (u + u * u * b) / (u + 1.0);
    }
    return bb_c(833u) * (log2(bb_c(834u) * v + 1.0) * 0.693147182464599609375);
}

// The game's curve: T in [0, 1].
vec3 bb_curve(BbComposite o, vec3 x) {
    if (o.curve == 3) {
        return clamp(x, 0.0, 1.0);
    }
    if (o.floor) {
        x = max(x, BB_FLOOR);
    }
    if (!o.luminance) {
        return clamp(vec3(bb_f(o.curve, x.r), bb_f(o.curve, x.g), bb_f(o.curve, x.b)), 0.0, 1.0);
    }
    float l = bb_mean(x);
    float t = bb_f(o.curve, l);
    if (o.curve != 1) {
        t = clamp(t, 0.0, 1.0); // the game clamps the log value, not the Reinhard one
    }
    return clamp(x * (t / (l + BB_EPS)), 0.0, 1.0);
}

// Where the curve reaches `target` (< 1): input and slope there. False if it does not.
bool bb_knee(int curve, float target, out float v, out float slope) {
    if (curve == 3) {
        v = target;
        slope = 1.0;
        return true;
    }
    if (curve == 0) {
        float k = bb_c(832u), c = bb_c(833u);
        float y = target;
        for (int i = 0; i < 8; ++i) { // Newton on y: y (c (y - 1) + 1)^2 = target
            float a = c * y - c + 1.0;
            y -= (y * a * a - target) / (a * a + 2.0 * c * y * a);
        }
        y = clamp(y, 0.0, 0.999999);
        float a = c * y - c + 1.0;
        v = -log(1.0 - y) / k;
        slope = (a * a + 2.0 * c * y * a) * k * (1.0 - y);
    } else if (curve == 1) {
        float a = bb_c(833u), b = bb_c(834u);
        // b u^2 + (1 - target) u - target = 0
        float u = b > 1e-6 ? (sqrt((1.0 - target) * (1.0 - target) + 4.0 * b * target) -
                              (1.0 - target)) / (2.0 * b)
                           : target / (1.0 - target);
        v = u / a;
        slope = a * ((1.0 + 2.0 * b * u) * (1.0 + u) - (u + b * u * u)) / ((1.0 + u) * (1.0 + u));
    } else {
        float a = bb_c(833u), b = bb_c(834u);
        v = (exp(target / a) - 1.0) / b;
        slope = a * b / (1.0 + b * v);
    }
    return v > 0.0 && slope > 0.0 && !isinf(v) && !isnan(v) && !isnan(slope);
}

// The game's color matrix, rows 840, 844 and 848 (+ offsets 843, 847, 851), in export order.
vec3 bb_grade(BbComposite o, vec3 s) {
    if (!o.grade) {
        return s;
    }
    return vec3(bb_c(840u) * s.r + bb_c(841u) * s.g + bb_c(842u) * s.b + bb_c(843u),
                bb_c(844u) * s.r + bb_c(845u) * s.g + bb_c(846u) * s.b + bb_c(847u),
                bb_c(848u) * s.r + bb_c(849u) * s.g + bb_c(850u) * s.b + bb_c(851u));
}

#endif // BBPORT_BB_COMPOSITE_GLSL
