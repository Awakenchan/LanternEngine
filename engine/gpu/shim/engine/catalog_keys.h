// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BbEngine::CatalogKeys {
#ifdef LANTERN_CATALOG_KEYS_MUTANTS
enum class Mutant : int { None = 0, NoVertexRule, NoFragmentRule, KeyMotionIgnored, VertexStageIgnored, WrongStage, kCount };
inline int g_mutant = 0;
inline bool Is(Mutant m) { return g_mutant == int(m); }
#define LANTERN_CK_MUTANT(name) (::BbEngine::CatalogKeys::Is(::BbEngine::CatalogKeys::Mutant::name))
#else
#define LANTERN_CK_MUTANT(name) false
#endif

inline constexpr std::size_t kHeaderBytes = 8;
inline constexpr std::size_t kFragmentStage = 0;
inline constexpr std::size_t kVertexStage = 3;

struct StageMotion {
    bool present = false;
    bool vertex = false;
    bool motion = false;
};

inline bool Stale(std::uint32_t key_motion, StageMotion vs, StageMotion fs) {
    const bool vs_writes = vs.present && (vs.vertex || LANTERN_CK_MUTANT(VertexStageIgnored)) && vs.motion;
    if (!LANTERN_CK_MUTANT(NoFragmentRule) && fs.present && fs.motion && !vs_writes) {
        return true;
    }
    if (LANTERN_CK_MUTANT(NoVertexRule)) {
        return false;
    }
    return (key_motion == 0 || LANTERN_CK_MUTANT(KeyMotionIgnored)) && vs_writes;
}

template <class Meta>
inline bool StaleKey(const unsigned char* key, std::size_t size, std::size_t motion_offset, Meta&& meta) {
    if (size < kHeaderBytes) {
        return false;
    }
    std::uint32_t is_compute = 0;
    std::memcpy(&is_compute, key + 4, 4);
    const std::size_t motion_at = kHeaderBytes + motion_offset;
    if (is_compute != 0 || size < motion_at + 4 || size < kHeaderBytes + (kVertexStage + 1) * 8) {
        return false;
    }
    const std::size_t vertex = LANTERN_CK_MUTANT(WrongStage) ? kVertexStage + 1 : kVertexStage;
    std::uint64_t vs_hash = 0, fs_hash = 0;
    std::uint32_t key_motion = 0;
    std::memcpy(&vs_hash, key + kHeaderBytes + vertex * 8, 8);
    std::memcpy(&fs_hash, key + kHeaderBytes + kFragmentStage * 8, 8);
    std::memcpy(&key_motion, key + motion_at, 4);
    const StageMotion vs = vs_hash ? meta(vs_hash) : StageMotion{};
    const StageMotion fs = fs_hash ? meta(fs_hash) : StageMotion{};
    return Stale(key_motion, vs, fs);
}
}
