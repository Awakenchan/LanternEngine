// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_objects.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace BbEngine::GxObjects {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct Creator {
    const char* name;
    std::size_t offset;
    const char* entry;
    Kind kind;
    u32 description_bytes;
};
constexpr const char* kPushes58 = "554889e54157415641554154534883ec58";
constexpr Creator kCreators[] = {
    {"CreateBuffer", 0x2165c10, kPushes58, Buffer, 24},
    {"CreateTexture2D", 0x2165f90, kPushes58, Texture2D, 44},
    {"CreateTexture3D", 0x2166300, kPushes58, Texture3D, 36},
    {"CreateDepthStencilView", 0x2166610, "554889e54157415641554154534883ec68", DepthView, 0},
    {"CreateRenderTargetView", 0x21667a0, kPushes58, TargetView, 0},
    {"CreateShaderResourceView", 0x21669a0, kPushes58, ResourceView, 0},
    {"CreateUnorderedAccessView", 0x2166b60, kPushes58, AccessView, 0},
    {"shader 0x2166d00", 0x2166d00, "554889e541574156534881ec38010000", Shader, 0},
    {"shader 0x2166e10", 0x2166e10, "554889e541574156534881ec28010000", Shader, 0},
    {"shader 0x2166f20", 0x2166f20, "554889e541574156534881ec28010000", Shader, 0},
    {"CreateGeometryShader", 0x2167030, "554889e541574156534881ec58010000", Shader, 0},
    {"shader 0x21672d0", 0x21672d0, "554889e541574156534881ec28010000", Shader, 0},
    {"shader 0x21673e0", 0x21673e0, "554889e541574156534881ec28010000", Shader, 0},
};
constexpr std::size_t kCreatorCount = sizeof kCreators / sizeof *kCreators;

struct Vtable {
    std::size_t offset;
    std::size_t complete, deleting;
    Kind kind;
};
constexpr Vtable kVtables[] = {
    {0x535f3a0, 0x216ce40, 0x216ce10, Buffer},
    {0x535f3e0, 0x216d1e0, 0x216d1a0, Texture2D},
    {0x535f400, 0x216d350, 0x216d310, Texture3D},
    {0x535f420, 0x2179b00, 0x2179aa0, DepthView},
    {0x535f440, 0x2179cb0, 0x2179c50, TargetView},
    {0x535f460, 0x2179e40, 0x2179de0, ResourceView},
    {0x535f480, 0x2179fd0, 0x2179f70, AccessView},
};
constexpr std::size_t kVtableCount = sizeof kVtables / sizeof *kVtables;

constexpr const char* kKindNames[kKinds] = {"buffers", "2D textures", "3D textures", "depth views", "target views",
                                            "resource views", "access views", "shaders", "blend states",
                                            "depth states", "rasterizer states", "sampler states"};

constexpr std::size_t kStageBlocks[6] = {0x598, 0xa90, 0xf88, 0x1480, 0x1978, 0x1e70};
constexpr std::size_t kStageSamplers = 0, kStageViews = 0x80, kStageBuffers = 0x480, kStageShader = 0x4f0;
constexpr std::size_t kIndexBuffer = 0x10, kVertexBuffers = 0x20, kRaster = 0x240, kBlend = 0x4d0, kDepthState = 0x4d8;
constexpr std::size_t kTargets = 0x4e0, kDepthTarget = 0x520, kAccess = 0x528;
constexpr u32 kWholeEvery = 256;
constexpr std::size_t kStateBytes[kKinds] = {0, 0, 0, 0, 0, 0, 0, 0, 64, 56, 36, 64};

constexpr std::size_t kShards = 64;
struct Shard {
    std::mutex mutex;
    std::unordered_map<u64, Object> objects;
};
Shard* g_shards = nullptr;
std::atomic<bool> g_on{false};
std::array<std::uint8_t*, kCreatorCount> g_originals{};
std::array<std::uint8_t*, kVtableCount * 2> g_release_stubs{};

std::atomic<u64> g_made[kKinds]{};
std::atomic<u64> g_failed[kKinds]{};
std::atomic<u64> g_released[kKinds]{};
std::atomic<u64> g_reused[kKinds]{};
std::atomic<u64> g_checked[kKinds]{};
std::atomic<u64> g_unknown[kKinds]{};
std::atomic<u64> g_wrong_kind[kKinds]{};
std::atomic<u64> g_dead[kKinds]{};
std::atomic<u64> g_draws{0};
std::atomic<u64> g_stage_creator[6][kCreatorCount]{};
std::atomic<int> g_logged{0};
std::atomic<u64> g_epoch{0};

Shard& ShardFor(u64 address) {
    return g_shards[((address >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - 6)];
}

template <typename T> T Peek(u64 address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof value);
    return value;
}

