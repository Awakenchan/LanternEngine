// Numerical check of host_shaders/hdr/psychov30.glsl against RenoDX's test30.hlsl (run.sh), then
// the measurements of the tone mapper audit (LanternEngine-NativePlusHDR's docs/TONEMAPPERS.md):
// the grey ramp and the detail at the top, saturated colors, finiteness, peak and gamut,
// continuity, the anchor, with the parameters bbport passes (bb_composite_hdr.frag). Returns the
// number of port configurations that differ.
#include "spirv_cross/external_interface.h"
#define GLM_FORCE_SWIZZLE
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "glob.h"
glm::vec4* g_in; glm::vec4* g_out; PCData g_pc;
extern "C" const spirv_cross_interface* orig30_iface(void);
extern "C" const spirv_cross_interface* port30_iface(void);

using Batch = std::vector<glm::vec4>;

static Batch run(const spirv_cross_interface* iface, const PCData& pc, Batch in) {
    while (in.size() % 64) in.push_back({0.18f, 0.18f, 0.18f, 1});
    Batch out(in.size());
    g_pc = pc;
    auto* sh = iface->construct();
    g_in = in.data(); g_out = out.data();
    glm::uvec3 groups(in.size() / 64, 1, 1), id(0, 0, 0);
    spirv_cross_set_builtin(sh, SPIRV_CROSS_BUILTIN_NUM_WORK_GROUPS, &groups, sizeof(groups));
    spirv_cross_set_builtin(sh, SPIRV_CROSS_BUILTIN_WORK_GROUP_ID, &id, sizeof(id));
    for (unsigned g = 0; g < groups.x; ++g) { id.x = g; iface->invoke(sh); }
    iface->destruct(sh);
    return out;
}

// The scenes' tone curves (measured in the game on 15/10/2026: exp curve k 2.30 c 0.46 in
// interiors, k 1.97 c 0.33 outdoors): where the curve gives 0.18 and its log-log slope there
// (bb_knee).
constexpr float kInteriorGrey = 0.19475f, kInteriorSlope = 1.1656f;
constexpr float kExteriorGrey = 0.18358f, kExteriorSlope = 1.0459f;
constexpr float kPeak = 1000.f / 203.f, kNits = 203.f;

static PCData base(float peak = kPeak) {
    // peak mode boundary h | exposure highlights shadows contrast | purity cone adaptive
    // background | gamut_compression (pad): RenoDX's defaults.
    return {peak, 1, 1, 1.f, 1, 1, 1, 1, 1, 1, 0.18f, 0.18f, 1, 1, 1, 0};
}
// bbport's PsychoV30 before 0.9.38.2 beta 2: the game's slope as contrast.
static PCData v30_old(float grey = kInteriorGrey, float slope = kInteriorSlope) {
    PCData p = base(); p.contrast = slope; p.adaptive = grey; return p;
}
// bbport's PsychoV30 now: the game's slope as cone_response_exponent.
static PCData v30_new(float grey = kInteriorGrey, float slope = kInteriorSlope) {
    PCData p = base(); p.cone = slope; p.adaptive = grey; return p;
}
static PCData with_highlights(float slider) {
    PCData p = v30_new(); p.highlights = slider / 50.f; return p;
}

static glm::vec3 to2020(glm::vec4 c) {
    return {0.6274039f * c.x + 0.3292830f * c.y + 0.0433131f * c.z,
            0.0690973f * c.x + 0.9195404f * c.y + 0.0113623f * c.z,
            0.0163914f * c.x + 0.0880133f * c.y + 0.8955953f * c.z};
}
static float lum(glm::vec4 c) { return 0.2126390f * c.x + 0.7151687f * c.y + 0.0721923f * c.z; }
static float max3(glm::vec3 v) { return std::max(v.x, std::max(v.y, v.z)); }
static float min3(glm::vec3 v) { return std::min(v.x, std::min(v.y, v.z)); }
// Saturation of a BT.2020 color: 1 - min / max (0 grey, 1 a primary, above 1 outside BT.2020).
static float sat2020(glm::vec4 c) { glm::vec3 b = to2020(c); return max3(b) > 0 ? 1 - min3(b) / max3(b) : 0; }

