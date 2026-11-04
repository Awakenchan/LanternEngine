// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_bindings.h"
#include <bit>
#include <cstring>

namespace BbEngine::GxBindings {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

u32 InputKind(u32 input) {
    return input & 0xff;
}

u32 InputRegister(u32 input) {
    return (input >> 8) & 0xff;
}

u32 InputSlot(u32 input) {
    return (input >> 16) & 0xff;
}

bool DirectSource(u32 kind, u32 slot, Fill& fill, u32& width) {
    const auto set = [&](From from, u32 words) {
        fill = {from, static_cast<std::uint8_t>(slot), 0, 0, 0};
        width = words;
        return true;
    };
    switch (kind) {
    case View4: case SpillView4: return set(FromView, 4);
    case View8: case SpillView8: return set(FromView, 8);
    case Access4: case SpillAccess4: return set(FromAccess, 4);
    case Access8: case SpillAccess8: return set(FromAccess, 8);
    case Sampler: case SpillSampler: return set(FromSampler, 4);
    case Constant: case SpillConstant: return set(FromConstant, 4);
    default: return false;
    }
}

bool Direct(const Stage& stage, u32 reg, Fill& fill) {
    for (u32 i = 0; i < stage.count; ++i) {
        const u32 input = stage.inputs[i];
        const u32 at = InputRegister(input);
        u32 width = 0;
        if (at >= 16 || reg < at || !DirectSource(InputKind(input), InputSlot(input), fill, width)) continue;
        if (reg >= at + width) continue;
        fill.word = static_cast<std::uint8_t>(reg - at);
        return true;
    }
    return false;
}

bool TableInput(const Stage& stage, u32 reg, bool spilled, u32& input) {
    for (u32 i = 0; i < stage.count; ++i) {
        const u32 kind = InputKind(stage.inputs[i]);
        if (InputRegister(stage.inputs[i]) != reg) continue;
        const bool table = spilled ? kind >= SpillShared && kind <= SpillConstantTable
                                   : (kind >= Shared && kind <= ConstantTable) || kind == Upload;
        if (table) {
            input = stage.inputs[i];
            return true;
        }
    }
    return false;
}

Reason TableFill(u32 table, Fill& fill) {
    switch (InputKind(table)) {
    case Upload: fill = {FromSpill, static_cast<std::uint8_t>(InputSlot(table)), 0, 0, static_cast<std::uint16_t>(table >> 24)}; return Covered;
    case Shared: case SpillShared: fill = {FromShared, 0, 0, 0, kSharedWords}; return Covered;
    case ViewTable: case SpillViewTable: fill = {FromView, 0, 0, 8, 128}; return Covered;
    case AccessTable: case SpillAccessTable: fill = {FromAccess, 0, 0, 8, 8}; return Covered;
    case SamplerTable: case SpillSamplerTable: fill = {FromSampler, 0, 0, 4, 16}; return Covered;
    case ConstantTable: case SpillConstantTable: fill = {FromConstant, 0, 0, 4, 16}; return Covered;
    default: return NoInput;
    }
}

Reason EntryFill(const Stage& stage, const Plan& plan, const Entry& entry, Fill& fill) {
    u32 table = 0;
    if (entry.parent < 16) return TableInput(stage, entry.parent, false, table) ? TableFill(table, fill) : NoInput;
    if (entry.parent - 16u >= plan.count) return Outside;
    const Entry& pointer = plan.entries[entry.parent - 16];
    if (pointer.expr_len) return Dynamic;
    if (pointer.parent >= 16) return NoInput;
    u32 upload = 0;
    if (!TableInput(stage, pointer.parent, false, upload)) return NoInput;
    if (InputKind(upload) != Upload) return InputKind(upload) == Shared ? SharedBlock : NoInput;
    if (pointer.offset >= (upload >> 24)) return Outside;
    if (!TableInput(stage, 16 + InputSlot(upload) + pointer.offset, true, table)) return NoInput;
    if (InputKind(table) == SpillShared) {
        fill = {FromShared, 0, 0, 0, kSharedWords};
        return Covered;
    }
    return TableFill(table, fill);
}
}

