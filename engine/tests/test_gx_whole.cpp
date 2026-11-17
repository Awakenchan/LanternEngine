// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/gx_whole.h"
#include "gpu/shim/bbport_native_calls.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace W = BbEngine::GxWhole;
namespace G = BbEngine::GxBindings;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

namespace {
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++g_failed;
    }
}

u32 F(float value) {
    return std::bit_cast<u32>(value);
}

u32 Ctx(u32 index) {
    return W::kContext + index;
}

constexpr u32 kVsRsrc1 = 0x00c003c1, kModifier = 0x01000040, kIndexHigh = 0x12, kIndexLow = 0x3456789a;

std::unique_ptr<G::State> Recorded() {
    auto state = std::make_unique<G::State>();
    std::memset(static_cast<void*>(state.get()), 0, sizeof *state);
    G::Render& r = state->render;
    r.blend[0] = 0x0000000f;
    for (u32 i = 0; i < 8; ++i) r.blend[1 + i] = 0x0000e000u | (0x10 + i);
    r.blend_factor[0] = 0.25f;
    r.blend_factor[1] = 0.5f;
    r.blend_factor[2] = 0.75f;
    r.blend_factor[3] = 1.0f;
    r.depth_stencil[0] = 0xffffffff;
    r.depth_stencil[1] = 0xab123456;
    r.depth_stencil[2] = 0xcc77ee11;
    r.stencil_ref = 0x1a5;
    r.raster[0] = 0xffffffff;
    r.raster[1] = 0xffffffff;
    r.raster[2] = F(2.5f);
    r.raster[3] = F(0.75f);
    r.raster[4] = F(1.5f);
    r.raster[5] = 0x0101;
    r.viewports = 1;
    const float viewport[6] = {10.0f, 20.0f, 640.0f, 360.0f, 0.25f, 1.0f};
    std::memcpy(r.viewport, viewport, sizeof viewport);
    r.scissors = 1;
    const std::int32_t scissor[4] = {-5, 7, 100000, 200};
    std::memcpy(r.scissor, scissor, sizeof scissor);
    r.color_valid = 0b101;
    for (u32 w = 0; w < 11; ++w) {
        r.targets[0][w] = 0x1000 + w;
        r.targets[2][w] = 0x3000 + w;
    }
    r.depth_valid = 1;
    for (u32 w = 0; w < 12; ++w) r.depth_target[w] = 0x5000 + w;
    r.index_known = 1;
    r.index_format = 0x2a;
    r.index_base = u64(kIndexHigh) << 32 | kIndexLow;
    G::Whole& w = state->whole;
    w.captured = 1;
    w.topology = 5;
    w.depth_bounds[0] = 0.125f;
    w.depth_bounds[1] = 0.875f;
    w.index_bound = 1;
    w.vs_bound = 1;
    const u32 vs[7] = {0x100, 0, kVsRsrc1, 0x14, 0x2, 0x4, 0x101};
    std::memcpy(w.vs_registers, vs, sizeof vs);
    w.vs_modifier = kModifier;
    w.ps_bound = 1;
    for (u32 i = 0; i < 12; ++i) w.ps_registers[i] = 0x700 + i;
    w.ps_registers[1] = 0;
    w.semantics_valid = 1;
    w.vertex_semantics_count = 3;
    const std::uint8_t vertex[3][2] = {{5, 0}, {7, 1}, {9, 2}};
    std::memcpy(w.vertex_semantics, vertex, sizeof vertex);
    w.pixel_semantics_count = 3;
    w.pixel_semantics[0] = 0x0107;
    w.pixel_semantics[1] = 0x1009;
    w.pixel_semantics[2] = 0x0003;
    w.depth_clear = 1.0f;
    w.stencil_clear = 0x80;
    return state;
}

