// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::GxBindings {
constexpr std::uint32_t kMaxInputs = 64, kMaxDescriptors = 48, kStages = 2, kMaxRegisters = 160, kMaxSpill = 128, kSharedWords = 48, kMaxVertex = 32;
constexpr std::uint8_t kUnbound = 0xff;
constexpr std::uint32_t kMaxViewKeys = 32;

enum Kind : std::uint32_t {
    View4, View8, Access4, Access8, Sampler, Constant, Literal, Shared, ViewTable, AccessTable, SamplerTable, ConstantTable,
    SpillView4 = 13, SpillView8, SpillAccess4, SpillAccess8, SpillSampler, SpillConstant, SpillLiteral, SpillShared,
    SpillViewTable, SpillAccessTable, SpillSamplerTable, SpillConstantTable, Upload = 26
};

enum Reason : std::uint8_t {
    Covered, NoState, ManyInputs, ManyDescriptors, NoShader, NoPlan, Dynamic, NoInput, SharedBlock, Unbound, Outside,
    VertexStream, kReasons
};
const char* ReasonName(Reason reason);

struct Stage {
    std::uint64_t shader;
    std::uint64_t inputs_hash;
    std::uint32_t count;
    std::uint32_t inputs[kMaxInputs];
    std::uint8_t views[128];
    std::uint8_t access[8];
    std::uint8_t samplers[16];
    std::uint8_t constants[16];
    std::uint32_t constant_versions[16];
    std::uint32_t spill_count;
    std::uint32_t spill[kMaxSpill];
};

struct Render {
    std::uint32_t raster[6];
    std::uint32_t blend[10];
    std::uint32_t depth_stencil[3];
    float blend_factor[4];
    std::uint32_t stencil_ref;
    std::uint32_t viewports, scissors;
    float viewport[6];
    std::int32_t scissor[4];
    std::uint64_t color[8];
    std::uint64_t depth;
    std::uint32_t color_valid;
    std::uint32_t targets[8][11];
    std::uint32_t depth_valid;
    std::uint32_t depth_target[12];
    std::uint32_t index_known;
    std::uint32_t index_format;
    std::uint64_t index_base;
    std::uint64_t hash;
};

struct State {
    std::uint64_t record;
    std::uint8_t reason;
    std::uint32_t descriptors;
    Stage stages[kStages];
    std::uint32_t words[kMaxDescriptors][8];
    std::uint32_t shared_valid;
    std::uint32_t shared[kSharedWords];
    std::uint32_t vertex_first;
    std::uint32_t vertex_count;
    std::uint32_t vertex[kMaxVertex][4];
    std::uint64_t vertex_keys[kMaxVertex];
    std::uint64_t index_key;
    std::uint32_t num_view_keys;
    std::uint64_t view_keys[kMaxViewKeys][2];
    Render render;
};

struct Entry {
    std::uint16_t parent;
    std::uint16_t expr_len;
    std::uint32_t offset;
};

struct Plan {
    const Entry* entries;
    std::uint32_t count;
    bool valid;
};

enum From : std::uint8_t { FromNone, FromView, FromAccess, FromSampler, FromConstant, FromSpill, FromShared };
struct Fill {
    std::uint8_t from;
    std::uint8_t slot;
    std::uint8_t word;
    std::uint8_t stride;
    std::uint16_t limit;
};

void Clear(Stage& stage);
std::uint8_t Add(State& state, const void* words, std::uint32_t count);
Reason Prepare(const Stage& stage, const Plan& plan, std::uint32_t flat_dwords, Fill* out);
std::uint64_t InputsHash(const std::uint32_t* inputs, std::uint32_t count);

inline Reason Value(const State& state, std::uint32_t stage_index, const Fill& fill, std::uint32_t offset, std::uint32_t& value) {
    const Stage& stage = state.stages[stage_index];
    std::uint32_t slot = fill.slot, word = fill.word;
    if (fill.stride) {
        slot = offset / fill.stride;
        word = offset % fill.stride;
        if (slot >= fill.limit) return Outside;
    }
    std::uint8_t at = kUnbound;
    switch (fill.from) {
    case FromView: at = slot < 128 ? stage.views[slot] : kUnbound; break;
    case FromAccess: at = slot < 8 ? stage.access[slot] : kUnbound; break;
    case FromSampler: at = slot < 16 ? stage.samplers[slot] : kUnbound; break;
    case FromConstant: at = slot < 16 ? stage.constants[slot] : kUnbound; break;
    case FromSpill: {
        const std::uint32_t index = fill.slot + offset;
        if (offset >= fill.limit || index >= stage.spill_count) return Outside;
        value = stage.spill[index];
        return Covered;
    }
    case FromShared:
        if (!state.shared_valid) return SharedBlock;
        if (offset >= kSharedWords) return Outside;
        value = state.shared[offset];
        return Covered;
    default:
        return NoInput;
    }
    if (at == kUnbound || at >= state.descriptors) return Unbound;
    value = state.words[at][word];
    return Covered;
}
void ConstantWords(std::uint64_t address, std::uint32_t bytes, std::uint32_t out[4]);

struct Register {
    std::uint16_t index;
    std::uint32_t value;
};
std::uint32_t Registers(const Render& render, Register* out);
std::uint64_t RenderHash(const Render& render);
bool VertexWords(const State& state, std::uint32_t dword_offset, std::uint32_t out[4]);
std::uint64_t VertexKey(const State& state, std::uint32_t dword_offset);
std::uint64_t BufferKey(std::uint64_t object, std::uint32_t version);
const char* RegisterName(std::uint16_t index);
}
