// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BbEngine::InsightSync {

#ifdef LANTERN_INSIGHT_SYNC_MUTANTS
enum class Mutant : int { None = 0, GateUsesOwn, StaleHostUsed, HostUsedOutsideParty, AboveForAtLeast, AnySizeTaken, QuestWithoutFacts, NoHeartbeat, RefundKept, RefundWithPaidBells, AnyInsightSkipped, ForgetIgnored, kCount };
inline int g_mutant = 0;
inline bool Is(Mutant m) { return g_mutant == int(m); }
#define LANTERN_IS_MUTANT(name) (::BbEngine::InsightSync::Is(::BbEngine::InsightSync::Mutant::name))
#else
#define LANTERN_IS_MUTANT(name) false
#endif

constexpr std::uint8_t kFrameKind = 29;
constexpr std::size_t kFrameBytes = 4;
constexpr std::int32_t kMostInsight = 999;
constexpr std::uint8_t kQuestPresence = 1;
constexpr int kRoleHost = 3, kRoleClient = 6;
constexpr std::int64_t kHeartbeatMs = 15000, kFreshMs = 45000;

constexpr std::size_t kGate = 0x1454770, kGateSlot = 0x53219e0;
constexpr std::uint8_t kGateEntry[] = {0xb0, 0x01, 0x0f, 0xb6, 0x4f, 0x29, 0x48, 0x83, 0xf9, 0x05};
constexpr std::size_t kGateValue = 0x28, kGateCompare = 0x29, kGateResult = 0x10;

struct Frame {
    std::uint16_t insight = 0;
    std::uint8_t facts = 0;
};

constexpr std::uint16_t Clamp(std::int64_t insight) {
    return insight < 0 ? 0 : insight > kMostInsight ? std::uint16_t(kMostInsight) : std::uint16_t(insight);
}

inline std::array<std::uint8_t, kFrameBytes> Encode(const Frame& frame) {
    std::array<std::uint8_t, kFrameBytes> out{kFrameKind};
    const std::uint16_t insight = Clamp(frame.insight);
    std::memcpy(out.data() + 1, &insight, 2);
    out[3] = frame.facts;
    return out;
}

inline bool Decode(const std::uint8_t* data, std::size_t size, Frame& out) {
    if (!data || size < 1 || data[0] != kFrameKind) return false;
    if (size != kFrameBytes && !(LANTERN_IS_MUTANT(AnySizeTaken) && size >= kFrameBytes)) return false;
    std::uint16_t insight = 0;
    std::memcpy(&insight, data + 1, 2);
    out.insight = Clamp(insight);
    out.facts = data[3];
    return true;
}

constexpr bool ShouldSend(bool host, std::int32_t insight, std::int32_t last_sent, std::int64_t since_sent_ms, bool asked) {
    if (!host || insight < 0) return false;
    if (asked || insight != last_sent) return true;
    return !LANTERN_IS_MUTANT(NoHeartbeat) && since_sent_ms >= kHeartbeatMs;
}

constexpr bool Fresh(std::int32_t host, std::int64_t host_age_ms) {
    if (host < 0 || host_age_ms < 0) return false;
    return host_age_ms <= kFreshMs || LANTERN_IS_MUTANT(StaleHostUsed);
}

constexpr std::int32_t ValueForGate(int role, std::int32_t own, std::int32_t host, std::int64_t host_age_ms) {
    if (role != kRoleClient && !LANTERN_IS_MUTANT(HostUsedOutsideParty)) return own;
    return Fresh(host, host_age_ms) && !LANTERN_IS_MUTANT(GateUsesOwn) ? host : own;
}

constexpr bool HostSendsQuest(std::uint8_t facts, std::int32_t host, std::int64_t host_age_ms) {
    return Fresh(host, host_age_ms) && ((facts & kQuestPresence) != 0 || LANTERN_IS_MUTANT(QuestWithoutFacts));
}

class HostValue {
public:
    void Take(const Frame& frame, std::int64_t now_ms) {
        frame_ = frame;
        at_ms_ = now_ms;
        have_ = true;
    }
    void Forget() {
        if (!LANTERN_IS_MUTANT(ForgetIgnored)) have_ = false;
    }
    bool Get(std::int64_t now_ms, Frame& frame, std::int64_t& age_ms) const {
        if (!have_) return false;
        frame = frame_;
        age_ms = now_ms - at_ms_;
        return true;
    }

private:
    Frame frame_{};
    std::int64_t at_ms_ = 0;
    bool have_ = false;
};

constexpr bool Passes(std::int64_t insight, std::uint8_t value, std::uint8_t compare) {
    const auto have = std::uint32_t(insight), want = std::uint32_t(value);
    switch (compare) {
    case 0: return have == want;
    case 1: return have != want;
    case 2: return want < have;
    case 3: return have < want;
    case 4: return LANTERN_IS_MUTANT(AboveForAtLeast) ? want < have : want <= have;
    case 5: return have <= want;
    default: return true;
    }
}

constexpr std::uint8_t kEqual = 0, kBelow = 3, kAtLeast = 4, kAtMost = 5;

struct CathedralFlags {
    bool f12404002 = false, f12404001 = false, f12404003 = false;
};

constexpr CathedralFlags CathedralModel(std::int32_t owner_insight) {
    return {!Passes(owner_insight, 15, kBelow), !Passes(owner_insight, 40, kBelow), !Passes(owner_insight, 50, kBelow)};
}

constexpr bool CathedralAmygdalaShown(bool night, bool f12404001) {
    return night || f12404001;
}

constexpr bool YahargulAmygdalaShown(bool night, std::int32_t gate_insight) {
    return night || Passes(gate_insight, 40, kAtLeast);
}

constexpr bool Mask2410600Shown(std::int32_t gate_insight) {
    return Passes(gate_insight, 20, kAtLeast);
}

constexpr std::int32_t kBellRefundEvent = 9183, kBellRefundStep = 6, kSetEffectBank = 2004, kSetEffectId = 8, kInsightEffect = 4680;

constexpr bool SkipBellRefund(bool free_bells, std::int32_t event, std::int32_t step, std::int32_t bank, std::int32_t id, std::int32_t effect) {
    if (step != kBellRefundStep || bank != kSetEffectBank || id != kSetEffectId || effect != kInsightEffect) return false;
    if (event != kBellRefundEvent && !LANTERN_IS_MUTANT(AnyInsightSkipped)) return false;
    if (!free_bells && !LANTERN_IS_MUTANT(RefundWithPaidBells)) return false;
    return !LANTERN_IS_MUTANT(RefundKept);
}

constexpr std::int32_t kBellCost = 1, kRefund = 1;

constexpr std::int32_t BellCycle(bool free_bells, std::int32_t insight, bool cooperator_came) {
    const std::int32_t paid = free_bells ? 0 : kBellCost;
    const bool refund = !cooperator_came && !SkipBellRefund(free_bells, kBellRefundEvent, kBellRefundStep, kSetEffectBank, kSetEffectId, kInsightEffect);
    return insight - paid + (refund ? kRefund : 0);
}

}
