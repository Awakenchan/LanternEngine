// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GT7 tone mapping operator is adapted from Polyphony Digital's sample implementation, as
// ported to HLSL at github.com/aletschfirn/gt7tonemapper, under this licence:
//
// MIT License
//
// Copyright (c) 2025 Polyphony Digital Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include "shader_recompiler/native/gt7_tonemap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Native {
namespace {

using F32 = IR::F32;
using Vec3 = std::array<F32, 3>;
using Mat3 = std::array<std::array<float, 3>, 3>;

// SDR: GT7 maps to its 250 cd/m^2 paper white (2.5 in frame-buffer units of 100 cd/m^2), then
// scales that to 1.
constexpr float Target = 2.5f;
constexpr float SdrCorrection = 1.0f / Target;
// GTToneMappingCurveV2 as GT7 initialises it.
constexpr float Alpha = 0.25f, MidPoint = 0.538f, LinearSection = 0.444f, ToeStrength = 1.28f;
constexpr float BlendRatio = 0.6f, FadeStart = 0.98f, FadeEnd = 1.16f;
// ST 2084 (PQ).
constexpr float PqM1 = 0.1593017578125f, PqM2 = 78.84375f, PqC1 = 0.8359375f, PqC2 = 18.8515625f,
                PqC3 = 18.6875f;
// The scene value (k * x, the curve's own units) where GT7 meets the game's curve by default.
constexpr float MatchPoint = 0.3f;

constexpr Mat3 Rec709To2020{{{0.6274040f, 0.3292820f, 0.0433136f},
                             {0.0690970f, 0.9195400f, 0.0113612f},
                             {0.0163916f, 0.0880132f, 0.8955950f}}};
constexpr Mat3 Rec2020To709{{{1.6604910f, -0.5876411f, -0.0728499f},
                             {-0.1245505f, 1.1328999f, -0.0083494f},
                             {-0.0181508f, -0.1005789f, 1.1187297f}}};
constexpr Mat3 RgbToLms{{{1688.0f / 4096, 2146.0f / 4096, 262.0f / 4096},
                         {683.0f / 4096, 2951.0f / 4096, 462.0f / 4096},
                         {99.0f / 4096, 309.0f / 4096, 3688.0f / 4096}}};
constexpr Mat3 LmsToICtCp{{{2048.0f / 4096, 2048.0f / 4096, 0.0f},
                           {6610.0f / 4096, -13613.0f / 4096, 7003.0f / 4096},
                           {17933.0f / 4096, -17390.0f / 4096, -543.0f / 4096}}};
constexpr Mat3 ICtCpToLms{{{1.0f, 0.00860904f, 0.11103f},
                           {1.0f, -0.00860904f, -0.11103f},
                           {1.0f, 0.560031f, -0.320627f}}};
constexpr Mat3 LmsToRgb{{{3.43661f, -2.50645f, 0.0698454f},
                         {-0.79133f, 1.9836f, -0.192271f},
                         {-0.0259499f, -0.0989137f, 1.12486f}}};

/// Frame-buffer value (1 = 100 cd/m^2) to PQ, on the host.
double InversePq(double v) {
    const double ym = std::pow(v * 100.0 / 10000.0, double(PqM1));
    return std::exp2(PqM2 * (std::log2(PqC1 + PqC2 * ym) - std::log2(1.0 + PqC3 * ym)));
}

struct Emit {
    IR::IREmitter& ir;
    F32 F(const IR::Value& v) const { return F32{v}; }
    F32 C(float v) const { return ir.Imm32(v); }
    F32 Add(F32 a, F32 b) const { return F(ir.FPAdd(a, b)); }
    F32 Sub(F32 a, F32 b) const { return F(ir.FPSub(a, b)); }
    F32 Mul(F32 a, F32 b) const { return F(ir.FPMul(a, b)); }
    F32 Div(F32 a, F32 b) const { return F(ir.FPDiv(a, b)); }
    F32 Max(F32 a, F32 b) const { return F(ir.FPMax(a, b)); }
    F32 Min(F32 a, F32 b) const { return F(ir.FPMin(a, b)); }
    F32 Clamp(F32 v, float lo, float hi) const { return F(ir.FPClamp(v, C(lo), C(hi))); }
    F32 Exp(F32 v) const { return ir.FPExp2(Mul(v, C(1.4426950f))); }
    /// x^y for x >= 0, y > 0.
    F32 Pow(F32 x, float y) const { return ir.FPPow(Max(x, C(0.0f)), C(y)); }
    F32 Select(const IR::U1& c, F32 a, F32 b) const { return F32{ir.Select(c, a, b)}; }

