// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_records.h"
#include "gx_whole_stage_a.h"
#include "hook_entries.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include "gx_draws.h"
#include "gx_objects.h"
#include "gx_whole.h"
#include "gx_whole_binds.h"
#include "gx_record_hash.h"
#include "pipe_census.h"
#include "bbport_native_calls.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <thread>

namespace BbEngine::GxRecords {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using GxBindings::Reason;

constexpr std::size_t kDraws[4] = {0x21696c0, 0x2169780, 0x2169840, 0x2169900};
constexpr const auto& kPushes = HookEntries::kGxRecordsPushes;
constexpr const auto& kPushesWide = HookEntries::kGxRecordsPushesWide;

constexpr std::size_t kLayout = 0, kTopology = 0x8, kRaster = 0x240, kBlend = 0x4d0, kDepthState = 0x4d8;
constexpr std::size_t kTargets = 0x4e0, kDepthTarget = 0x520, kAccess = 0x528;
constexpr std::size_t kViewports = 0x248, kScissors = 0x3c8, kViewportCount = 0x4c8, kScissorCount = 0x4cc;
constexpr std::size_t kBlendFactor = 0x568, kStencilRef = 0x584;
constexpr std::size_t kVertexBlock = 0x598, kHullBlock = 0xa90, kDomainBlock = 0xf88, kGeometryBlock = 0x1480, kPixelBlock = 0x1978;
constexpr std::size_t kBlockSamplers = 0, kBlockViews = 0x80, kBlockBuffers = 0x480, kBlockShader = 0x4f0;
constexpr std::size_t kBlockBytes = kBlockShader;
constexpr std::size_t kShaderViewsLow = 0, kShaderViewsHigh = 0x8, kShaderOther = 0x10;
constexpr std::size_t kShaderHasGlobal = 0xd2, kShaderGlobal = 0xf0;
constexpr std::size_t kShaderInputs = 0xe0, kShaderInputCount = 0xe8;
constexpr std::size_t kViewSharp = 0x10, kViewResource = 0x8, kResourceBase = 0x20, kBufferHolder = 0x18, kBufferSize = 0x2c;
constexpr std::size_t kRename = 0x26ae840, kContextRenames = 0xb3c0;
constexpr std::size_t kTargetValid = 0x1086ca0, kDepthValid = 0x1072770, kStencilValid = 0x1072790;
constexpr std::size_t kIndexBuffer = 0x10, kIndexFormat = 0x18, kIndexOffset = 0x1c, kResourceDynamic = 0x40;
constexpr std::size_t kVertexTable = 0xa260, kVertexFirst = 0x28cc, kVertexCount = 0x28cd;
constexpr std::size_t kVertexBuffers = 0x20, kSlotElements = 0x25b8, kElementTable = 0x2638;
constexpr u32 kVertexSlots = 32, kElementCount = 32;
constexpr std::size_t kBufferVtable = 0x535f3a0;
constexpr std::size_t kDefaultSampler = 0x546c8b0, kDefaultBlend = 0x546c8f4, kDefaultDepthState = 0x546ca1c, kDefaultRaster = 0x546ca58;
constexpr u64 kAccessMask = 0xff000000000ull, kBufferMask = 0x3fff0000ull, kGlobalUsed = 0x80000000ull;
constexpr int kAccessShift = 36, kBufferShift = 16;
constexpr u32 kGlobalSlot = 15;
constexpr u32 kMaxInputs = 256;
constexpr std::size_t kDepthBounds = 0x578;
constexpr std::size_t kShaderBinary = 0x100, kBinaryRegisters = 8, kBinaryUserWords = 3, kBinaryVertexOutputs = 0x24;
constexpr std::size_t kBinaryVertexOutputCount = 0x25, kBinaryVertexTable = 0x28, kBinaryPixelInputCount = 0x38, kBinaryPixelTable = 0x3c;
constexpr std::size_t kLayoutFetch = 0x10, kLayoutModifier = 0x40;
constexpr std::size_t kContextOutput = 0xb558, kOutputDepthClear = 0xf8, kOutputStencilClear = 0xfc;
constexpr u32 kWholePublishEvery = 256, kEvidenceBytes = 256;

constexpr std::size_t kDynamicUpdate = 0x26adb90, kDefaultUpdate = 0x26b3560;
constexpr const auto& kUpdateEntry = HookEntries::kGxRecordsUpdateEntry;
constexpr const auto& kDefaultUpdateEntry = HookEntries::kGxRecordsDefaultUpdateEntry;
constexpr u32 kVersionBits = 16;

constexpr u32 kSlotBits = 14, kPoolBits = 13;
using Slot = GxWhole::RecordSlot;
struct Payload {
    std::atomic<u64> sequence{0};
    GxBindings::State state;
};
std::unique_ptr<Slot[]> g_slots;
std::unique_ptr<Payload[]> g_pool;
std::atomic<u64> g_next{0};
std::atomic<bool> g_on{false}, g_checked{false}, g_compared{false}, g_used{false};
u64 g_image = 0;
std::unique_ptr<std::atomic<u32>[]> g_versions;
std::uint8_t* g_dynamic_update = nullptr;
std::atomic<bool> g_constants{false}, g_updates{false}, g_binds_by_object{false};
std::atomic<GxWhole::Mode> g_whole{GxWhole::Mode::Off};
std::atomic<GxWhole::Mode> g_whole_requested{GxWhole::Mode::Off};
std::atomic<bool> g_whole_counting{false};

struct WholeTally {
    GxWhole::Counts counts;
    GxWhole::KeptMemory kept;
    std::array<bool, GxWhole::kFields> logged{};
    std::array<bool, GxWhole::kUnusedClasses> unused_logged{};
    u64 seen = 0;
    u32 unpublished = 0;
    std::thread::id writer{};
};
WholeTally g_whole_tally;
std::array<std::atomic<u64>, GxWhole::kCountWords> g_whole_published{};

struct Current {
    bool set = false;
    u32 count = 0;
    u64 record = 0, pending = 0, context = 0;
};
thread_local Current g_current;
thread_local u64 g_taken_sequence = 0;

struct Tally {
    u64 constants = 0, reused = 0, constants_checked = 0, constants_differ = 0;
    u64 binds = 0, binds_reused = 0, binds_checked = 0, binds_differ = 0;
    u64 draws = 0, covered = 0, kept_words = 0, render = 0, no_render = 0;
    u64 taken = 0, stale = 0, lost = 0, unstamped = 0, weak = 0, weak_taken = 0;
    u64 reasons[GxBindings::kReasons]{};
    u64 words = 0, words_differ = 0;
    u64 registers = 0, registers_differ = 0;
    u64 targets = 0, targets_differ = 0;
    u32 logged = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
Tally g_tally;

template <typename T> T Peek(u64 address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof value);
    return value;
}

using GxRecordHash::Mix;

u64 MixBlock(u64 hash, u64 at) {
    return GxRecordHash::Block(hash, reinterpret_cast<const void*>(at), kBlockBytes / 8);
}

std::atomic<u32>& VersionOf(u64 address) {
    return g_versions[((address >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kVersionBits)];
}

u32 Version(u64 address) {
    return g_versions ? VersionOf(address).load(std::memory_order_acquire) : 0;
}

Slot& SlotFor(u64 address) {
    return g_slots[((address >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - kSlotBits)];
}

u64 MixStage(u64 hash, u64 pending, std::size_t block) {
    const u64 at = pending + block;
    const u64 shader = Peek<u64>(at + kBlockShader);
    hash = Mix(hash, shader);
    if (!shader) return hash;
    const u64 inputs = Peek<u64>(shader + kShaderInputs);
    const u32 count = Peek<u32>(shader + kShaderInputCount);
    if (!inputs || count > kMaxInputs) return MixBlock(hash, at);
    bool whole = false;
    for (u32 i = 0; i < count && !whole; ++i) {
        const u32 word = Peek<u32>(inputs + u64(i) * 4);
        const u32 kind = word & 0xff, slot = (word >> 16) & 0xff;
        switch (kind) {
        case 0:
        case 1:
            hash = Mix(hash, Peek<u64>(at + kBlockViews + 8 * u64(slot & 0x7f)));
            break;
        case 2:
        case 3:
            hash = Mix(hash, Peek<u64>(pending + kAccess + 8 * u64(slot & 7)));
            break;
        case 4:
            hash = Mix(hash, Peek<u64>(at + kBlockSamplers + 8 * u64(slot & 0xf)));
            break;
        case 5:
            hash = Mix(hash, Peek<u64>(at + kBlockBuffers + 8 * u64(slot & 0xf)));
            break;
        default:
            whole = kind >= 8;
            break;
        }
    }
    return whole ? MixBlock(hash, at) : hash;
}

u64 RecordOf(u64 pending) {
    if (Peek<u64>(pending + kHullBlock + kBlockShader) || Peek<u64>(pending + kDomainBlock + kBlockShader) ||
        Peek<u64>(pending + kGeometryBlock + kBlockShader)) {
        return 0;
    }
    u64 hash = Mix(0xCBF29CE484222325ull, GxObjects::Epoch());
    for (const std::size_t state : {kLayout, kTopology, kRaster, kBlend, kDepthState, kDepthTarget}) hash = Mix(hash, Peek<u64>(pending + state));
    for (int i = 0; i < 8; ++i) hash = Mix(hash, Peek<u64>(pending + kTargets + 8 * i));
    hash = MixStage(hash, pending, kVertexBlock);
    hash = MixStage(hash, pending, kPixelBlock);
    return hash ? hash : 1;
}

u64 ViewBase(u64 view) {
    if (view + 1 <= 1) return 0;
    const u64 resource = Peek<u64>(view + kViewResource);
    return resource ? Peek<u64>(resource + kResourceBase) : 0;
}

void MarkDirect(GxBindings::Stage& stage, u64& views_low, u64& views_high, u64& other) {
    for (u32 i = 0; i < stage.count; ++i) {
        const u32 input = stage.inputs[i];
        const u32 kind = input & 0xff, slot = (input >> 16) & 0xff;
        u32 base = kind;
        if (kind >= GxBindings::SpillView4 && kind <= GxBindings::SpillConstant) base = kind - GxBindings::SpillView4;
        else if (kind > GxBindings::Constant) continue;
        switch (base) {
        case GxBindings::View4:
        case GxBindings::View8:
            if (slot < 64) views_low |= u64(1) << slot;
            else if (slot < 128) views_high |= u64(1) << (slot - 64);
            break;
        case GxBindings::Access4:
        case GxBindings::Access8:
            if (slot < 8) other |= u64(1) << (kAccessShift + slot);
            break;
        case GxBindings::Sampler:
            if (slot < 16) other |= u64(1) << slot;
            break;
        case GxBindings::Constant:
            if (slot < 14) other |= u64(1) << (kBufferShift + slot);
            break;
        default:
            break;
        }
    }
}

void CaptureSpill(GxBindings::Stage& stage, u64 bound, int index) {
    u32 need = 0;
    for (u32 i = 0; i < stage.count; ++i) {
        const u32 input = stage.inputs[i];
        if ((input & 0xff) != GxBindings::Upload) continue;
        const u32 end = ((input >> 16) & 0xff) + (input >> 24);
        need = end > need ? end : need;
    }
    if (!bound || !need) return;
    stage.spill_count = need < GxBindings::kMaxSpill ? need : GxBindings::kMaxSpill;
    std::memcpy(stage.spill, reinterpret_cast<const void*>(GxDraws::SpillArea(bound, index)), stage.spill_count * sizeof(u32));
}

void NoteViewKey(GxBindings::State& state, u64 view) {
    if (!g_binds_by_object.load(std::memory_order_relaxed) || state.num_view_keys >= GxBindings::kMaxViewKeys) return;
    const u32 word1 = Peek<u32>(view + kViewSharp + 4), word3 = Peek<u32>(view + kViewSharp + 12);
    if (word3 >> 30) return;
    const u64 resource = Peek<u64>(view + kViewResource);
    if (!resource || Peek<u64>(resource) != g_image + kBufferVtable || Peek<std::uint8_t>(resource + kResourceDynamic)) return;
    const u64 address = u64(Peek<u32>(view + kViewSharp)) | (u64(word1 & 0xfff) << 32);
    if (!address) return;
    state.view_keys[state.num_view_keys][0] = address;
    state.view_keys[state.num_view_keys][1] = GxBindings::BufferKey(resource, GxObjects::BufferVersion(resource));
    ++state.num_view_keys;
}

void CaptureStage(GxBindings::State& state, GxBindings::Stage& stage, u64 pending, u64 context, std::size_t block, u64 bound, int index) {
    GxBindings::Clear(stage);
    const u64 at = pending + block;
    const u64 shader = Peek<u64>(at + kBlockShader);
    if (!shader) return;
    stage.shader = shader;
    const u64 inputs = Peek<u64>(shader + kShaderInputs);
    const u32 count = Peek<u32>(shader + kShaderInputCount);
    if (count > GxBindings::kMaxInputs || (count && !inputs)) {
        state.reason = GxBindings::ManyInputs;
        return;
    }
    stage.count = count;
    if (count) std::memcpy(stage.inputs, reinterpret_cast<const void*>(inputs), count * sizeof(u32));
    stage.inputs_hash = GxBindings::InputsHash(stage.inputs, count);
    u64 views_low = Peek<u64>(shader + kShaderViewsLow), views_high = Peek<u64>(shader + kShaderViewsHigh);
    u64 other = Peek<u64>(shader + kShaderOther);
    MarkDirect(stage, views_low, views_high, other);
    for (u32 half = 0; half < 2; ++half) {
        for (u64 bits = half ? views_high : views_low; bits; bits &= bits - 1) {
            const u32 slot = half * 64 + static_cast<u32>(__builtin_ctzll(bits));
            const u64 view = Peek<u64>(at + kBlockViews + 8 * u64(slot));
            if (view + 1 <= 1) continue;
            stage.views[slot] = GxBindings::Add(state, reinterpret_cast<const void*>(view + kViewSharp), 8);
            NoteViewKey(state, view);
        }
    }
    for (u64 bits = (other & kAccessMask) >> kAccessShift; bits; bits &= bits - 1) {
        const u32 slot = static_cast<u32>(__builtin_ctzll(bits));
        const u64 view = Peek<u64>(pending + kAccess + 8 * u64(slot));
        if (view + 1 <= 1) continue;
        stage.access[slot] = GxBindings::Add(state, reinterpret_cast<const void*>(view + kViewSharp), 8);
    }
    for (u64 bits = other & 0xffff; bits; bits &= bits - 1) {
        const u32 slot = static_cast<u32>(__builtin_ctzll(bits));
        const u64 sampler = Peek<u64>(at + kBlockSamplers + 8 * u64(slot));
        stage.samplers[slot] = GxBindings::Add(state, reinterpret_cast<const void*>(sampler ? sampler : g_image + kDefaultSampler), 4);
    }
    for (u64 bits = (other & kBufferMask) >> kBufferShift; bits; bits &= bits - 1) {
        const u32 slot = static_cast<u32>(__builtin_ctzll(bits));
        const u64 buffer = Peek<u64>(at + kBlockBuffers + 8 * u64(slot));
        if (!buffer) continue;
        const u64 address = static_cast<u64>(Call(g_image + kRename, context + kContextRenames, buffer + kBufferHolder));
        u32 words[4];
        GxBindings::ConstantWords(address, Peek<u32>(buffer + kBufferSize), words);
        stage.constants[slot] = GxBindings::Add(state, words, 4);
        stage.constant_versions[slot] = Version(address);
    }
    if ((other & kGlobalUsed) && Peek<std::uint8_t>(shader + kShaderHasGlobal)) {
        stage.constants[kGlobalSlot] = GxBindings::Add(state, reinterpret_cast<const void*>(shader + kShaderGlobal), 4);
    }
    CaptureSpill(stage, bound, index);
}

void CaptureSemantics(GxBindings::Whole& whole, u64 vertex_binary, u64 pixel_binary) {
    const u32 inputs = Peek<std::uint8_t>(pixel_binary + kBinaryPixelInputCount);
    const u32 outputs = vertex_binary ? Peek<std::uint8_t>(vertex_binary + kBinaryVertexOutputCount) : 0;
    whole.semantics_valid = vertex_binary && inputs <= GxBindings::kMaxSemantics && outputs <= GxBindings::kMaxSemantics;
    whole.vertex_semantics_count = whole.pixel_semantics_count = 0;
    if (!whole.semantics_valid) return;
    const u64 skipped = u64(Peek<std::uint8_t>(vertex_binary + kBinaryVertexOutputs)) + Peek<std::uint8_t>(vertex_binary + kBinaryUserWords);
    std::memcpy(whole.vertex_semantics, reinterpret_cast<const void*>(vertex_binary + kBinaryVertexTable + skipped * 4), u64(outputs) * 2);
    const u64 pixel_table = pixel_binary + kBinaryPixelTable + u64(Peek<std::uint8_t>(pixel_binary + kBinaryUserWords)) * 4;
    std::memcpy(whole.pixel_semantics, reinterpret_cast<const void*>(pixel_table), u64(inputs) * 2);
    whole.vertex_semantics_count = outputs;
    whole.pixel_semantics_count = inputs;
}

void CaptureWhole(GxBindings::Whole& whole, u64 pending, u64 context) {
    whole.topology = Peek<u32>(pending + kTopology);
    std::memcpy(whole.depth_bounds, reinterpret_cast<const void*>(pending + kDepthBounds), sizeof whole.depth_bounds);
    whole.index_bound = Peek<u64>(pending + kIndexBuffer) != 0;
    const u64 vertex = Peek<u64>(pending + kVertexBlock + kBlockShader);
    const u64 vertex_binary = vertex ? Peek<u64>(vertex + kShaderBinary) : 0;
    whole.vs_bound = vertex_binary != 0;
    if (vertex_binary) std::memcpy(whole.vs_registers, reinterpret_cast<const void*>(vertex_binary + kBinaryRegisters), sizeof whole.vs_registers);
    const u64 layout = Peek<u64>(pending + kLayout);
    whole.vs_modifier = layout && Peek<u64>(layout + kLayoutFetch) ? Peek<u32>(layout + kLayoutModifier) : 0;
    const u64 pixel = Peek<u64>(pending + kPixelBlock + kBlockShader);
    const u64 pixel_binary = pixel ? Peek<u64>(pixel + kShaderBinary) : 0;
    whole.ps_bound = pixel_binary != 0;
    whole.semantics_valid = 0;
    if (pixel_binary) {
        std::memcpy(whole.ps_registers, reinterpret_cast<const void*>(pixel_binary + kBinaryRegisters), sizeof whole.ps_registers);
        CaptureSemantics(whole, vertex_binary, pixel_binary);
    }
    whole.depth_clear = Peek<float>(context + kContextOutput + kOutputDepthClear);
    whole.stencil_clear = Peek<std::uint8_t>(context + kContextOutput + kOutputStencilClear);
    whole.captured = 1;
}

void Capture(GxBindings::State& state, u64 pending, u64 context, u64 bound) {
    state.whole.captured = 0;
    if (g_whole.load(std::memory_order_relaxed) != GxWhole::Mode::Off) CaptureWhole(state.whole, pending, context);
    state.reason = GxBindings::Covered;
    state.descriptors = 0;
    state.num_view_keys = 0;
    CaptureStage(state, state.stages[0], pending, context, kVertexBlock, bound, 0);
    CaptureStage(state, state.stages[1], pending, context, kPixelBlock, bound, 1);
    const u64 shared = bound ? GxDraws::SharedBlockCopy(bound) : 0;
    state.shared_valid = shared != 0;
    if (shared) std::memcpy(state.shared, reinterpret_cast<const void*>(shared), sizeof state.shared);
    GxBindings::Render& render = state.render;
    const u64 raster = Peek<u64>(pending + kRaster), blend = Peek<u64>(pending + kBlend), depth = Peek<u64>(pending + kDepthState);
    std::memcpy(render.raster, reinterpret_cast<const void*>(raster ? raster : g_image + kDefaultRaster), sizeof render.raster);
    std::memcpy(render.blend, reinterpret_cast<const void*>(blend ? blend : g_image + kDefaultBlend), sizeof render.blend);
    std::memcpy(render.depth_stencil, reinterpret_cast<const void*>(depth ? depth : g_image + kDefaultDepthState),
                sizeof render.depth_stencil);
    std::memcpy(render.blend_factor, reinterpret_cast<const void*>(pending + kBlendFactor), sizeof render.blend_factor);
    render.stencil_ref = Peek<std::uint8_t>(pending + kStencilRef);
    render.viewports = Peek<u32>(pending + kViewportCount);
    render.scissors = Peek<u32>(pending + kScissorCount);
    std::memcpy(render.viewport, reinterpret_cast<const void*>(pending + kViewports), sizeof render.viewport);
    std::memcpy(render.scissor, reinterpret_cast<const void*>(pending + kScissors), sizeof render.scissor);
    render.color_valid = 0;
    state.objects_epoch = GxObjects::Epoch();
    for (int i = 0; i < 8; ++i) {
        const u64 view = Peek<u64>(pending + kTargets + 8 * i);
        state.target_views[i] = view + 1 <= 1 ? 0 : view;
        render.color[i] = ViewBase(view);
        if (view + 1 <= 1 || !(Call(g_image + kTargetValid, view + kViewSharp) & 0xffffffff)) continue;
        render.color_valid |= 1u << i;
        std::memcpy(render.targets[i], reinterpret_cast<const void*>(view + kViewSharp), sizeof render.targets[i]);
    }
    const u64 depth_view = Peek<u64>(pending + kDepthTarget);
    state.target_views[GxBindings::kTargetViews - 1] = depth_view + 1 <= 1 ? 0 : depth_view;
    render.depth = ViewBase(depth_view);
    render.depth_valid = depth_view + 1 > 1 && ((Call(g_image + kDepthValid, depth_view + kViewSharp) & 0xffffffff) ||
                                                 (Call(g_image + kStencilValid, depth_view + kViewSharp) & 0xffffffff));
    if (render.depth_valid) {
        std::memcpy(render.depth_target, reinterpret_cast<const void*>(depth_view + kViewSharp), sizeof render.depth_target);
    }
    const u64 index = Peek<u64>(pending + kIndexBuffer);
    render.index_known = index && !Peek<std::uint8_t>(index + kResourceDynamic);
    render.index_format = Peek<u32>(pending + kIndexFormat);
    render.index_base = render.index_known ? Peek<u64>(index + kResourceBase) + Peek<u32>(pending + kIndexOffset) : 0;
    state.vertex_first = state.vertex_count = 0;
    if (bound) {
        const u32 first = Peek<std::uint8_t>(bound + kVertexFirst), count = Peek<std::uint8_t>(bound + kVertexCount);
        if (first + count <= GxBindings::kMaxVertex) {
            state.vertex_first = first;
            state.vertex_count = count;
            std::memcpy(state.vertex[first], reinterpret_cast<const void*>(bound + kVertexTable + u64(first) * 16), u64(count) * 16);
        }
    }
    std::memset(state.vertex_keys, 0, sizeof state.vertex_keys);
    state.index_key = 0;
    if (!g_updates.load(std::memory_order_relaxed)) return;
    state.index_key = render.index_known ? GxBindings::BufferKey(index, GxObjects::BufferVersion(index)) : 0;
    if (!bound || !state.vertex_count) return;
    for (u32 slot = 0; slot < kVertexSlots; ++slot) {
        const u64 buffer = Peek<u64>(pending + kVertexBuffers + 8 * u64(slot));
        if (!buffer || Peek<std::uint8_t>(buffer + kResourceDynamic)) continue;
        const u32 range = Peek<u32>(bound + kSlotElements + 4 * u64(slot));
        const u32 first = range & 0xff, count = (range >> 8) & 0xff;
        const u64 key = GxBindings::BufferKey(buffer, GxObjects::BufferVersion(buffer));
        for (u32 k = first; k < first + count && k < kElementCount; ++k) {
            const u32 entry = Peek<u32>(bound + kElementTable + 4 * u64(k)) & 0xff;
            if (entry < GxBindings::kMaxVertex) state.vertex_keys[entry] = key;
        }
    }
}

Payload& PayloadOf(u64 sequence) {
    return g_pool[sequence & ((u64(1) << kPoolBits) - 1)];
}

void Report() {
    Tally& t = g_tally;
    std::printf("GX records: %llu draws with a record, bindings from the record for %llu (%llu vertex-stream words kept)",
                static_cast<unsigned long long>(t.draws), static_cast<unsigned long long>(t.covered),
                static_cast<unsigned long long>(t.kept_words));
    for (u32 r = 1; r < GxBindings::kReasons; ++r) {
        if (t.reasons[r]) std::printf(", %s %llu", GxBindings::ReasonName(static_cast<Reason>(r)), static_cast<unsigned long long>(t.reasons[r]));
    }
    std::printf("; render state from the record for %llu, %llu without it\n", static_cast<unsigned long long>(t.render),
                static_cast<unsigned long long>(t.no_render));
    std::printf("GX records: keys %llu taken, %llu stale (address stamped again), %llu lost (slot gone), %llu unstamped; "
                "%llu indexed draws carrying base vertex and instances keyed by check word (%llu taken)\n",
                static_cast<unsigned long long>(t.taken), static_cast<unsigned long long>(t.stale),
                static_cast<unsigned long long>(t.lost), static_cast<unsigned long long>(t.unstamped),
                static_cast<unsigned long long>(t.weak), static_cast<unsigned long long>(t.weak_taken));
    if (t.binds) {
        std::printf("GX records: vertex and index buffers %llu bound, %llu by object (%llu checked, %llu differ)\n",
                    static_cast<unsigned long long>(t.binds), static_cast<unsigned long long>(t.binds_reused),
                    static_cast<unsigned long long>(t.binds_checked), static_cast<unsigned long long>(t.binds_differ));
    }
    if (t.constants) {
        std::printf("GX records: constants %llu looked up, %llu reused from the ring (%llu checked, %llu differ)\n",
                    static_cast<unsigned long long>(t.constants), static_cast<unsigned long long>(t.reused),
                    static_cast<unsigned long long>(t.constants_checked), static_cast<unsigned long long>(t.constants_differ));
    }
    if (Compared() || t.words || t.registers || t.targets) {
        std::printf("GX records: compared %llu binding words (%llu differ), %llu registers (%llu differ), %llu targets (%llu differ)\n",
                    static_cast<unsigned long long>(t.words), static_cast<unsigned long long>(t.words_differ),
                    static_cast<unsigned long long>(t.registers), static_cast<unsigned long long>(t.registers_differ),
                    static_cast<unsigned long long>(t.targets), static_cast<unsigned long long>(t.targets_differ));
    }
    std::fflush(stdout);
}

bool LogDifference() {
    if (g_tally.logged >= 48) return false;
    ++g_tally.logged;
    return true;
}

std::int64_t __attribute__((sysv_abi)) OnDynamicUpdate(std::uint64_t, std::uint64_t* saved) noexcept {
    Call(reinterpret_cast<u64>(g_dynamic_update), saved[5], saved[4], saved[3], saved[2], saved[1]);
    const u64 address = static_cast<u64>(Call(g_image + kRename, saved[5], saved[2] + kBufferHolder));
    if (address) VersionOf(address).fetch_add(1, std::memory_order_release);
    GxObjects::TouchBuffer(saved[2]);
    saved[-1] = 0;
    return 1;
}

std::int64_t __attribute__((sysv_abi)) OnDefaultUpdate(std::uint64_t, std::uint64_t* saved) noexcept {
    const u64 address = Peek<u64>(saved[4] + 8);
    if (address) VersionOf(address).fetch_add(1, std::memory_order_release);
    GxObjects::TouchBuffer(saved[4] - kBufferHolder);
    return 0;
}

std::int64_t __attribute__((sysv_abi)) OnDraw(std::uint64_t, std::uint64_t* saved) noexcept {
    Current& current = g_current;
    GxDraws::ForgetBound();
    current.pending = saved[4];
    current.context = saved[5];
    current.record = RecordOf(current.pending);
    current.count = static_cast<u32>(saved[3]);
    current.set = current.record != 0;
    return 0;
}
}

bool On() {
    return g_on.load(std::memory_order_relaxed);
}

bool Checked() {
    return g_checked.load(std::memory_order_relaxed);
}

bool Compared() {
    return g_compared.load(std::memory_order_relaxed);
}

bool Used() {
    return g_used.load(std::memory_order_relaxed);
}

std::uint64_t Stamped() {
    return g_next.load(std::memory_order_relaxed);
}

void Stamp(std::uint64_t record_address, std::uint32_t count) {
    Current& current = g_current;
    auto* packet = reinterpret_cast<u32*>(record_address);
    Slot& slot = SlotFor(record_address);
    if (!current.set || current.count != count) {
        GxWhole::Forget(slot, record_address);
        return;
    }
    current.set = false;
    u32 stamp_slot = 0, words = 0;
    if (!BbNativeCalls::RecordLayout(packet, stamp_slot, words)) {
        GxWhole::Forget(slot, record_address);
        return;
    }
    u64 sequence = g_next.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!static_cast<u32>(sequence)) sequence = g_next.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_pool) {
        Payload& payload = PayloadOf(sequence);
        payload.sequence.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        Capture(payload.state, current.pending, current.context, GxDraws::BoundState());
        payload.state.record = current.record;
        payload.sequence.store(sequence, std::memory_order_release);
    }
    const u32 key = stamp_slot ? static_cast<u32>(sequence) : BbNativeCalls::TokenKey(packet, stamp_slot, words);
    GxWhole::Publish(slot, record_address, current.record, sequence, key);
    if (stamp_slot) BbNativeCalls::Seal(packet, stamp_slot, words, key);
}