void Note(u64 address, u32 creator, u64 source, const void* description) {
    const Creator& c = kCreators[creator];
    Object fresh;
    fresh.kind = c.kind;
    fresh.creator = static_cast<std::uint8_t>(creator);
    fresh.live = true;
    fresh.source = source;
    if (description && c.description_bytes) std::memcpy(fresh.description, description, c.description_bytes);
    Shard& shard = ShardFor(address);
    std::scoped_lock lock(shard.mutex);
    Object& slot = shard.objects[address];
    if (slot.generation) {
        g_reused[c.kind].fetch_add(1, std::memory_order_relaxed);
        g_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    fresh.generation = slot.generation + 1;
    slot = fresh;
}

constexpr std::size_t kResourceBase = 0x20, kResourceBytes = 0x30;
struct ImageSeen {
    u64 uid;
    u32 width, height, format, bytes;
};
struct Resource {
    u64 object = 0, base = 0;
    u32 width = 0, height = 0, mips = 0, layers = 0, format = 0, generation = 0;
    u64 bytes = 0;
    u64 held_uid = 0;
    u32 held_index = 0;
    bool display = false;
    Kind kind = Buffer;
    std::vector<ImageSeen> images;
};
std::atomic<bool> g_textures_owned{false};
std::atomic<bool> g_textures_checked{false};
std::atomic<u64> g_widen[kWidenResults]{};
std::atomic<u64> g_paths[kObjectPaths]{};
std::atomic<u64> g_held_checks[kHeldChecks]{};
std::atomic<u64> g_held_others{0};
std::atomic<u64> g_lookups{0};
constexpr u64 kReportEvery = u64(1) << 18;
std::map<std::string, u64> g_refused;
std::map<u64, u64> g_gone;
std::map<std::string, u64> g_overlaps;
std::mutex g_resource_mutex;
std::map<u64, Resource> g_resources;
std::map<u64, u32> g_image_resources;
std::vector<Resource> g_finished;
u64 g_image_exact = 0, g_image_inside = 0, g_image_none = 0, g_image_buffer = 0, g_image_same_size = 0, g_image_other_size = 0;
u64 g_image_display = 0, g_none_released = 0, g_none_unaligned = 0, g_none_never = 0;
struct Foreign {
    u64 address;
    u32 bytes, width, height, format;
    u64 count;
};
std::vector<Foreign> g_foreign;

std::array<std::atomic<u32>, 1u << 16> g_buffer_versions{};

std::atomic<u32>& BufferVersionOf(u64 object) {
    return g_buffer_versions[((object >> 3) * 0x9E3779B97F4A7C15ull) >> 48];
}

void NoteResource(u64 object, Kind kind, const u32* description, u64 base, u64 bytes = 0, bool display = false) {
    if (!base) return;
    Resource fresh;
    fresh.object = object;
    fresh.base = base;
    fresh.kind = kind;
    fresh.bytes = bytes;
    fresh.display = display;
    if (kind == Texture2D) {
        fresh.width = description[0];
        fresh.height = description[1];
        fresh.mips = description[2];
        fresh.layers = description[3];
        fresh.format = description[4];
    } else if (kind == Texture3D) {
        fresh.width = description[0];
        fresh.height = description[1];
        fresh.layers = description[2];
        fresh.mips = description[3];
        fresh.format = description[4];
    } else {
        fresh.width = description[0];
    }
    std::scoped_lock lock(g_resource_mutex);
    g_gone.erase(base);
    const auto [at, inserted] = g_resources.try_emplace(base, fresh);
    if (!inserted) {
        g_finished.push_back(std::move(at->second));
        at->second = fresh;
    }
}

void DropResource(u64 object) {
    const u64 base = Peek<u64>(object + kResourceBase);
    std::scoped_lock lock(g_resource_mutex);
    const auto at = g_resources.find(base);
    if (at == g_resources.end() || at->second.object != object) return;
    g_gone[base] = at->second.bytes;
    g_finished.push_back(std::move(at->second));
    g_resources.erase(at);
}

std::atomic<u64> g_input_kinds[kCreatorCount][32]{};
std::atomic<u64> g_input_shaders[kCreatorCount]{};
constexpr std::size_t kShaderInputs = 0xe0, kShaderInputCount = 0xe8;

void NoteInputs(u64 shader, u32 creator) {
    const u64 inputs = Peek<u64>(shader + kShaderInputs);
    const u32 count = Peek<u32>(shader + kShaderInputCount);
    g_input_shaders[creator].fetch_add(1, std::memory_order_relaxed);
    if (!inputs || count > 64) return;
    for (u32 i = 0; i < count; ++i) {
        const u32 kind = Peek<u32>(inputs + 4 * i) & 0xff;
        g_input_kinds[creator][kind < 32 ? kind : 31].fetch_add(1, std::memory_order_relaxed);
    }
}

template <int C> std::int64_t __attribute__((sysv_abi)) OnCreate(std::uint64_t, const std::uint64_t* saved) {
    const u64 out = saved[4], third = saved[3];
    const std::int64_t result = Call(reinterpret_cast<u64>(g_originals[C]), saved[5], out, third, saved[2], saved[1], saved[0], saved[7]);
    const Kind kind = kCreators[C].kind;
    const u64 made = result == 0 && out ? Peek<u64>(out) : 0;
    if (made) {
        if (kind == Shader) NoteInputs(made, C);
        g_made[kind].fetch_add(1, std::memory_order_relaxed);
        Note(made, C, third, reinterpret_cast<const void*>(third));
        if (kind == Buffer) {
            BufferVersionOf(made).fetch_add(1, std::memory_order_release);
            NoteResource(made, kind, reinterpret_cast<const u32*>(third), Peek<u64>(made + kResourceBase), Peek<u32>(third));
        } else if (kind == Texture2D || kind == Texture3D) {
            NoteResource(made, kind, reinterpret_cast<const u32*>(third), Peek<u64>(made + kResourceBase), Peek<u32>(made + kResourceBytes));
        }
    } else {
        g_failed[kind].fetch_add(1, std::memory_order_relaxed);
    }
    const_cast<std::uint64_t*>(saved)[-1] = static_cast<std::uint64_t>(result);
    return 1;
}

struct YebisCreator {
    std::size_t offset;
    std::uint8_t entry[20];
    Kind kind;
};
constexpr YebisCreator kYebisCreators[] = {
    {0x11df570, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18, 0x4d, 0x89, 0xcd}, Texture2D},
    {0x11e1750, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x4d, 0x89, 0xce, 0x45, 0x89, 0xc4}, Texture3D},
};
constexpr std::size_t kYebisCreatorCount = sizeof kYebisCreators / sizeof *kYebisCreators;
constexpr std::size_t kYebisBase = 0x98;
std::array<std::uint8_t*, kYebisCreatorCount> g_yebis_originals{};
std::atomic<u64> g_yebis_made{0}, g_yebis_failed{0};

