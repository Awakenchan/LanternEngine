// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <bitset>
#include <cstdint>

namespace BbEngine::TrophyRules {
#ifdef LANTERN_TROPHY_RULES_MUTANTS
enum class Mutant : int { None = 0, AnyKind, AnyBlockInCopy, ReadInCopy, MarkReadInCopy, UnknownCounts, RelayAnyTime, RelayNeverFought, kCount };
inline int g_mutant = 0;
inline bool Is(Mutant m) { return g_mutant == int(m); }
#define LANTERN_TR_MUTANT(name) (::BbEngine::TrophyRules::Is(::BbEngine::TrophyRules::Mutant::name))
#else
#define LANTERN_TR_MUTANT(name) false
#endif

struct Award {
    int trophy;
    std::uint32_t event;
    std::uint32_t ending = 0;
};
inline constexpr Award kAwards[] = {
    {1, 12100180, 6600},  {2, 12100000, 6601},  {3, 12100002, 6602},
    {12, 12110990},
    {23, 12201800},
    {22, 12301800}, {24, 12301700},
    {15, 12401800},
    {21, 12411700}, {14, 12411800},
    {28, 12421800}, {27, 12421700}, {11, 12420990},
    {10, 12500000}, {26, 12501800},
    {20, 12601800}, {19, 12601850},
    {16, 12701800},
    {18, 12801800},
    {17, 13201800}, {13, 13200960},
    {25, 13301800},
    {36, 13401800}, {39, 13401850},
    {37, 13501800}, {38, 13501850},
    {35, 13601800},
};
inline constexpr int kAwardCount = int(sizeof(kAwards) / sizeof(kAwards[0]));

inline constexpr std::int32_t kOwnMode = 0;
inline constexpr std::uint32_t kOwnKind = 1;
inline constexpr std::uint32_t kFirstOwnBlock = 6, kLastOwnBlock = 8;
inline constexpr std::int64_t kRelayWindowMs = 60000;

inline bool OwnBlock(std::int32_t mode, std::uint32_t block, std::uint32_t kind) {
    if (kind != kOwnKind && !LANTERN_TR_MUTANT(AnyKind)) return false;
    if (mode == kOwnMode || LANTERN_TR_MUTANT(AnyBlockInCopy)) return true;
    return block >= kFirstOwnBlock && block <= kLastOwnBlock;
}

template <typename Man> bool OwnFlag(const Man& man, std::uint32_t id, bool* value) {
    std::uint32_t per = 0, kind = 0;
    std::int32_t mode = -1;
    if (!man.Per(&per) || !per || !man.Mode(&mode)) return false;
    const std::uint32_t block = id / per, bit = id % per;
    const std::uint64_t data = man.Block(block, &kind);
    std::uint8_t v = 0;
    if (!data || !OwnBlock(mode, block, kind) || !man.Byte(data, bit / 8, &v)) return false;
    if (value) *value = (v & (0x80u >> (bit % 8))) != 0;
    return true;
}

inline constexpr int kHostRole = 3, kGuestRole = 6;

inline const char* RoleName(int party_role) {
    if (party_role == kHostRole) return "host";
    if (party_role == kGuestRole) return "guest";
    return "own";
}

inline bool AcceptRelayed(std::int64_t fight_seen_ms, std::int64_t now_ms) {
    if (!fight_seen_ms && !LANTERN_TR_MUTANT(RelayNeverFought)) return false;
    return now_ms - fight_seen_ms <= kRelayWindowMs || LANTERN_TR_MUTANT(RelayAnyTime);
}

class Sync {
public:
    bool Done() const { return reported_.all(); }

    template <typename Flag, typename Report>
    void Read(std::uint64_t record, bool in_world, bool have_mode, std::int32_t mode, Flag&& own_flag, Report&& report) {
        if (Done()) return;
        if (!record || !in_world) {
            read_for_ = 0;
            return;
        }
        if ((!have_mode || mode != kOwnMode) && !LANTERN_TR_MUTANT(ReadInCopy)) {
            if (LANTERN_TR_MUTANT(MarkReadInCopy)) read_for_ = record;
            return;
        }
        if (record == read_for_) return;
        read_for_ = record;
        const auto set = [&](std::uint32_t flag) {
            bool on = false;
            const bool known = flag && own_flag(flag, &on);
            return known ? on : (flag && LANTERN_TR_MUTANT(UnknownCounts));
        };
        for (int i = 0; i < kAwardCount; ++i) {
            if (reported_.test(i) || !(set(kAwards[i].event) || set(kAwards[i].ending))) continue;
            reported_.set(i);
            report(kAwards[i].trophy);
        }
    }

private:
    std::bitset<kAwardCount> reported_;
    std::uint64_t read_for_ = 0;
};
}
