// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_trace.h"
#include "hook_entries.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include "gx_draws.h"
#include "gx_objects.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace BbEngine::GxTrace {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum Method { Dispatch, DispatchIndirect, Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawInstancedIndirect,
              DrawIndexedInstancedIndirect, CommitState, kMethods };
constexpr const char* kMethodNames[kMethods] = {"Dispatch", "DispatchIndirect", "Draw", "DrawIndexed", "DrawInstanced",
                                                "DrawIndexedInstanced", "DrawInstancedIndirect",
                                                "DrawIndexedInstancedIndirect", "Commit"};
constexpr std::size_t kOffsets[kMethods] = {0x21695c0, 0x2169640, 0x21696c0, 0x2169780, 0x2169840,
                                            0x2169900, 0x21699d0, 0x2169ab0, 0x2169ba0};
constexpr const auto& kPushes = HookEntries::kGxTracePushes;
constexpr const auto& kPushesWide = HookEntries::kGxTracePushesWide;
constexpr const auto& kCommitEntry = HookEntries::kGxTraceCommitEntry;

constexpr std::size_t kStageBlocks[6] = {0x598, 0xa90, 0xf88, 0x1480, 0x1978, 0x1e70};
constexpr std::size_t kStageShader = 0x4f0;
constexpr std::size_t kTopology = 0x8, kIndexBuffer = 0x10, kIndexFormat = 0x18, kIndexOffset = 0x1c;
constexpr std::size_t kRenderTargets = 0x4e0, kDepthView = 0x520, kViewports = 0x248, kViewportCount = 0x4c8;
constexpr std::size_t kViewResource = 0x8, kResourceBase = 0x20, kResourceDynamic = 0x40, kShaderDesc = 0x100;
constexpr u32 kIndexFormat32 = 0x2a;
constexpr std::size_t kDescProgram = 0x8;
constexpr u32 kPrimitiveRectList = 17;
constexpr int kHullStage = 1, kGeometryStage = 3;
enum Program { kProgramPs, kProgramVs, kProgramGs, kProgramEs, kProgramHs, kProgramLs };

struct Sample {
    u64 views[5][128];
    u64 samplers[5][16];
    u64 buffers[5][14];
    u64 buffers_now[5][14];
    u32 buffer_sizes[5][14];
    u64 shaders[5];
    u64 access[8];
    u64 vertex[32];
    u32 vertex_stride[32];
    u32 vertex_offset[32];
};
constexpr u32 kSamples = 256, kSampleEvery = 64;
constexpr std::size_t kStageSamplers = 0, kStageViews = 0x80, kStageBuffers = 0x480, kAccess = 0x528;
constexpr std::size_t kViewSharp = 0x10, kResourceSize = 0x2c;
constexpr std::size_t kShaderInputs = 0xe0, kShaderInputCount = 0xe8;
constexpr std::size_t kRenameLookup = 0x26ae840, kContextRenames = 0xb3c0, kResourceHolder = 0x18;
constexpr std::size_t kVertexBuffers = 0x20, kVertexStrides = 0x120, kVertexOffsets = 0x1a0;
std::unique_ptr<Sample[]> g_sample_pool;
template <typename T> T Peek(u64 address);
u64 g_image = 0;
u64 g_input_checked[32]{}, g_input_differs[32]{}, g_input_empty[32]{}, g_input_skipped[32]{};
u64 g_input_logged = 0;

void CheckWords(int kind, int stage, int reg, const u32* expected, const u32* actual, int words) {
    ++g_input_checked[kind];
    if (!std::memcmp(expected, actual, words * sizeof(u32))) return;
    ++g_input_differs[kind];
    if (g_input_logged < 40) {
        ++g_input_logged;
        std::printf("GX draws: stage %d kind %d register %d: ours %08x %08x %08x %08x, the game's %08x %08x %08x %08x\n", stage, kind, reg,
                    expected[0], expected[1], words > 2 ? expected[2] : 0, words > 3 ? expected[3] : 0, actual[0], actual[1],
                    words > 2 ? actual[2] : 0, words > 3 ? actual[3] : 0);
    }
}

void CheckBuffer(int stage, int reg, u64 address, const u32* actual, int kind) {
    ++g_input_checked[kind];
    const u64 seen = actual[0] | (u64(actual[1] & 0xfff) << 32);
    if (seen == address) return;
    ++g_input_differs[kind];
    if (g_input_logged < 40) {
        ++g_input_logged;
        std::printf("GX draws: stage %d kind %d register %d: ours %#llx, the game's %#llx\n", stage, kind, reg,
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(seen));
    }
}