std::vector<u32> ExpectedRegisters() {
    std::vector<u32> regs(W::kRegisterWords, 0);
    regs[Ctx(0x08e)] = 0xf;
    for (u32 i = 0; i < 8; ++i) regs[Ctx(0x1e0 + i)] = 0x10 + i;
    regs[Ctx(0x105)] = F(0.25f);
    regs[Ctx(0x106)] = F(0.5f);
    regs[Ctx(0x107)] = F(0.75f);
    regs[Ctx(0x108)] = F(1.0f);
    regs[Ctx(0x200)] = 0xc07007ff;
    regs[Ctx(0x10b)] = 0x00123456;
    regs[Ctx(0x10c)] = 0xa577eea5;
    regs[Ctx(0x10d)] = 0xa577eea5;
    regs[Ctx(0x008)] = F(0.125f);
    regs[Ctx(0x009)] = F(0.875f);
    regs[Ctx(0x204)] = 0x0f7fe03f;
    regs[Ctx(0x205)] = 0x00393fff;
    regs[Ctx(0x2df)] = F(0.75f);
    regs[Ctx(0x2e0)] = F(24.0f);
    regs[Ctx(0x2e1)] = F(2.5f);
    regs[Ctx(0x2e2)] = F(24.0f);
    regs[Ctx(0x2e3)] = F(2.5f);
    regs[Ctx(0x292)] = 0x3;
    regs[Ctx(0x0b4)] = F(0.0f);
    regs[Ctx(0x0b5)] = F(1.0f);
    regs[Ctx(0x10f)] = F(320.0f);
    regs[Ctx(0x110)] = F(330.0f);
    regs[Ctx(0x111)] = F(-180.0f);
    regs[Ctx(0x112)] = F(200.0f);
    regs[Ctx(0x113)] = F(0.75f);
    regs[Ctx(0x114)] = 0;
    regs[Ctx(0x206)] = 0x43f;
    regs[Ctx(0x00c)] = 20u << 16 | 10u;
    regs[Ctx(0x00d)] = 380u << 16 | 650u;
    regs[Ctx(0x094)] = 0x80000000u | 7u << 16 | 0u;
    regs[Ctx(0x095)] = 200u << 16 | (100000u & 0x7fff);
    for (u32 w = 0; w < 11; ++w) {
        regs[Ctx(0x318 + w)] = 0x1000 + w;
        regs[Ctx(0x318 + 30 + w)] = 0x3000 + w;
    }
    for (u32 w = 0; w < 8; ++w) regs[Ctx(0x010 + w)] = 0x5000 + w;
    regs[Ctx(0x002)] = 0x5008;
    regs[Ctx(0x005)] = 0x5009;
    regs[Ctx(0x00b)] = F(1.0f);
    regs[Ctx(0x00a)] = 0x80;
    regs[Ctx(0x29f)] = 1;
    regs[Ctx(0x1f9)] = kIndexHigh;
    regs[Ctx(0x1fa)] = kIndexLow;
    regs[W::kUconfig + 0x242] = 6;
    regs[Ctx(0x2d5)] = 0;
    regs[W::kSh + 0x048] = 0x100;
    regs[W::kSh + 0x04a] = (kVsRsrc1 & 0xfcfffc3f) | kModifier;
    regs[W::kSh + 0x04b] = 0x14;
    regs[Ctx(0x207)] = 0x101;
    regs[W::kSh + 0x008] = 0x700;
    regs[W::kSh + 0x00a] = 0x702;
    regs[W::kSh + 0x00b] = 0x703;
    regs[Ctx(0x1c4)] = 0x704;
    regs[Ctx(0x1c5)] = 0x705;
    regs[Ctx(0x1b3)] = 0x706;
    regs[Ctx(0x1b4)] = 0x707;
    regs[Ctx(0x1b6)] = 0x708;
    regs[Ctx(0x1b8)] = 0x709;
    regs[Ctx(0x203)] = 0x70a;
    regs[Ctx(0x08f)] = 0x70b;
    regs[Ctx(0x191)] = 0x101;
    regs[Ctx(0x192)] = 0x422;
    regs[Ctx(0x193)] = 0x20;
    return regs;
}

