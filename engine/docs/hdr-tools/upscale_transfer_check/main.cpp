// Exercises the compiled production GLSL functions (SPIRV-Cross' CPU backend), including FP16
// storage boundaries. No game, Vulkan device, screenshots, or reimplementation of the shader.
// The contract: the decode gives linear BT.2020 (>= 0) from the chain's signed gamma-2.2 BT.709,
// the encode the reverse; a color inside BT.2020 survives the round trip, sign included.
#include <spirv_cross/external_interface.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include "glob.h"

glm::vec4* g_in;
glm::vec4* g_out;

namespace {
using Vec = std::array<double, 3>;
// Rows of the shader's matrices (upscale_transfer.glsl).
constexpr double to2020[3][3] = {{0.6274040, 0.3292820, 0.0433136},
                                 {0.0690970, 0.9195400, 0.0113612},
                                 {0.0163916, 0.0880132, 0.8955950}};
constexpr double to709[3][3] = {{1.6604902958, -0.5876391058, -0.0728515982},
                                {-0.1245499701, 1.1328999220, -0.0083479642},
                                {-0.0181511189, -0.1005787239, 1.1187298782}};
constexpr double fp16_max = 65504.0;

Vec Mul(const double (&m)[3][3], const Vec& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}
double SignedPow(double v, double p) {
    return std::copysign(std::pow(std::abs(v), p), v);
}
// The decode's expected linear BT.2020.
Vec Decode(const Vec& encoded) {
    Vec linear;
    for (int c = 0; c < 3; ++c) linear[c] = std::clamp(SignedPow(encoded[c], 2.2), -fp16_max, fp16_max);
    Vec out = Mul(to2020, linear);
    for (auto& v : out) v = std::clamp(v, 0.0, fp16_max);
    return out;
}
// The encode's expected signal.
Vec Encode(const Vec& linear) {
    Vec clamped;
    for (int c = 0; c < 3; ++c) clamped[c] = std::clamp(linear[c], 0.0, fp16_max);
    Vec out = Mul(to709, clamped);
    for (auto& v : out) v = SignedPow(std::clamp(v, -fp16_max, fp16_max), 1.0 / 2.2);
    return out;
}
} // namespace