bool Pending(std::uint64_t record_address) {
    if (!g_slots) return false;
    const Slot& slot = SlotFor(record_address);
    return slot.sequence.load(std::memory_order_acquire) != 0 && slot.address.load(std::memory_order_relaxed) == record_address;
}

std::uint64_t Take(std::uint64_t record_address) {
    g_taken_sequence = 0;
    Tally& t = g_tally;
    const auto* packet = reinterpret_cast<const u32*>(record_address);
    u32 stamp_slot = 0, words = 0;
    if (!BbNativeCalls::RecordLayout(packet, stamp_slot, words)) return 0;
    if (!stamp_slot) ++t.weak;
    const GxWhole::Taking taking = GxWhole::Consume(SlotFor(record_address), record_address, BbNativeCalls::TokenKey(packet, stamp_slot, words));
    switch (taking.key) {
    case GxWhole::Key::Unstamped: ++t.unstamped; return 0;
    case GxWhole::Key::Lost: ++t.lost; return 0;
    case GxWhole::Key::Stale: ++t.stale; return 0;
    case GxWhole::Key::Taken: break;
    }
    ++t.taken;
    if (!stamp_slot) ++t.weak_taken;
    if (g_pool) g_taken_sequence = taking.sequence;
    return taking.record;
}

const GxBindings::State* Taken(std::uint64_t record) {
    if (!g_taken_sequence || !record) return nullptr;
    Payload& payload = PayloadOf(g_taken_sequence);
    if (payload.sequence.load(std::memory_order_acquire) != g_taken_sequence || payload.state.record != record) return nullptr;
    return &payload.state;
}