struct Result {
    u32 count = 0;
    u32 differing = 0;
    W::Counts counts;
    std::array<W::Value, W::kMaxValues> values{};
    std::array<W::Difference, W::kEvidencePerDraw> evidence{};
};

std::unique_ptr<Result> Run(const G::State& state, bool indexed, const std::vector<u32>& regs) {
    auto result = std::make_unique<Result>();
    result->count = W::RegistersFromRecord(state, indexed, result->values);
    result->differing = W::CompareRecord(regs, std::span<const W::Value>(result->values.data(), result->count), result->counts, result->evidence);
    return result;
}

u32 CountOf(const Result& result, W::Field field) {
    u32 n = 0;
    for (u32 i = 0; i < result.count; ++i) n += result.values[i].field == field;
    return n;
}

void TestWholeRecordMatches() {
    const auto state = Recorded();
    const auto regs = ExpectedRegisters();
    const auto result = Run(*state, true, regs);
    Check(result->differing == 0, "a record with every field set gives the registers GX's flushes write");
    Check(result->counts.fields == result->count && result->count == 99 && result->count < W::kMaxValues,
          "every value produced is compared and the list fits its bound");
    for (u32 i = 0; i < result->count; ++i) {
        const W::Value& v = result->values[i];
        const W::FieldInfo& info = W::kFieldInfo[v.field];
        if (v.reg < info.first || v.reg >= info.first + info.count || info.source != W::Source::Record) {
            Check(false, "every value lies inside its own field's registers and comes from a record field");
            break;
        }
    }
    Check(CountOf(*result, W::CbColorTarget) == 22 && CountOf(*result, W::CbColorUnbound) == 0,
          "two bound color targets give 11 words each; unbound slots outside CB_TARGET_MASK give none");
    Check(CountOf(*result, W::SpiPsInputCntl) == 3, "one input control per pixel input");
}

void TestMasks() {
    const auto state = Recorded();
    auto regs = ExpectedRegisters();
    regs[Ctx(0x1fa)] |= 1;
    regs[Ctx(0x1f9)] |= 0xff00;
    regs[Ctx(0x00a)] |= 0xff00;
    regs[Ctx(0x29f)] |= 0xfc;
    regs[W::kUconfig + 0x242] |= 0xc0;
    Check(Run(*state, true, regs)->differing == 0, "bits outside what a field's reader uses are not compared");
}

void TestDifferenceNamesTheField() {
    const auto state = Recorded();
    auto regs = ExpectedRegisters();
    regs[Ctx(0x1e3)] ^= 0x4;
    const auto result = Run(*state, true, regs);
    Check(result->differing == 1 && result->counts.fields_differ == 1, "one changed register is one difference");
    Check(result->counts.field_differ[W::CbBlendControl] == 1 && result->evidence[0].field == W::CbBlendControl &&
              result->evidence[0].reg == Ctx(0x1e3) && result->evidence[0].ours == 0x13 && result->evidence[0].registers == 0x17,
          "the difference names the field, the register and both values");
    char line[512];
    W::FormatLine(line, sizeof line, W::Mode::Compare, result->counts);
    Check(std::string(line).find("CB_BLEND_CONTROL 1") != std::string::npos, "the counts line lists the field by name");
}