Reason Prepare(const Stage& stage, const Plan& plan, std::uint32_t flat_dwords, Fill* out) {
    if (!stage.shader) return NoShader;
    for (u32 f = 0; f < flat_dwords && f < 16; ++f) {
        out[f] = {};
        Direct(stage, f, out[f]);
    }
    if (flat_dwords <= 16) return Covered;
    if (!plan.valid || plan.count < flat_dwords - 16) return NoPlan;
    Reason first = Covered;
    for (u32 f = 16; f < flat_dwords; ++f) {
        out[f] = {};
        const Reason reason = EntryFill(stage, plan, plan.entries[f - 16], out[f]);
        if (reason != Covered && first == Covered) first = reason;
    }
    return first;
}

namespace {
u32 Bits(float value) {
    return std::bit_cast<u32>(value);
}
}

const char* ReasonName(Reason reason) {
    static constexpr const char* kNames[kReasons] = {"covered", "no state", "too many inputs", "too many descriptors",
                                                     "no shader", "no table plan", "computed offset", "not an input",
                                                     "shared block", "unbound slot", "outside a table", "vertex stream"};
    return reason < kReasons ? kNames[reason] : "?";
}

std::uint64_t InputsHash(const std::uint32_t* inputs, std::uint32_t count) {
    std::uint64_t hash = 0xCBF29CE484222325ull ^ count;
    for (u32 i = 0; i < count; ++i) hash = (hash ^ inputs[i]) * 0x100000001B3ull;
    return hash;
}

void Clear(Stage& stage) {
    stage.shader = 0;
    stage.inputs_hash = 0;
    stage.count = 0;
    stage.spill_count = 0;
    std::memset(stage.views, kUnbound, sizeof stage.views);
    std::memset(stage.access, kUnbound, sizeof stage.access);
    std::memset(stage.samplers, kUnbound, sizeof stage.samplers);
    std::memset(stage.constants, kUnbound, sizeof stage.constants);
    std::memset(stage.constant_versions, 0, sizeof stage.constant_versions);
}

std::uint8_t Add(State& state, const void* words, std::uint32_t count) {
    if (state.descriptors >= kMaxDescriptors) {
        state.reason = ManyDescriptors;
        return kUnbound;
    }
    u32* out = state.words[state.descriptors];
    std::memset(out, 0, sizeof state.words[0]);
    std::memcpy(out, words, (count < 8 ? count : 8) * sizeof(u32));
    return static_cast<std::uint8_t>(state.descriptors++);
}

void ConstantWords(std::uint64_t address, std::uint32_t bytes, std::uint32_t out[4]) {
    out[0] = static_cast<u32>(address);
    out[1] = (static_cast<u32>(address >> 32) & 0xfff) | 0x108000;
    out[2] = (bytes + 15) >> 4;
    out[3] = 0x00077fac;
}