bool Intact() {
    std::atomic_thread_fence(std::memory_order_acquire);
    return g_taken_sequence && PayloadOf(g_taken_sequence).sequence.load(std::memory_order_relaxed) == g_taken_sequence;
}

void NoteDraw(GxBindings::Reason bindings, std::uint32_t kept, bool render) {
    Tally& t = g_tally;
    ++t.draws;
    if (bindings == GxBindings::Covered) ++t.covered;
    else if (bindings < GxBindings::kReasons) ++t.reasons[bindings];
    t.kept_words += kept;
    ++(render ? t.render : t.no_render);
    if (t.draws & 1023) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - t.last >= std::chrono::seconds(10)) {
        t.last = now;
        Report();
    }
}

void NoteWord(std::uint32_t stage, std::uint32_t flat, std::uint32_t ours, std::uint32_t old, std::uint64_t program) {
    ++g_tally.words;
    if (ours == old) return;
    ++g_tally.words_differ;
    if (LogDifference()) {
        std::printf("GX records: binding differs, shader %016llx stage %u flat word %u: record %08x, old path %08x\n",
                    static_cast<unsigned long long>(program), stage, flat, ours, old);
    }
}

void NoteRegister(std::uint16_t index, std::uint32_t ours, std::uint32_t old) {
    ++g_tally.registers;
    if (ours == old) return;
    ++g_tally.registers_differ;
    if (LogDifference()) {
        std::printf("GX records: register %s (%#x) differs: record %08x, registers %08x\n", GxBindings::RegisterName(index), index,
                    ours, old);
    }
}

