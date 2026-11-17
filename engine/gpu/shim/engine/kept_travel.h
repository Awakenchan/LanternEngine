// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BbEngine::KeptTravel {

#ifdef LANTERN_KEPT_TRAVEL_MUTANTS
enum class Mutant : int { None = 0, IgnoreMembers, MembersOnlyWhenSettled, kCount };
inline int g_mutant = 0;
inline bool Is(Mutant m) { return g_mutant == int(m); }
#define LANTERN_KT_MUTANT(name) (::BbEngine::KeptTravel::Is(::BbEngine::KeptTravel::Mutant::name))
#else
#define LANTERN_KT_MUTANT(name) false
#endif

constexpr std::uint32_t kDreamArea = 21, kDungeonArea = 29;
constexpr std::uint32_t kFlagsMagic = 0x45464e49u;
constexpr std::size_t kFlagsLargest = 4096;
constexpr std::uint32_t kFlagsBlocksMost = 64;

constexpr bool CanKeep(std::uint32_t lamp) {
    const std::uint32_t area = lamp / 100000;
    return lamp && area != kDreamArea && area != kDungeonArea;
}

constexpr bool CanKeepMap(std::uint32_t map) {
    const std::uint32_t area = map >> 24;
    return area && area != kDreamArea && area != kDungeonArea;
}

constexpr std::int32_t kNoType = -1, kOwnType = 0, kOwnGreyType = 8;
constexpr int kPhaseSummonedLoad = 3;

constexpr bool PhantomType(std::int32_t type) {
    return type >= 0 && type < 14 && (0x3006u >> type & 1) != 0;
}

constexpr bool WrongTypeForOwnWorld(int phase, bool own_world, std::int32_t remembered, std::int32_t pending) {
    if (phase == kPhaseSummonedLoad || !own_world) return false;
    return pending == kNoType ? PhantomType(remembered) : PhantomType(pending);
}

constexpr std::int32_t OwnWorldType(std::int32_t home) {
    return home == kOwnGreyType ? kOwnGreyType : kOwnType;
}

constexpr bool HoldsHit(std::uint64_t hits, int count, std::uint64_t stride, std::uint64_t hit) {
    return hit >= hits && count > 0 && hit < hits + std::uint64_t(count) * stride && (hit - hits) % stride == 0;
}

constexpr std::int32_t kSnatcherEvent = 13201803;

constexpr bool StoryWarp(std::int32_t event) {
    switch (event) {
    case 3503:
    case 12200131:
    case 12405263:
    case 12800430:
    case 12800431:
    case 13200310:
    case 13200311:
    case kSnatcherEvent: return true;
    default: return false;
    }
}

enum class WarpChoice { Pass, Drop, Hold, Ask, Solo, Stay };
constexpr std::int64_t kStoryHoldMs = 15000, kNotHeld = -1;

constexpr WarpChoice GuestWarpChoice(bool client, std::int32_t script, std::uint32_t lamp, bool following, std::int64_t held_since, std::int64_t now,
                                     bool keep_session) {
    if (!client) return WarpChoice::Pass;
    if (script == kSnatcherEvent) return WarpChoice::Stay;
    const WarpChoice alone = keep_session && CanKeep(lamp) ? WarpChoice::Ask : WarpChoice::Solo;
    if (!StoryWarp(script)) return alone;
    if (following) return WarpChoice::Drop;
    if (held_since == kNotHeld || now - held_since < kStoryHoldMs) return WarpChoice::Hold;
    return alone;
}

class StoryHold {
public:
    WarpChoice Warp(bool client, std::int32_t script, std::uint32_t lamp, bool following, std::int64_t now, bool keep_session) {
        const WarpChoice choice = GuestWarpChoice(client, script, lamp, following, kNotHeld, now, keep_session);
        if (choice == WarpChoice::Hold && !lamp_.load(std::memory_order_acquire)) {
            since_.store(now, std::memory_order_relaxed);
            script_.store(script, std::memory_order_relaxed);
            lamp_.store(lamp, std::memory_order_release);
        }
        return choice;
    }
    WarpChoice Tick(bool client, bool following, std::int64_t now, bool keep_session, std::uint32_t& lamp) {
        lamp = lamp_.load(std::memory_order_acquire);
        if (!lamp) return WarpChoice::Pass;
        const WarpChoice choice = client ? GuestWarpChoice(true, script_.load(std::memory_order_relaxed), lamp, following, since_.load(std::memory_order_relaxed), now, keep_session)
                                         : WarpChoice::Pass;
        if (choice != WarpChoice::Hold) lamp_.store(0, std::memory_order_relaxed);
        return choice;
    }
    std::uint32_t Followed() { return lamp_.exchange(0, std::memory_order_relaxed); }
    bool Held() const { return lamp_.load(std::memory_order_relaxed) != 0; }

private:
    std::atomic<std::uint32_t> lamp_{0};
    std::atomic<std::int64_t> since_{kNotHeld};
    std::atomic<std::int32_t> script_{0};
};

