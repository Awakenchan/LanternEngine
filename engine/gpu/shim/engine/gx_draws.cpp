// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_draws.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BbEngine::GxDraws {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr std::size_t kBind = 0x26bc320;
constexpr std::uint8_t kBindEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                       0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x28, 0x01, 0x00, 0x00};
constexpr std::size_t kVertexFlush = 0x26cfcf0, kRingAlloc = 0x26addc0, kRename = 0x26ae840;
constexpr std::size_t kSetPrimitive = 0x1075960, kSetTessellation = 0x10756b0, kOutputFlush = 0x26d2790;
constexpr std::size_t kLearnShader = 0x26d04e0, kSetStages = 0x1076030, kSetFetch = 0x1076390, kSetVertexShader = 0x1075480;
constexpr std::size_t kSetPixelShader = 0x10753c0, kPrefetch = 0x1073f50, kPixelSemantics = 0x10873a0, kSetSemantics = 0x1075000;
constexpr std::size_t kDefaultSampler = 0x546c8b0;

constexpr std::size_t kContextCommands = 0x10, kContextRing = 0xb3e0, kContextRenames = 0xb3c0, kContextOutput = 0xb558;
constexpr std::size_t kContextFlags = 0x38, kContextStages = 0xb657;
constexpr std::size_t kCommandEnd = 0x8, kCommandCursor = 0x10, kCommandReserve = 0x18, kCommandUser = 0x20;
constexpr std::size_t kPendingLayout = 0;
constexpr std::size_t kBlockSamplers = 0, kBlockViews = 0x80, kBlockBuffers = 0x480, kBlockShader = 0x4f0;
constexpr std::size_t kHullShader = 0xa90 + kBlockShader, kGeometryShader = 0x1480 + kBlockShader;
constexpr std::size_t kViewSharp = 0x10, kBufferHolder = 0x18, kBufferSize = 0x2c;
constexpr std::size_t kShaderInputs = 0xe0, kShaderInputCount = 0xe8;
constexpr std::size_t kShaderHasGlobal = 0xd2, kShaderGlobal = 0xf0, kShaderBinary = 0x100, kShaderProgram = 0x108;
constexpr std::size_t kBinaryRegisters = 8, kBinaryUserWords = 3, kBinaryVertexOutputs = 0x24, kBinaryVertexOutputCount = 0x25;
constexpr std::size_t kBinaryVertexTable = 0x28, kBinaryPixelInputCount = 0x38, kBinaryPixelTable = 0x3c;
constexpr std::size_t kLayoutFetch = 0x10, kLayoutElements = 0x20, kLayoutBuffers = 0x28, kLayoutSlots = 0x30, kLayoutProgram = 0x38;
constexpr std::size_t kLayoutModifier = 0x40, kLayoutSlotCount = 0x44, kLayoutEntryCount = 0x45, kLayoutElementCount = 0x46;
constexpr std::size_t kLayoutTableSizes = 0x47, kLayoutDirty = 0x4c, kLayoutUsed = 0x50;

