// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <x86intrin.h>

namespace BbSections {
enum Id : std::uint32_t {
    Packet,
    Task,
    DrawRecord,
    PrepareRenderState,
    BindResources,
    BindBuffers,
    BindTextures,
    BeginRendering,
    ResolveVertexBuffers,
    ResolveIndexBuffer,
    EmitVertexBuffers,
    UpdateDynamicState,
    DispatchRecord,
    ShaderHle,
    ObtainBuffer,
    Flush,
    Motion,
    PipelineBind,
    Kick,
    Preupload,
    EnsureResident,
    ObtainStream,
    ObtainVram,
    TexSet,
    TexSetCheck,
    TexSetApply,
    TexFull,
    Samplers,
    VramImage,
    VramWritten,
    TexFullEmpty,
    RefreshHash,
    RefreshObtain,
    RefreshDetile,
    RefreshUpload,
    Upscaler,
    UpscalerEval,
    HleBatch,
    HleObtain,
    HleCopy,
    VramTrack,
    VramUpload,
    Protect,
    WaitCopies,
    WaitSignals,
    Count,
};
inline constexpr const char* Names[Count] = {
    "packet",       "tasks",       "draw",          "render state", "bind",
    "buffers",      "textures",    "begin render",  "vertex",       "index",
    "emit vertex",  "dynamic",     "dispatch",      "HLE shader",   "ObtainBuffer",
    "flush",        "motion",      "descriptors",   "kick",         "preupload",
    "EnsureResident", "stream copy", "VRAM path", "tex set", "tex set check", "tex set apply",
    "tex full", "samplers", "VRAM image sync", "VRAM written", "tex full empty",
    "refresh hash", "refresh obtain", "refresh detile", "refresh upload", "upscaler",
    "upscaler eval", "HLE batch", "HLE obtain", "HLE copy", "VRAM track", "VRAM upload",
    "protect", "wait copies", "wait fences",
};
inline std::array<std::atomic<std::uint64_t>, Count> cycles{};

inline const bool enabled = [] {
    const char* env = std::getenv("BB_SECTIONS");
    return env && env[0] == '1';
}();
inline bool Enabled() {
    return enabled;
}
inline thread_local bool recording_thread = false;

struct Scope {
    explicit Scope(Id id_) : id{id_} {
        if (enabled) [[unlikely]] {
            start = recording_thread ? __rdtsc() : 0;
        }
    }
    ~Scope() {
        End();
    }
    void End() {
        if (start) [[unlikely]] {
            cycles[id].fetch_add(__rdtsc() - start, std::memory_order_relaxed);
            start = 0;
        }
    }
    Id id;
    std::uint64_t start = 0;
};
struct ByKey {
    struct Entry {
        std::uint64_t key = 0, cycles = 0, calls = 0;
    };
    std::array<Entry, 1024> entries{};
    void Add(std::uint64_t key, std::uint64_t spent) {
        key |= 1;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            auto& e = entries[(key + i) % entries.size()];
            if (e.key == key || e.key == 0) {
                e.key = key;
                e.cycles += spent;
                ++e.calls;
                return;
            }
        }
    }
    template <typename Print>
    void Top(std::size_t n, Print&& print) {
        std::array<Entry, 1024> copy = entries;
        entries = {};
        std::sort(copy.begin(), copy.end(),
                  [](const Entry& a, const Entry& b) { return a.cycles > b.cycles; });
        for (std::size_t i = 0; i < n && copy[i].key; ++i) {
            print(copy[i]);
        }
    }
};
inline ByKey tex_full_by_shader, dispatch_by_shader;

enum AId : std::uint32_t {
    ADraw,
    APipeline,
    ARing,
    AInputs,
    APacket,
    AKey,
    ARecord,
    ACount,
};
inline constexpr const char* ANames[ACount] = {"draw", "pipeline", "constants", "inputs",
                                               "packet", "key", "record"};
inline std::array<std::atomic<std::uint64_t>, ACount> a_cycles{};
inline std::atomic<std::uint64_t> a_draws{0};
struct ScopeA {
    explicit ScopeA(AId id_) : id{id_} {
        if (enabled) [[unlikely]] {
            start = __rdtsc();
        }
    }
    ~ScopeA() {
        End();
    }
    void End() {
        if (start) [[unlikely]] {
            a_cycles[id].fetch_add(__rdtsc() - start, std::memory_order_relaxed);
            start = 0;
        }
    }
    AId id;
    std::uint64_t start = 0;
};

struct KeyScope {
    KeyScope(ByKey& table_, std::uint64_t key_) : table{table_}, key{key_} {
        if (enabled) [[unlikely]] {
            start = recording_thread ? __rdtsc() : 0;
        }
    }
    ~KeyScope() {
        if (start) [[unlikely]] {
            table.Add(key, __rdtsc() - start);
        }
    }
    ByKey& table;
    std::uint64_t key;
    std::uint64_t start = 0;
};
}

#define BB_SECTION_JOIN2(a, b) a##b
#define BB_SECTION_JOIN(a, b) BB_SECTION_JOIN2(a, b)
#define BB_SECTION(id) BbSections::Scope BB_SECTION_JOIN(bb_section_scope_, __LINE__){BbSections::id}