void CheckInputs(const Sample& sample, const Seen& seen) {
    const auto bound = [](u64 value) { return value + 1 > 1; };
    for (int stage = 0; stage < 5; ++stage) {
        const u64 shader = sample.shaders[stage];
        const u32* registers = seen.user_data[stage];
        if (!shader || !registers) continue;
        const u64 inputs = Peek<u64>(shader + kShaderInputs);
        const u32 count = Peek<u32>(shader + kShaderInputCount);
        if (!inputs || count > 64) continue;
        u64 spill = 0;
        u32 spill_start = 0;
        for (u32 i = 0; i < count; ++i) {
            const u32 word = Peek<u32>(inputs + 4 * i);
            if ((word & 0xff) == 26) {
                spill = registers[(word >> 8) & 0xff] | (u64(registers[((word >> 8) & 0xff) + 1]) << 32);
                spill_start = (word >> 16) & 0xff;
            }
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 word = Peek<u32>(inputs + 4 * i);
            const int kind = word & 0xff, reg = (word >> 8) & 0xff, slot = (word >> 16) & 0xff;
            const bool spilled = kind >= 13 && kind <= 18;
            const u32* actual = nullptr;
            if (spilled) {
                if (!spill || reg < 16 + int(spill_start)) {
                    ++g_input_skipped[kind];
                    continue;
                }
                actual = reinterpret_cast<const u32*>(spill) + (reg - 16 - spill_start);
            } else if (kind <= 5 && reg < 16) {
                actual = registers + reg;
            }
            switch (kind) {
            case 0: case 1: case 13: case 14: {
                const u64 view = slot < 128 ? sample.views[stage][slot] : 0;
                if (!bound(view) || !actual) {
                    ++g_input_empty[kind];
                    break;
                }
                CheckWords(kind, stage, reg, reinterpret_cast<const u32*>(view + kViewSharp), actual, kind == 0 || kind == 13 ? 4 : 8);
                break;
            }
            case 2: case 3: case 15: case 16: {
                const u64 view = slot < 8 ? sample.access[slot] : 0;
                if (!bound(view) || !actual) {
                    ++g_input_empty[kind];
                    break;
                }
                CheckWords(kind, stage, reg, reinterpret_cast<const u32*>(view + kViewSharp), actual, kind == 2 || kind == 15 ? 4 : 8);
                break;
            }
            case 4: case 17: {
                const u64 state = slot < 16 ? sample.samplers[stage][slot] : 0;
                if (!bound(state) || !actual) {
                    ++g_input_empty[kind];
                    break;
                }
                CheckWords(kind, stage, reg, reinterpret_cast<const u32*>(state), actual, 4);
                break;
            }
            case 5: case 18: {
                const u64 now = slot < 14 ? sample.buffers_now[stage][slot] : 0;
                if (!now || !actual) {
                    ++g_input_empty[kind];
                    break;
                }
                CheckBuffer(stage, reg, now, actual, kind);
                break;
            }
            case 8: case 11: case 21: case 24: {
                const bool views = kind == 8 || kind == 21;
                const u64 table = kind <= 11 && reg + 1 < 16 ? registers[reg] | (u64(registers[reg + 1]) << 32)
                                  : spill && reg >= 16 + int(spill_start)
                                      ? *reinterpret_cast<const u64*>(reinterpret_cast<const u32*>(spill) + (reg - 16 - spill_start))
                                      : 0;
                if (!table) {
                    ++g_input_skipped[kind];
                    break;
                }
                if (views) {
                    for (int entry = 0; entry < 128; ++entry) {
                        if (bound(sample.views[stage][entry]) &&
                            (Peek<u64>(shader + (entry < 64 ? 0 : 8)) >> (entry & 63)) & 1) {
                            CheckWords(kind, stage, reg, reinterpret_cast<const u32*>(sample.views[stage][entry] + kViewSharp),
                                       reinterpret_cast<const u32*>(table + entry * 32), 8);
                        }
                    }
                } else {
                    for (int entry = 0; entry < 14; ++entry) {
                        if (sample.buffers_now[stage][entry] && (Peek<u64>(shader + 0x10) >> (16 + entry)) & 1) {
                            CheckBuffer(stage, reg, sample.buffers_now[stage][entry], reinterpret_cast<const u32*>(table + entry * 16), kind);
                        }
                    }
                }
                break;
            }
            case 10: case 23: {
                const u64 table = kind == 10 && reg + 1 < 16 ? registers[reg] | (u64(registers[reg + 1]) << 32)
                                  : spill && reg >= 16 + int(spill_start)
                                      ? *reinterpret_cast<const u64*>(reinterpret_cast<const u32*>(spill) + (reg - 16 - spill_start))
                                      : 0;
                if (!table) {
                    ++g_input_skipped[kind];
                    break;
                }
                for (int entry = 0; entry < 16; ++entry) {
                    if (bound(sample.samplers[stage][entry]) && (Peek<u64>(shader + 0x10) >> entry) & 1) {
                        CheckWords(kind, stage, reg, reinterpret_cast<const u32*>(sample.samplers[stage][entry]),
                                   reinterpret_cast<const u32*>(table + entry * 16), 4);
                    }
                }
                break;
            }
            default:
                ++g_input_skipped[kind];
                break;
            }
        }
    }
}
std::vector<std::pair<u64, u64>> g_regions;
u64 g_contents_logged = 0;
u64 g_buffers_renamed_found = 0, g_buffers_in_renamed = 0;
std::atomic<u32> g_sample_next{0};
thread_local const Sample* g_taken = nullptr;
u64 g_images_exact = 0, g_images_base = 0, g_images_none = 0, g_images_wide = 0;
u64 g_buffers_view = 0, g_buffers_constant = 0, g_buffers_renamed = 0, g_buffers_none = 0;
u64 g_buffers_inside = 0, g_buffers_vertex = 0, g_buffers_view_range = 0, g_buffers_logged = 0;
u64 g_samplers_found = 0, g_samplers_none = 0, g_sampler_at[16]{};
u64 g_none_logged = 0;

