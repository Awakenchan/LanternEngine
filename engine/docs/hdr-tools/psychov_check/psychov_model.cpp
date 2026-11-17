// The tone mapper PsychoV of host_shaders/hdr/bb_composite_hdr.frag (+ 16) modelled on the CPU
// with RenoDX's own test30 (orig30, from run.sh), against Vanilla+ and PsychoV-30 alone (0.9.38.1's
// tone mapper: its scene path with the game's screen-blended bloom). Interiors (the game's curve
// k 2.30, c 0.46: mid grey at a scene 0.19475, log-log slope 1.1656 there), peak 1000 nits, white
// 203; the game's color matrix left out (identity). LanternEngine-NativePlusHDR's
// docs/TONEMAPPERS.md quotes these tables.
//  Vanilla+: the game's curve below 0.18, its tangent above, per channel; the bloom added;
//    CInfinityRollOff (0.18, 1.5) per channel to the peak.
//  PsychoV-30 alone: test30 on the scene, anchored at mid grey with the game's slope as its cone;
//    the bloom screen blended, clamped to 1.
//  PsychoV: the signal (signal()), the bloom added, test30 on the scene color of that signal, then
//    RenoDX's hue correction (OKLab) to Vanilla+'s output by the menu's Hue Correction. Vanilla+
//    Highlights 100 % with Hue Correction 100 % is 0.9.38.2's PsychoV.
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
using Batch = std::vector<glm::vec4>;
static Batch run(PCData pc, Batch in) {
    size_t n = in.size();
    while (in.size() % 64) in.push_back({0.18f, 0.18f, 0.18f, 1});
    Batch out(in.size()); g_pc = pc;
    auto* sh = orig30_iface()->construct(); g_in = in.data(); g_out = out.data();
    glm::uvec3 groups(in.size() / 64, 1, 1), id(0, 0, 0);
    spirv_cross_set_builtin(sh, SPIRV_CROSS_BUILTIN_NUM_WORK_GROUPS, &groups, sizeof(groups));
    spirv_cross_set_builtin(sh, SPIRV_CROSS_BUILTIN_WORK_GROUP_ID, &id, sizeof(id));
    for (unsigned g = 0; g < groups.x; ++g) { id.x = g; orig30_iface()->invoke(sh); }
    orig30_iface()->destruct(sh); out.resize(n); return out;
}
using V = glm::dvec3;
const double K = 2.30, C = 0.46, P = 1000.0 / 203.0, N = 203.0, GI = 0.19475, GS = 1.1656;
const V Y709(0.2126390059, 0.7151686788, 0.0721923154);
double game_f(double v) { double y = 1 - std::exp(-K * v), a = C * (y - 1) + 1; return y * a * a; }
double vplus_f(double v) {
    if (v < 0.18) return game_f(std::max(v, 6.1e-5));
    double e = std::exp(-0.18 * K), y = (1 - e) + (v - 0.18) * K * e, a = C * (y - 1) + 1; return y * a * a;
}
double cinf(double x) { double d = std::max(x - 0.18, 0.0); if (d <= 0) return x; double r = P - 0.18, ip = r / (1.5 * d); return r / (1.5 * ip + std::exp2(-ip)) + x - d; }
V per(V c, double (*f)(double)) { return {f(c.x), f(c.y), f(c.z)}; }
// PsychoV's signal on the luminance y: PsychoV-30's v = 0.18 (y / grey_in)^cone below grey_in;
// above it, that v and Vanilla+'s extended curve (scaled to meet at grey_in) mixed in the log by
// the menu's Vanilla+ Highlights `t` (0..1).
double signal(double y, double t) {
    const double v = 0.18 * std::pow(y / GI, GS);
    if (y < GI || t <= 0) return v;
    const double vp = vplus_f(y) * 0.18 / vplus_f(GI);
    return std::exp((1 - t) * std::log(v) + t * std::log(vp));
}
V to2020(V c) { return {0.6274039*c.x+0.3292830*c.y+0.0433131*c.z, 0.0690973*c.x+0.9195404*c.y+0.0113623*c.z, 0.0163914*c.x+0.0880133*c.y+0.8955953*c.z}; }
double nits(V c) { V b = to2020(c); return (0.2627002*b.x+0.6779981*b.y+0.0593017*b.z) * N; }
double top(V c) { V b = to2020(c); return std::max(b.x, std::max(b.y, b.z)) * N; }
V oklab(V c) {
    V lms(0.4122214615*c.x+0.5363325477*c.y+0.0514459945*c.z, 0.2119034976*c.x+0.6806995273*c.y+0.1073969603*c.z, 0.0883024633*c.x+0.2817188501*c.y+0.6299787164*c.z);
    for (int i = 0; i < 3; ++i) lms[i] = std::cbrt(lms[i]);
    return {0.2104542553*lms.x+0.7936177850*lms.y-0.0040720468*lms.z, 1.9779984951*lms.x-2.4285922050*lms.y+0.4505937099*lms.z, 0.0259040371*lms.x+0.7827717662*lms.y-0.8086757660*lms.z};
}
V from_oklab(V l) {
    V lms(l.x+0.3963377774*l.y+0.2158037573*l.z, l.x-0.1055613458*l.y-0.0638541728*l.z, l.x-0.0894841775*l.y-1.2914855480*l.z);
    lms = lms * lms * lms;
    return {4.0767416621*lms.x-3.3077115913*lms.y+0.2309699292*lms.z, -1.2684380046*lms.x+2.6097574011*lms.y-0.3413193965*lms.z, -0.0041960863*lms.x-0.7034186147*lms.y+1.7076147010*lms.z};
}
double hue(V c) { V l = oklab(c); double h = std::atan2(l.z, l.y) * 57.2957795; return h < 0 ? h + 360 : h; }
double chroma(V c) { V l = oklab(c); return std::hypot(l.y, l.z) / std::max(l.x, 1e-9); }
// RenoDX's HueOKLab: (a, b) moved towards the reference's by `strength`, then the color's chroma.
V correct_hue(V c, V reference, double strength) {
    if (strength <= 0) return c;
    V a = oklab(c), b = oklab(reference);
    const double ca = std::hypot(a.y, a.z);
    a.y += (b.y - a.y) * strength;
    a.z += (b.z - a.z) * strength;
    const double cb = std::hypot(a.y, a.z);
    if (cb > 0) { a.y *= ca / cb; a.z *= ca / cb; }
    return from_oklab(a);
}
// test30 as the composite calls it: BT.2020, BT.709 source boundary, the menu's sliders neutral,
// the game's slope as the cone, anchored at its mid grey.
PCData now() {
    PCData p{(float)P, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.18f, 0.18f, 1, 1, 1, 0};
    p.cone = (float)GS;
    p.adaptive = (float)GI;
    return p;
}
V vanilla_plus(V x, V g) { return per(per(glm::max(x, V(6.1e-5)), vplus_f) + g, cinf); }
// PsychoV-30 alone (0.9.38.1): test30 on the scene, the bloom screen blended, clamped to 1.
std::vector<V> v30(const std::vector<V>& xs, const std::vector<V>& gs) {
    Batch in;
    for (auto& x : xs) { V c = glm::max(x, V(6.1e-5)); in.push_back({(float)c.x, (float)c.y, (float)c.z, 1}); }
    Batch r = run(now(), in);
    std::vector<V> out;
    for (size_t i = 0; i < xs.size(); ++i) {
        V t(r[i].x, r[i].y, r[i].z), glow = glm::clamp(gs[i], 0.0, 1.0);
        out.push_back(t + glow * (V(1) - glm::min(t, V(1))));
    }
    return out;
}
// PsychoV with Vanilla+ Highlights `t` and Hue Correction `h` (0..1).
std::vector<V> psychov(const std::vector<V>& xs, const std::vector<V>& gs, double t, double h) {
    Batch in;
    for (size_t i = 0; i < xs.size(); ++i) {
        V x = glm::max(xs[i], V(6.1e-5));
        double y = glm::dot(x, Y709);
        V s = x * (signal(y, t) / y) + gs[i];
        double ys = glm::dot(s, Y709);
        V eq = s * (GI * std::pow(ys / 0.18, 1 / GS) / ys);
        in.push_back({(float)eq.x, (float)eq.y, (float)eq.z, 1});
    }
    Batch r = run(now(), in);
    std::vector<V> out;
    for (size_t i = 0; i < xs.size(); ++i)
        out.push_back(correct_hue(V(r[i].x, r[i].y, r[i].z), vanilla_plus(xs[i], gs[i]), h));
    return out;
}
int main() {
    const double xs[] = {0.002, 0.01, 0.05, 0.1, 0.18, 0.3, 0.5, 1, 2, 4, 8, 16, 32, 64};
    std::vector<V> g, none;
    for (double x : xs) { g.push_back(V(x)); none.push_back(V(0)); }
    const double ts[] = {0, 0.25, 0.5, 0.75, 1};
    std::vector<V> pv[5];
    for (int k = 0; k < 5; ++k) pv[k] = psychov(g, none, ts[k], 0);
    auto a30 = v30(g, none);
    printf("== Grey (nits): Vanilla+, PsychoV-30 alone, PsychoV at Vanilla+ Highlights 0 / 25 / 50 / 75 / 100 %%\n");
    printf("  %6s | %9s | %10s | %8s | %8s | %8s | %8s | %8s\n", "scene", "Vanilla+", "PsychoV-30", "VH 0", "VH 25",
           "VH 50", "VH 75", "VH 100");
    for (size_t i = 0; i < g.size(); ++i) {
        printf("  %6.3f | %9.2f | %10.2f", xs[i], nits(vanilla_plus(g[i], V(0))), nits(a30[i]));
        for (int k = 0; k < 5; ++k) printf(" | %8.2f", nits(pv[k][i]));
        printf("\n");
    }
    printf("\n== Detail at the top: nits gained per doubling of the scene's light\n");
    for (size_t j = 9; j + 1 < g.size(); ++j) {
        printf("  %4.0f -> %4.0f | %9.1f | %10.1f", xs[j], xs[j + 1],
               nits(vanilla_plus(g[j + 1], V(0))) - nits(vanilla_plus(g[j], V(0))), nits(a30[j + 1]) - nits(a30[j]));
        for (int k = 0; k < 5; ++k) printf(" | %8.1f", nits(pv[k][j + 1]) - nits(pv[k][j]));
        printf("\n");
    }
    const struct { const char* n; V c; } cols[] = {{"flame orange", {1, .45, .08}}, {"lantern amber", {1, .62, .25}},
                                                   {"blood red", {1, .1, .05}}, {"violet glass", {.45, .3, 1}}};
    printf("\n== Colors: nits / largest BT.2020 channel (nits) / OKLab hue (degrees) / chroma per lightness\n");
    printf("  %-20s | %-24s | %-24s | %-24s | %-24s\n", "", "Vanilla+", "PsychoV-30 alone", "PsychoV (VH 0, hue 0)",
           "PsychoV (VH 100, hue 100)");
    for (auto& c : cols) {
        std::vector<V> in, z;
        const double lv[] = {0.5, 1, 2, 5, 20};
        for (double l : lv) { in.push_back(c.c * l); z.push_back(V(0)); }
        auto c30 = v30(in, z), p0 = psychov(in, z, 0, 0), p1 = psychov(in, z, 1, 1);
        for (size_t i = 0; i < in.size(); ++i) {
            printf("  %-13s x %4.1f", c.n, lv[i]);
            for (V v : {vanilla_plus(in[i], V(0)), c30[i], p0[i], p1[i]})
                printf(" | %5.0f %5.0f %4.0f %.3f", nits(v), top(v), hue(v), chroma(v));
            printf("\n");
        }
    }
    const struct { const char* n; V x, g; } bl[] = {{"dark spire, strong orange glow", {0.02, 0.012, 0.006}, {2.0, 0.9, 0.25}},
                                                    {"dark spire, mild orange glow", {0.02, 0.012, 0.006}, {0.8, 0.36, 0.1}},
                                                    {"grey stone, mild glow", {0.1, 0.1, 0.1}, {0.3, 0.14, 0.04}},
                                                    {"bright sky, mild glow", {1.0, 0.9, 0.8}, {0.5, 0.22, 0.06}}};
    printf("\n== Bloom: nits / saturation in BT.2020 / green to red / OKLab hue\n");
    std::vector<V> bx, bg;
    for (auto& b : bl) { bx.push_back(b.x); bg.push_back(b.g); }
    auto b30 = v30(bx, bg), b0 = psychov(bx, bg, 0, 0), b1 = psychov(bx, bg, 1, 1);
    for (size_t i = 0; i < bx.size(); ++i) {
        printf("  %-31s", bl[i].n);
        for (V v : {vanilla_plus(bx[i], bg[i]), b30[i], b0[i], b1[i]}) {
            V b = to2020(v);
            double mx = std::max(b.x, std::max(b.y, b.z)), mn = std::min(b.x, std::min(b.y, b.z));
            printf(" | %5.0f %.2f %.2f %4.0f", nits(v), mx > 0 ? 1 - mn / mx : 0, b.y / b.x, hue(v));
        }
        printf("\n");
    }
}