template <int C> std::int64_t __attribute__((sysv_abi)) OnYebisCreate(std::uint64_t, const std::uint64_t* saved) {
    const u64 description = saved[2], out = saved[0];
    const std::int64_t result = Call(reinterpret_cast<u64>(g_yebis_originals[C]), saved[5], saved[4], saved[3], description, saved[1], out);
    const u64 made = static_cast<int>(result) == 0 && out ? Peek<u64>(out) : 0;
    if (made) {
        g_yebis_made.fetch_add(1, std::memory_order_relaxed);
        NoteResource(made, kYebisCreators[C].kind, reinterpret_cast<const u32*>(description), Peek<u64>(made + kYebisBase));
    } else {
        g_yebis_failed.fetch_add(1, std::memory_order_relaxed);
    }
    const_cast<std::uint64_t*>(saved)[-1] = static_cast<std::uint64_t>(result);
    return 1;
}

constexpr std::size_t kSwapChainCreator = 0x26d5e90;
constexpr std::uint8_t kSwapChainEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x98, 0x01, 0x00, 0x00};
constexpr std::size_t kChainTargetStride = 0x2c, kChainDescription = 0x300, kChainBuffers = 0x328;
constexpr u32 kChainMaxBuffers = 16;
std::uint8_t* g_swap_chain_original = nullptr;
std::atomic<u64> g_display_buffers{0};

std::int64_t __attribute__((sysv_abi)) OnSwapChain(std::uint64_t, const std::uint64_t* saved) {
    const u64 chain = saved[5];
    const std::int64_t result = Call(reinterpret_cast<u64>(g_swap_chain_original), saved[5], saved[4], saved[3], saved[2], saved[1], saved[0], saved[7]);
    if (static_cast<int>(result) == 0) {
        const u32 count = std::min(Peek<u32>(chain + kChainBuffers), kChainMaxBuffers);
        const u32 description[5] = {Peek<u32>(chain + kChainDescription), Peek<u32>(chain + kChainDescription + 4), 1, 1, Peek<u32>(chain + kChainDescription + 0x10)};
        const auto base = [&](u32 i) { return u64(Peek<u32>(chain + i * kChainTargetStride)) << 8; };
        const u64 stride = count > 1 ? base(1) - base(0) : 0;
        for (u32 i = 0; i < count; ++i) NoteResource(base(i), Texture2D, description, base(i), stride, true);
        g_display_buffers.fetch_add(count, std::memory_order_relaxed);
    }
    const_cast<std::uint64_t*>(saved)[-1] = static_cast<std::uint64_t>(result);
    return 1;
}

constexpr std::size_t kUploadPool = 0x26a9b50;
constexpr std::uint8_t kUploadPoolEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
std::atomic<bool> g_buffers_owned{false}, g_arena_waiting{false};
std::atomic<u64> g_arena_base{0}, g_arena_bytes{0};