void TestUnboundSlot() {
    constexpr u32 kSlot1Info = 0x32b, kStale = 0x00060718;
    auto state = Recorded();
    state->render.color_valid = 0b001;
    state->render.blend[0] = 0xff;
    state->whole.ps_registers[11] = 0x0f;
    auto regs = ExpectedRegisters();
    regs[Ctx(0x08e)] = 0xff;
    regs[Ctx(0x08f)] = 0x0f;
    for (u32 w = 0; w < 11; ++w) regs[Ctx(0x318 + 30 + w)] = 0;
    regs[Ctx(kSlot1Info)] = kStale;
    auto result = Run(*state, true, regs);
    Check(CountOf(*result, W::CbColorUnbound) == 0,
          "draw 513: slot 1 unbound, in CB_TARGET_MASK but not written by the pixel shader: its info is not the record's");
    Check(result->counts.field_differ[W::CbColorUnbound] == 0 && result->differing == 0,
          "draw 513: the stale CB_COLOR1_INFO 0x60718 GX left behind is no difference");
    state->whole.ps_registers[11] = 0xff;
    regs[Ctx(0x08f)] = 0xff;
    result = Run(*state, true, regs);
    Check(CountOf(*result, W::CbColorUnbound) == 1 && result->differing == 1 && result->counts.field_differ[W::CbColorUnbound] == 1 &&
              result->evidence[0].reg == Ctx(kSlot1Info) && result->evidence[0].ours == 0 && result->evidence[0].registers == kStale,
          "a written slot unbound in the record but bound in the registers is a real difference at 0xa32b");
    state->whole.ps_bound = 0;
    result = Run(*state, true, regs);
    Check(CountOf(*result, W::CbColorUnbound) == 0, "no pixel shader: no slot is written");
    state->whole.ps_bound = 1;
    state->whole.ps_registers[11] = 0xf0;
    state->render.blend[0] = 0x0f;
    Check(!W::SlotWritten(state->render, state->whole, 1) && !W::SlotWritten(state->render, state->whole, 0),
          "a slot is written only where CB_TARGET_MASK and CB_SHADER_MASK overlap");
    state->render.blend[0] = 0xf0000000;
    state->whole.ps_registers[11] = 0x10000000;
    Check(W::SlotWritten(state->render, state->whole, 7), "the last slot's nibble counts");
}

std::unique_ptr<Result> RunRead(const G::State& state, bool indexed, const std::vector<u32>& regs) {
    auto result = std::make_unique<Result>();
    result->count = W::RegistersFromRecord(state, indexed, result->values);
    result->differing = W::CompareRecord(regs, std::span<const W::Value>(result->values.data(), result->count), result->counts,
                                         result->evidence, &state.render);
    return result;
}