constexpr bool LeaveChecksHeld(std::int64_t rejoin_until, bool kept_active, std::int64_t now) {
    return kept_active || now < rejoin_until;
}

constexpr bool HoldForHost(bool link_up, bool host_here, bool past_limit) {
    return link_up && !host_here && !past_limit;
}

constexpr std::uint32_t kPlaceTextBase = 70000000;

constexpr std::uint32_t LampMapText(std::uint32_t lamp) {
    return kPlaceTextBase + lamp / 10000 * 10;
}

constexpr std::uint32_t LampText(std::uint32_t lamp) {
    return lamp % 1000 < 950 ? LampMapText(lamp) : LampMapText(lamp) + lamp % 1000 - 950;
}

constexpr std::uint32_t SceneSeenFlag(std::uint32_t event) {
    switch (event) {
    case 12405263: return 12401000;
    case 12200130: return 12204020;
    case 12800431: return 12800434;
    default: return event;
    }
}

inline std::uint32_t ReadWord(const std::uint8_t* data, std::size_t word) {
    std::uint32_t value = 0;
    std::memcpy(&value, data + word * 4, 4);
    return value;
}

inline bool ValidFlags(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 20 || size > kFlagsLargest) return false;
    std::uint16_t version = 0;
    std::memcpy(&version, data + 4, 2);
    if (ReadWord(data, 0) != kFlagsMagic || version != 1) return false;
    const std::uint32_t blocks = ReadWord(data, 2), table = ReadWord(data, 3), body = ReadWord(data, 4);
    if (!blocks || blocks > kFlagsBlocksMost || table % 4 || table > size || body > size) return false;
    if (std::uint64_t(table) + std::uint64_t(blocks) * 12 > size) return false;
    for (std::uint32_t i = 0; i < blocks; ++i) {
        const std::size_t word = table / 4 + std::size_t(i) * 3;
        const std::uint64_t end = std::uint64_t(body) + ReadWord(data, word + 1) + ReadWord(data, word + 2);
        if (end > size) return false;
    }
    return true;
}

enum class Step { Wait, Rebuild, Done, GiveUp };

template <typename Clock> class Load {
public:
    using Time = typename Clock::time_point;
    using Seconds = std::chrono::seconds;
    static constexpr Seconds kLoadStart{30}, kLoadLimit{300}, kSettle{2}, kBuildAnyway{20}, kBuildEvery{3}, kBackLimit{120};

    void Begin(Time now) {
        members_left_ = false;
        state_ = State::Asked;
        begun_ = now;
        placed_ = rebuilt_ = Time{};
    }
    bool Active() const { return state_ != State::Off; }
    bool LoadOver() const { return state_ == State::Placed; }
    bool MembersLeft() const { return members_left_; }

    Step Tick(Time now, bool loading, bool placed, bool session, bool others_here, int missing, bool members_in = true) {
        if (state_ == State::Off) return Step::Wait;
        if (!session) return Finish(Step::GiveUp);
        if (loading) {
            state_ = State::Loading;
            if (now - begun_ > kLoadLimit) return Finish(Step::GiveUp);
            return Step::Wait;
        }
        if (state_ == State::Asked) return now - begun_ > kLoadStart ? Finish(Step::GiveUp) : Step::Wait;
        if (state_ == State::Loading) {
            if (!placed) return now - begun_ > kLoadLimit ? Finish(Step::GiveUp) : Step::Wait;
            state_ = State::Placed;
            placed_ = now;
            return Step::Wait;
        }
        const auto since = now - placed_;
        if (!members_in && !LANTERN_KT_MUTANT(IgnoreMembers) && !(LANTERN_KT_MUTANT(MembersOnlyWhenSettled) && since < kSettle)) {
            members_left_ = true;
            return Finish(Step::GiveUp);
        }
        if (since < kSettle) return Step::Wait;
        if (missing == 0 && others_here) return Finish(Step::Done);
        if (since > kBackLimit) return Finish(Step::GiveUp);
        if (missing > 0 && (others_here || since >= kBuildAnyway) && (rebuilt_ == Time{} || now - rebuilt_ >= kBuildEvery)) {
            rebuilt_ = now;
            return Step::Rebuild;
        }
        return Step::Wait;
    }

private:
    enum class State { Off, Asked, Loading, Placed };
    Step Finish(Step step) {
        state_ = State::Off;
        return step;
    }
    State state_ = State::Off;
    Time begun_{}, placed_{}, rebuilt_{};
    bool members_left_ = false;
};

}