bool ConstantsTracked() {
    return g_constants.load(std::memory_order_relaxed);
}

static bool BindsOff() {
    static const bool off = [] {
        const char* value = std::getenv("LANTERN_GX_BINDS");
        return value && value[0] == '0';
    }();
    return off;
}

bool KeysCaptured() {
    return !BindsOff() && g_updates.load(std::memory_order_relaxed);
}

bool BindsByObject() {
    return KeysCaptured() && g_binds_by_object.load(std::memory_order_relaxed);
}

void NoteBind(bool reused, bool checked, bool same, std::uint64_t address, std::uint64_t size) {
    Tally& t = g_tally;
    ++t.binds;
    t.binds_reused += reused;
    if (!checked) return;
    ++t.binds_checked;
    if (same) return;
    ++t.binds_differ;
    if (LogDifference()) {
        std::printf("GX records: a buffer bound by object differs from the address path at %#llx, %llu bytes\n",
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(size));
    }
}

void NoteConstant(bool reused, bool checked, bool same, std::uint64_t address, std::uint32_t size) {
    Tally& t = g_tally;
    ++t.constants;
    t.reused += reused;
    if (!checked) return;
    ++t.constants_checked;
    if (same) return;
    ++t.constants_differ;
    if (LogDifference()) {
        std::printf("GX records: reused constants differ from memory at %#llx, %u bytes\n", static_cast<unsigned long long>(address), size);
    }
}