constexpr std::size_t kStateFresh = 0x0, kStateFlag = 0x4, kStatePrimitive = 0x8, kStateTessellation = 0x10;
constexpr std::size_t kStateLayout = 0x25a8, kStateVertexDirty = 0x25a0, kStateVertexMasks = 0x25b0, kStateVertexSlots = 0x25b8;
constexpr std::size_t kStateElementBuffers = 0x2638, kStateElements = 0x26c0, kStateLayoutProgram = 0x28c0, kStateLayoutEntries = 0x28c8;
constexpr std::size_t kStateTableSizes = 0x28cc;
constexpr std::size_t kDirtyViewsLow = 0x18, kDirtyViewsHigh = 0x20, kDirtyOther = 0x28, kDirtyInputs = 0x30, kBoundShader = 0x38;
constexpr std::size_t kUsedViewsLow = 0x60, kUsedViewsHigh = 0x68, kUsedOther = 0x70;
constexpr std::size_t kInputsOfViewLow = 0x78, kInputsOfViewHigh = 0x278, kInputsOfSampler = 0x478, kInputsOfBuffer = 0x4f8;
constexpr std::size_t kInputsOfGlobal = 0x4f8 + 15 * 8;
constexpr u64 kGlobalDirty = 0x80000000ull;
constexpr std::uint8_t kUsesGlobal = 0x80;
constexpr std::size_t kUsedOtherTop = kUsedOther + 3;
constexpr std::size_t kStagesOfVertexOnly = 0;
constexpr std::size_t kSharedBlock = 0xb2a0, kSharedCopy = 0xb360, kSharedSize = 0xc0;
constexpr u64 kAccessMask = 0xff000000000ull, kBufferTableMask = 0xfffff0000ull, kBufferMask = 0x3fff0000ull;
constexpr int kAccessShift = 36, kBufferShift = 16;
constexpr u32 kBufferLiteral = 0x20;
constexpr u32 kBufferWord1 = 0x108000, kBufferWord3 = 0x00077fac;

std::atomic<bool> g_on{false};
u64 g_image = 0;
std::atomic<u64> g_native{0}, g_game{0}, g_words{0};
thread_local u64 g_bound = 0;

template <typename T> T Peek(u64 address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof value);
    return value;
}

template <typename T> void Poke(u64 address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof value);
}