struct Stamped {
    std::atomic<u32> lock{0};
    const Sample* sample = nullptr;
    u64 address = 0;
    u64 index_base = 0;
    u64 shader_desc[6]{};
    u64 color[8]{};
    u64 depth = 0;
    u32 method = 0, count = 0, topology = 0, index_format = 0, viewports = 0;
    float viewport[4]{};
    bool index_dynamic = false, full = false;
};
constexpr u32 kSlotBits = 16;
std::unique_ptr<Stamped[]> g_slots;
std::atomic<bool> g_on{false};

struct Current {
    u64 pending = 0, context = 0;
    u32 method = 0, count = 0;
    bool set = false;
};
thread_local Current g_current;

std::atomic<u64> g_calls[kMethods]{};
std::atomic<u64> g_stamped{0};
std::atomic<u64> g_internal{0};
u64 g_decoded = 0, g_foreign = 0, g_internal_rects = 0;
u64 g_diff_count = 0, g_diff_index = 0, g_diff_type = 0, g_diff_color = 0, g_diff_depth = 0, g_diff_viewport = 0;
u64 g_checked_index = 0, g_dynamic_index = 0, g_checked_color = 0, g_checked_depth = 0, g_checked_viewport = 0;
u64 g_logged = 0;
u64 g_diff_program = 0, g_checked_program = 0;
u64 g_topology[256][32]{};
std::chrono::steady_clock::time_point g_last_report = std::chrono::steady_clock::now();

template <typename T> T Peek(u64 address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof value);
    return value;
}

u64 ViewBase(u64 view) {
    if (!view) return 0;
    const u64 resource = Peek<u64>(view + kViewResource);
    return resource ? Peek<u64>(resource + kResourceBase) : 0;
}

Stamped& SlotFor(u64 address) {
    const u64 h = (address >> 2) * 0x9E3779B97F4A7C15ull;
    return g_slots[h >> (64 - kSlotBits)];
}

void Lock(Stamped& s) {
    while (s.lock.exchange(1, std::memory_order_acquire)) {
        while (s.lock.load(std::memory_order_relaxed)) __builtin_ia32_pause();
    }
}

void Unlock(Stamped& s) {
    s.lock.store(0, std::memory_order_release);
}

