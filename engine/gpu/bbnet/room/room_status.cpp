// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_status.h"

#include <algorithm>

#include "room_announce.h"

namespace BbRoom {

std::vector<int> StaleTwins(const std::map<int, std::string>& names, const std::vector<int>& members, int joining) {
    std::vector<int> out;
    const auto own = names.find(joining);
    if (own == names.end() || own->second.empty()) return out;
    for (const int slot : members) {
        if (slot == joining) continue;
        const auto other = names.find(slot);
        if (other != names.end() && other->second == own->second) out.push_back(slot);
    }
    return out;
}

void PartyStatus::Heard(int slot, Time now) {
    Peer& peer = peers_[slot];
    peer.heard = now;
    if (!peer.silent) return;
    peer.silent = false;
    if (!peer.name.empty()) AddLine(PartyLineKind::Reconnected, peer.name, now);
}

void PartyStatus::Named(int slot, const std::string& name, Time now) {
    const std::string clean = CleanAnnouncement(name);
    if (clean.empty()) return;
    for (auto it = peers_.begin(); it != peers_.end();) {
        const Peer& other = it->second;
        const bool stale = other.silent || now - other.heard >= kSilentAfter;
        if (it->first == slot || other.name != clean || !stale) {
            ++it;
            continue;
        }
        asks_.erase(it->first);
        if (it->first == lamp_slot_) LampAskDone();
        it = peers_.erase(it);
    }
    Peer& peer = peers_[slot];
    const bool first = peer.name.empty();
    peer.name = clean;
    if (first && now >= quiet_until_) AddLine(PartyLineKind::EnteredRoom, clean, now);
}

void PartyStatus::Pinged(int slot) {
    peers_[slot].newer = true;
}

void PartyStatus::Rtt(int slot, int ms) {
    if (ms < 0 || ms > 60000) return;
    Peer& peer = peers_[slot];
    peer.ping_ms = peer.ping_ms < 0 ? ms : (peer.ping_ms * 3 + ms) / 4;
}

void PartyStatus::Vitals(int slot, std::int32_t hp, std::int32_t hp_max, std::uint32_t map, Time now) {
    Peer& peer = peers_[slot];
    const bool died = peer.has_vitals && peer.hp > 0 && hp <= 0 && hp_max > 0 && peer.map == map;
    peer.hp = hp;
    peer.map = map;
    peer.has_vitals = true;
    if (died && !peer.name.empty()) AddLine(PartyLineKind::Died, peer.name, now);
}

void PartyStatus::Left(int slot, Time now) {
    asks_.erase(slot);
    if (slot == lamp_slot_) LampAskDone();
    const auto it = peers_.find(slot);
    if (it == peers_.end()) return;
    const std::string name = it->second.name;
    peers_.erase(it);
    if (!name.empty()) AddLine(PartyLineKind::Left, name, now);
}

void PartyStatus::Away(int slot, Time now) {
    const auto it = peers_.find(slot);
    if (it == peers_.end() || it->second.silent) return;
    it->second.silent = true;
    it->second.heard = now - kSilentAfter;
}

void PartyStatus::OwnConnected(Time now) {
    if (ever_up_ && down_) AddLine(PartyLineKind::BackOnline, {}, now);
    if (!ever_up_) quiet_until_ = now + kOwnQuiet;
    ever_up_ = true;
    down_ = false;
    gave_up_at_ = {};
    for (auto& [slot, peer] : peers_) {
        peer.heard = now;
        peer.silent = false;
    }
}

void PartyStatus::OwnLost(Time now) {
    if (down_) return;
    down_ = true;
    lost_at_ = now;
}

void PartyStatus::OwnGaveUp(Time now) {
    gave_up_at_ = now;
}

void PartyStatus::Tick(Time now) {
    if (guest_asked_ != Time{} && !guest_answered_ && now - guest_asked_ >= kWarpAnswerWithin) {
        guest_asked_ = {};
        AddLine(PartyLineKind::WarpNoAnswer, {}, now);
    }
    if (guest_lamp_at_ != Time{} && now - guest_lamp_at_ >= (guest_lamp_seen_ ? kLampAskLasts : kLampAnswerWithin)) {
        guest_lamp_at_ = {};
        AddLine(PartyLineKind::LampNoAnswer, {}, now);
    }
    if (down_) return;
    for (auto& [slot, peer] : peers_) {
        if (peer.newer && !peer.silent && now - peer.heard >= kSilentAfter) peer.silent = true;
    }
}

bool PartyStatus::Ask(int slot, Time now) {
    const auto it = asks_.find(slot);
    const bool fresh = it == asks_.end() || now - it->second >= kWarpAskLasts;
    asks_[slot] = now;
    if (fresh) {
        const auto peer = peers_.find(slot);
        AddLine(PartyLineKind::WarpAsked, peer == peers_.end() ? std::string() : peer->second.name, now);
    }
    return fresh;
}

void PartyStatus::ClearAsk(int slot) {
    if (slot) asks_.erase(slot);
    else asks_.clear();
}

void PartyStatus::GuestAsked(Time now) {
    guest_asked_ = now;
    guest_answered_ = false;
}

void PartyStatus::GuestAnswered(std::uint8_t answer, Time now) {
    if (guest_asked_ == Time{}) return;
    if (answer == kWarpGot) {
        if (!guest_answered_) AddLine(PartyLineKind::WarpSeen, {}, now);
        guest_answered_ = true;
    } else if (answer == kWarpRefused) {
        guest_asked_ = {};
        AddLine(PartyLineKind::WarpOff, {}, now);
    }
}

void PartyStatus::GuestBrought() {
    guest_asked_ = {};
    guest_answered_ = false;
}

bool PartyStatus::GuestWaiting(Time now) const {
    return guest_asked_ != Time{} && now - guest_asked_ < kWarpAskLasts;
}

bool PartyStatus::LampAsk(int slot, std::uint32_t lamp, Time now) {
    int current = 0;
    std::string name;
    std::uint32_t asked = 0;
    if (LampAskNow(now, current, name, asked) && current != slot) return false;
    lamp_slot_ = slot;
    lamp_ = lamp;
    lamp_at_ = now;
    const auto peer = peers_.find(slot);
    AddLine(PartyLineKind::LampAsked, peer == peers_.end() ? std::string() : peer->second.name, now);
    return true;
}

bool PartyStatus::LampAskNow(Time now, int& slot, std::string& name, std::uint32_t& lamp) const {
    if (!lamp_slot_ || now - lamp_at_ >= kLampAskLasts) return false;
    const auto peer = peers_.find(lamp_slot_);
    slot = lamp_slot_;
    name = peer == peers_.end() ? std::string() : peer->second.name;
    lamp = lamp_;
    return true;
}

void PartyStatus::LampAskDone() {
    lamp_slot_ = 0;
    lamp_ = 0;
    lamp_at_ = {};
}

void PartyStatus::GuestLampAsked(Time now) {
    guest_lamp_at_ = now;
    guest_lamp_seen_ = false;
}

void PartyStatus::GuestLampAnswered(std::uint8_t answer, Time now) {
    if (guest_lamp_at_ == Time{}) return;
    if (answer == kLampGot) {
        if (!guest_lamp_seen_) {
            guest_lamp_at_ = now;
            AddLine(PartyLineKind::LampSeen, {}, now);
        }
        guest_lamp_seen_ = true;
        return;
    }
    guest_lamp_at_ = {};
    AddLine(answer == kLampAgreed ? PartyLineKind::LampAgreed : PartyLineKind::LampRefused, {}, now);
}

bool PartyStatus::GuestLampWaiting(Time now) const {
    return guest_lamp_at_ != Time{} && now - guest_lamp_at_ < kLampAskLasts;
}

void PartyStatus::AddLine(PartyLineKind kind, const std::string& name, Time now) {
    const auto same = [&](const Line& line) { return line.kind == kind && line.name == name && now - line.at < kLineLasts; };
    if (std::any_of(lines_.begin(), lines_.end(), same)) return;
    lines_.push_back({kind, name, now});
    if (lines_.size() > kMostLines) lines_.erase(lines_.begin());
}

std::vector<PartyLine> PartyStatus::Lines(Time now) {
    lines_.erase(std::remove_if(lines_.begin(), lines_.end(), [&](const Line& line) { return now - line.at >= kLineLasts; }), lines_.end());
    std::vector<PartyLine> out;
    out.reserve(lines_.size());
    for (const Line& line : lines_) {
        const float left = std::chrono::duration<float>(kLineLasts - (now - line.at)).count();
        const float fade = std::chrono::duration<float>(kLineFade).count();
        out.push_back({line.kind, line.name, std::clamp(left / fade, 0.0f, 1.0f)});
    }
    return out;
}

std::vector<PeerStatus> PartyStatus::List(Time now) const {
    std::vector<PeerStatus> out;
    for (const auto& [slot, peer] : peers_) {
        if (peer.name.empty()) continue;
        PeerStatus row;
        row.slot = slot;
        row.name = peer.name;
        row.ping_ms = peer.ping_ms;
        if (down_ || peer.silent) row.state = LinkState::Reconnecting;
        else if (peer.ping_ms < 0) row.state = LinkState::Unknown;
        else row.state = peer.ping_ms >= kSlowPingMs ? LinkState::Slow : LinkState::Good;
        const auto ask = asks_.find(slot);
        row.asked = ask != asks_.end() && now - ask->second < kWarpAskLasts;
        out.push_back(std::move(row));
    }
    return out;
}

bool PartyStatus::Reconnecting(Time now, ReconnectNotice& out) const {
    out = {};
    if (gave_up_at_ != Time{}) {
        if (now - gave_up_at_ >= kGaveUpShown) return false;
        out.gave_up = true;
        return true;
    }
    if (down_) {
        out.own = true;
        out.seconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - lost_at_).count());
        return true;
    }
    for (const auto& [slot, peer] : peers_) {
        if (peer.silent && !peer.name.empty()) out.names.push_back(peer.name);
    }
    return !out.names.empty();
}

} // namespace BbRoom