void Count(std::atomic<u64>& counter, u64 by = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

struct Stage {
    std::size_t state, block, views, access, samplers, buffers, spill;
    u32 user_data;
};
constexpr Stage kVertex{0, 0x598, 0x28e0, 0x88e0, 0x94e0, 0x9ae0, 0xa8a0, 0x4c};
constexpr Stage kPixel{0x1900, 0x1978, 0x68e0, 0x90e0, 0x98e0, 0x9fe0, 0xa6a0, 0x0c};

void NoteGlobal(const Stage& stage, u64 state, u64 shader) {
    if (!Peek<std::uint8_t>(shader + kShaderHasGlobal) || !(Peek<std::uint8_t>(state + stage.state + kUsedOtherTop) & kUsesGlobal)) return;
    std::memcpy(reinterpret_cast<void*>(state + stage.buffers + 15 * 16), reinterpret_cast<const void*>(shader + kShaderGlobal), 16);
    Poke<u64>(state + stage.state + kDirtyOther, Peek<u64>(state + stage.state + kDirtyOther) | kGlobalDirty);
    Poke<u64>(state + stage.state + kDirtyInputs, Peek<u64>(state + stage.state + kDirtyInputs) | Peek<u64>(state + stage.state + kInputsOfGlobal));
}

void Prefetch(u64 commands, u64 shader) {
    Call(g_image + kPrefetch, commands, Peek<u64>(shader + kShaderProgram), Peek<u32>(Peek<u64>(shader + kShaderBinary)) & 0x7fffff);
}

void Reset(u64 state, u64 context, u64 commands, u32 stages) {
    Poke<u32>(state + kStatePrimitive, 0xff);
    Poke<u64>(state + kStateTessellation, 0);
    if (stages == 0xad || stages == 0xb0) Poke<std::uint8_t>(context + kContextStages, Peek<std::uint8_t>(context + kContextStages) & 0xfc);
    for (const std::size_t mask : {kDirtyInputs, kDirtyOther, kDirtyViewsHigh, kDirtyViewsLow, kBoundShader}) Poke<u64>(state + mask, ~u64(0));
    Poke<u32>(state + kStateVertexDirty, ~u32(0));
    Poke<u64>(state + kStateLayout, ~u64(0));
    Call(g_image + kSetStages, commands, kStagesOfVertexOnly);
    Poke<u32>(state + kStateFresh, 0);
}

void LearnLayout(u64 state, u64 layout) {
    if (!layout) {
        Poke<u64>(state + kStateVertexMasks, 0);
        Poke<std::uint16_t>(state + kStateTableSizes, 0);
    } else {
        Poke<u32>(state + kStateVertexDirty, ~u32(0));
        Poke<u32>(state + kStateVertexMasks, Peek<u32>(layout + kLayoutDirty));
        Poke<u32>(state + kStateVertexMasks + 4, Peek<u32>(layout + kLayoutUsed));
        Poke<std::uint16_t>(state + kStateTableSizes, Peek<std::uint16_t>(layout + kLayoutTableSizes));
        const u64 slots = Peek<u64>(layout + kLayoutSlots);
        for (u32 i = 0, n = Peek<std::uint8_t>(layout + kLayoutSlotCount); i < n; ++i) {
            const u32 slot = Peek<u32>(slots + u64(i) * 4);
            Poke<u32>(state + kStateVertexSlots + u64((slot >> 16) & 0xff) * 4, slot);
        }
        const u64 elements = Peek<u64>(layout + kLayoutElements), buffers = Peek<u64>(layout + kLayoutBuffers);
        for (u32 i = 0, n = Peek<std::uint8_t>(layout + kLayoutElementCount); i < n; ++i) {
            Poke<u32>(state + kStateElementBuffers + u64(i) * 4, Peek<u32>(buffers + u64(i) * 4));
            std::memcpy(reinterpret_cast<void*>(state + kStateElements + u64(i) * 16), reinterpret_cast<const void*>(elements + u64(i) * 16), 16);
        }
        Poke<u64>(state + kStateLayoutProgram, Peek<u64>(layout + kLayoutProgram));
        Poke<u32>(state + kStateLayoutEntries, Peek<std::uint8_t>(layout + kLayoutEntryCount));
    }
    Poke<u64>(state + kStateLayout, layout);
}

void BindShaders(u64 state, u64 context, u64 commands, u64 pending, u64 vertex, u64 pixel) {
    bool changed = Call(g_image + kLearnShader, state, 0, vertex) & 0xff;
    const u64 layout = Peek<u64>(pending + kPendingLayout);
    const bool same_layout = Peek<u64>(state + kStateLayout) == layout;
    if (!same_layout) LearnLayout(state, layout);
    if (changed || !same_layout) {
        NoteGlobal(kVertex, state, vertex);
        u32 modifier = 0;
        if (layout && Peek<u64>(layout + kLayoutFetch)) {
            Call(g_image + kSetFetch, commands, 2, 0, Peek<u64>(layout + kLayoutFetch));
            modifier = Peek<u32>(layout + kLayoutModifier);
        }
        Call(g_image + kSetVertexShader, commands, Peek<u64>(vertex + kShaderBinary) + kBinaryRegisters, modifier);
        Prefetch(commands, vertex);
        changed = true;
    }
    if (Call(g_image + kLearnShader, state, 4, pixel) & 0xff) {
        if (!pixel) {
            Call(g_image + kSetPixelShader, commands, 0);
            Poke<std::uint8_t>(context + kContextStages, Peek<std::uint8_t>(context + kContextStages) | 1);
        } else {
            NoteGlobal(kPixel, state, pixel);
            Call(g_image + kSetPixelShader, commands, Peek<u64>(pixel + kShaderBinary) + kBinaryRegisters);
            Prefetch(commands, pixel);
            Poke<std::uint8_t>(context + kContextStages, Peek<std::uint8_t>(context + kContextStages) & 0xfe);
            changed = true;
        }
    }
    if (!changed || !pixel) return;
    const u64 binary = Peek<u64>(pixel + kShaderBinary);
    const std::uint8_t inputs = Peek<std::uint8_t>(binary + kBinaryPixelInputCount);
    if (!inputs) return;
    const u64 from = Peek<u64>(vertex + kShaderBinary);
    alignas(16) u32 semantics[64];
    Call(g_image + kPixelSemantics, semantics,
         from + kBinaryVertexTable + (u64(Peek<std::uint8_t>(from + kBinaryVertexOutputs)) + Peek<std::uint8_t>(from + kBinaryUserWords)) * 4,
         Peek<std::uint8_t>(from + kBinaryVertexOutputCount), binary + kBinaryPixelTable + u64(Peek<std::uint8_t>(binary + kBinaryUserWords)) * 4, inputs);
    Call(g_image + kSetSemantics, commands, semantics, inputs);
}

enum InputKind : u32 {
    View4, View8, Access4, Access8, Sampler, Constant, Literal, Shared, ViewTable, AccessTable, SamplerTable, ConstantTable,
    SpillView4 = 13, SpillView8, SpillAccess4, SpillAccess8, SpillSampler, SpillConstant, SpillLiteral, SpillShared,
    SpillViewTable, SpillAccessTable, SpillSamplerTable, SpillConstantTable, Upload = 26
};

constexpr u32 kMaxWords = 2048;
struct Packet {
    u32 words[kMaxWords];
    u32 count = 0;
    void Put(u32 offset, const void* data, u32 dwords) {
        words[count] = offset << 16 | dwords;
        std::memcpy(words + count + 1, data, dwords * sizeof(u32));
        count += dwords + 1;
    }
    void PutPointer(u32 offset, u64 pointer) {
        Put(offset, &pointer, 2);
    }
};

u64 RingAlloc(u64 context, u64 commands, u64 bytes) {
    u64 owner = commands;
    return static_cast<u64>(Call(g_image + kRingAlloc, context + kContextRing, &owner, bytes));
}

u64 CopyToRing(u64 context, u64 commands, u64 source, u64 first, u64 entries, u64 stride) {
    const u64 ring = RingAlloc(context, commands, (first + entries) * stride);
    if (ring) std::memcpy(reinterpret_cast<void*>(ring + first * stride), reinterpret_cast<const void*>(source + first * stride), entries * stride);
    return ring;
}

u64 SharedCopy(u64 state, u64 context, u64 commands) {
    u64 shared = Peek<u64>(state + kSharedCopy);
    if (shared) return shared;
    shared = RingAlloc(context, commands, kSharedSize);
    Poke<u64>(state + kSharedCopy, shared);
    if (shared) std::memcpy(reinterpret_cast<void*>(shared), reinterpret_cast<const void*>(state + kSharedBlock), kSharedSize);
    return shared;
}

bool ViewTableRange(u64 low, u64 high, u64& first, u64& entries) {
    if (!low && !high) return false;
    first = low ? std::countr_zero(low) : 64 + std::countr_zero(high);
    const u64 last = high ? 127 - std::countl_zero(high) : 63 - std::countl_zero(low);
    entries = last + 1 - first;
    return true;
}

u64 InputTable(const Stage& stage, u32 kind, u64 state, u64 context, u64 commands, bool& present) {
    const u64 used = Peek<u64>(state + stage.state + kUsedOther);
    u64 mask = 0, source = 0, stride = 16;
    switch (kind) {
    case ViewTable:
    case SpillViewTable: {
        u64 first = 0, entries = 0;
        present = ViewTableRange(Peek<u64>(state + stage.state + kUsedViewsLow), Peek<u64>(state + stage.state + kUsedViewsHigh), first, entries);
        return present ? CopyToRing(context, commands, state + stage.views, first, entries, 32) : 0;
    }
    case AccessTable:
    case SpillAccessTable:
        mask = (used & kAccessMask) >> kAccessShift;
        source = state + stage.access;
        stride = 32;
        break;
    case SamplerTable:
    case SpillSamplerTable:
        mask = used & 0xffff;
        source = state + stage.samplers;
        break;
    default:
        mask = (used & kBufferTableMask) >> kBufferShift;
        source = state + stage.buffers;
        break;
    }
    present = mask != 0;
    if (!present) return 0;
    const u64 first = std::countr_zero(mask);
    return CopyToRing(context, commands, source, first, 64 - first - std::countl_zero(mask), stride);
}

void WriteInputs(const Stage& stage, u64 state, u64 context, u64 commands, u64 shader, u64 dirty, Packet& packet) {
    const u64 inputs = Peek<u64>(shader + kShaderInputs);
    const u64 spill = state + stage.spill;
    while (dirty) {
        const int index = std::countr_zero(dirty);
        dirty &= dirty - 1;
        const u32 input = Peek<u32>(inputs + 4 * u64(index));
        const u32 kind = input & 0xff, reg = (input >> 8) & 0xff, slot = (input >> 16) & 0xff;
        const u32 offset = stage.user_data + reg;
        const u64 spilled = spill + (u64(reg) - 16) * 4;
        switch (kind) {
        case View4: packet.Put(offset, reinterpret_cast<const void*>(state + stage.views + u64(slot) * 32), 4); break;
        case View8: packet.Put(offset, reinterpret_cast<const void*>(state + stage.views + u64(slot) * 32), 8); break;
        case Access4: packet.Put(offset, reinterpret_cast<const void*>(state + stage.access + u64(slot) * 32), 4); break;
        case Access8: packet.Put(offset, reinterpret_cast<const void*>(state + stage.access + u64(slot) * 32), 8); break;
        case Sampler: packet.Put(offset, reinterpret_cast<const void*>(state + stage.samplers + u64(slot) * 16), 4); break;
        case Constant: packet.Put(offset, reinterpret_cast<const void*>(state + stage.buffers + u64(slot) * 16), 4); break;
        case Literal: packet.Put(offset, &kBufferLiteral, 1); break;
        case Shared: packet.PutPointer(offset, SharedCopy(state, context, commands)); break;
        case ViewTable:
        case AccessTable:
        case SamplerTable:
        case ConstantTable: {
            bool present = false;
            const u64 table = InputTable(stage, kind, state, context, commands, present);
            if (present) packet.PutPointer(offset, table);
            break;
        }
        case SpillView4: std::memcpy(reinterpret_cast<void*>(spilled), reinterpret_cast<const void*>(state + stage.views + u64(slot) * 32), 16); break;
        case SpillView8: std::memcpy(reinterpret_cast<void*>(spilled), reinterpret_cast<const void*>(state + stage.views + u64(slot) * 32), 32); break;
        case SpillAccess4: std::memcpy(reinterpret_cast<void*>(spilled), reinterpret_cast<const void*>(state + stage.access + u64(slot) * 32), 16); break;
        case SpillAccess8: std::memcpy(reinterpret_cast<void*>(spilled), reinterpret_cast<const void*>(state + stage.access + u64(slot) * 32), 32); break;
        case SpillSampler: std::memcpy(reinterpret_cast<void*>(spilled), reinterpret_cast<const void*>(state + stage.samplers + u64(slot) * 16), 16); break;
        case SpillConstant: std::memcpy(reinterpret_cast<void*>(spilled), reinterpret_cast<const void*>(state + stage.buffers + u64(slot) * 16), 16); break;
        case SpillLiteral: Poke<u32>(spilled, kBufferLiteral); break;
        case SpillShared: Poke<u64>(spilled, SharedCopy(state, context, commands)); break;
        case SpillViewTable:
        case SpillAccessTable:
        case SpillSamplerTable:
        case SpillConstantTable: {
            bool present = false;
            const u64 table = InputTable(stage, kind, state, context, commands, present);
            if (present) Poke<u64>(spilled, table);
            break;
        }
        case Upload: {
            const u64 bytes = u64(input >> 24) * 4;
            const u64 ring = RingAlloc(context, commands, bytes);
            if (ring) std::memcpy(reinterpret_cast<void*>(ring), reinterpret_cast<const void*>(spill + u64(slot) * 4), bytes);
            packet.PutPointer(offset, ring);
            break;
        }
        default:
            break;
        }
    }
}

void BindStage(const Stage& stage, u64 state, u64 context, u64 commands, u64 pending, u64 shader, Packet& packet) {
    const u64 masks = state + stage.state;
    const u64 block = pending + stage.block;
    u64 dirty = 0;
    for (u64 bits = Peek<u64>(masks + kDirtyViewsLow) & Peek<u64>(masks + kUsedViewsLow); bits; bits &= bits - 1) {
        const u64 slot = std::countr_zero(bits);
        const u64 view = Peek<u64>(block + kBlockViews + slot * 8);
        if (!view) continue;
        std::memcpy(reinterpret_cast<void*>(state + stage.views + slot * 32), reinterpret_cast<const void*>(view + kViewSharp), 32);
        dirty |= Peek<u64>(masks + kInputsOfViewLow + slot * 8);
    }
    for (u64 bits = Peek<u64>(masks + kDirtyViewsHigh) & Peek<u64>(masks + kUsedViewsHigh); bits; bits &= bits - 1) {
        const u64 slot = std::countr_zero(bits);
        const u64 view = Peek<u64>(block + kBlockViews + (slot + 64) * 8);
        if (!view) continue;
        std::memcpy(reinterpret_cast<void*>(state + stage.views + (slot + 64) * 32), reinterpret_cast<const void*>(view + kViewSharp), 32);
        dirty |= Peek<u64>(masks + kInputsOfViewHigh + slot * 8);
    }
    const u64 other = Peek<u64>(masks + kDirtyOther) & Peek<u64>(masks + kUsedOther);
    for (u64 bits = other & 0xffff; bits; bits &= bits - 1) {
        const u64 slot = std::countr_zero(bits);
        const u64 sampler = Peek<u64>(block + kBlockSamplers + slot * 8);
        std::memcpy(reinterpret_cast<void*>(state + stage.samplers + slot * 16),
                    reinterpret_cast<const void*>(sampler ? sampler : g_image + kDefaultSampler), 16);
        dirty |= Peek<u64>(masks + kInputsOfSampler + slot * 8);
    }
    for (u64 bits = (other & kBufferMask) >> kBufferShift; bits; bits &= bits - 1) {
        const u64 slot = std::countr_zero(bits);
        const u64 buffer = Peek<u64>(block + kBlockBuffers + slot * 8);
        if (!buffer) continue;
        const u64 address = static_cast<u64>(Call(g_image + kRename, context + kContextRenames, buffer + kBufferHolder));
        const u32 sharp[4] = {static_cast<u32>(address), (static_cast<u32>(address >> 32) & 0xfff) | kBufferWord1,
                              (Peek<u32>(buffer + kBufferSize) + 15) >> 4, kBufferWord3};
        std::memcpy(reinterpret_cast<void*>(state + stage.buffers + slot * 16), sharp, sizeof sharp);
        dirty |= Peek<u64>(masks + kInputsOfBuffer + slot * 8);
    }
    dirty |= Peek<u64>(masks + kDirtyInputs);
    Poke<u64>(masks + kDirtyInputs, dirty);
    const u32 count = Peek<u32>(shader + kShaderInputCount);
    if (count < 64) dirty &= (u64(1) << count) - 1;
    if (!dirty) return;
    WriteInputs(stage, state, context, commands, shader, dirty, packet);
    Poke<u64>(masks + kDirtyViewsLow, Peek<u64>(masks + kDirtyViewsLow) & ~Peek<u64>(masks + kUsedViewsLow));
    Poke<u64>(masks + kDirtyViewsHigh, Peek<u64>(masks + kDirtyViewsHigh) & ~Peek<u64>(masks + kUsedViewsHigh));
    Poke<u64>(masks + kDirtyOther, Peek<u64>(masks + kDirtyOther) & ~Peek<u64>(masks + kUsedOther));
    Poke<u64>(masks + kDirtyInputs, 0);
}

void WritePacket(u64 commands, const Packet& packet) {
    if (!packet.count) return;
    const u32 total = packet.count + 2;
    if (((Peek<u64>(commands + kCommandEnd) - Peek<u64>(commands + kCommandCursor)) >> 2) < total) {
        const u64 reserve = Peek<u64>(commands + kCommandReserve);
        if (!(Call(reserve, commands, total, Peek<u64>(commands + kCommandUser)) & 0xff)) return;
    }
    const u64 cursor = Peek<u64>(commands + kCommandCursor);
    auto* out = reinterpret_cast<u32*>(cursor);
    out[0] = 0xc0001000u | ((total - 2) << 16);
    out[1] = kBindTag;
    std::memcpy(out + 2, packet.words, packet.count * sizeof(u32));
    Poke<u64>(commands + kCommandCursor, cursor + u64(total) * 4);
    Count(g_words, total);
}

std::int64_t __attribute__((sysv_abi)) OnBind(std::uint64_t, const std::uint64_t* saved) {
    const u64 state = saved[5], context = saved[4], pending = saved[3];
    const u64 vertex = Peek<u64>(pending + kVertex.block + kBlockShader);
    const u64 pixel = Peek<u64>(pending + kPixel.block + kBlockShader);
    if (Peek<u64>(pending + kHullShader) || Peek<u64>(pending + kGeometryShader) || !vertex) {
        Count(g_game);
        return 0;
    }
    const u64 commands = context + kContextCommands;
    if (Peek<u32>(state + kStateFlag)) {
        Poke<std::uint8_t>(context + kContextFlags, Peek<std::uint8_t>(context + kContextFlags) & 0xfd);
        Poke<u32>(state + kStateFlag, 0);
    }
    if (const u32 stages = Peek<u32>(state + kStateFresh)) Reset(state, context, commands, stages);
    BindShaders(state, context, commands, pending, vertex, pixel);
    Call(g_image + kVertexFlush, state, context, pending, 0, 2);
    Packet packet;
    BindStage(kVertex, state, context, commands, pending, vertex, packet);
    if (pixel) BindStage(kPixel, state, context, commands, pending, pixel, packet);
    WritePacket(commands, packet);
    const u32 primitive = Peek<u32>(state + kStatePrimitive);
    if (primitive != 0xff) Call(g_image + kSetPrimitive, commands, primitive & 0xff, Peek<std::uint8_t>(state + kStatePrimitive + 1));
    const u64 tessellation = Peek<u64>(state + kStateTessellation);
    if (tessellation) Call(g_image + kSetTessellation, commands, tessellation, Peek<u32>(state + kStateTessellation + 4));
    Call(g_image + kOutputFlush, context + kContextOutput, context, pending);
    Count(g_native);
    g_bound = state;
    return 1;
}
}