std::uint32_t Registers(const Render& render, Register* out) {
    u32 count = 0;
    const auto put = [&](u32 index, u32 value) { out[count++] = {static_cast<std::uint16_t>(index), value}; };
    put(0x8e, render.blend[0]);
    for (u32 i = 0; i < 8; ++i) put(0x1e0 + i, render.blend[1 + i] & 0xffff1fff);
    for (u32 i = 0; i < 4; ++i) put(0x105 + i, Bits(render.blend_factor[i]));
    put(0x200, render.depth_stencil[0] & 0xc07007ff);
    put(0x10b, render.depth_stencil[1] & 0xffffff);
    const u32 ref = render.stencil_ref & 0xff;
    const u32 masks = (render.depth_stencil[2] & 0xffff00) | ref | ref << 24;
    put(0x10c, masks);
    put(0x10d, masks);
    put(0x204, render.raster[0] & 0xf7fe03f);
    put(0x205, render.raster[1] & 0x393fff);
    if (render.viewports) {
        const float x = render.viewport[0], y = render.viewport[1];
        const float right = x + render.viewport[2], bottom = y + render.viewport[3];
        const float height = bottom - y;
        const float xscale = (right - x) * 0.5f, yscale = height * -0.5f;
        put(0x10f, Bits(xscale));
        put(0x110, Bits(x + xscale));
        put(0x111, Bits(yscale));
        put(0x112, Bits(y + height * 0.5f));
        put(0x113, Bits(render.viewport[5] - render.viewport[4]));
        put(0x114, 0);
        put(0xb4, Bits(0.0f));
        put(0xb5, Bits(1.0f));
    }
    if (render.scissors && ((render.raster[5] >> 8) & 0xff)) {
        const auto clamp = [](std::int32_t v) { return static_cast<u32>(v < 0 ? 0 : v) & 0x7fff; };
        put(0x94, 1u << 31 | clamp(render.scissor[1]) << 16 | clamp(render.scissor[0]));
        put(0x95, clamp(render.scissor[3]) << 16 | clamp(render.scissor[2]));
    }
    for (u32 i = 0; i < 8; ++i) {
        if (!((render.color_valid >> i) & 1)) continue;
        for (u32 w = 0; w < 11; ++w) put(0x318 + 15 * i + w, render.targets[i][w]);
    }
    if (render.depth_valid) {
        for (u32 w = 0; w < 8; ++w) put(0x10 + w, render.depth_target[w]);
        put(0x2, render.depth_target[8]);
        put(0x5, render.depth_target[9]);
        put(0x2af, render.depth_target[10]);
        put(0xf, render.depth_target[11]);
    }
    return count;
}

std::uint64_t RenderHash(const Render& render) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&render);
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < offsetof(Render, hash); ++i) hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    return hash;
}

bool VertexWords(const State& state, std::uint32_t dword_offset, std::uint32_t out[4]) {
    if (dword_offset % 4) return false;
    const u32 entry = dword_offset / 4;
    if (entry < state.vertex_first || entry >= state.vertex_first + state.vertex_count || entry >= kMaxVertex) return false;
    for (u32 w = 0; w < 4; ++w) out[w] = state.vertex[entry][w];
    return true;
}

std::uint64_t VertexKey(const State& state, std::uint32_t dword_offset) {
    if (dword_offset % 4) return 0;
    const u32 entry = dword_offset / 4;
    if (entry < state.vertex_first || entry >= state.vertex_first + state.vertex_count || entry >= kMaxVertex) return 0;
    return state.vertex_keys[entry];
}

std::uint64_t BufferKey(std::uint64_t object, std::uint32_t version) {
    if (!object) return 0;
    std::uint64_t key = (object ^ (std::uint64_t(version) << 40)) * 0x9E3779B97F4A7C15ull;
    key ^= key >> 31;
    return key | 1;
}

const char* RegisterName(std::uint16_t index) {
    if (index >= 0x1e0 && index < 0x1e8) return "CB_BLEND_CONTROL";
    if (index >= 0x105 && index < 0x109) return "CB_BLEND_COLOR";
    if (index >= 0x10f && index < 0x115) return "PA_CL_VPORT";
    if (index >= 0x318 && index < 0x318 + 15 * 8) return "CB_COLOR";
    if (index >= 0x10 && index < 0x18) return "DB_DEPTH_TARGET";
    switch (index) {
    case 0x8e: return "CB_TARGET_MASK";
    case 0x200: return "DB_DEPTH_CONTROL";
    case 0x10b: return "DB_STENCIL_CONTROL";
    case 0x10c: return "DB_STENCILREFMASK";
    case 0x10d: return "DB_STENCILREFMASK_BF";
    case 0x204: return "PA_CL_CLIP_CNTL";
    case 0x205: return "PA_SU_SC_MODE_CNTL";
    case 0xb4: return "PA_SC_VPORT_ZMIN";
    case 0xb5: return "PA_SC_VPORT_ZMAX";
    case 0x94: return "PA_SC_VPORT_SCISSOR_TL";
    case 0x95: return "PA_SC_VPORT_SCISSOR_BR";
    case 0x2: return "DB_DEPTH_VIEW";
    case 0x5: return "DB_HTILE_DATA_BASE";
    case 0xf: return "DB_DEPTH_INFO";
    case 0x2af: return "DB_HTILE_SURFACE";
    default: return "?";
    }
}
}