std::int64_t __attribute__((sysv_abi)) OnUploadPool(std::uint64_t, const std::uint64_t* saved) {
    g_arena_base.store(saved[3], std::memory_order_relaxed);
    g_arena_bytes.store(saved[2] & 0xffffffff, std::memory_order_relaxed);
    g_arena_waiting.store(true, std::memory_order_release);
    std::printf("GX buffers: the upload pool takes %#llx, %llu bytes\n", static_cast<unsigned long long>(saved[3]), static_cast<unsigned long long>(saved[2] & 0xffffffff));
    return 0;
}

template <int V> std::int64_t __attribute__((sysv_abi)) OnRelease(std::uint64_t, const std::uint64_t* saved) {
    const u64 address = saved[5];
    if (kVtables[V].kind == Buffer) BufferVersionOf(address).fetch_add(1, std::memory_order_release);
    if (kVtables[V].kind == Buffer || kVtables[V].kind == Texture2D || kVtables[V].kind == Texture3D) DropResource(address);
    Shard& shard = ShardFor(address);
    std::scoped_lock lock(shard.mutex);
    const auto found = shard.objects.find(address);
    if (found != shard.objects.end() && found->second.live) {
        found->second.live = false;
        g_released[found->second.kind].fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

bool Bound(u64 value) {
    return value + 1 > 1;
}

void Expect(u64 address, Kind kind, Object* out = nullptr) {
    if (!Bound(address)) return;
    g_checked[kind].fetch_add(1, std::memory_order_relaxed);
    Object object;
    if (!Find(address, &object)) {
        g_unknown[kind].fetch_add(1, std::memory_order_relaxed);
        if (g_logged.fetch_add(1, std::memory_order_relaxed) < 32)
            std::printf("GX objects: a bound %s at %#llx was never made by a creator we hook\n", kKindNames[kind],
                        static_cast<unsigned long long>(address));
        return;
    }
    if (object.kind != kind) {
        g_wrong_kind[kind].fetch_add(1, std::memory_order_relaxed);
        if (g_logged.fetch_add(1, std::memory_order_relaxed) < 32)
            std::printf("GX objects: a bound %s at %#llx was made as %s\n", kKindNames[kind],
                        static_cast<unsigned long long>(address), kKindNames[object.kind]);
        return;
    }
    if (!object.live) g_dead[kind].fetch_add(1, std::memory_order_relaxed);
    if (out) *out = object;
}

void Learn(u64 address, Kind kind) {
    if (!Bound(address)) return;
    g_checked[kind].fetch_add(1, std::memory_order_relaxed);
    Object seen;
    seen.kind = kind;
    seen.live = true;
    std::memcpy(seen.description, reinterpret_cast<const void*>(address), kStateBytes[kind]);
    Shard& shard = ShardFor(address);
    std::scoped_lock lock(shard.mutex);
    Object& slot = shard.objects[address];
    if (slot.generation && slot.kind != kind) {
        g_wrong_kind[kind].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (slot.generation && !std::memcmp(slot.description, seen.description, sizeof seen.description)) return;
    if (slot.generation) {
        g_reused[kind].fetch_add(1, std::memory_order_relaxed);
        g_epoch.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_made[kind].fetch_add(1, std::memory_order_relaxed);
    }
    seen.generation = slot.generation + 1;
    slot = seen;
}

void CheckStage(u64 pending, int stage, bool whole) {
    const u64 block = pending + kStageBlocks[stage];
    Object shader;
    const u64 bound = Peek<u64>(block + kStageShader);
    Expect(bound, Shader, &shader);
    if (Bound(bound) && shader.live) g_stage_creator[stage][shader.creator].fetch_add(1, std::memory_order_relaxed);
    if (!whole) return;
    for (int i = 0; i < 14; ++i) Expect(Peek<u64>(block + kStageBuffers + 8 * i), Buffer);
    for (int i = 0; i < 128; ++i) Expect(Peek<u64>(block + kStageViews + 8 * i), ResourceView);
    for (int i = 0; i < 16; ++i) Learn(Peek<u64>(block + kStageSamplers + 8 * i), SamplerState);
}

template <int... C> void Fill(std::array<Callback, kCreatorCount>& out, std::integer_sequence<int, C...>) {
    ((out[C] = OnCreate<C>), ...);
}
template <int... V> void FillRelease(std::array<Callback, kVtableCount>& out, std::integer_sequence<int, V...>) {
    ((out[V] = OnRelease<V>), ...);
}

std::vector<std::uint8_t> Bytes(const char* hex) {
    std::vector<std::uint8_t> out;
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (; hex[0] && hex[1]; hex += 2) out.push_back(static_cast<std::uint8_t>(digit(hex[0]) * 16 + digit(hex[1])));
    return out;
}
}

void PrintLookups();

bool On() {
    return g_on.load(std::memory_order_relaxed);
}

bool Find(std::uint64_t address, Object* out) {
    Shard& shard = ShardFor(address);
    std::scoped_lock lock(shard.mutex);
    const auto found = shard.objects.find(address);
    if (found == shard.objects.end()) return false;
    *out = found->second;
    return true;
}

void CheckDraw(std::uint64_t pending) {
    const bool whole = g_draws.fetch_add(1, std::memory_order_relaxed) % kWholeEvery == 0;
    for (int stage = 0; stage < 5; ++stage) CheckStage(pending, stage, whole);
    Learn(Peek<u64>(pending + kRaster), RasterState);
    Learn(Peek<u64>(pending + kBlend), BlendState);
    Learn(Peek<u64>(pending + kDepthState), DepthState);
    Expect(Peek<u64>(pending + kDepthTarget), DepthView);
    for (int i = 0; i < 8; ++i) Expect(Peek<u64>(pending + kTargets + 8 * i), TargetView);
    if (!whole) return;
    Expect(Peek<u64>(pending + kIndexBuffer), Buffer);
    for (int i = 0; i < 32; ++i) Expect(Peek<u64>(pending + kVertexBuffers + 8 * i), Buffer);
    for (int i = 0; i < 8; ++i) Expect(Peek<u64>(pending + kAccess + 8 * i), AccessView);
}

void CheckDispatch(std::uint64_t pending) {
    CheckStage(pending, 5, g_draws.fetch_add(1, std::memory_order_relaxed) % kWholeEvery == 0);
}

bool TexturesOwned() {
    return g_textures_owned.load(std::memory_order_relaxed);
}

std::uint32_t BufferVersion(std::uint64_t object) {
    return BufferVersionOf(object).load(std::memory_order_acquire);
}

void TouchBuffer(std::uint64_t object) {
    BufferVersionOf(object).fetch_add(1, std::memory_order_release);
}

std::uint64_t Epoch() {
    return g_epoch.load(std::memory_order_relaxed);
}

bool BuffersOwned() {
    return g_buffers_owned.load(std::memory_order_relaxed);
}

bool TakeUploadArena(std::uint64_t* base, std::uint64_t* bytes) {
    if (!g_arena_waiting.load(std::memory_order_acquire) || !g_arena_waiting.exchange(false)) return false;
    *base = g_arena_base.load(std::memory_order_relaxed);
    *bytes = g_arena_bytes.load(std::memory_order_relaxed);
    return true;
}

bool TexturesChecked() {
    return g_textures_checked.load(std::memory_order_relaxed);
}

HeldImage HeldAt(std::uint64_t address) {
    std::scoped_lock lock(g_resource_mutex);
    const auto at = g_resources.find(address);
    if (at == g_resources.end() || at->second.kind == Buffer) return {};
    return {at->second.object, at->second.held_uid, at->second.held_index};
}

void HoldImage(std::uint64_t address, std::uint32_t index, std::uint64_t uid) {
    std::scoped_lock lock(g_resource_mutex);
    const auto at = g_resources.find(address);
    if (at == g_resources.end() || at->second.kind == Buffer) return;
    at->second.held_index = index;
    at->second.held_uid = uid;
}

void NoteObjectPath(ObjectPath path, const char* refused, std::size_t length) {
    g_paths[path].fetch_add(1, std::memory_order_relaxed);
    if (!refused) return;
    std::scoped_lock lock(g_resource_mutex);
    ++g_refused[std::string(refused, length)];
}

void NoteHeldCheck(HeldCheck result, std::uint32_t others) {
    g_held_checks[result].fetch_add(1, std::memory_order_relaxed);
    g_held_others.fetch_add(others, std::memory_order_relaxed);
}

bool TextureAt(std::uint64_t address, TextureExtent* out) {
    std::scoped_lock lock(g_resource_mutex);
    const auto at = g_resources.find(address);
    if (at == g_resources.end() || at->second.kind == Buffer) return false;
    const Resource& resource = at->second;
    u32 levels = resource.mips;
    if (!levels) {
        for (u32 side = std::max(resource.width, resource.height); side; side >>= 1) ++levels;
    }
    out->width = resource.width;
    out->height = resource.height;
    out->levels = levels;
    out->volume = resource.kind == Texture3D;
    out->layers = out->volume || !resource.layers ? 1 : resource.layers;
    const auto next = std::next(at);
    out->room = next == g_resources.end() ? ~u64(0) : next->first - address;
    return true;
}

void NoteWiden(WidenResult result) {
    g_widen[result].fetch_add(1, std::memory_order_relaxed);
}

void NoteOverlap(const char* action, std::size_t length) {
    std::scoped_lock lock(g_resource_mutex);
    ++g_overlaps[std::string(action, length)];
}

void NoteImage(std::uint64_t address, std::uint32_t bytes, std::uint64_t uid, std::uint32_t width, std::uint32_t height,
               std::uint32_t format) {
    if (!On()) return;
    std::scoped_lock lock(g_resource_mutex);
    if (g_lookups.fetch_add(1, std::memory_order_relaxed) % kReportEvery == kReportEvery - 1) {
        PrintLookups();
        std::fflush(stdout);
    }
    auto at = g_resources.upper_bound(address);
    const bool found = at != g_resources.begin();
    if (found) --at;
    const auto span = [](const Resource& r) { return r.bytes ? r.bytes : r.kind == Buffer ? u64(r.width) : u64(r.width) * r.height * 16 * (r.layers ? r.layers : 1) * 2; };
    if (!found || address >= at->second.base + span(at->second)) {
        ++g_image_none;
        ++(g_gone.contains(address) ? g_none_released : (address & 0xfff) ? g_none_unaligned : g_none_never);
        for (Foreign& seen : g_foreign) {
            if (seen.address == address && seen.width == width && seen.height == height && seen.format == format) {
                ++seen.count;
                return;
            }
        }
        if (g_foreign.size() < 256) g_foreign.push_back({address, bytes, width, height, format, 1});
        return;
    }
    Resource& resource = at->second;
    if (resource.display) ++g_image_display;
    if (resource.kind == Buffer) ++g_image_buffer;
    else if (address == resource.base) ++g_image_exact;
    else ++g_image_inside;
    if (resource.kind != Buffer && address == resource.base) ++(resource.width == width && resource.height == height ? g_image_same_size : g_image_other_size);
    for (const ImageSeen& seen : resource.images) {
        if (seen.uid == uid) return;
    }
    resource.images.push_back({uid, width, height, format, bytes});
    ++g_image_resources[uid];
}

void PrintLookups() {
    const u64 lookups = g_image_exact + g_image_inside + g_image_buffer + g_image_none;
    std::printf("GX objects: image lookups %llu, %llu map to an object (%.2f%%), %llu of them a display buffer (%llu buffers); not mapped: %llu released, %llu not page-aligned, %llu page-aligned and never made\n",
                static_cast<unsigned long long>(lookups), static_cast<unsigned long long>(lookups - g_image_none), lookups ? 100.0 * double(lookups - g_image_none) / double(lookups) : 0.0,
                static_cast<unsigned long long>(g_image_display), static_cast<unsigned long long>(g_display_buffers.load()), static_cast<unsigned long long>(g_none_released),
                static_cast<unsigned long long>(g_none_unaligned), static_cast<unsigned long long>(g_none_never));
    std::printf("GX objects: searches past the memo %llu from the object's image; %llu first lookups and %llu whose image was freed, of which %llu made with no other image in their bytes and %llu through the resolver for overlaps; the resolver also for %llu with no object and %llu refused:",
                static_cast<unsigned long long>(g_paths[PathServed].load()), static_cast<unsigned long long>(g_paths[PathNothingHeld].load()),
                static_cast<unsigned long long>(g_paths[PathHeldGone].load()), static_cast<unsigned long long>(g_paths[PathMadeDirectly].load()),
                static_cast<unsigned long long>(g_paths[PathOverlapped].load()), static_cast<unsigned long long>(g_paths[PathNoObject].load()),
                static_cast<unsigned long long>(g_paths[PathHeldRefused].load()));
    for (const auto& [reason, count] : g_refused) std::printf(" %s %llu;", reason.c_str(), static_cast<unsigned long long>(count));
    std::printf("\n");
    if (TexturesChecked()) {
        std::printf("GX objects: checked against the old search: %llu same image, %llu resolver cases, %llu another image; %llu other images in those regions\n",
                    static_cast<unsigned long long>(g_held_checks[CheckSame].load()), static_cast<unsigned long long>(g_held_checks[CheckResolver].load()),
                    static_cast<unsigned long long>(g_held_checks[CheckOther].load()), static_cast<unsigned long long>(g_held_others.load()));
    }
}

void Report() {
    {
        std::scoped_lock lock(g_resource_mutex);
        u64 histogram[5]{}, used = 0, unused = 0;
        int examples = 0;
        const auto count = [&](const Resource& resource) {
            if (resource.kind == Buffer) return;
            if (resource.images.empty()) {
                ++unused;
                return;
            }
            ++used;
            ++histogram[resource.images.size() < 4 ? resource.images.size() : 4];
            if (resource.images.size() > 1 && examples < 12) {
                ++examples;
                std::printf("GX objects: resource %#llx %ux%u, %u mips, %u layers, format %u has %zu images:", static_cast<unsigned long long>(resource.base), resource.width,
                            resource.height, resource.mips, resource.layers, resource.format, resource.images.size());
                for (const ImageSeen& seen : resource.images) std::printf(" %ux%u format %u, %u bytes;", seen.width, seen.height, seen.format, seen.bytes);
                std::printf("\n");
            }
        };
        for (const auto& [base, resource] : g_resources) count(resource);
        for (const Resource& resource : g_finished) count(resource);
        std::sort(g_foreign.begin(), g_foreign.end(), [](const Foreign& a, const Foreign& b) { return a.count > b.count; });
        for (std::size_t i = 0; i < g_foreign.size() && i < 40; ++i) {
            const Foreign& seen = g_foreign[i];
            std::printf("GX objects: no GX resource: %#llx, %u bytes, %ux%u, format %u, %llu lookups\n", static_cast<unsigned long long>(seen.address), seen.bytes, seen.width,
                        seen.height, seen.format, static_cast<unsigned long long>(seen.count));
        }
        std::printf("GX objects: %zu distinct images in no GX resource; YEBIS made %llu textures, %llu failed\n", g_foreign.size(),
                    static_cast<unsigned long long>(g_yebis_made.load()), static_cast<unsigned long long>(g_yebis_failed.load()));
        std::printf("GX objects: image requests widened to their texture %llu, too large %llu, another layout %llu, another size %llu; image cache actions:",
                    static_cast<unsigned long long>(g_widen[Widened].load()), static_cast<unsigned long long>(g_widen[WidenTooLarge].load()),
                    static_cast<unsigned long long>(g_widen[WidenOtherLayout].load()), static_cast<unsigned long long>(g_widen[WidenOtherSize].load()));
        for (const auto& [action, count] : g_overlaps) std::printf(" %s %llu;", action.c_str(), static_cast<unsigned long long>(count));
        std::printf("\n");
        PrintLookups();
        u64 shared = 0;
        for (const auto& [uid, resources] : g_image_resources) shared += resources > 1;
        std::printf("GX objects: image lookups %llu at a texture's address (%llu its size, %llu another), %llu inside a texture, %llu in a buffer, %llu in no GX resource; "
                    "textures %llu never looked up, images per used texture: 1: %llu, 2: %llu, 3: %llu, more: %llu; of %zu images %llu served several textures\n",
                    static_cast<unsigned long long>(g_image_exact), static_cast<unsigned long long>(g_image_same_size), static_cast<unsigned long long>(g_image_other_size),
                    static_cast<unsigned long long>(g_image_inside), static_cast<unsigned long long>(g_image_buffer), static_cast<unsigned long long>(g_image_none),
                    static_cast<unsigned long long>(unused), static_cast<unsigned long long>(histogram[1]), static_cast<unsigned long long>(histogram[2]),
                    static_cast<unsigned long long>(histogram[3]), static_cast<unsigned long long>(histogram[4]), g_image_resources.size(), static_cast<unsigned long long>(shared));
    }
    for (int kind = 0; kind < kKinds; ++kind) {
        std::printf("GX objects: %s: %llu made, %llu failed, %llu released, %llu at a reused address; at draws %llu checked, "
                    "%llu unknown, %llu wrong kind, %llu released\n",
                    kKindNames[kind], static_cast<unsigned long long>(g_made[kind].load()),
                    static_cast<unsigned long long>(g_failed[kind].load()), static_cast<unsigned long long>(g_released[kind].load()),
                    static_cast<unsigned long long>(g_reused[kind].load()), static_cast<unsigned long long>(g_checked[kind].load()),
                    static_cast<unsigned long long>(g_unknown[kind].load()), static_cast<unsigned long long>(g_wrong_kind[kind].load()),
                    static_cast<unsigned long long>(g_dead[kind].load()));
    }
    for (std::size_t creator = 0; creator < kCreatorCount; ++creator) {
        if (!g_input_shaders[creator].load()) continue;
        std::printf("GX objects: %s, %llu shaders, inputs by kind:", kCreators[creator].name,
                    static_cast<unsigned long long>(g_input_shaders[creator].load()));
        for (int kind = 0; kind < 32; ++kind) {
            const u64 count = g_input_kinds[creator][kind].load();
            if (count) std::printf(" %d:%llu", kind, static_cast<unsigned long long>(count));
        }
        std::printf("\n");
    }
    for (int stage = 0; stage < 6; ++stage) {
        for (std::size_t creator = 0; creator < kCreatorCount; ++creator) {
            const u64 draws = g_stage_creator[stage][creator].load();
            if (draws) std::printf("GX objects: stage block %#zx binds shaders from %s, %llu draws\n", kStageBlocks[stage],
                                   kCreators[creator].name, static_cast<unsigned long long>(draws));
        }
    }
    std::fflush(stdout);
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("LANTERN_GX_OBJECTS");
    const char* owned = std::getenv("LANTERN_GX_TEXTURES");
    const bool checked = owned && owned[0] == '2';
    const bool textures = !owned || owned[0] == '1' || checked;
    const char* buffers_env = std::getenv("LANTERN_GX_BUFFERS");
    const bool buffers = !buffers_env || buffers_env[0] == '1';
    const char* records = std::getenv("LANTERN_GX_RECORDS");
    const bool want_records = !records || records[0] == '1' || records[0] == '2' || records[0] == '3';
    if (!textures && !buffers && !want_records && (!on || on[0] != '1')) return false;
    for (const Vtable& v : kVtables) {
        if (v.offset + 16 > size || Peek<u64>(reinterpret_cast<u64>(image) + v.offset) != reinterpret_cast<u64>(image) + v.complete ||
            Peek<u64>(reinterpret_cast<u64>(image) + v.offset + 8) != reinterpret_cast<u64>(image) + v.deleting) {
            host_log("GX objects off: GX's object tables differ from 1.09");
            return false;
        }
    }
    std::array<Callback, kCreatorCount> creators{};
    Fill(creators, std::make_integer_sequence<int, kCreatorCount>{});
    std::array<Callback, kVtableCount> releases{};
    FillRelease(releases, std::make_integer_sequence<int, kVtableCount>{});
    const Callback yebis_callbacks[kYebisCreatorCount] = {OnYebisCreate<0>, OnYebisCreate<1>};
    std::array<Hook, kCreatorCount + kYebisCreatorCount + 2> hooks;
    bool ready = true;
    for (std::size_t c = 0; c < kCreatorCount && ready; ++c) {
        const std::vector<std::uint8_t> entry = Bytes(kCreators[c].entry);
        ready = Prepare(hooks[c], image, size, kCreators[c].offset, entry, creators[c]);
        if (ready) {
            g_originals[c] = OriginalStub(hooks[c]);
            ready = g_originals[c] != nullptr;
        }
    }
    for (std::size_t c = 0; c < kYebisCreatorCount && ready; ++c) {
        ready = Prepare(hooks[kCreatorCount + c], image, size, kYebisCreators[c].offset, kYebisCreators[c].entry, yebis_callbacks[c]);
        if (ready) {
            g_yebis_originals[c] = OriginalStub(hooks[kCreatorCount + c]);
            ready = g_yebis_originals[c] != nullptr;
        }
    }
    if (ready) {
        Hook& hook = hooks[kCreatorCount + kYebisCreatorCount];
        ready = Prepare(hook, image, size, kSwapChainCreator, kSwapChainEntry, OnSwapChain);
        if (ready) {
            g_swap_chain_original = OriginalStub(hook);
            ready = g_swap_chain_original != nullptr;
        }
    }
    if (ready) ready = Prepare(hooks[kCreatorCount + kYebisCreatorCount + 1], image, size, kUploadPool, kUploadPoolEntry, OnUploadPool);
    for (std::size_t v = 0; v < kVtableCount && ready; ++v) {
        g_release_stubs[2 * v] = TailStub(releases[v], reinterpret_cast<u64>(image) + kVtables[v].complete);
        g_release_stubs[2 * v + 1] = TailStub(releases[v], reinterpret_cast<u64>(image) + kVtables[v].deleting);
        ready = g_release_stubs[2 * v] && g_release_stubs[2 * v + 1];
    }
    if (!ready) {
        for (Hook& hook : hooks) Discard(hook);
        for (std::uint8_t* stub : g_originals) FreeStub(stub);
        for (std::uint8_t* stub : g_yebis_originals) FreeStub(stub);
        FreeStub(g_swap_chain_original);
        for (std::uint8_t* stub : g_release_stubs) FreeStub(stub);
        host_log("GX objects off: GX's creators differ from 1.09");
        return false;
    }
    g_shards = new Shard[kShards];
    for (std::size_t v = 0; v < kVtableCount; ++v) {
        const u64 complete = reinterpret_cast<u64>(g_release_stubs[2 * v]), deleting = reinterpret_cast<u64>(g_release_stubs[2 * v + 1]);
        std::memcpy(image + kVtables[v].offset, &complete, sizeof complete);
        std::memcpy(image + kVtables[v].offset + 8, &deleting, sizeof deleting);
    }
    g_on.store(true, std::memory_order_relaxed);
    g_textures_owned.store(textures, std::memory_order_relaxed);
    g_textures_checked.store(checked, std::memory_order_relaxed);
    g_buffers_owned.store(buffers, std::memory_order_relaxed);
    Commit(std::span<Hook>(hooks));
    if (buffers) host_log("GX objects: GX's upload arena stays in place (LANTERN_GX_BUFFERS=1)");
    host_log(textures ? "GX objects: on, textures hold their images (LANTERN_GX_TEXTURES)" : "GX objects: on (LANTERN_GX_OBJECTS=1)");
    return true;
}
}