    Vec3 Apply(const Mat3& m, const Vec3& v) const {
        Vec3 out;
        for (size_t row = 0; row < 3; ++row) {
            out[row] = Add(Mul(C(m[row][0]), v[0]), Add(Mul(C(m[row][1]), v[1]), Mul(C(m[row][2]), v[2])));
        }
        return out;
    }
    F32 SmoothStep(F32 x, float edge0, float edge1) const {
        const F32 t = Clamp(Mul(Sub(x, C(edge0)), C(1.0f / (edge1 - edge0))), 0.0f, 1.0f);
        return Mul(Mul(t, t), Sub(C(3.0f), Mul(C(2.0f), t)));
    }
    F32 InversePq(F32 v) const {
        const F32 ym = Pow(Mul(v, C(100.0f / 10000.0f)), PqM1);
        return ir.FPExp2(Mul(C(PqM2), Sub(ir.FPLog2(Add(C(PqC1), Mul(C(PqC2), ym))),
                                          ir.FPLog2(Add(C(1.0f), Mul(C(PqC3), ym))))));
    }
    F32 Pq(F32 n) const {
        const F32 np = Pow(Clamp(n, 0.0f, 1.0f), 1.0f / PqM2);
        const F32 l = Div(Max(Sub(np, C(PqC1)), C(0.0f)), Sub(C(PqC2), Mul(C(PqC3), np)));
        return Mul(Pow(l, 1.0f / PqM1), C(10000.0f / 100.0f));
    }
    Vec3 ToICtCp(const Vec3& rgb) const {
        Vec3 lms = Apply(RgbToLms, rgb);
        for (F32& c : lms) {
            c = InversePq(c);
        }
        return Apply(LmsToICtCp, lms);
    }
    Vec3 FromICtCp(const Vec3& ictcp) const {
        Vec3 lms = Apply(ICtCpToLms, ictcp);
        for (F32& c : lms) {
            c = Pq(c);
        }
        Vec3 rgb = Apply(LmsToRgb, lms);
        for (F32& c : rgb) {
            c = Max(c, C(0.0f));
        }
        return rgb;
    }
    /// GTToneMappingCurveV2: a toe blending into a linear section, then a shoulder converging on
    /// the target.
    F32 Curve(F32 x) const {
        const float k = (LinearSection - 1.0f) / (Alpha - 1.0f);
        const float ka = Target * LinearSection + Target * k;
        const float kb = -Target * k * std::exp(LinearSection / k);
        const float kc = -1.0f / (k * Target);
        x = Max(x, C(0.0f));
        const F32 linear_weight = SmoothStep(x, 0.0f, MidPoint);
        const F32 toe = Mul(C(MidPoint), Pow(Mul(x, C(1.0f / MidPoint)), ToeStrength));
        const F32 low = Add(Mul(Sub(C(1.0f), linear_weight), toe), Mul(linear_weight, x));
        const F32 shoulder = Add(C(ka), Mul(C(kb), Exp(Mul(x, C(kc)))));
        return Select(ir.FPLessThan(x, C(LinearSection * Target)), low, shoulder);
    }
};

/// The colour output component (0..2) each of `clamps` reaches, or nothing when one reaches none
/// or several, or two reach the same.
std::optional<std::array<u32, 3>> OutputComponents(const std::array<IR::Inst*, 3>& clamps,
                                                    const IR::Block& block) {
    std::array<u32, 3> components{};
    std::set<u32> taken;
    for (size_t channel = 0; channel < 3; ++channel) {
        std::set<u32> reached;
        std::vector<IR::Inst*> work{clamps[channel]};
        std::set<IR::Inst*> seen;
        while (!work.empty()) {
            IR::Inst* inst = work.back();
            work.pop_back();
            for (const IR::Use& use : inst->Uses()) {
                IR::Inst* user = use.user;
                if (!seen.insert(user).second) {
                    continue;
                }
                if (user->GetParent() != &block) {
                    return std::nullopt;
                }
                if (user->GetOpcode() == IR::Opcode::SetAttribute) {
                    if (user->Arg(0).Attribute() != IR::Attribute::RenderTarget0 ||
                        !user->Arg(2).IsImmediate() || user->Arg(2).U32() > 2) {
                        return std::nullopt;
                    }
                    reached.insert(user->Arg(2).U32());
                    continue;
                }
                work.push_back(user);
            }
        }
        if (reached.size() != 1 || !taken.insert(*reached.begin()).second) {
            return std::nullopt;
        }
        components[channel] = *reached.begin();
    }
    return components;
}

/// Where the curve mixes its channels before export, the clamps' order in the block: the
/// shader's x, y, z, which the colour buffer's export swizzle moves to the output components.
std::optional<std::array<u32, 3>> SwizzleComponents(const RuntimeInfo& runtime_info) {
    const auto& swizzle = runtime_info.hw.fs.color_buffers[0].swizzle;
    std::array<u32, 3> components{};
    std::set<u32> taken;
    for (u32 channel = 0; channel < 3; ++channel) {
        std::optional<u32> output;
        for (u32 i = 0; i < 3; ++i) {
            if (swizzle.Map(i) == channel) {
                output = i;
            }
        }
        if (!output || !taken.insert(*output).second) {
            return std::nullopt;
        }
        components[channel] = *output;
    }
    return components;
}
} // namespace