static int compare(const char* name, const PCData& pc, const Batch& in) {
    Batch a = run(orig30_iface(), pc, in), b = run(port30_iface(), pc, in);
    double worst = 0; size_t wi = 0; int nan_mismatch = 0;
    for (size_t i = 0; i < in.size(); ++i) for (int k = 0; k < 3; ++k) {
        float x = a[i][k], y = b[i][k];
        if (std::isnan(x) != std::isnan(y)) { ++nan_mismatch; continue; }
        if (std::isnan(x)) continue;
        // Relative to the color's own magnitude (a channel near 0 of a bright color is not
        // compared relative to itself).
        double scale = std::max({1e-3, (double)std::fabs(a[i].x), (double)std::fabs(a[i].y),
                                 (double)std::fabs(a[i].z)});
        double rel = std::fabs(x - y) / scale;
        if (rel > worst) { worst = rel; wi = i; }
    }
    bool ok = worst < 1e-3 && !nan_mismatch;
    printf("  %-58s max diff %.2e  NaN mismatches %d  %s\n", name, worst, nan_mismatch, ok ? "OK" : "DIFF");
    if (!ok) printf("     worst input (%g %g %g): orig (%g %g %g) port (%g %g %g)\n", in[wi].x, in[wi].y,
                    in[wi].z, a[wi].x, a[wi].y, a[wi].z, b[wi].x, b[wi].y, b[wi].z);
    return !ok;
}

