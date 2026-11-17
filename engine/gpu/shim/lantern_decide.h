// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#include "lantern_diag_c.h"

namespace Lantern::Decide {

inline constexpr std::uint32_t kNoHash = ~0u;
inline constexpr std::uint32_t kMinShareMask = 63;

constexpr std::uint32_t MixHash(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    const std::uint32_t h = std::uint32_t(x ^ (x >> 32));
    return h == kNoHash ? h ^ 1u : h;
}

constexpr bool MaskHit(std::uint32_t hash, std::uint32_t mask) {
    return LANTERN_MUTANT(10) ? (hash & mask) != 0 : (hash & mask) == 0;
}

constexpr bool MinShareDraw(std::uint32_t hash) {
    return (hash & kMinShareMask) == 0;
}

constexpr bool MayEnterSlowPath(bool shared, std::uint32_t hash, std::uint32_t mask) {
    if (shared || hash == kNoHash) return false;
    return MaskHit(hash, mask) || MinShareDraw(hash);
}

struct MinShare {
    std::uint64_t second;
    std::uint64_t last_second;
    std::uint32_t frame_in_second;
    std::uint32_t frames_per_second;
    int step;
};

constexpr bool MinShareAllowed(const MinShare& m) {
    if (m.frames_per_second == 0) return false;
    if (m.last_second >= m.second && !LANTERN_MUTANT(11)) return false;
    return m.frame_in_second == std::uint32_t(m.step) % m.frames_per_second;
}

enum class Verdict : std::uint8_t { Skip, Check, Starved };

constexpr Verdict SampleVerdict(bool mask_hit, bool tokens, bool min_share) {
    if (mask_hit && (tokens || LANTERN_MUTANT(12))) return Verdict::Check;
    if (min_share) return Verdict::Check;
    return mask_hit ? Verdict::Starved : Verdict::Skip;
}

struct Buckets {
    std::int64_t rate;
    std::int64_t first;
};

struct Earn {
    std::int64_t rate;
    std::int64_t first;
    std::int64_t cap;
    bool spill;
};

constexpr Buckets Refill(Buckets b, const Earn& e) {
    b.rate += e.rate;
    b.first += e.first;
    if (b.first > e.cap) {
        if (e.spill) b.rate += b.first - e.cap;
        b.first = e.cap;
    }
    if (b.rate > e.cap && !LANTERN_MUTANT(13)) b.rate = e.cap;
    return b;
}

constexpr std::int64_t Spend(std::int64_t bucket, std::uint64_t ticks) {
    const std::int64_t spent = ticks > std::uint64_t(INT64_MAX / 4) ? INT64_MAX / 4 : std::int64_t(ticks);
    if (LANTERN_MUTANT(14)) return bucket - std::int64_t(std::uint32_t(spent));
    return bucket < INT64_MIN / 2 ? bucket : bucket - spent;
}

constexpr bool InPart(int index, int count, int k, int n) {
    if (n <= 0 || k < 1 || k > n || count <= 0) return false;
    const long begin = long(k - 1) * count / n;
    long end = LANTERN_MUTANT(15) ? long(k) * count / n + 1 : long(k) * count / n;
    return index >= begin && index < end;
}

enum class Suspect : std::uint8_t { None, NothingCovered, CoveredButUnchecked, AllDiffer };

constexpr Suspect InstrumentSuspect(bool checking_mode, std::uint64_t covered, std::uint64_t total,
                                    std::uint64_t checked, std::uint64_t differ) {
    if (total > 0 && covered == 0) return Suspect::NothingCovered;
    if (!checking_mode) return Suspect::None;
    if (covered > 0 && checked == 0 && !LANTERN_MUTANT(16)) return Suspect::CoveredButUnchecked;
    if (checked > 0 && differ == checked) return Suspect::AllDiffer;
    return Suspect::None;
}

inline constexpr int kFirstStepMutant = 10;
inline constexpr int kLastStepMutant = 16;

}