int main() {
    std::vector<glm::vec4> inputs;
    for (int i = 0; i < 512; ++i) {
        const float nits = std::pow(10.f, -5.f + 9.f * i / 511.f);
        const float encoded = std::pow(nits / 203.f, 1.f / 2.2f);
        inputs.push_back({encoded, encoded, encoded, 1.f});
        inputs.push_back({encoded, encoded * 0.13f, encoded * 0.02f, 1.f});
        inputs.push_back({encoded * 0.02f, encoded * 0.13f, encoded, 1.f});
    }
    inputs.push_back({0.f, 0.f, 0.f, 1.f});
    inputs.push_back({1.f, 1.f, 1.f, 1.f});
    inputs.push_back({0.5f, 0.5f, 0.5f, 1.f});
    inputs.push_back({-1.f, 0.f, 2.f, 1.f});
    inputs.push_back({65504.f, 1000.f, 100.f, 1.f});
    // Colors outside BT.709, inside BT.2020 (a channel below 0 in BT.709): the case the signed
    // chain exists for. BT.2020 green and red at a few levels, through the encoding.
    for (const double level : {0.05, 0.5, 2.0, 8.0}) {
        for (const Vec& bt2020 : {Vec{0.0, level, 0.0}, Vec{level, 0.0, 0.0}, Vec{0.0, 0.0, level}}) {
            const Vec bt709 = Mul(to709, bt2020);
            inputs.push_back({float(SignedPow(bt709[0], 1 / 2.2)), float(SignedPow(bt709[1], 1 / 2.2)),
                              float(SignedPow(bt709[2], 1 / 2.2)), 1.f});
        }
    }
    const auto tested = inputs.size();
    while (inputs.size() % 64) inputs.push_back({0.18f, 0.18f, 0.18f, 1.f});
    // A backend that accidentally executes zero invocations must fail, including black cases.
    std::vector<glm::vec4> outputs(inputs.size() * 6,
                                  glm::vec4(std::numeric_limits<float>::quiet_NaN()));
    g_in = inputs.data(); g_out = outputs.data();
    const auto* iface = spirv_cross_get_interface();
    auto* shader = iface->construct();
    glm::uvec3 groups(unsigned(inputs.size() / 64), 1, 1), id(0);
    spirv_cross_set_builtin(shader, SPIRV_CROSS_BUILTIN_NUM_WORK_GROUPS, &groups, sizeof(groups));
    spirv_cross_set_builtin(shader, SPIRV_CROSS_BUILTIN_WORK_GROUP_ID, &id, sizeof(id));
    for (unsigned group = 0; group < groups.x; ++group) {
        id.x = group;
        iface->invoke(shader);
    }
    iface->destruct(shader);

    int failures = 0;
    double worst_storage_nits = 0;
    const auto check = [&](bool ok, const char* what, size_t i, int channel) {
        if (!ok && failures++ < 12)
            std::printf("FAIL %s case %zu channel %d: in %g %g %g\n", what, i, channel, inputs[i].x,
                        inputs[i].y, inputs[i].z);
    };
    const auto close = [](double a, double b, double rel) {
        return std::abs(a - b) <= std::max(2e-6, std::abs(b) * rel);
    };
    for (size_t i = 0; i < tested; ++i) {
        const Vec signal{inputs[i][0], inputs[i][1], inputs[i][2]};
        const Vec light = Decode(signal);
        const Vec roundtrip = Encode(light);
        Vec half_light;
        for (int c = 0; c < 3; ++c) half_light[c] = light[c] * 0.5;
        const Vec half = Encode(half_light);
        // Errors are measured in light against the color's brightest channel: a channel near 0
        // beside a bright one carries that one's rounding (the primaries' conversion mixes them).
        const double top = std::max({light[0], light[1], light[2]});
        const auto light_close = [&](double signal_a, double signal_b, double rel) {
            return std::abs(SignedPow(signal_a, 2.2) - SignedPow(signal_b, 2.2)) <=
                   std::max(2e-7, std::max(top, std::abs(SignedPow(signal_b, 2.2))) * rel);
        };
        for (int channel = 0; channel < 3; ++channel) {
            for (int result = 0; result < 6; ++result)
                check(std::isfinite(outputs[6 * i + result][channel]), "finite", i, channel);
            check(std::abs(outputs[6 * i][channel] - light[channel]) <= std::max(2e-7, top * 3e-6),
                  "linear light", i, channel);
            check(light_close(outputs[6 * i + 2][channel], roundtrip[channel], 1e-5), "round trip",
                  i, channel);
            check(light_close(outputs[6 * i + 4][channel], half[channel], 1e-5),
                  "half-light averaging", i, channel);
        }
        // FP16 storage of the linear BT.2020 and of the signal: the light within 0.2 % of the
        // brightest channel (or a tiny absolute error near black), in nits at 203 nits per unit.
        const Vec stored{outputs[6 * i + 3][0], outputs[6 * i + 3][1], outputs[6 * i + 3][2]};
        const Vec stored_light = Decode(stored);
        for (int channel = 0; channel < 3; ++channel) {
            const double error = std::abs(stored_light[channel] - light[channel]) * 203.0;
            check(error <= std::max(3e-4, top * 203.0 * 0.002), "FP16 round trip", i, channel);
            if (top * 203.0 <= 1.0) worst_storage_nits = std::max(worst_storage_nits, error);
        }
        // A color inside BT.2020 comes back as it went in, sign included (not for the clipped
        // or out-of-range inputs: black, the extreme case and the one outside BT.2020).
        bool inside = true;
        for (int c = 0; c < 3; ++c) {
            Vec linear709{SignedPow(signal[0], 2.2), SignedPow(signal[1], 2.2), SignedPow(signal[2], 2.2)};
            inside = inside && Mul(to2020, linear709)[c] >= 0.0 && std::abs(signal[c]) < 100.0;
        }
        if (inside) {
            // In light (BT.709, linear): within 1e-5 of the color's brightest channel (well below
            // a thousandth of a nit at 203 nits per unit for the colors of a frame).
            double top = 0;
            for (int c = 0; c < 3; ++c) top = std::max(top, std::abs(SignedPow(signal[c], 2.2)));
            for (int channel = 0; channel < 3; ++channel)
                check(std::abs(SignedPow(outputs[6 * i + 2][channel], 2.2) - SignedPow(signal[channel], 2.2)) <=
                          1e-5 * std::max(top, 1e-3),
                      "identity inside BT.2020", i, channel);
        }
        if (i % 3 == 0 && i < 1536) {
            const auto& grey = outputs[6 * i + 3];
            check(std::abs(grey.x - grey.y) <= 2e-3f * std::abs(grey.y) + 1e-6f &&
                      std::abs(grey.y - grey.z) <= 2e-3f * std::abs(grey.y) + 1e-6f,
                  "neutral grey", i, 0);
        }
    }
    // A non-linear input of 0.5 is about 0.218 of the light. Averaging white and black in
    // linear light instead gives a signal about 0.730; the two contracts must not be confused.
    const size_t white_case = 1537;
    check(std::abs(outputs[6 * white_case + 4].x - 0.72974005f) < 2e-5f,
          "white/black energy", white_case, 0);
    // The signed cases (inside BT.2020, outside BT.709) keep a channel below 0 through the trip.
    for (size_t i = 1541; i < tested; ++i) {
        bool negative = false;
        for (int c = 0; c < 3; ++c) negative = negative || inputs[i][c] < 0.f;
        bool kept = false;
        for (int c = 0; c < 3; ++c) kept = kept || outputs[6 * i + 2][c] < 0.f;
        check(!negative || kept, "sign kept", i, 0);
    }
    std::printf("Mode 5 transfer: %zu inputs, BT.2020 linear-light/round-trip/FP16/neutral/energy/"
                "sign checks; max FP16 error below 1 nit %.6g nits; %d failures\n",
                tested, worst_storage_nits, failures);
    return failures ? 1 : 0;
}