template <int M> std::int64_t __attribute__((sysv_abi)) OnMethod(std::uint64_t, std::uint64_t* saved) noexcept {
    g_calls[M].fetch_add(1, std::memory_order_relaxed);
    if (GxObjects::On()) {
        if (M == Dispatch || M == DispatchIndirect) GxObjects::CheckDispatch(saved[4]);
        else if (M != CommitState) GxObjects::CheckDraw(saved[4]);
    }
    const bool direct = M == Draw || M == DrawIndexed || M == DrawInstanced || M == DrawIndexedInstanced;
    g_current.set = direct;
    if (direct) {
        g_current.pending = saved[4];
        g_current.context = saved[5];
        g_current.method = M;
        g_current.count = static_cast<u32>(saved[3]);
    }
    return 0;
}

void Report() {
    if (GxObjects::On()) GxObjects::Report();
    if (GxDraws::On()) GxDraws::Report();
    for (const auto& [region, count] : g_regions) {
        std::printf("GX trace: none buffers in region %#llx, size class %llu: %llu\n", static_cast<unsigned long long>(region >> 20),
                    static_cast<unsigned long long>(region & 0xf), static_cast<unsigned long long>(count));
    }
    std::printf("GX trace: shader resources found in GX's slots: images %llu exact, %llu same address, %llu wide, %llu none; "
                "buffers %llu views, %llu constant, %llu current address, %llu inside current, %llu inside constant, %llu vertex, %llu view range, %llu renamed, %llu none; samplers %llu found, %llu none (at",
                static_cast<unsigned long long>(g_images_exact), static_cast<unsigned long long>(g_images_base),
                static_cast<unsigned long long>(g_images_wide), static_cast<unsigned long long>(g_images_none),
                static_cast<unsigned long long>(g_buffers_view), static_cast<unsigned long long>(g_buffers_constant),
                static_cast<unsigned long long>(g_buffers_renamed_found), static_cast<unsigned long long>(g_buffers_in_renamed),
                static_cast<unsigned long long>(g_buffers_inside), static_cast<unsigned long long>(g_buffers_vertex),
                static_cast<unsigned long long>(g_buffers_view_range),
                static_cast<unsigned long long>(g_buffers_renamed), static_cast<unsigned long long>(g_buffers_none),
                static_cast<unsigned long long>(g_samplers_found), static_cast<unsigned long long>(g_samplers_none));
    for (int at = 0; at < 16; ++at) {
        if (g_sampler_at[at]) std::printf(" +%#x %llu", at * 4, static_cast<unsigned long long>(g_sampler_at[at]));
    }
    std::printf(")\n");
    for (int kind = 0; kind < 32; ++kind) {
        if (g_input_checked[kind] || g_input_empty[kind] || g_input_skipped[kind])
            std::printf("GX draws: input kind %d: %llu checked, %llu differ, %llu empty, %llu left out\n", kind,
                        static_cast<unsigned long long>(g_input_checked[kind]), static_cast<unsigned long long>(g_input_differs[kind]),
                        static_cast<unsigned long long>(g_input_empty[kind]), static_cast<unsigned long long>(g_input_skipped[kind]));
    }
    std::printf("GX trace: draws inside GX's flushes: %llu of another count, %llu rectangles\n",
                static_cast<unsigned long long>(g_internal.load()), static_cast<unsigned long long>(g_internal_rects));
    std::printf("GX trace: calls");
    for (int m = 0; m < kMethods; ++m) std::printf(" %s %llu", kMethodNames[m], static_cast<unsigned long long>(g_calls[m].load()));
    std::printf("; records %llu stamped, %llu decoded, %llu not from GX\n",
                static_cast<unsigned long long>(g_stamped.load()), static_cast<unsigned long long>(g_decoded),
                static_cast<unsigned long long>(g_foreign));
    std::printf("GX trace: differences count %llu, index address %llu of %llu (%llu dynamic), index type %llu, "
                "render target %llu of %llu, depth %llu of %llu, viewport %llu of %llu, program %llu of %llu\n",
                static_cast<unsigned long long>(g_diff_count), static_cast<unsigned long long>(g_diff_index),
                static_cast<unsigned long long>(g_checked_index), static_cast<unsigned long long>(g_dynamic_index),
                static_cast<unsigned long long>(g_diff_type), static_cast<unsigned long long>(g_diff_color),
                static_cast<unsigned long long>(g_checked_color), static_cast<unsigned long long>(g_diff_depth),
                static_cast<unsigned long long>(g_checked_depth), static_cast<unsigned long long>(g_diff_viewport),
                static_cast<unsigned long long>(g_checked_viewport), static_cast<unsigned long long>(g_diff_program),
                static_cast<unsigned long long>(g_checked_program));
    for (int topology = 0; topology < 256; ++topology) {
        for (int primitive = 0; primitive < 32; ++primitive) {
            if (g_topology[topology][primitive])
                std::printf("GX trace: topology %d draws as primitive type %d, %llu draws\n", topology, primitive,
                            static_cast<unsigned long long>(g_topology[topology][primitive]));
        }
    }
    std::fflush(stdout);
}