void NoteTarget(int target, std::uint64_t ours, std::uint64_t old) {
    ++g_tally.targets;
    if (ours == old) return;
    ++g_tally.targets_differ;
    if (LogDifference()) {
        std::printf("GX records: %s target %d differs: record %#llx, registers %#llx\n", target < 0 ? "depth" : "color", target,
                    static_cast<unsigned long long>(ours), static_cast<unsigned long long>(old));
    }
}

namespace {
void PublishWhole(const GxWhole::Counts& counts) {
    std::array<u64, GxWhole::kCountWords> words;
    std::memcpy(words.data(), &counts, sizeof counts);
    for (std::size_t i = 0; i < words.size(); ++i) g_whole_published[i].store(words[i], std::memory_order_relaxed);
}

void WriteLine(const char* text, int length) {
    if (length <= 0) return;
    const std::size_t bytes = std::min<std::size_t>(std::size_t(length), kEvidenceBytes - 1);
    if (std::fwrite(text, 1, bytes, stdout) != bytes) return;
}

void ReportWholeEvidence(WholeTally& t, std::span<const GxWhole::Difference> differences, u64 record, GxWhole::Mode mode) {
    for (const GxWhole::Difference& d : differences) {
        if (t.logged[d.field]) continue;
        t.logged[d.field] = true;
        char line[kEvidenceBytes];
        const int length = std::snprintf(line, sizeof line,
                                         "GX whole: first difference in %s at register %#x: record %08x, registers %08x (covered draw %llu, record %016llx, LANTERN_GX_WHOLE=%d)\n",
                                         GxWhole::kFieldInfo[d.field].name, d.reg, d.ours, d.registers,
                                         static_cast<unsigned long long>(t.seen), static_cast<unsigned long long>(record),
                                         GxWhole::ModeNumber(mode));
        WriteLine(line, length);
    }
}

void ReportKeptChanges(WholeTally& t, GxWhole::Mode mode) {
    for (u32 f = 0; f < GxWhole::kFields; ++f) {
        if (t.counts.field_changed[f] != 1 || t.logged[f]) continue;
        t.logged[f] = true;
        char line[kEvidenceBytes];
        const int length = std::snprintf(line, sizeof line,
                                         "GX whole: kept field %s (registers %#x, %u words) changed between two covered draws (covered draw %llu, LANTERN_GX_WHOLE=%d)\n",
                                         GxWhole::kFieldInfo[f].name, GxWhole::kFieldInfo[f].first, GxWhole::kFieldInfo[f].count,
                                         static_cast<unsigned long long>(t.seen), GxWhole::ModeNumber(mode));
        WriteLine(line, length);
    }
}

void CompareWhole(WholeTally& t, const GxBindings::State& state, u64 record, bool indexed, std::span<const u32> registers, GxWhole::Mode mode) {
    GxWhole::Counts& c = t.counts;
    const u64 index = t.seen++;
    if (!GxWhole::ShouldCompare(mode, index)) {
        ++c.skipped;
        return;
    }
    std::array<GxWhole::Value, GxWhole::kMaxValues> values;
    const u32 count = GxWhole::RegistersFromRecord(state, indexed, values);
    if (!Intact()) {
        ++c.overwritten;
        GxWhole::CheckKept(registers, t.kept, false, c);
        return;
    }
    ++c.checked;
    std::array<GxWhole::Difference, GxWhole::kEvidencePerDraw> evidence;
    GxWhole::UnusedFound unused;
    const u32 differing = GxWhole::CompareRecord(registers, std::span<const GxWhole::Value>(values.data(), count), c, evidence, &state.render,
                                                 &unused);
    for (u32 u = 1; mode == GxWhole::Mode::Compare && u < GxWhole::kUnusedClasses; ++u) {
        if (!((unused.mask >> u) & 1) || t.unused_logged[u]) continue;
        t.unused_logged[u] = true;
        const GxWhole::Difference& d = unused.first[u];
        char line[kEvidenceBytes];
        const int length = std::snprintf(line, sizeof line,
                                         "GX whole: first unread difference (%s) in %s at register %#x: record %08x, registers %08x (covered draw %llu, record %016llx, LANTERN_GX_WHOLE=2)\n",
                                         GxWhole::kUnusedNames[u], GxWhole::kFieldInfo[d.field].name, d.reg, d.ours, d.registers,
                                         static_cast<unsigned long long>(t.seen), static_cast<unsigned long long>(record));
        WriteLine(line, length);
    }
    if (differing) {
        ++c.differ;
        ReportWholeEvidence(t, std::span<const GxWhole::Difference>(evidence.data(), std::min(differing, GxWhole::kEvidencePerDraw)), record, mode);
    }
    if (GxWhole::CheckKept(registers, t.kept, true, c)) ReportKeptChanges(t, mode);
}
}

