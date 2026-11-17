// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef LANTERN_FOG_SCALE_MUTANTS
namespace BbEngine::FogScale {
inline int g_mutant = 0;
}
#define LANTERN_FOG_SCALE_PICK(n, wrong, right) (::BbEngine::FogScale::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_FOG_SCALE_PICK(n, wrong, right) (right)
#endif

namespace BbEngine::FogScale {
inline constexpr int kMutants = 4;
inline constexpr std::size_t kBlockBytes = 0x60;
inline constexpr std::size_t kBlockFloats = kBlockBytes / 4;
inline constexpr std::uint32_t kMaxBlocks = 4096;
inline constexpr std::size_t kMaxBytes = std::size_t(kMaxBlocks) * kBlockBytes;
inline constexpr int kOpacities[] = {0, 8, 12};

inline bool Untouched(bool on, float k) { return LANTERN_FOG_SCALE_PICK(4, !on, !on || k == 1.0f); }

inline float Scaled(float a, float k) { return k > 1 ? std::min(a * k, std::max(a, 1.0f)) : a * k; }

inline std::size_t CopyBytes(std::uint32_t count, std::uint32_t upload) {
    const std::size_t want = std::max(std::size_t(count) * kBlockBytes, (std::size_t(upload) + 3) & ~std::size_t(3));
    return LANTERN_FOG_SCALE_PICK(3, true, want <= kMaxBytes) ? want : 0;
}

inline std::uint8_t* ScaleCopy(const std::uint8_t* table, std::uint32_t count, std::size_t bytes, float k,
                               std::uint8_t* scratch) {
    std::uint8_t* out = LANTERN_FOG_SCALE_PICK(1, const_cast<std::uint8_t*>(table), scratch);
    if (out != table) std::memcpy(out, table, bytes);
    const std::size_t blocks = std::min<std::size_t>(count, bytes / kBlockBytes);
    for (std::size_t i = LANTERN_FOG_SCALE_PICK(2, 1, 0); i < blocks; ++i) {
        for (const int o : kOpacities) {
            std::uint8_t* at = out + i * kBlockBytes + std::size_t(o) * 4;
            float a = 0;
            std::memcpy(&a, at, sizeof a);
            a = Scaled(a, k);
            std::memcpy(at, &a, sizeof a);
        }
    }
    return out;
}

inline float SnapToOne(float v) { return std::fabs(v - 1.0f) < 0.005f ? 1.0f : v; }

inline std::int32_t MenuStep(float value, float base, float step, std::int32_t top) {
    const float steps = (value - base) / step;
    const std::int32_t n = std::isfinite(steps) ? std::int32_t(std::clamp(std::lround(steps), 0l, long(top))) : 0;
    if (base + float(n) * step != 1.0f || value == 1.0f) return n;
    if (value > 1.0f && n < top) return n + 1;
    return n > 0 ? n - 1 : n + 1;
}
}
