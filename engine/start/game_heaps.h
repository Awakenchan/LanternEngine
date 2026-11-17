// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

#ifdef GAME_HEAPS_TRIALS
inline int game_heaps_trial = 0;
#define GAME_HEAPS_PICK(trial, shipped, wrong) (game_heaps_trial == (trial) ? (wrong) : (shipped))
#define GAME_HEAPS_CONSTEXPR inline
#else
#define GAME_HEAPS_PICK(trial, shipped, wrong) (shipped)
#define GAME_HEAPS_CONSTEXPR constexpr
#endif

namespace start::game_heaps {

constexpr std::uint64_t kMiB = std::uint64_t(1) << 20;

constexpr std::uint64_t kHeapTable = 0x4736dd8;
constexpr std::uint64_t kHeapWord = 8;
constexpr std::uint64_t kGraphicsPrivateAt = kHeapTable;
constexpr std::uint64_t kGraphicsPrivateBAt = kHeapTable + kHeapWord;
constexpr std::uint64_t kHeapTableBytes = 2 * kHeapWord;
constexpr std::uint64_t kSunAtlasSideTable = 0x48a9668;
constexpr bool kHeapTableCheckedByGxFacts = false;
constexpr bool kSunAtlasSideTableCheckedByGxFacts = false;

static_assert(kGraphicsPrivateBAt >= kGraphicsPrivateAt + kHeapWord);

constexpr std::uint64_t kGraphicsPrivatePs4 = 0x8ef00000;
constexpr std::uint64_t kGraphicsPrivateBPs4 = 0x37600000;
constexpr std::uint64_t kGraphicsPrivateEnlarged = 0xf7000000;
constexpr std::uint64_t kGraphicsPrivateBEnlarged = 0xa0000000;
constexpr std::uint64_t kGraphicsPrivateStart = 0x42c00000;
constexpr std::uint64_t kHeapsAfterB = 0x16100000;

constexpr std::uint64_t kPoolPs4Mib = 5056;
constexpr std::uint64_t kPoolEnlargedMib = 9152;
constexpr std::uint64_t kPoolMaxMib = 16384;

constexpr int kSunShadowGame = 0;
constexpr int kSunShadowMax = 2;
constexpr std::uint64_t kSunAtlasGameSide = 2048;
constexpr std::uint64_t kSunAtlasSmallestGameSide = 1024;
constexpr std::uint64_t kSunAtlasTexelBytes = 4;
constexpr std::uint64_t kSunAtlases = 1;

enum class Base { Ps4, Enlarged };

struct Sizes {
    std::uint64_t graphics_private = 0;
    std::uint64_t graphics_private_b = 0;
    friend constexpr bool operator==(const Sizes&, const Sizes&) = default;
};

constexpr Sizes kPs4{kGraphicsPrivatePs4, kGraphicsPrivateBPs4};
constexpr Sizes kEnlarged{kGraphicsPrivateEnlarged, kGraphicsPrivateBEnlarged};

GAME_HEAPS_CONSTEXPR int SunShadowQuality(long setting) {
    return int(std::clamp<long>(setting, GAME_HEAPS_PICK(1, kSunShadowGame, -1), kSunShadowMax));
}

constexpr std::uint64_t SunAtlasSide(int quality) {
    return kSunAtlasGameSide << std::clamp(quality, kSunShadowGame, kSunShadowMax);
}

GAME_HEAPS_CONSTEXPR std::uint64_t RoundUpToMiB(std::uint64_t bytes) {
    return GAME_HEAPS_PICK(2, (bytes + kMiB - 1) / kMiB * kMiB, bytes / kMiB * kMiB);
}

GAME_HEAPS_CONSTEXPR std::uint64_t SunShadowExtraBytes(int quality) {
    if (GAME_HEAPS_PICK(3, quality <= kSunShadowGame, quality < kSunShadowGame)) return 0;
    const std::uint64_t side = SunAtlasSide(quality);
    const std::uint64_t smallest = GAME_HEAPS_PICK(4, kSunAtlasSmallestGameSide, kSunAtlasGameSide);
    return RoundUpToMiB((side * side - smallest * smallest) * kSunAtlasTexelBytes * kSunAtlases);
}

GAME_HEAPS_CONSTEXPR Sizes Plan(Base base, std::uint64_t gx_region_extra, std::uint64_t shadow_extra) {
    if (base == Base::Enlarged)
        return {kGraphicsPrivateEnlarged + GAME_HEAPS_PICK(5, shadow_extra, 0), kGraphicsPrivateBEnlarged};
    return {kGraphicsPrivatePs4 + gx_region_extra + shadow_extra,
            kGraphicsPrivateBPs4 + GAME_HEAPS_PICK(6, 0, shadow_extra)};
}

GAME_HEAPS_CONSTEXPR std::uint64_t DirectMemoryNeed(Sizes sizes) {
    return kGraphicsPrivateStart + sizes.graphics_private + sizes.graphics_private_b +
           GAME_HEAPS_PICK(7, kHeapsAfterB, 0);
}

GAME_HEAPS_CONSTEXPR bool Fits(Sizes sizes, std::uint64_t pool_mib) {
    return GAME_HEAPS_PICK(8, DirectMemoryNeed(sizes) <= pool_mib * kMiB, DirectMemoryNeed(sizes) < pool_mib);
}

constexpr std::uint64_t PoolMib(bool set, std::uint64_t parsed, Base base) {
    if (!set) return base == Base::Enlarged ? kPoolEnlargedMib : kPoolPs4Mib;
    return parsed < kPoolPs4Mib || parsed > kPoolMaxMib ? kPoolPs4Mib : parsed;
}

struct Inputs {
    bool table_is_ps4 = true;
    bool table_written_by_patch = false;
    bool enlarge_wanted = false;
    bool enlarge_allowed = false;
    std::uint64_t gx_region_extra = 0;
    int sun_quality = 0;
    std::uint64_t pool_ps4_mib = kPoolPs4Mib;
    std::uint64_t pool_enlarged_mib = kPoolEnlargedMib;
};

enum class Outcome { Sized, PatchOwnsTable, TableDiffers };

struct Decision {
    Outcome outcome = Outcome::Sized;
    Base base = Base::Ps4;
    Sizes sizes = kPs4;
    int sun_quality = 0;
    bool write_private = false;
    bool write_private_b = false;
    bool heap_patch = false;
    bool enlarged_for_room = false;
    bool sun_room_reduced = false;
    bool fits = true;
};

GAME_HEAPS_CONSTEXPR Decision Decide(const Inputs& in) {
    Decision d;
    d.sun_quality = in.sun_quality;
    if (GAME_HEAPS_PICK(9, in.table_written_by_patch, false)) {
        d.outcome = Outcome::PatchOwnsTable;
        d.heap_patch = true;
        return d;
    }
    if (!in.table_is_ps4) {
        d.outcome = Outcome::TableDiffers;
        return d;
    }
    d.base = in.enlarge_wanted ? Base::Enlarged : Base::Ps4;
    const auto size_for = [&](int quality) { return Plan(d.base, in.gx_region_extra, SunShadowExtraBytes(quality)); };
    d.sizes = size_for(d.sun_quality);
    if (d.base == Base::Ps4 && !Fits(d.sizes, in.pool_ps4_mib) && GAME_HEAPS_PICK(10, in.enlarge_allowed, true)) {
        d.base = Base::Enlarged;
        d.enlarged_for_room = true;
        d.sizes = size_for(d.sun_quality);
    }
    const std::uint64_t pool = d.base == Base::Enlarged ? in.pool_enlarged_mib : in.pool_ps4_mib;
    while (d.sun_quality > kSunShadowGame && !Fits(d.sizes, pool)) {
        --d.sun_quality;
        d.sun_room_reduced = true;
        d.sizes = size_for(d.sun_quality);
    }
    d.fits = Fits(d.sizes, pool);
    d.write_private = d.sizes.graphics_private != kGraphicsPrivatePs4;
    d.write_private_b = GAME_HEAPS_PICK(11, d.sizes.graphics_private_b != kGraphicsPrivateBPs4, d.write_private);
    d.heap_patch = d.base == Base::Enlarged;
    return d;
}

constexpr int kTrials = 11;

}