bool WholeCounting() {
    return g_whole_counting.load(std::memory_order_relaxed);
}

GxWhole::Mode WholeMode() {
    return g_whole.load(std::memory_order_relaxed);
}

GxWhole::Mode WholeRequested() {
    return g_whole_requested.load(std::memory_order_relaxed);
}

void NoteWhole(std::uint64_t record, bool indexed, std::span<const std::uint32_t> registers) {
    WholeTally& t = g_whole_tally;
    const GxWhole::Mode mode = g_whole.load(std::memory_order_relaxed);
    if (mode == GxWhole::Mode::Compare) {
        const std::thread::id self = std::this_thread::get_id();
        if (t.writer == std::thread::id{}) t.writer = self;
        else if (t.writer != self) ++t.counts.other_writers;
    }
    GxWhole::Counts& c = t.counts;
    ++c.total;
    c.covered += record != 0;
    if (mode != GxWhole::Mode::Off) {
        const GxBindings::State* state = record ? Taken(record) : nullptr;
        if (record && !state) ++c.no_payload;
        else if (state && !state->whole.captured) ++c.no_whole;
        if (state && state->whole.captured) CompareWhole(t, *state, record, indexed, registers, mode);
        else GxWhole::CheckKept(registers, t.kept, false, c);
    }
    if (++t.unpublished >= kWholePublishEvery) {
        t.unpublished = 0;
        PublishWhole(c);
    }
}

