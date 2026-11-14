#include "spirv_cross/external_interface.h"
#define GLM_FORCE_SWIZZLE
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "glob.h"
glm::vec4* g_in; glm::vec4* g_out;
extern "C" const spirv_cross_interface* orig_iface(void);
extern "C" const spirv_cross_interface* port_iface(void);

static void run(const spirv_cross_interface* iface, std::vector<glm::vec4>& in, std::vector<glm::vec4>& out) {
    auto* sh = iface->construct();
    g_in = in.data(); g_out = out.data();
    glm::uvec3 groups(in.size() / 64, 1, 1), id(0, 0, 0);
    spirv_cross_set_builtin(sh, SPIRV_CROSS_BUILTIN_NUM_WORK_GROUPS, &groups, sizeof(groups));
    spirv_cross_set_builtin(sh, SPIRV_CROSS_BUILTIN_WORK_GROUP_ID, &id, sizeof(id));
    for (unsigned g = 0; g < groups.x; ++g) { id.x = g; iface->invoke(sh); }
    iface->destruct(sh);
}

int main() {
    // Grey, BT.709 colors (in gamut), BT.2020 primaries and mixes given in BT.709 (outside it:
    // negative channels), pastel and saturated, from dark to well above white.
    std::vector<glm::vec4> in;
    for (int i = 0; i < 32; ++i) { float v = std::pow(10.f, -4.f + 6.f * i / 31.f); in.push_back({v, v, v, 1}); }
    const float hues[][3] = {{1,0,0},{0,1,0},{0,0,1},{1,1,0},{0,1,1},{1,0,1},{1,.5f,0},{1,.2f,.1f},
                             {.2f,1,.3f},{.3f,.4f,1},{1,.9f,.7f},{.6f,.7f,1},
                             // BT.2020 red, green, blue, and partly outside BT.709
                             {1.6605f,-.1246f,-.0182f},{-.5876f,1.1329f,-.1006f},{-.0728f,-.0083f,1.1187f},
                             {1.2f,-.05f,-.02f},{-.1f,1,.2f},{.1f,-.08f,1},{.5f,.5f,-.2f},{1,-.3f,.4f}};
    for (auto& h : hues) for (int i = 0; i < 24; ++i) {
        float v = std::pow(10.f, -3.f + 5.f * i / 23.f); in.push_back({h[0]*v, h[1]*v, h[2]*v, 1}); }
    in.push_back({0, 0, 0, 1});
    while (in.size() % 64) in.push_back({0.18f, 0.18f, 0.18f, 1});
    std::vector<glm::vec4> a(in.size()), b(in.size());
    const char* names[] = {"compression scale", "compressed", "compressed then decompressed"};
    int bad = 0;
    for (int mode = 0; mode < 3; ++mode) {
        for (auto& v : in) v.w = float(mode);
        run(orig_iface(), in, a); run(port_iface(), in, b);
        double worst = 0; int worst_i = 0, nan_mismatch = 0;
        for (size_t i = 0; i < in.size(); ++i) {
            double ref = std::max(1e-3, (double)std::max({std::fabs(a[i].x), std::fabs(a[i].y), std::fabs(a[i].z)}));
            for (int k = 0; k < 3; ++k) {
                float x = a[i][k], y = b[i][k];
                if (std::isnan(x) != std::isnan(y)) { ++nan_mismatch; continue; }
                if (std::isnan(x)) continue;
                double rel = std::fabs(x - y) / ref;
                if (rel > worst) { worst = rel; worst_i = int(i); }
            }
        }
        bool ok = worst < 1e-3 && !nan_mismatch;
        bad += !ok;
        printf("%-30s max diff %.2e (of the largest channel)  NaN mismatches %d  %s\n", names[mode], worst,
               nan_mismatch, ok ? "OK" : "DIFF");
        if (!ok) printf("   input (%g %g %g): orig (%g %g %g) port (%g %g %g)\n", in[worst_i].x, in[worst_i].y,
                        in[worst_i].z, a[worst_i].x, a[worst_i].y, a[worst_i].z, b[worst_i].x, b[worst_i].y, b[worst_i].z);
    }
    // What the bridge does to colors outside BT.709 (RenoDX's own code): their compression scale
    // and the most negative channel left after it.
    for (auto& v : in) v.w = 0.f;
    run(orig_iface(), in, a);
    for (auto& v : in) v.w = 1.f;
    run(orig_iface(), in, b);
    int outside = 0, compressed = 0; float worst_neg = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        if (std::min({in[i].x, in[i].y, in[i].z}) < 0) {
            ++outside;
            compressed += a[i].x < 1.f;
            worst_neg = std::min({worst_neg, b[i].x, b[i].y, b[i].z});
        }
    }
    printf("\noutside BT.709: %d inputs, %d compressed (scale < 1); most negative channel after: %g\n",
           outside, compressed, worst_neg);
    return bad;
}