int main() {
    // Neutral ramp (1e-4 .. 1e4), saturated and pastel colors at many levels, out-of-gamut
    // (negative components), extremes.
    Batch in;
    for (int i = 0; i < 64; ++i) { float v = std::pow(10.f, -4.f + 8.f * i / 63.f); in.push_back({v, v, v, 1}); }
    const float hues[][3] = {{1,0,0},{0,1,0},{0,0,1},{1,1,0},{0,1,1},{1,0,1},{1,.5f,0},{1,.45f,.08f},
                             {1,.62f,.25f},{1,.2f,.1f},{.2f,1,.3f},{.3f,.4f,1},{1,.9f,.7f},{.6f,.7f,1},
                             {1.2f,-.05f,-.02f},{-.1f,1,.2f},{.1f,-.08f,1},{.5f,.5f,-.2f}};
    for (auto& h : hues) for (int i = 0; i < 40; ++i) {
        float v = std::pow(10.f, -3.f + 7.f * i / 39.f); in.push_back({h[0]*v, h[1]*v, h[2]*v, 1}); }
    for (float v : {0.f, 1e-30f, 1e6f, 1e10f}) in.push_back({v, v, v, 1});
    in.push_back({1e6f, 0, 0, 1}); in.push_back({0, 0, 1e6f, 1}); in.push_back({1e-6f, 0, 0, 1});

    int bad = 0;
    printf("== Port against RenoDX b6b32f4 (max |port - orig| / the color's largest channel)\n");
    struct Cfg { const char* name; PCData pc; };
    PCData moved = v30_new(); moved.exposure = 1.3f; moved.highlights = 1.4f; moved.shadows = 0.6f;
    moved.contrast = 1.2f; moved.purity = 1.6f;
    PCData bt709 = v30_new(); bt709.mode = 0;
    PCData autoh = v30_new(); autoh.compression = 0;
    for (const Cfg& c : {Cfg{"RenoDX defaults", base()},
                         Cfg{"bbport before (slope as contrast)", v30_old()},
                         Cfg{"bbport now, interiors (slope as cone exponent)", v30_new()},
                         Cfg{"bbport now, outdoors", v30_new(kExteriorGrey, kExteriorSlope)},
                         Cfg{"bbport now, sliders moved", moved},
                         Cfg{"bbport now, BT.709 cube", bt709},
                         Cfg{"bbport now, auto compression", autoh},
                         Cfg{"bbport now, peak 400 nits", [] { PCData p = v30_new(); p.peak = 400.f / 203.f; return p; }()},
                         Cfg{"bbport now, peak 2991 nits", [] { PCData p = v30_new(); p.peak = 2991.f / 203.f; return p; }()}})
        bad += compare(c.name, c.pc, in);

    // The measurements, on RenoDX's own code (the original).
    const auto t30 = [&](const Batch& b, const PCData& p) { return run(orig30_iface(), p, b); };

    printf("\n== Grey ramp, interiors, peak 1000 nits (nits at 203 per unit)\n");
    printf("  %9s %9s %9s %9s %9s %9s\n", "scene", "before", "now", "auto h", "HL 60", "HL 70");
    Batch grey;
    for (float v : {0.01f, 0.05f, 0.1f, 0.18f, 0.3f, 0.5f, 1.f, 2.f, 4.f, 8.f, 16.f, 32.f, 64.f}) grey.push_back({v, v, v, 1});
    Batch go = t30(grey, v30_old()), gn = t30(grey, v30_new()), ga = t30(grey, autoh);
    Batch h60 = t30(grey, with_highlights(60)), h70 = t30(grey, with_highlights(70));
    for (size_t i = 0; i < 13; ++i)
        printf("  %9.3g %9.2f %9.2f %9.2f %9.2f %9.2f\n", grey[i].x, go[i].x * kNits, gn[i].x * kNits,
               ga[i].x * kNits, h60[i].x * kNits, h70[i].x * kNits);
    printf("  detail at the top (nits gained per doubling of the light):\n");
    for (size_t i = 8; i + 1 < 13; ++i)
        printf("  %4.0f -> %4.0f %9.1f %9.1f %9.1f %9.1f %9.1f\n", grey[i].x, grey[i + 1].x,
               (go[i + 1].x - go[i].x) * kNits, (gn[i + 1].x - gn[i].x) * kNits, (ga[i + 1].x - ga[i].x) * kNits,
               (h60[i + 1].x - h60[i].x) * kNits, (h70[i + 1].x - h70[i].x) * kNits);

    printf("\n== Saturated colors, interiors: luminance nits / BT.2020 saturation (1 - min/max)\n");
    const struct { const char* name; float c[3]; } cols[] = {
        {"flame orange", {1, .45f, .08f}}, {"lantern amber", {1, .62f, .25f}}, {"blood red", {1, .1f, .05f}},
        {"moon blue", {.25f, .45f, 1}}, {"pale skin", {1, .8f, .65f}}};
    for (auto& col : cols) {
        Batch b;
        const float levels[] = {0.1f, 0.5f, 1.f, 2.f, 5.f, 20.f};
        for (float l : levels) b.push_back({col.c[0] * l, col.c[1] * l, col.c[2] * l, 1});
        Batch o = t30(b, v30_old()), n = t30(b, v30_new()), h = t30(b, with_highlights(70));
        printf("  %s\n    %8s | %-15s | %-15s | %-15s | %s\n", col.name, "scene", "before", "now", "now, HL 70",
               "max BT.2020 ch. nits before/now/HL 70");
        for (size_t i = 0; i < 6; ++i)
            printf("    %8.2f | %6.1f n %5.3f s | %6.1f n %5.3f s | %6.1f n %5.3f s | %6.0f %6.0f %6.0f\n", levels[i],
                   lum(o[i]) * kNits, sat2020(o[i]), lum(n[i]) * kNits, sat2020(n[i]), lum(h[i]) * kNits,
                   sat2020(h[i]), max3(to2020(o[i])) * kNits, max3(to2020(n[i])) * kNits, max3(to2020(h[i])) * kNits);
    }

    printf("\n== Finite values, peak and BT.2020 gamut over all %zu inputs\n", in.size());
    for (const Cfg& c : {Cfg{"now", v30_new()}, Cfg{"now, sliders moved", moved}, Cfg{"now, HL 70", with_highlights(70)}}) {
        Batch o = t30(in, c.pc);
        int nonfinite = 0, above = 0, outside = 0; float worst_above = 0, worst_outside = 0;
        for (size_t i = 0; i < in.size(); ++i) {
            glm::vec3 b = to2020(o[i]);
            if (!std::isfinite(o[i].x) || !std::isfinite(o[i].y) || !std::isfinite(o[i].z)) { ++nonfinite; continue; }
            float m = max3(b) / c.pc.peak - 1, n = -min3(b) / std::max(max3(b), 1e-6f);
            if (m > 1e-3f) { ++above; worst_above = std::max(worst_above, m); }
            if (n > 1e-3f) { ++outside; worst_outside = std::max(worst_outside, n); }
        }
        printf("  %-20s non-finite %d | above the peak %d (worst +%.1f %%) | outside BT.2020 %d (worst %.1f %% of max)\n",
               c.name, nonfinite, above, 100 * worst_above, outside, 100 * worst_outside);
    }

    printf("\n== Continuity of the grey ramp (2000 steps, 1e-4 .. 1e4): largest step in log output\n");
    Batch fine;
    for (int i = 0; i < 2000; ++i) { float v = std::pow(10.f, -4.f + 8.f * i / 1999.f); fine.push_back({v, v, v, 1}); }
    for (const Cfg& c : {Cfg{"now", v30_new()}, Cfg{"now, HL 70", with_highlights(70)}}) {
        Batch o = t30(fine, c.pc);
        double worst = 0; int drops = 0;
        for (int i = 1; i < 2000; ++i) {
            worst = std::max(worst, std::fabs(std::log((double)o[i].y / o[i - 1].y)));
            drops += o[i].y < o[i - 1].y;
        }
        printf("  %-12s largest step %.4f (log; input step %.4f); decreasing steps %d\n", c.name, worst,
               std::log(10.0) * 8 / 1999, drops);
    }

    printf("\n== Anchor: output at the game's mid grey input, and the log-log slope there\n");
    for (const Cfg& c : {Cfg{"interiors", v30_new()}, Cfg{"outdoors", v30_new(kExteriorGrey, kExteriorSlope)}}) {
        const float g = c.pc.adaptive, h = 1.001f;
        Batch b = {{g, g, g, 1}, {g * h, g * h, g * h, 1}, {g / h, g / h, g / h, 1}};
        Batch o = t30(b, c.pc);
        printf("  %-10s V(%.5f) = %.5f (target 0.18), slope %.4f (the game's %.4f)\n", c.name, g, o[0].y,
               std::log(o[1].y / o[2].y) / std::log(h * h), c.pc.cone);
    }
    return bad;
}