bool WholeLine(char* out, std::size_t size, bool* suspect) {
    if (!WholeCounting()) return false;
    std::array<u64, GxWhole::kCountWords> words;
    for (std::size_t i = 0; i < words.size(); ++i) words[i] = g_whole_published[i].load(std::memory_order_relaxed);
    GxWhole::Counts counts;
    std::memcpy(&counts, words.data(), sizeof counts);
    const GxWhole::Mode mode = WholeMode();
    GxWhole::FormatLine(out, size, mode, counts);
    if (suspect) *suspect = GxWhole::InstrumentSuspect(mode, counts);
    return true;
}

namespace {
bool InstallRecords(std::uint8_t* image, std::size_t size, GxWhole::Mode whole) {
    const char* on = std::getenv("LANTERN_GX_RECORDS");
    if (!on || (on[0] != '1' && on[0] != '2' && on[0] != '3')) return false;
    const bool compare = on[0] == '2';
    const char* native = std::getenv("LANTERN_GX_NATIVE");
    const bool use = GxWhole::StageA::RecordsFeed(on, native, whole != GxWhole::Mode::Off);
    if (!GxObjects::On()) {
        host_log("GX records off: they need the object table");
        return false;
    }
    std::array<Hook, 4> hooks;
    bool ready = true;
    for (int i = 0; i < 4 && ready; ++i) {
        const std::span<const std::uint8_t> entry = i == 3 ? std::span<const std::uint8_t>(kPushesWide) : std::span<const std::uint8_t>(kPushes);
        ready = Prepare(hooks[i], image, size, kDraws[i], SiteKind::Entry, entry, OnDraw);
    }
    if (!ready) {
        for (Hook& hook : hooks) Discard(hook);
        host_log("GX records off: GX's draw calls differ from 1.09, or the trace holds them");
        return false;
    }
    g_image = reinterpret_cast<u64>(image);
    const char* constants = std::getenv("LANTERN_GX_CONSTANTS");
    const bool by_object = GxWholeBinds::BindsByObjectWanted(compare, use, whole);
    if (whole != GxWhole::Mode::Off && BindsOff())
        host_log("GX whole binds off (LANTERN_GX_BINDS=0): no seals and no versions for the whole draw's vertex and index binds");
    if (GxWholeBinds::VersionsWanted(compare, use, whole, BindsOff())) {
        std::array<Hook, 2> updates;
        if (Prepare(updates[0], image, size, kDynamicUpdate, SiteKind::Entry, kUpdateEntry, OnDynamicUpdate) &&
            Prepare(updates[1], image, size, kDefaultUpdate, SiteKind::Entry, kDefaultUpdateEntry, OnDefaultUpdate)) {
            g_versions = std::make_unique<std::atomic<u32>[]>(std::size_t(1) << kVersionBits);
            g_dynamic_update = OriginalStub(updates[0]);
            if (const WriteResult result = Commit(std::span<Hook>(updates)); result != WriteResult::Applied) {
                if (result == WriteResult::NotApplied) {
                    FreeStub(g_dynamic_update);
                    g_dynamic_update = nullptr;
                }
                host_log("GX records: buffer updates could not be written (%s), buffers bound by address", Describe(result));
            } else {
                g_updates.store(true, std::memory_order_relaxed);
                g_binds_by_object.store(by_object, std::memory_order_relaxed);
                g_constants.store(by_object && constants && constants[0] == '1', std::memory_order_relaxed);
                if (!by_object)
                    host_log("GX records: buffers versioned by GX's updates for the whole draw's vertex and index binds (LANTERN_GX_WHOLE=%d), still bound by address", GxWhole::ModeNumber(whole));
                else
                    host_log(g_constants.load() ? "GX records: buffers versioned by GX's updates; unchanged constants reused (LANTERN_GX_CONSTANTS=1), vertex and index buffers bound by object"
                                                : "GX records: buffers versioned by GX's updates; vertex and index buffers bound by object (LANTERN_GX_BINDS=0 turns it off)");
            }
        } else {
            for (Hook& hook : updates) Discard(hook);
            host_log("GX records: buffer updates differ from 1.09, buffers bound by address");
        }
    }
    g_slots = std::make_unique<Slot[]>(std::size_t(1) << kSlotBits);
    if (compare || use || whole != GxWhole::Mode::Off) g_pool = std::make_unique<Payload[]>(std::size_t(1) << kPoolBits);
    g_checked.store(compare, std::memory_order_relaxed);
    g_compared.store(compare, std::memory_order_relaxed);
    if (const WriteResult result = Commit(std::span<Hook>(hooks)); result != WriteResult::Applied) {
        host_log("GX records off: GX's draw calls could not be written (%s)", Describe(result));
        return false;
    }
    g_used.store(use, std::memory_order_relaxed);
    g_on.store(true, std::memory_order_relaxed);
    host_log(compare ? "GX records: on, each pipeline, binding and render state compared with the old path's (LANTERN_GX_RECORDS=2)"
             : use   ? "GX records: on, pipelines, bindings and render state from the record (LANTERN_GX_RECORDS=3)"
                     : "GX records: on (LANTERN_GX_RECORDS=0 turns them off)");
    return true;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    const GxWhole::Mode whole = GxWhole::ModeOf(std::getenv(GxWhole::kSwitch));
    g_whole_requested.store(whole, std::memory_order_relaxed);
    g_whole.store(whole, std::memory_order_relaxed);
    const bool records = InstallRecords(image, size, whole);
    if (GxWholeBinds::SealsForWhole(records, whole, BindsOff())) GxObjects::SealForWhole(GxWhole::ModeNumber(whole));
    if (!records) {
        g_whole.store(GxWhole::Mode::Off, std::memory_order_relaxed);
        if (whole != GxWhole::Mode::Off) host_log("GX whole off: it needs GX records (LANTERN_GX_RECORDS=1, 2 or 3)");
        return false;
    }
    g_whole_counting.store(whole != GxWhole::Mode::Off || PipeCensus::On(), std::memory_order_relaxed);
    if (whole == GxWhole::Mode::Compare) {
        host_log("GX whole: every covered draw's register fields compared with the record (LANTERN_GX_WHOLE=2), counts in the BB_FRAME_STATS log");
    } else if (whole == GxWhole::Mode::Feed) {
        host_log("GX whole: the record carries every field a covered draw reads, one covered draw in %u compared (LANTERN_GX_WHOLE=1)", GxWhole::kSampleEvery);
    } else if (WholeCounting()) {
        host_log("GX whole: count only, covered and total draws (LANTERN_GX_WHOLE=0, BB_FRAME_STATS=1)");
    }
    return true;
}
}