void LogDifference(const char* what, u64 expected, u64 actual, u32 method) {
    if (g_logged >= 48) return;
    ++g_logged;
    std::printf("GX trace: %s differs at %s: GX %#llx, registers %#llx\n", what, kMethodNames[method],
                static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual));
}
}

bool On() {
    return g_on.load(std::memory_order_relaxed);
}

void Stamp(std::uint64_t record_address, std::uint32_t count) {
    Current& current = g_current;
    Stamped& s = SlotFor(record_address);
    if (!current.set || current.count != count) {
        if (current.set) g_internal.fetch_add(1, std::memory_order_relaxed);
        Lock(s);
        if (s.address == record_address) s.full = false;
        Unlock(s);
        return;
    }
    current.set = false;
    const u64 pending = current.pending;
    Lock(s);
    s.address = record_address;
    s.method = current.method;
    s.count = current.count;
    s.topology = Peek<u32>(pending + kTopology);
    s.index_format = Peek<u32>(pending + kIndexFormat);
    const u64 buffer = Peek<u64>(pending + kIndexBuffer);
    s.index_dynamic = buffer && Peek<std::uint8_t>(buffer + kResourceDynamic) != 0;
    s.index_base = buffer && !s.index_dynamic ? Peek<u64>(buffer + kResourceBase) + Peek<u32>(pending + kIndexOffset) : 0;
    for (int stage = 0; stage < 6; ++stage) {
        const u64 shader = Peek<u64>(pending + kStageBlocks[stage] + kStageShader);
        s.shader_desc[stage] = shader ? Peek<u64>(shader + kShaderDesc) : 0;
    }
    for (int target = 0; target < 8; ++target) s.color[target] = ViewBase(Peek<u64>(pending + kRenderTargets + 8 * target));
    s.depth = ViewBase(Peek<u64>(pending + kDepthView));
    s.viewports = Peek<u32>(pending + kViewportCount);
    s.sample = nullptr;
    if (g_stamped.load(std::memory_order_relaxed) % kSampleEvery == 0) {
        Sample& sample = g_sample_pool[g_sample_next.fetch_add(1, std::memory_order_relaxed) % kSamples];
        for (int stage = 0; stage < 5; ++stage) {
            const u64 block = pending + kStageBlocks[stage];
            std::memcpy(sample.views[stage], reinterpret_cast<const void*>(block + kStageViews), sizeof sample.views[stage]);
            std::memcpy(sample.samplers[stage], reinterpret_cast<const void*>(block + kStageSamplers), sizeof sample.samplers[stage]);
            std::memcpy(sample.buffers[stage], reinterpret_cast<const void*>(block + kStageBuffers), sizeof sample.buffers[stage]);
            sample.shaders[stage] = Peek<u64>(block + kStageShader);
            for (int slot = 0; slot < 14; ++slot) {
                const u64 buffer = sample.buffers[stage][slot];
                sample.buffer_sizes[stage][slot] = buffer + 1 > 1 ? Peek<u32>(buffer + kResourceSize) : 0;
                sample.buffers_now[stage][slot] =
                    buffer + 1 > 1 ? static_cast<u64>(Call(g_image + kRenameLookup, current.context + kContextRenames, buffer + kResourceHolder)) : 0;
            }
        }
        std::memcpy(sample.access, reinterpret_cast<const void*>(pending + kAccess), sizeof sample.access);
        std::memcpy(sample.vertex, reinterpret_cast<const void*>(pending + kVertexBuffers), sizeof sample.vertex);
        std::memcpy(sample.vertex_stride, reinterpret_cast<const void*>(pending + kVertexStrides), sizeof sample.vertex_stride);
        std::memcpy(sample.vertex_offset, reinterpret_cast<const void*>(pending + kVertexOffsets), sizeof sample.vertex_offset);
        s.sample = &sample;
    }
    std::memcpy(s.viewport, reinterpret_cast<const void*>(pending + kViewports), sizeof s.viewport);
    s.full = true;
    Unlock(s);
    g_stamped.fetch_add(1, std::memory_order_relaxed);
}