bool IsKnownComposite(u64 hash) {
    switch (hash) {
    case 0x15fa3515:
    case 0x5c02eaf8:
    case 0x5e67d866:
    case 0x925d5712:
    case 0xcbe2dd84:
    case 0xccb08339:
        return true;
    default:
        return false;
    }
}

bool ApplyGt7ToneMap(IR::Program& program, const RuntimeInfo& runtime_info) {
    if (gt7_lab_override == 0 || ToneCurveEnabled() ||
        program.info.sw_stage != SwStage::Fragment) {
        return false;
    }
    auto matches = FindToneCurves(program);
    if (!IsComposite(matches)) {
        return false;
    }
    program.info.gt7_checked = true;
    const u64 hash = program.info.pgm_hash;
    const auto decline = [&](const char* why) {
        std::printf("GT7 tonemap: composite %#llx keeps the game's curve (%s)\n",
                    static_cast<unsigned long long>(hash), why);
        return false;
    };
    IR::Block& block = *matches[0].result->GetParent();
    for (const auto& m : matches) {
        if (m.result->GetParent() != &block) {
            return decline("the curve spans blocks");
        }
    }
    std::map<const IR::Inst*, size_t> order;
    size_t index = 0;
    for (IR::Inst& inst : block.Instructions()) {
        order[&inst] = index++;
    }
    std::ranges::sort(matches, [&](const auto& a, const auto& b) {
        return order.at(a.result) < order.at(b.result);
    });
    // GT7 needs only the curve's inputs: it goes after the last of them, and each channel's
    // select after both it and that channel's clamp, before the clamp's first use.
    IR::Inst* inputs_end = nullptr;
    const auto later = [&](IR::Inst* inst) {
        if (inst && order.contains(inst) && (!inputs_end || order.at(inst) > order.at(inputs_end))) {
            inputs_end = inst;
        }
    };
    for (const auto& m : matches) {
        if (!order.contains(m.exponent)) {
            return decline("the curve spans blocks");
        }
        later(m.exponent);
    }
    if (!matches[0].shoulder.IsImmediate()) {
        IR::Inst* shoulder = matches[0].shoulder.Inst();
        if (shoulder->GetParent() == &block) {
            later(shoulder);
        }
    }
    const size_t inputs = order.at(inputs_end);
    for (const auto& m : matches) {
        const size_t after = std::max(inputs, order.at(m.result));
        for (const IR::Use& use : m.result->Uses()) {
            if (use.user->GetParent() != &block || order.at(use.user) <= after) {
                return decline("a channel is used before the curve's inputs are all computed");
            }
        }
    }
    auto components =
        OutputComponents({matches[0].result, matches[1].result, matches[2].result}, block);
    if (!components) {
        components = SwizzleComponents(runtime_info);
    }
    if (!components) {
        return decline("the channels do not map one to one onto the colour output");
    }

    IR::IREmitter ir{block, std::next(IR::Block::InstructionList::s_iterator_to(*inputs_end))};
    const Emit e{ir};
    const auto& m0 = matches[0];

    // Each channel in the curve's units (k * x), by output component.
    Vec3 scene;
    for (size_t channel = 0; channel < 3; ++channel) {
        const F32 exponent{IR::Value{matches[channel].exponent}};
        scene[(*components)[channel]] =
            m0.family == ToneCurveMatch::Family::Exponential
                ? e.Mul(exponent, e.C(-1.0f / 1.4426950f))
                : exponent;
    }
    // GT7 meets the game's curve at MatchPoint at exposure 1: its linear section is the identity
    // on frame-buffer values, then scaled by SdrCorrection.
    const F32 shoulder{m0.shoulder};
    F32 game_at_match;
    if (m0.family == ToneCurveMatch::Family::Exponential) {
        const float ex = std::exp(-MatchPoint);
        const F32 b = e.Sub(e.C(1.0f), e.Mul(shoulder, e.C(ex)));
        game_at_match = e.Mul(e.C(1.0f - ex), e.Mul(b, b));
    } else {
        game_at_match = e.Mul(e.C(MatchPoint / (1.0f + MatchPoint)),
                              e.Add(e.C(1.0f), e.Mul(e.Max(shoulder, e.C(0.0f)), e.C(MatchPoint))));
    }
    const F32 gain = e.Clamp(e.Mul(game_at_match, e.C(Target / MatchPoint)), 0.05f, 10.0f);
    const F32 exposure = e.Clamp(ir.ToneCurveParam(1), 0.05f, 16.0f);
    const F32 scale = e.Mul(gain, exposure);
    Vec3 linear;
    for (size_t c = 0; c < 3; ++c) {
        linear[c] = e.Mul(e.Max(scene[c], e.C(0.0f)), scale);
    }
    const Vec3 rgb = e.Apply(Rec709To2020, linear);

    // applyToneMapping (SDR): per-channel curve ("skewed"), and the skewed intensity with the
    // input's chroma faded out towards the target; blended.
    const Vec3 ucs = e.ToICtCp(rgb);
    Vec3 skewed;
    for (size_t c = 0; c < 3; ++c) {
        skewed[c] = e.Curve(rgb[c]);
    }
    const Vec3 skewed_ucs = e.ToICtCp(skewed);
    const float target_ucs = float(InversePq(Target));
    const F32 chroma = e.Sub(e.C(1.0f), e.SmoothStep(e.Mul(ucs[0], e.C(1.0f / target_ucs)),
                                                    FadeStart, FadeEnd));
    const Vec3 scaled = e.FromICtCp({skewed_ucs[0], e.Mul(ucs[1], chroma), e.Mul(ucs[2], chroma)});
    Vec3 mapped;
    for (size_t c = 0; c < 3; ++c) {
        const F32 blended = e.Add(e.Mul(e.C(1.0f - BlendRatio), skewed[c]), e.Mul(e.C(BlendRatio), scaled[c]));
        mapped[c] = e.Mul(e.C(SdrCorrection), e.Min(blended, e.C(Target)));
    }
    const Vec3 out = e.Apply(Rec2020To709, mapped);

    // Each clamp's uses take GT7 or the game's value.
    const IR::U1 on = ir.FPGreaterThan(ir.ToneCurveParam(0), e.C(0.5f));
    for (size_t channel = 0; channel < 3; ++channel) {
        IR::Inst* clamp = matches[channel].result;
        const F32 game{IR::Value{clamp}};
        const F32 gt7 = e.Clamp(out[(*components)[channel]], 0.0f, 1.0f);
        IR::Value select;
        if (order.at(clamp) < inputs) {
            select = ir.Select(on, gt7, game);
        } else {
            IR::IREmitter after{block, std::next(IR::Block::InstructionList::s_iterator_to(*clamp))};
            select = after.Select(on, gt7, game);
        }
        std::vector<IR::Use> uses(clamp->Uses().begin(), clamp->Uses().end());
        for (const IR::Use& use : uses) {
            if (order.contains(use.user)) {
                use.user->SetArg(use.operand, select);
            }
        }
    }
    program.info.uses_tone_curve = true;
    return true;
}

} // namespace Shader::Native
