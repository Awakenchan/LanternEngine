// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#ifdef LANTERN_PARTY_DEATH_MUTANTS
namespace BbEngine::PartyDeath {
inline int g_mutant = 0;
}
#define LANTERN_PARTY_DEATH_PICK(n, wrong, right) (::BbEngine::PartyDeath::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_PARTY_DEATH_PICK(n, wrong, right) (right)
#endif

namespace BbEngine::PartyDeath {

inline constexpr int kMutants = 3;

inline std::uint32_t SpectateCameraInput(std::uint32_t game_flag, bool watching_other) {
    return LANTERN_PARTY_DEATH_PICK(1, game_flag,
           LANTERN_PARTY_DEATH_PICK(2, watching_other ? game_flag : 0u,
           LANTERN_PARTY_DEATH_PICK(3, 0u, watching_other ? 0u : game_flag)));
}

struct Situation {
    bool in_party = false;
    bool boss_up = false;
    bool wipe_called = false;
    bool all_down_held = false;
    bool anyone_else_up = false;
};

enum class Move { Stay, ToLamp, PartyWipe, GameLoad };

inline Move Decide(const Situation& s) {
    if (!s.in_party) return Move::GameLoad;
    if (s.wipe_called) return Move::PartyWipe;
    if (!s.boss_up) return Move::ToLamp;
    if (s.anyone_else_up) return Move::Stay;
    return s.all_down_held ? Move::PartyWipe : Move::Stay;
}

template <typename Clock> class Held {
public:
    using Time = typename Clock::time_point;
    using Span = typename Clock::duration;

    bool Update(bool now_true, Time now, Span window) {
        if (!now_true) {
            on_ = false;
            return false;
        }
        if (!on_) {
            on_ = true;
            since_ = now;
        }
        return now - since_ >= window;
    }

private:
    bool on_ = false;
    Time since_{};
};

template <typename Clock> class FightWatch {
public:
    using Time = typename Clock::time_point;
    using Span = typename Clock::duration;

    explicit FightWatch(Span gone_after) : gone_after_(gone_after) {}

    void Update(bool bar_up, Time now) {
        if (!bar_up) return;
        seen_ = true;
        last_ = now;
    }
    void Begin(Time now) { seen_ = seen_ && now - last_ < gone_after_; }
    bool Seen() const { return seen_; }
    bool Up(Time now) const { return seen_ && now - last_ < gone_after_; }
    bool Over(Time now) const { return seen_ && !Up(now); }

private:
    Span gone_after_;
    bool seen_ = false;
    Time last_{};
};

struct Living {
    int slot = 0;
    std::string name;
};

class Spectate {
public:
    int Update(std::vector<Living> living, bool next) {
        std::sort(living.begin(), living.end(), [](const Living& a, const Living& b) { return a.slot < b.slot; });
        count_ = int(living.size());
        if (living.empty()) {
            slot_ = -1;
            name_.clear();
            return slot_;
        }
        const auto after = std::upper_bound(living.begin(), living.end(), slot_,
                                            [](int slot, const Living& l) { return slot < l.slot; });
        const auto following = after == living.end() ? living.begin() : after;
        const auto here = std::find_if(living.begin(), living.end(), [&](const Living& l) { return l.slot == slot_; });
        const auto chosen = here == living.end() || next ? following : here;
        slot_ = chosen->slot;
        name_ = chosen->name;
        return slot_;
    }

    void Clear() {
        slot_ = -1;
        count_ = 0;
        name_.clear();
    }
    int Slot() const { return slot_; }
    int Count() const { return count_; }
    const std::string& Name() const { return name_; }

private:
    int slot_ = -1;
    int count_ = 0;
    std::string name_;
};

}