void Check(std::uint64_t record_address, const Seen& seen) {
    ++g_decoded;
    Stamped& slot = SlotFor(record_address);
    Lock(slot);
    const bool found = slot.full && slot.address == record_address;
    Stamped s;
    if (found) {
        s.address = slot.address;
        s.index_base = slot.index_base;
        std::memcpy(s.shader_desc, slot.shader_desc, sizeof s.shader_desc);
        std::memcpy(s.color, slot.color, sizeof s.color);
        s.depth = slot.depth;
        s.method = slot.method;
        s.count = slot.count;
        s.topology = slot.topology;
        s.index_format = slot.index_format;
        s.index_dynamic = slot.index_dynamic;
        s.viewports = slot.viewports;
        s.sample = slot.sample;
        std::memcpy(s.viewport, slot.viewport, sizeof s.viewport);
        slot.full = false;
    }
    Unlock(slot);
    g_taken = found ? s.sample : nullptr;
    if (found && s.sample && seen.primitive != kPrimitiveRectList) CheckInputs(*s.sample, seen);
    if (!found) {
        ++g_foreign;
    } else if (seen.primitive == kPrimitiveRectList) {
        ++g_internal_rects;
    } else {
        if (s.count != seen.index_count) {
            ++g_diff_count;
            LogDifference("count", s.count, seen.index_count, s.method);
        }
        if (s.topology < 256 && seen.primitive < 32) ++g_topology[s.topology][seen.primitive];
        if (seen.indexed) {
            if (s.index_dynamic) {
                ++g_dynamic_index;
            } else {
                ++g_checked_index;
                if (s.index_base != seen.index_base) {
                    ++g_diff_index;
                    LogDifference("index address", s.index_base, seen.index_base, s.method);
                }
            }
            if ((s.index_format == kIndexFormat32) != (seen.index_type == 1)) {
                ++g_diff_type;
                LogDifference("index type", s.index_format, seen.index_type, s.method);
            }
        }
        for (int target = 0; target < 8; ++target) {
            if (!s.color[target]) continue;
            ++g_checked_color;
            if (s.color[target] != seen.color[target]) {
                ++g_diff_color;
                LogDifference("render target", s.color[target], seen.color[target], s.method);
            }
        }
        if (s.depth) {
            ++g_checked_depth;
            if (s.depth != seen.depth) {
                ++g_diff_depth;
                LogDifference("depth", s.depth, seen.depth, s.method);
            }
        }
        if (s.viewports) {
            ++g_checked_viewport;
            const float half_width = s.viewport[2] / 2, half_height = s.viewport[3] / 2;
            const bool same = std::fabs(half_width - std::fabs(seen.viewport[0])) < 0.51f &&
                              std::fabs(s.viewport[0] + half_width - seen.viewport[1]) < 0.51f &&
                              std::fabs(half_height - std::fabs(seen.viewport[2])) < 0.51f &&
                              std::fabs(s.viewport[1] + half_height - seen.viewport[3]) < 0.51f;
            if (!same) {
                ++g_diff_viewport;
                LogDifference("viewport width", static_cast<u64>(s.viewport[2]),
                              static_cast<u64>(std::fabs(seen.viewport[0]) * 2), s.method);
            }
        }
        const bool tessellated = s.shader_desc[kHullStage] != 0;
        const int first = tessellated ? kProgramLs : s.shader_desc[kGeometryStage] ? kProgramEs : kProgramVs;
        const int expected[5] = {first, kProgramHs, kProgramVs, kProgramGs, kProgramPs};
        for (int stage = 0; stage < 5; ++stage) {
            if (!s.shader_desc[stage]) continue;
            ++g_checked_program;
            const u32 program = Peek<u32>(s.shader_desc[stage] + kDescProgram);
            const u32 registers = static_cast<u32>(seen.programs[expected[stage]]);
            if (program != registers) {
                ++g_diff_program;
                LogDifference("program", program, registers, s.method);
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_report >= std::chrono::seconds(10)) {
        g_last_report = now;
        Report();
    }
}

const void* TakeSample() {
    const Sample* taken = g_taken;
    g_taken = nullptr;
    return taken;
}

void NoteResource(const void* sample_address, ResourceKind kind, const void* sharp, bool wide) {
    const Sample& sample = *static_cast<const Sample*>(sample_address);
    const auto bound = [](u64 value) { return value + 1 > 1; };
    if (kind == ResourceImage) {
        const std::size_t bytes = wide ? 32 : 16;
        bool base = false;
        const auto look = [&](u64 view) {
            if (!bound(view)) return false;
            const void* held = reinterpret_cast<const void*>(view + kViewSharp);
            if (!std::memcmp(held, sharp, bytes)) return true;
            base |= Peek<u32>(view + kViewSharp) == *static_cast<const u32*>(sharp);
            return false;
        };
        for (int stage = 0; stage < 5; ++stage) {
            for (const u64 view : sample.views[stage]) {
                if (look(view)) {
                    ++(wide ? g_images_exact : g_images_wide);
                    return;
                }
            }
        }
        for (const u64 view : sample.access) {
            if (look(view)) {
                ++(wide ? g_images_exact : g_images_wide);
                return;
            }
        }
        ++(base ? g_images_base : g_images_none);
        if (g_none_logged < 24) {
            ++g_none_logged;
            const u32* w = static_cast<const u32*>(sharp);
            std::printf("GX trace: an image not in GX's slots: %08x %08x %08x %08x %08x %08x %08x %08x%s\n", w[0], w[1], w[2], w[3], w[4],
                        w[5], w[6], w[7], base ? " (same address)" : "");
        }
        return;
    }
    if (kind == ResourceBuffer) {
        const u32* w = static_cast<const u32*>(sharp);
        const u64 address = w[0] | (u64(w[1] & 0xfff) << 32);
        for (int stage = 0; stage < 5; ++stage) {
            for (const u64 view : sample.views[stage]) {
                if (bound(view) && !std::memcmp(reinterpret_cast<const void*>(view + kViewSharp), sharp, 16)) {
                    ++g_buffers_view;
                    return;
                }
            }
        }
        bool dynamic = false;
        for (int stage = 0; stage < 5; ++stage) {
            for (const u64 buffer : sample.buffers[stage]) {
                if (!bound(buffer)) continue;
                dynamic |= Peek<std::uint8_t>(buffer + kResourceDynamic) != 0;
                if (Peek<u64>(buffer + kResourceBase) == address) {
                    ++g_buffers_constant;
                    return;
                }
            }
        }
        for (int stage = 0; stage < 5; ++stage) {
            for (int slot = 0; slot < 14; ++slot) {
                const u64 now = sample.buffers_now[stage][slot];
                if (now && now == address) {
                    ++g_buffers_renamed_found;
                    return;
                }
                if (now && address > now && address < now + 0x10000) {
                    GxObjects::Object object;
                    if (GxObjects::On() && GxObjects::Find(sample.buffers[stage][slot], &object) && address < now + object.description[0]) {
                        ++g_buffers_in_renamed;
                        return;
                    }
                }
            }
        }
        const auto width = [](u64 buffer) {
            GxObjects::Object object;
            return GxObjects::On() && GxObjects::Find(buffer, &object) ? u64(object.description[0]) : u64(0);
        };
        for (int stage = 0; stage < 5; ++stage) {
            for (const u64 buffer : sample.buffers[stage]) {
                if (!bound(buffer)) continue;
                const u64 base = Peek<u64>(buffer + kResourceBase);
                if (address >= base && address < base + width(buffer)) {
                    ++g_buffers_inside;
                    return;
                }
            }
        }
        for (int slot = 0; slot < 32; ++slot) {
            if (!bound(sample.vertex[slot])) continue;
            const u64 base = Peek<u64>(sample.vertex[slot] + kResourceBase);
            if (address >= base && address < base + width(sample.vertex[slot])) {
                ++g_buffers_vertex;
                return;
            }
        }
        for (int stage = 0; stage < 5; ++stage) {
            for (const u64 view : sample.views[stage]) {
                if (!bound(view)) continue;
                const u64 resource = Peek<u64>(view + kViewResource);
                if (!resource) continue;
                const u64 base = Peek<u64>(resource + kResourceBase);
                if (address >= base && address < base + width(resource)) {
                    ++g_buffers_view_range;
                    return;
                }
            }
        }
        ++(dynamic ? g_buffers_renamed : g_buffers_none);
        {
            const u64 bytes = u64((w[1] >> 16) & 0x3fff) * w[2];
            const u64 key = (address & ~u64(0xffff)) << 20 | (address & 0xffff) << 4 | (bytes < 15 * 16 ? bytes / 16 : 15);
            const u64 region = key & ~(u64(0xffff) << 4);
            std::size_t at = 0;
            for (; at < g_regions.size() && g_regions[at].first != region; ++at) {
            }
            if (at == g_regions.size() && g_regions.size() < 64) g_regions.push_back({region, 0});
            if (at < g_regions.size()) ++g_regions[at].second;
            const u64 words = bytes >= 32 ? 4 : 2;
            if (g_contents_logged < 12) {
                ++g_contents_logged;
                const u32* c = reinterpret_cast<const u32*>(address);
                std::printf("GX trace: none buffer at %#llx bytes %llu holds", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
                for (u64 i = 0; i < words * 2; ++i) std::printf(" %08x", c[i]);
                std::printf("\n");
            }
        }
        if (g_buffers_logged < 2) {
            ++g_buffers_logged;
            int constants = 0;
            u64 first = 0;
            for (int stage = 0; stage < 5; ++stage) {
                for (const u64 buffer : sample.buffers[stage]) {
                    if (!bound(buffer)) continue;
                    if (!constants++) first = buffer;
                }
            }
            std::printf("GX trace: a buffer not in GX's slots: %#llx, stride %u, size %u; %d constant buffers bound, the first base %#llx width %llu dynamic %u; vertex slot 0 base %#llx\n",
                        static_cast<unsigned long long>(address), (w[1] >> 16) & 0x3fff, w[2], constants,
                        static_cast<unsigned long long>(first ? Peek<u64>(first + kResourceBase) : 0),
                        static_cast<unsigned long long>(first ? width(first) : 0), first ? unsigned(Peek<std::uint8_t>(first + kResourceDynamic)) : 0u,
                        static_cast<unsigned long long>(bound(sample.vertex[0]) ? Peek<u64>(sample.vertex[0] + kResourceBase) : 0));
        }
        return;
    }
    for (int stage = 0; stage < 5; ++stage) {
        for (const u64 state : sample.samplers[stage]) {
            if (!bound(state)) continue;
            for (int at = 0; at < 13; ++at) {
                if (!std::memcmp(reinterpret_cast<const void*>(state + at * 4), sharp, 16)) {
                    ++g_samplers_found;
                    ++g_sampler_at[at];
                    return;
                }
            }
        }
    }
    ++g_samplers_none;
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("LANTERN_GX_TRACE");
    if (!on || on[0] != '1') return false;
    g_slots = std::make_unique<Stamped[]>(std::size_t(1) << kSlotBits);
    g_sample_pool = std::make_unique<Sample[]>(kSamples);
    g_image = reinterpret_cast<u64>(image);
    const Callback callbacks[kMethods] = {OnMethod<0>, OnMethod<1>, OnMethod<2>, OnMethod<3>, OnMethod<4>,
                                          OnMethod<5>, OnMethod<6>, OnMethod<7>, OnMethod<8>};
    std::array<Hook, kMethods> hooks;
    bool ready = true;
    for (int m = 0; m < kMethods && ready; ++m) {
        const std::span<const std::uint8_t> entry = m == DrawIndexedInstanced ? std::span<const std::uint8_t>(kPushesWide)
                                                    : m == CommitState        ? std::span<const std::uint8_t>(kCommitEntry)
                                                                              : std::span<const std::uint8_t>(kPushes);
        ready = Prepare(hooks[m], image, size, kOffsets[m], SiteKind::Entry, entry, callbacks[m]);
    }
    if (!ready) {
        for (Hook& hook : hooks) Discard(hook);
        host_log("GX trace off: GX's draw calls differ from 1.09");
        return false;
    }
    if (const WriteResult result = Commit(std::span<Hook>(hooks)); result != WriteResult::Applied) {
        host_log("GX trace off: GX's draw calls could not be written (%s)", Describe(result));
        return false;
    }
    g_on.store(true, std::memory_order_relaxed);
    host_log("GX trace: on (LANTERN_GX_TRACE=1)");
    return true;
}
}