void TestUnusedFields() {
    auto state = Recorded();
    auto regs = ExpectedRegisters();
    regs[Ctx(0x318 + 30 + 4)] ^= 0x40;
    regs[Ctx(0x1e2)] ^= 0x4;
    auto result = RunRead(*state, true, regs);
    Check(result->differing == 0 && result->counts.unused_differ == 2 &&
              result->counts.unused_by[static_cast<u32>(W::Unused::SlotNotWritten)] == 2,
          "slot 2's target and blend outside CB_TARGET_MASK 0xf are unread: counted, not differences");
    regs[Ctx(0x1e0)] ^= 0x4;
    result = RunRead(*state, true, regs);
    Check(result->differing == 1 && result->evidence[0].reg == Ctx(0x1e0), "slot 0's blend is read: a difference");
    regs = ExpectedRegisters();
    regs[Ctx(0x10b)] ^= 0x10;
    regs[Ctx(0x008)] ^= 1;
    result = RunRead(*state, true, regs);
    Check(result->differing == 2 && result->counts.unused_differ == 0, "stencil and depth bounds on: their words are read");
    state->render.depth_stencil[0] &= ~0x9u;
    regs[Ctx(0x200)] &= ~0x9u;
    result = RunRead(*state, true, regs);
    Check(result->differing == 0 && result->counts.unused_by[static_cast<u32>(W::Unused::StencilOff)] == 1 &&
              result->counts.unused_by[static_cast<u32>(W::Unused::BoundsOff)] == 1,
          "stencil and depth bounds off: their words are unread, counted by class");
    char line[2048];
    W::FormatLine(line, sizeof line, W::Mode::Feed, result->counts);
    Check(std::string(line).find("stencil off 1") != std::string::npos && std::string(line).find("no turn-off") != std::string::npos,
          "the counts line lists unread differences by class");
    result = Run(*state, true, regs);
    Check(result->differing == 2, "without the record's render state every difference counts");

    state = Recorded();
    state->render.blend[0] = 0xf0;
    regs = ExpectedRegisters();
    regs[Ctx(0x08e)] = 0xf0;
    regs[Ctx(0x1e0)] ^= 0x4;
    regs[Ctx(0x318 + 4)] ^= 0x40;
    result = RunRead(*state, true, regs);
    Check(result->differing == 2 && result->counts.unused_differ == 0,
          "slot 0 outside CB_TARGET_MASK is still read (dual source, shadow, UI and display readers): both words are differences");
    regs = ExpectedRegisters();
    state->render.blend[0] = 0xff;
    regs[Ctx(0x08e)] = 0xff;
    regs[Ctx(0x1e1)] ^= 0x4;
    result = RunRead(*state, true, regs);
    Check(result->differing == 1 && result->evidence[0].reg == Ctx(0x1e1), "slot 1 inside CB_TARGET_MASK: its blend is read");
    W::Value slot0{W::CbBlendControl, Ctx(0x1e0), 0, ~0u};
    Check(W::UnusedBy(slot0, state->render) == W::Unused::Read, "slot 0's blend control is never classed unread");

    state = Recorded();
    state->render.depth_stencil[0] &= ~0x1u;
    regs = ExpectedRegisters();
    regs[Ctx(0x200)] &= ~0x1u;
    regs[Ctx(0x008)] ^= 1;
    result = RunRead(*state, true, regs);
    Check(result->differing == 1 && result->evidence[0].reg == Ctx(0x008),
          "stencil off but depth bounds on: the bounds are read");
    state->render.depth_stencil[0] &= ~0x8u;
    regs[Ctx(0x200)] &= ~0x8u;
    W::UnusedFound found;
    std::array<W::Value, W::kMaxValues> values{};
    const u32 count = W::RegistersFromRecord(*state, true, values);
    W::Counts counts;
    std::array<W::Difference, W::kEvidencePerDraw> evidence{};
    const u32 differing = W::CompareRecord(regs, std::span<const W::Value>(values.data(), count), counts, evidence, &state->render, &found);
    Check(differing == 0 && counts.unused_by[static_cast<u32>(W::Unused::BoundsOff)] == 1,
          "depth bounds off: the bounds word is unread, counted as such");
    const u32 bounds = static_cast<u32>(W::Unused::BoundsOff);
    Check(((found.mask >> bounds) & 1) && found.first[bounds].reg == Ctx(0x008),
          "the first unread difference of a class keeps its register for the =2 log line");
}

void TestTopologyAndIndex() {
    Check(W::PrimitiveFromTopology(1) == 1 && W::PrimitiveFromTopology(5) == 6 && W::PrimitiveFromTopology(6) == 10 &&
              W::PrimitiveFromTopology(9) == 13 && W::PrimitiveFromTopology(10) == 9 && W::PrimitiveFromTopology(41) == 9,
          "GX's topology maps as the table at 0x486fb20");
    Check(W::PrimitiveFromTopology(0x2a) == 0 && W::PrimitiveFromTopology(0xffffffff) == 0, "topologies past the table give 0");
    Check(W::IndexType(0x2a) == 1 && W::IndexType(0x39) == 0, "R32 indices are 32-bit, the rest 16-bit");
    auto state = Recorded();
    auto regs = ExpectedRegisters();
    const auto plain = Run(*state, false, regs);
    Check(CountOf(*plain, W::VgtIndexType) == 0 && CountOf(*plain, W::VgtIndexBase) == 0, "a draw without indices has no index fields");
    state->render.index_known = 0;
    const auto dynamic = Run(*state, true, regs);
    Check(CountOf(*dynamic, W::VgtIndexType) == 1 && CountOf(*dynamic, W::VgtIndexBase) == 0,
          "an index buffer whose address is not known still gives its type");
    state->whole.topology = 4;
    regs[W::kUconfig + 0x242] = 4;
    Check(Run(*state, true, regs)->differing == 0, "a triangle list record matches a triangle list register");
}