bool On() {
    return g_on.load(std::memory_order_relaxed);
}

std::uint64_t BoundState() {
    return g_bound;
}

void ForgetBound() {
    g_bound = 0;
}

std::uint64_t SpillArea(std::uint64_t state, int stage) {
    return state + (stage ? kPixel.spill : kVertex.spill);
}

std::uint64_t SharedBlockCopy(std::uint64_t state) {
    return Peek<u64>(state + kSharedCopy);
}

void Report() {
    const u64 native = g_native.load(), game = g_game.load();
    std::printf("GX draws: binds %llu ours, %llu the game's (hull or geometry), %.1f words per bind of ours\n",
                static_cast<unsigned long long>(native), static_cast<unsigned long long>(game), native ? double(g_words.load()) / double(native) : 0.0);
    std::fflush(stdout);
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("LANTERN_GX_DRAWS");
    if (!on || on[0] != '1') {
        host_log("GX draws: off (LANTERN_GX_DRAWS=1 turns it on)");
        return false;
    }
    g_image = reinterpret_cast<u64>(image);
    Hook hook;
    if (!Prepare(hook, image, size, kBind, kBindEntry, OnBind)) {
        host_log("GX draws off: GX's bind differs from 1.09");
        return false;
    }
    g_on.store(true, std::memory_order_relaxed);
    Commit(std::span<Hook>(&hook, 1));
    host_log("GX draws: on (LANTERN_GX_DRAWS=1)");
    return true;
}
}
