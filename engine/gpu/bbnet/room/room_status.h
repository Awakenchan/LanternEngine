// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace BbRoom {

enum class LinkState { Unknown, Good, Slow, Reconnecting };

enum class PartyLineKind { EnteredRoom, JoinedWorld, Left, Kicked, Reconnected, Died, BackOnline, WarpAsked, WarpSeen, WarpOff, WarpNoAnswer, BringFar, LampAsked, LampSeen, LampAgreed, LampRefused, LampNoAnswer, Visitor, HostTravels, TravelLost, StayedWithParty };

struct PeerStatus {
    int slot = 0;
    std::string name;
    int ping_ms = -1;
    LinkState state = LinkState::Unknown;
    bool asked = false;
};

struct PartyLine {
    PartyLineKind kind = PartyLineKind::Left;
    std::string name;
    float alpha = 1.0f;
};

struct ReconnectNotice {
    bool own = false;
    bool gave_up = false;
    int seconds = 0;
    std::vector<std::string> names;
};

enum : std::uint8_t { kWarpGot = 1, kWarpRefused = 2 };
enum : std::uint8_t { kLampGot = 1, kLampAgreed = 2, kLampRefused = 3 };

constexpr auto kPingEvery = std::chrono::seconds(2);
constexpr auto kSilentAfter = std::chrono::seconds(6);
constexpr auto kOwnQuiet = std::chrono::seconds(8);
constexpr auto kLineLasts = std::chrono::seconds(6);
constexpr auto kLineFade = std::chrono::milliseconds(600);
constexpr auto kWarpAskLasts = std::chrono::seconds(60);
constexpr auto kWarpAnswerWithin = std::chrono::seconds(5);
constexpr auto kLampAskLasts = std::chrono::seconds(60);
constexpr auto kLampAnswerWithin = std::chrono::seconds(5);
constexpr auto kGaveUpShown = std::chrono::seconds(12);
constexpr int kSlowPingMs = 250;
constexpr std::size_t kMostLines = 4;

std::vector<int> StaleTwins(const std::map<int, std::string>& names, const std::vector<int>& members, int joining);

class PartyStatus {
public:
    using Time = std::chrono::steady_clock::time_point;

    void Heard(int slot, Time now);
    void Named(int slot, const std::string& name, Time now);
    void Pinged(int slot);
    void Rtt(int slot, int ms);
    void Vitals(int slot, std::int32_t hp, std::int32_t hp_max, std::uint32_t map, Time now);
    void Left(int slot, Time now);
    void Away(int slot, Time now);
    void OwnConnected(Time now);
    void OwnLost(Time now);
    void OwnGaveUp(Time now);
    void Tick(Time now);

    bool Ask(int slot, Time now);
    void ClearAsk(int slot);

    void GuestAsked(Time now);
    void GuestAnswered(std::uint8_t answer, Time now);
    void GuestBrought();
    bool GuestWaiting(Time now) const;

    bool LampAsk(int slot, std::uint32_t lamp, Time now);
    bool LampAskNow(Time now, int& slot, std::string& name, std::uint32_t& lamp) const;
    void LampAskDone();
    void GuestLampAsked(Time now);
    void GuestLampAnswered(std::uint8_t answer, Time now);
    bool GuestLampWaiting(Time now) const;

    void AddLine(PartyLineKind kind, const std::string& name, Time now);
    std::vector<PartyLine> Lines(Time now);
    std::vector<PeerStatus> List(Time now) const;
    bool Reconnecting(Time now, ReconnectNotice& out) const;

private:
    struct Peer {
        std::string name;
        Time heard{};
        int ping_ms = -1;
        bool newer = false;
        bool silent = false;
        std::int32_t hp = 0;
        std::uint32_t map = 0;
        bool has_vitals = false;
    };
    struct Line {
        PartyLineKind kind;
        std::string name;
        Time at;
    };
    std::map<int, Peer> peers_;
    std::map<int, Time> asks_;
    std::vector<Line> lines_;
    Time quiet_until_{};
    Time lost_at_{};
    Time gave_up_at_{};
    bool ever_up_ = false;
    bool down_ = false;
    Time guest_asked_{};
    bool guest_answered_ = false;
    int lamp_slot_ = 0;
    std::uint32_t lamp_ = 0;
    Time lamp_at_{};
    Time guest_lamp_at_{};
    bool guest_lamp_seen_ = false;
};

} // namespace BbRoom