void TestScissorDepthAndPixel() {
    auto state = Recorded();
    state->render.raster[5] = 0x0001;
    auto regs = ExpectedRegisters();
    regs[Ctx(0x292)] = 0x1;
    const auto off = Run(*state, true, regs);
    Check(CountOf(*off, W::PaScVportScissor0) == 0 && off->differing == 0, "with the viewport scissor off its rectangle is not a field");
    state = Recorded();
    state->render.depth_valid = 0;
    regs = ExpectedRegisters();
    const auto unbound = Run(*state, true, regs);
    Check(CountOf(*unbound, W::DbDepthTarget) == 2 && unbound->differing == 2,
          "no depth target gives DB_Z_INFO and DB_STENCIL_INFO 0, which a bound target's words contradict");
    state->render.depth_stencil[0] &= ~0x3u;
    regs[Ctx(0x200)] &= ~0x3u;
    const auto untested = Run(*state, true, regs);
    Check(CountOf(*untested, W::DbDepthTarget) == 0 && untested->differing == 0,
          "no depth target with depth and stencil off: GX's stale DB_Z_INFO and DB_STENCIL_INFO are not the record's");
    state = Recorded();
    state->whole.ps_bound = 0;
    regs = ExpectedRegisters();
    regs[W::kSh + 0x008] = 0;
    regs[Ctx(0x203)] = 0;
    const auto nopixel = Run(*state, true, regs);
    Check(nopixel->differing == 0 && CountOf(*nopixel, W::PsProgram) == 2 && CountOf(*nopixel, W::CbShaderMask) == 0,
          "no pixel shader clears its program and shader control only");
    state = Recorded();
    state->render.viewports = 0;
    Check(CountOf(*Run(*state, true, ExpectedRegisters()), W::PaClVport0) == 0, "no viewport set gives no viewport fields");
    state->whole.captured = 0;
    Check(Run(*state, true, ExpectedRegisters())->count == 0, "a record without its whole part gives no fields");
}

void TestSemantics() {
    const auto state = Recorded();
    Check(W::PixelInputControl(state->whole, 0) == 0x101, "a matched input takes the vertex output's offset and its default value");
    Check(W::PixelInputControl(state->whole, 1) == 0x422, "the flat bit and the high offset bit come from the pixel entry");
    Check(W::PixelInputControl(state->whole, 2) == 0x20, "an input without a vertex output reads offset 0x20");
}

void TestTruncate() {
    Check(W::Truncate(12.9f) == 12 && W::Truncate(-1.5f) == 0xffffffffu, "screen scissor values truncate toward zero");
    Check(W::Truncate(NAN) == 0x80000000u && W::Truncate(1e20f) == 0x80000000u, "values out of range give the integer indefinite");
}

void TestKept() {
    std::vector<u32> regs(W::kRegisterWords, 0);
    W::KeptMemory memory;
    W::Counts counts;
    Check(W::CheckKept(regs, memory, true, counts) == 0 && counts.kept_checks == 0, "the first covered draw only remembers");
    Check(W::CheckKept(regs, memory, true, counts) == 0 && counts.kept_checks == 1 && counts.kept_words == W::kKeptWords,
          "an unchanged kept field is no change");
    regs[Ctx(0x2a8)] = 4;
    Check(W::CheckKept(regs, memory, true, counts) == 1 && counts.field_changed[W::VgtInstanceStepRate] == 1 && counts.kept_changed == 1,
          "a step rate changed between covered draws is counted under its name");
    W::CheckKept(regs, memory, false, counts);
    regs[Ctx(0x202)] = 0xcc0010;
    Check(W::CheckKept(regs, memory, true, counts) == 0 && counts.field_changed[W::CbColorControl] == 0,
          "an uncovered draw between them starts the memory again");
    regs[Ctx(0x115)] = F(1.0f);
    W::CheckKept(regs, memory, true, counts);
    Check(counts.field_changed[W::PaClVportRest] == 1, "viewports 1 to 15 are watched as kept fields");
}

void TestSampling() {
    Check(W::ShouldCompare(W::Mode::Compare, 5) && W::ShouldCompare(W::Mode::Compare, 63), "mode 2 compares every covered draw");
    Check(W::ShouldCompare(W::Mode::Feed, 0) && !W::ShouldCompare(W::Mode::Feed, 1) && !W::ShouldCompare(W::Mode::Feed, W::kSampleEvery - 1) &&
              W::ShouldCompare(W::Mode::Feed, W::kSampleEvery),
          "mode 1 compares the first covered draw and then one in kSampleEvery");
    Check(!W::ShouldCompare(W::Mode::Off, 0), "mode 0 compares nothing");
}

void TestSuspect() {
    W::Counts counts;
    counts.total = 10;
    Check(W::InstrumentSuspect(W::Mode::Off, counts), "draws seen and none covered is suspect even in the count-only state");
    counts.covered = 10;
    Check(!W::InstrumentSuspect(W::Mode::Off, counts), "count only with covered draws is not suspect");
    Check(W::InstrumentSuspect(W::Mode::Feed, counts), "covered draws and no check in mode 1 is suspect");
    counts.checked = 4;
    counts.differ = 4;
    Check(W::InstrumentSuspect(W::Mode::Compare, counts), "every checked draw differing is suspect");
    counts.differ = 1;
    Check(!W::InstrumentSuspect(W::Mode::Compare, counts), "some differences among checks is not suspect");
}

void TestRecordKey() {
    namespace N = BbNativeCalls;
    N::DrawRecord draw;
    draw.kind = N::DrawIndex;
    draw.index_count = 36;
    draw.index_addr = 0x2000'0000ull;
    u32 list[8]{};
    const u64 at = reinterpret_cast<u64>(list);
    N::EncodeRecordNop(draw, list);
    u32 old[8];
    std::memcpy(old, list, sizeof old);
    N::EncodeRecordNop(draw, list);
    Check(std::memcmp(old, list, sizeof old) == 0, "the same draw written again at a reused address is the same token, check word included");
    N::SealRecordNop(old, 1);
    N::SealRecordNop(list, 2);
    u32 old_stamp = 0, new_stamp = 0;
    Check(N::RecordStamp(old, old_stamp) && N::RecordStamp(list, new_stamp) && old_stamp == 1 && new_stamp == 2,
          "each stamp is carried in its own token");
    const u64 slot_address = at, slot_sequence = 2;
    const u32 slot_key = 2;
    Check(W::CheckKey(slot_address, slot_sequence, slot_key, at, old_stamp) == W::Key::Stale,
          "the older token at an address stamped again does not take the newer record");
    Check(W::CheckKey(slot_address, slot_sequence, slot_key, at, new_stamp) == W::Key::Taken, "the newest token takes its record");
    Check(W::CheckKey(slot_address, 0, slot_key, at, new_stamp) == W::Key::Lost, "a record already taken is not taken twice");
    Check(W::CheckKey(at + 64, slot_sequence, slot_key, at, new_stamp) == W::Key::Lost, "a slot another address took over is lost");
    Check(W::CheckKey(slot_address, slot_sequence, slot_key, at, 0) == W::Key::Unstamped, "a token never stamped has no record");

    N::DrawRecord carried = draw;
    carried.flags = N::CarriesBaseVertex | N::CarriesInstances;
    carried.base_vertex = 4;
    carried.instances = 3;
    u32 full[8]{};
    N::EncodeRecordNop(carried, full);
    u32 stamp_slot = 9, words = 0;
    Check(N::RecordLayout(full, stamp_slot, words) && stamp_slot == 0 && !N::SealRecordNop(full, 5),
          "an indexed draw carrying base vertex and instances has no stamp word");
    const u32 check = N::TokenKey(full, stamp_slot, words);
    Check(check == full[words] && check != 0, "it is keyed by its check word");
    carried.instances = 4;
    u32 other[8]{};
    N::EncodeRecordNop(carried, other);
    Check(W::CheckKey(at, 7, N::TokenKey(other, stamp_slot, words), at, check) == W::Key::Stale,
          "another draw at the address is told apart by its check word");

    W::RecordSlot slot;
    W::Publish(slot, at, 0x5555, 9, 9);
    const W::Taking first = W::Consume(slot, at, 9);
    Check(first.key == W::Key::Taken && first.record == 0x5555 && first.sequence == 9, "a published record is taken by its key");
    Check(W::Consume(slot, at, 9).key == W::Key::Lost, "a consumed record is not taken again");
    W::Publish(slot, at, 0x6666, 10, 10);
    W::Forget(slot, at + 4);
    Check(W::Consume(slot, at, 9).key == W::Key::Stale, "a stale key leaves the record");
    W::Forget(slot, at);
    Check(W::Consume(slot, at, 10).key == W::Key::Lost, "a forgotten record is lost");
}

u64 RecordOfStamp(u32 stamp) {
    return 0x7000'0000'0000ull ^ (u64(stamp) * 0x9E3779B97F4A7C15ull >> 8);
}

void TestSlotThreads() {
    constexpr u32 kRounds = 20000;
    constexpr u64 kAt = 0x40'0000;
    W::RecordSlot slot;
    std::atomic<u32> token{0};
    std::atomic<bool> done{false};
    std::atomic<u32> ready{0};
    std::atomic<u64> takes{0}, wrong{0}, twice{0};
    std::unique_ptr<std::atomic<std::uint8_t>[]> taken(new std::atomic<std::uint8_t>[kRounds + 1]);
    for (u32 s = 0; s <= kRounds; ++s) taken[s].store(0, std::memory_order_relaxed);
    auto taker = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) {
            const u32 stamp = token.load(std::memory_order_acquire);
            const W::Taking t = W::Consume(slot, kAt, stamp);
            if (t.key != W::Key::Taken) continue;
            takes.fetch_add(1, std::memory_order_relaxed);
            if (t.sequence != stamp || t.record != RecordOfStamp(stamp)) wrong.fetch_add(1, std::memory_order_relaxed);
            if (taken[stamp].fetch_add(1, std::memory_order_relaxed)) twice.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread first(taker), second(taker);
    while (ready.load(std::memory_order_acquire) < 2) std::this_thread::yield();
    for (u32 s = 1; s <= kRounds; ++s) {
        W::Publish(slot, kAt, RecordOfStamp(s), s, s);
        token.store(s, std::memory_order_release);
        for (u32 spin = 0; (s & 1) && spin < 256 && !taken[s].load(std::memory_order_relaxed); ++spin) std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
    first.join();
    second.join();
    Check(takes.load() > 0, "the takers took records while the stamper stamped");
    Check(wrong.load() == 0, "a taken record is always its own stamp's");
    Check(twice.load() == 0, "no record is taken twice");
}

void TestFormatBound() {
    W::Counts counts;
    counts.total = 123456789;
    for (u32 f = 0; f < W::kFields; ++f) counts.field_differ[f] = 99999;
    char small[24];
    std::memset(small, 'x', sizeof small);
    const std::size_t used = W::FormatLine(small, sizeof small, W::Mode::Compare, counts);
    Check(used < sizeof small && small[used] == '\0' && std::strlen(small) == used, "a short buffer is filled and ended, never overrun");
    char big[4096];
    W::FormatLine(big, sizeof big, W::Mode::Compare, counts);
    Check(std::string(big).find("VGT_NUM_INSTANCES 99999") != std::string::npos, "a large buffer holds every field name");
}

int RunSuite() {
    g_failed = 0;
    TestWholeRecordMatches();
    TestMasks();
    TestDifferenceNamesTheField();
    TestUnboundSlot();
    TestUnusedFields();
    TestTopologyAndIndex();
    TestScissorDepthAndPixel();
    TestSemantics();
    TestTruncate();
    TestKept();
    TestSampling();
    TestSuspect();
    TestRecordKey();
    TestSlotThreads();
    TestFormatBound();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_GX_WHOLE_MUTANTS
    using W::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        W::selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("gx whole mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("gx whole mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("gx whole mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("gx whole mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("gx whole: %d failures\n", g_failed);
        return 1;
    }
    std::printf("gx whole: all tests passed\n");
    return 0;
#endif
}
