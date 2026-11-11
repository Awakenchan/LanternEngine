// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_service.h"
#include "room_host.h"
#include "../../shim/engine/online_gate.h"

#include <random>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
inline void CloseSocket(SocketHandle s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
inline void CloseSocket(SocketHandle s) { close(s); }
#endif

#include <httplib.h>
#include <openssl/evp.h>

#include "room_announce.h"
#include "room_link.h"
#include "../thd/thd_signal.h"

namespace BbRoom {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

enum : std::uint8_t { kRequest = 1, kReply = 2, kEvent = 3, kDatagram = 4, kBatch = 5, kVitals = 6, kTravel = 7, kProgress = 8, kDungeon = 9, kChalices = 10, kPortrait = 11, kBundle = 12, kBring = 13, kVoice = 14, kBossLot = 15, kBossTrophy = 16, kPiece = 17, kPartyCall = 18, kTravelFlags = 19, kPing = 20, kPong = 21, kWarpAsk = 22, kWarpAnswer = 23, kLampAsk = 24, kLampAnswer = 25, kWorldFlags = 26, kWorldAsk = 27, kStageTravel = 28 };
constexpr std::size_t kVitalsFixed = 1 + 1 + 4 * 4 + 2 + 3 * 4 + 4;
constexpr std::size_t kBatchBytes = 16000;
constexpr std::size_t kBundleBytes = 60000;
constexpr std::size_t kSlotBundleBytes = 65500;
constexpr int kResolveWaitMs = 1500;
constexpr std::size_t kLargestPortrait = 128 * 1024;
constexpr std::size_t kPieceBytes = 1200;
constexpr std::size_t kPieceHead = 4;
constexpr std::size_t kMostVoiceBacklog = 2048;
constexpr std::size_t kLongestPingName = 32;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

bool DirectRoom() {
    static const bool direct = Env("BB_ROOM_SERVER").rfind("ws://", 0) == 0;
    return direct;
}

std::chrono::milliseconds BundleWindow() {
    static const std::chrono::milliseconds window = [] {
        const std::string set = Env("BB_ROOM_BUNDLE_MS");
        return std::chrono::milliseconds(set.empty() ? (DirectRoom() ? 2 : 8) : std::atoi(set.c_str()));
    }();
    return window;
}

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("Seamless: %s\n", text);
    std::fflush(stdout);
}

long long IntOf(const Json& o, const char* key, long long dflt = 0) {
    if (!o.is_object()) return dflt;
    const auto it = o.find(key);
    if (it == o.end()) return dflt;
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_number()) return static_cast<long long>(it->get<double>());
    if (it->is_string()) return std::strtoll(it->get<std::string>().c_str(), nullptr, 10);
    return dflt;
}

std::string StrOf(const Json& o, const char* key) {
    if (!o.is_object()) return "";
    const auto it = o.find(key);
    if (it == o.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return "";
}

struct Member {
    int member_id = 0;
    int slot = 0;
};

struct MatchRoom {
    std::string session_id;
    int max_members = kMaxMembers;
    std::vector<Member> members;
};

struct Sign {
    std::string data;
    long long type = 0;
    long long chara = 0;
};

struct Channel {
    std::uint64_t id = 0;
    std::string key;
    std::string word;
    std::string form;
    Json fields;
    int owner = 0;
    long long share = 1;
    Clock::time_point made;
    std::map<int, Clock::time_point> ritual;
};

struct Pieces {
    std::uint8_t id = 0;
    std::uint8_t next = 0;
    std::vector<std::uint8_t> bytes;
};

struct State {
    Link link;
    std::atomic<int> game_port{0};
    int http_port = 0;
    bool host = false;
    std::atomic<int> own_slot{0};
    std::string player;
    std::string online_id;
    std::atomic<bool> started{false};

    std::mutex mu;
    std::condition_variable cv;
    std::uint32_t next_call = 1;
    std::map<std::uint32_t, Json> replies;
    std::deque<Json> events;
    long long next_event = 1;

    std::mutex host_mu;
    std::map<int, std::string> names;
    std::map<int, Clock::time_point> named_at;
    std::map<std::uint64_t, MatchRoom> rooms;
    std::uint64_t next_room = 1;
    std::map<int, Sign> signs;
    std::map<std::uint64_t, Channel> channels;

    std::mutex bridge_mu;
    std::map<int, SocketHandle> bridges;
    std::map<int, int> bridge_ports;

    std::mutex vitals_mu;
    std::map<int, std::pair<Vitals, Clock::time_point>> vitals;
    Clock::time_point bring_at{};
    std::deque<std::pair<int, std::vector<std::uint8_t>>> voice;
    Link voice_link;
    LocalRoom local_room;
    std::mutex voice_mu;
    bool voice_on = false;
    std::thread voice_thread;
    std::atomic<bool> voice_run{false};
    std::uint32_t travel = 0;
    Clock::time_point travel_at;
    std::vector<std::uint8_t> travel_flags;
    std::uint32_t stage_map = 0;
    std::int32_t stage_point = -1;
    Clock::time_point stage_at;
    std::vector<std::vector<std::uint8_t>> world_flags;
    std::vector<int> world_asks;
    std::vector<std::uint32_t> progress;
    std::vector<std::int32_t> boss_lots;
    std::vector<std::int32_t> boss_trophies;
    std::vector<std::pair<int, PartyCall>> party_calls;
    std::vector<Dungeon> dungeons;
    std::vector<std::uint32_t> chalices;
    bool chalices_new = false;
    std::map<int, std::vector<std::uint8_t>> portraits;
    std::map<int, Pieces> pieces;
    std::uint8_t next_pieces = 0;
    std::mutex announce_mu;
    AnnouncementQueue announcements;
    std::mutex status_mu;
    PartyStatus status;
    std::atomic<bool> warp_requests{true};
    Clock::time_point ping_at{};
};

State& S() {
    static State s;
    return s;
}

std::vector<std::uint8_t> Pack(std::uint8_t kind, const std::string& text, std::uint32_t id = 0, bool with_id = false) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() + 5);
    out.push_back(kind);
    if (with_id) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
    }
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

void SendJson(int slot, std::uint8_t kind, const Json& value, std::uint32_t id = 0, bool with_id = false) {
    const auto frame = Pack(kind, value.dump(), id, with_id);
    S().link.Send(slot, frame.data(), frame.size());
}

int BridgePort(int slot) {
    std::lock_guard lock(S().bridge_mu);
    const auto it = S().bridge_ports.find(slot);
    return it == S().bridge_ports.end() ? 0 : it->second;
}

void Localize(Json& value) {
    if (value.is_array()) {
        for (auto& v : value) Localize(v);
        return;
    }
    if (!value.is_object()) return;
    const int own = S().own_slot.load();
    if (value.contains("Slot")) {
        const int slot = static_cast<int>(IntOf(value, "Slot"));
        const int port = slot == own ? S().game_port.load() : BridgePort(slot);
        value["Addr"] = "127.0.0.1";
        value["Port"] = port;
        value["LocalAddr"] = "127.0.0.1";
        value["LocalPort"] = port;
        value["MappedAddr"] = "";
        value["MappedPort"] = 0;
    }
    if (value.contains("HostSlot")) {
        const int slot = static_cast<int>(IntOf(value, "HostSlot"));
        const int port = slot == own ? S().game_port.load() : BridgePort(slot);
        value["HostAddr"] = "127.0.0.1";
        value["HostPort"] = port;
        value["HostLocalAddr"] = "127.0.0.1";
        value["HostLocalPort"] = port;
        value["HostMappedAddr"] = "";
        value["HostMappedPort"] = 0;
    }
    if (value.contains("Members")) Localize(value["Members"]);
}

enum class Stage { None, Looking, Found, Calling, Waiting, Invited, Failed, HostOnly, Home, Arrived };

struct Notice {
    std::mutex mu;
    Stage stage = Stage::None;
    std::string who;
    int count = 0;
    Clock::time_point since, seen;
};

Notice& N() {
    static Notice n;
    return n;
}

std::string NameOf(int slot) {
    std::lock_guard lock(S().vitals_mu);
    const auto it = S().vitals.find(slot);
    return it == S().vitals.end() || it->second.first.name.empty() ? std::string("a hunter") : it->second.first.name;
}

void Note(Stage stage, const std::string& who = {}, int count = 0) {
    Notice& n = N();
    std::lock_guard lock(n.mu);
    const auto now = Clock::now();
    if (n.stage != stage || n.who != who) n.since = now;
    n.stage = stage;
    n.who = who;
    n.count = count;
    n.seen = now;
}

void AddPartyLine(PartyLineKind kind, const std::string& name) {
    std::lock_guard lock(S().status_mu);
    S().status.AddLine(kind, name, Clock::now());
}

std::string MemberName(const Json& ev) {
    const std::string name = PlainName(CleanAnnouncement(StrOf(ev, "OnlineId")));
    return name.empty() ? NameOf(static_cast<int>(IntOf(ev, "Slot"))) : name;
}

void NoteEvent(const Json& ev) {
    const std::string name = StrOf(ev, "Name");
    if (name == "guest_invite") {
        const std::string host = StrOf(ev, "HostOnlineId");
        Note(Stage::Invited, host.empty() ? std::string("the host") : host);
    } else if (name == "room_member_joined") {
        AddPartyLine(PartyLineKind::JoinedWorld, MemberName(ev));
    } else if (name == "room_member_left") {
        AddPartyLine(StrOf(ev, "Reason") == "kicked" && !ev.contains("ByChoice") ? PartyLineKind::Kicked : PartyLineKind::Left, MemberName(ev));
    } else if (name == "room_member_kicked" || name == "room_closed") {
        Note(Stage::Home);
    }
}

void NoteSummon(const std::string& api, const Json& body, const Json& reply, bool ok) {
    if (!ok) {
        if (api == "request") Note(Stage::Failed);
        return;
    }
    if (api == "create") {
        Note(Stage::Waiting);
    } else if (api == "delete") {
        Notice& n = N();
        std::lock_guard lock(n.mu);
        if (n.stage == Stage::Waiting) n.stage = Stage::None;
    } else if (reply.contains("LanternHostOnly")) {
        Note(Stage::HostOnly);
    } else if (api == "get") {
        const auto list = reply.contains("SummonDataList") && reply["SummonDataList"].is_array() ? reply["SummonDataList"]
                                                                                              : Json::array();
        if (list.empty()) {
            Notice& n = N();
            std::unique_lock lock(n.mu);
            const bool busy = n.stage == Stage::Calling && Clock::now() - n.since < std::chrono::seconds(20);
            lock.unlock();
            if (!busy) Note(Stage::Looking);
        } else {
            const std::string first = NameOf(static_cast<int>(IntOf(list[0], "UserId") - 1000));
            Note(Stage::Found, first, static_cast<int>(list.size()));
        }
    } else if (api == "request") {
        Note(Stage::Calling, NameOf(static_cast<int>(IntOf(body, "TargetUserId") - 1000)));
    }
}

void Deliver(int slot, Json event) {
    if (slot == S().own_slot.load()) {
        NoteEvent(event);
        Localize(event);
        {
            std::lock_guard lock(S().mu);
            event["EventId"] = S().next_event++;
            S().events.push_back(std::move(event));
        }
        S().cv.notify_all();
        return;
    }
    SendJson(slot, kEvent, event);
}

Json Record(const MatchRoom& room, const Member& m, std::uint64_t room_id) {
    Json r = {{"MemberId", m.member_id}, {"Slot", m.slot}, {"RoomId", static_cast<long long>(room_id)}};
    const auto it = S().names.find(m.slot);
    r["OnlineId"] = it == S().names.end() ? "" : it->second;
    (void)room;
    return r;
}

void LeaveLocked(std::uint64_t room_id, int slot, const char* reason, bool by_choice = false) {
    const auto it = S().rooms.find(room_id);
    if (it == S().rooms.end()) return;
    MatchRoom& room = it->second;
    const auto m = std::find_if(room.members.begin(), room.members.end(), [&](const Member& x) { return x.slot == slot; });
    if (m == room.members.end()) return;
    const Member gone = *m;
    room.members.erase(m);
    if (gone.member_id == 1) {
        for (const Member& other : room.members) {
            Deliver(other.slot, {{"Name", "room_closed"}, {"RoomId", static_cast<long long>(room_id)}, {"Reason", "host_left"}});
        }
        S().rooms.erase(it);
        return;
    }
    Json ev = Record(room, gone, room_id);
    ev["Name"] = "room_member_left";
    ev["Reason"] = reason;
    if (by_choice) ev["ByChoice"] = true;
    for (const Member& other : room.members) Deliver(other.slot, ev);
}

bool KickLocked(int target, bool by_choice) {
    const int own = S().own_slot.load();
    for (auto& [id, room] : S().rooms) {
        const bool mine = std::any_of(room.members.begin(), room.members.end(),
                                      [&](const Member& m) { return m.slot == own && m.member_id == 1; });
        if (!mine) continue;
        for (const Member& m : room.members) {
            if (m.slot != target || m.slot == own) continue;
            Deliver(m.slot, {{"Name", "room_member_kicked"}, {"RoomId", static_cast<long long>(id)},
                             {"MemberId", m.member_id}, {"OptData", ""}});
            LeaveLocked(id, target, "kicked", by_choice);
            {
                std::lock_guard status_lock(S().status_mu);
                S().status.ClearAsk(target);
            }
            return true;
        }
    }
    return false;
}

Json HandleSession(int slot, const std::string& path, const Json& body) {
    std::lock_guard lock(S().host_mu);
    if (path == "/mp/matching2/context_start") {
        const std::string name = StrOf(body, "OnlineId");
        if (!name.empty() && S().names[slot] != name) {
            S().names[slot] = name;
            S().named_at[slot] = Clock::now();
            Log("slot %d is %s", slot, name.c_str());
        }
        return {{"ResKind", 0}};
    }
    if (path == "/np/signaling/resolve") {
        const std::string id = StrOf(body, "OnlineId");
        const auto find = [&](const auto& same) {
            int best = 0;
            Clock::time_point best_at{};
            for (const auto& [s, name] : S().names) {
                if (!same(name)) continue;
                const auto at = S().named_at.count(s) ? S().named_at[s] : Clock::time_point{};
                if (!best || at > best_at) {
                    best = s;
                    best_at = at;
                }
            }
            return best;
        };
        int best = find([&](const std::string& name) { return name == id; });
        if (!best) best = find([&](const std::string& name) { return !name.empty() && id.rfind(name + "-", 0) == 0; });
        if (best) return {{"ResKind", 0}, {"Slot", best}};
        return {{"ResKind", 1}};
    }
    if (path == "/mp/matching2/create_room") {
        const std::string name = StrOf(body, "OnlineId");
        if (!name.empty() && S().names[slot] != name) {
            S().names[slot] = name;
            S().named_at[slot] = Clock::now();
        }
        for (auto it = S().rooms.begin(); it != S().rooms.end();) {
            const bool owned = std::any_of(it->second.members.begin(), it->second.members.end(),
                                           [&](const Member& m) { return m.slot == slot && m.member_id == 1; });
            if (owned) {
                const std::uint64_t id = it->first;
                ++it;
                LeaveLocked(id, slot, "leave_room");
            } else {
                ++it;
            }
        }
        const std::uint64_t id = S().next_room++;
        MatchRoom room;
        room.session_id = "room-" + std::to_string(id);
        room.max_members = kMaxMembers;
        room.members.push_back({1, slot});
        S().rooms[id] = room;
        return {{"ResKind", 0}, {"RoomId", static_cast<long long>(id)}, {"SessionId", room.session_id},
                {"MemberId", 1}, {"MaxMembers", room.max_members}};
    }
    if (path == "/mp/matching2/join_room") {
        const std::string name = StrOf(body, "OnlineId");
        if (!name.empty() && S().names[slot] != name) {
            S().names[slot] = name;
            S().named_at[slot] = Clock::now();
        }
        const auto id = static_cast<std::uint64_t>(IntOf(body, "RoomId"));
        const auto it = S().rooms.find(id);
        if (it == S().rooms.end()) return {{"ResKind", 1}, {"Message", "no such room"}};
        MatchRoom& room = it->second;
        for (const Member& m : room.members) {
            if (m.slot == slot) {
                Json members = Json::array();
                for (const Member& x : room.members) members.push_back(Record(room, x, id));
                return {{"ResKind", 0}, {"SessionId", room.session_id}, {"MemberId", m.member_id},
                        {"OwnerMemberId", 1}, {"MaxMembers", room.max_members}, {"Members", members}};
            }
        }
        std::vector<int> present;
        for (const Member& m : room.members)
            if (m.member_id != 1) present.push_back(m.slot);
        for (const int stale : StaleTwins(S().names, present, slot)) {
            Log("slot %d is %s again: the old entry in slot %d leaves the party", slot, S().names[slot].c_str(), stale);
            LeaveLocked(id, stale, "disconnected");
        }
        if (static_cast<int>(room.members.size()) >= room.max_members) return {{"ResKind", 1}, {"Message", "room full"}};
        int free_member = 2;
        while (std::any_of(room.members.begin(), room.members.end(),
                           [&](const Member& m) { return m.member_id == free_member; })) {
            ++free_member;
        }
        const Member joined{free_member, slot};
        room.members.push_back(joined);
        Json members = Json::array();
        for (const Member& x : room.members) members.push_back(Record(room, x, id));
        Json ev = Record(room, joined, id);
        ev["Name"] = "room_member_joined";
        for (const Member& other : room.members) {
            if (other.slot != slot) Deliver(other.slot, ev);
        }
        return {{"ResKind", 0}, {"SessionId", room.session_id}, {"MemberId", joined.member_id},
                {"OwnerMemberId", 1}, {"MaxMembers", room.max_members}, {"Members", members}};
    }
    if (path == "/mp/matching2/leave_room") {
        for (auto it = S().rooms.begin(); it != S().rooms.end();) {
            const std::uint64_t id = it->first;
            const bool match = it->second.session_id == StrOf(body, "SessionId");
            ++it;
            if (match) LeaveLocked(id, slot, "leave_room");
        }
        return {{"ResKind", 0}};
    }
    if (path == "/mp/matching2/heartbeat") {
        for (const auto& [id, room] : S().rooms) {
            if (room.session_id != StrOf(body, "SessionId")) continue;
            for (const Member& m : room.members) {
                if (m.slot == slot) return {{"ResKind", 0}, {"InRoom", 1}};
            }
        }
        return {{"ResKind", 0}, {"InRoom", 0}};
    }
    if (path == "/lantern/leave_party") {
        KickLocked(slot, true);
        return {{"ResKind", 0}};
    }
    if (path == "/mp/matching2/kick_member") {
        const int target = static_cast<int>(IntOf(body, "MemberId"));
        for (auto& [id, room] : S().rooms) {
            if (room.session_id != StrOf(body, "SessionId")) continue;
            const bool owner = std::any_of(room.members.begin(), room.members.end(),
                                           [&](const Member& m) { return m.slot == slot && m.member_id == 1; });
            if (!owner) return {{"ResKind", 1}};
            for (const Member& m : room.members) {
                if (m.member_id != target) continue;
                Deliver(m.slot, {{"Name", "room_member_kicked"}, {"RoomId", static_cast<long long>(id)},
                                 {"MemberId", target}, {"OptData", StrOf(body, "OptData")}});
                const int kicked = m.slot;
                LeaveLocked(id, kicked, "kicked");
                return {{"ResKind", 0}};
            }
        }
        return {{"ResKind", 1}};
    }
    return {{"ResKind", 0}};
}

void ForgetSlotLocked(int slot) {
    std::vector<std::uint64_t> ids;
    for (const auto& [id, room] : S().rooms) ids.push_back(id);
    for (const std::uint64_t id : ids) LeaveLocked(id, slot, "disconnected");
    S().signs.erase(slot);
    const auto name = S().names.find(slot);
    if (name != S().names.end()) {
        for (const auto& [other, n] : S().names) {
            if (other != slot) Deliver(other, {{"Name", "peer_deactivated"}, {"OnlineId", name->second}});
        }
        S().names.erase(name);
    }
    S().named_at.erase(slot);
}

bool B64Size(const std::string& text, std::size_t want) {
    std::size_t pad = 0;
    for (std::size_t i = text.size(); i > 0 && text[i - 1] == '='; --i) ++pad;
    if (text.empty() || text.size() % 4 != 0 || pad > 2) return false;
    for (std::size_t i = 0; i < text.size() - pad; ++i) {
        const char c = text[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/')) return false;
    }
    return text.size() / 4 * 3 - pad == want;
}

Json HandleSummon(int slot, const std::string& api, const Json& body) {
    std::lock_guard lock(S().host_mu);
    if (api == "create") {
        const std::string data = StrOf(body, "SummonData");
        if (!B64Size(data, 0xE0)) return {{"ResKind", 0}};
        S().signs[slot] = {data, IntOf(body, "SummonType"), IntOf(body, "CharaId", slot)};
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 20) Log("slot %d put up a summon sign (type %lld)", slot, IntOf(body, "SummonType"));
        return {{"ResKind", 0}};
    }
    if ((api == "get" || api == "request") && slot != S().own_slot.load()) {
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 8) Log("slot %d rang a Beckoning Bell, but only the room's host summons", slot);
        return {{"ResKind", 0}, {"SummonDataList", Json::array()}, {"LanternHostOnly", true}};
    }
    if (api == "get") {
        std::set<long long> types;
        if (body.contains("SummonTypeList") && body["SummonTypeList"].is_array()) {
            for (const auto& t : body["SummonTypeList"]) types.insert(IntOf(t, "SummonType", -1));
        }
        Json list = Json::array();
        static std::atomic<int> searched{0};
        if (searched.fetch_add(1) < 20) Log("the host searches the summon board: %zu sign(s) up", S().signs.size());
        for (const auto& [s, sign] : S().signs) {
            if (s == slot) continue;
            if (!types.empty() && !types.count(sign.type)) continue;
            list.push_back({{"SummonDataVersion", 3}, {"SummonData", sign.data}, {"SummonType", sign.type},
                            {"CharaId", sign.chara}, {"UserId", 1000 + s}});
        }
        return {{"ResKind", 0}, {"SummonDataList", list}};
    }
    if (api == "request") {
        const int target = static_cast<int>(IntOf(body, "TargetUserId") - 1000);
        if (target >= 1 && target <= kMaxMembers && target != slot && S().signs.count(target)) {
            Json ev = {{"Name", "guest_invite"}, {"HostSlot", slot}};
            const auto name = S().names.find(slot);
            ev["HostOnlineId"] = name == S().names.end() ? "" : name->second;
            for (const auto& [id, room] : S().rooms) {
                for (const Member& m : room.members) {
                    if (m.slot == slot && m.member_id == 1) ev["RoomId"] = static_cast<long long>(id);
                }
            }
            Deliver(target, ev);
        }
        return {{"ResKind", 0}};
    }
    if (api == "delete") {
        S().signs.erase(slot);
        return {{"ResKind", 0}};
    }
    return {{"ResKind", 0}};
}

std::uint64_t Fnv(const std::string& text) {
    std::uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

constexpr long long kNoChannel = 0x1040a;
constexpr std::size_t kMostChannels = 64;
constexpr std::size_t kLongestForm = 256 * 1024;
constexpr auto kRitualOwn = std::chrono::seconds(120);

std::string Stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &tm);
    return text;
}

std::string WordOf(const std::string& key) {
    static const char kWord[] = "abcdefghijkmnpqrstuvwxyz23456789";
    std::string word;
    std::uint64_t w = Fnv(key + "#word");
    for (int i = 0; i < 8; ++i, w >>= 5) word.push_back(kWord[w % 32]);
    return word;
}

Channel& MakeChannelLocked(const std::string& key, int slot, const Json& body) {
    std::uint64_t id = Fnv(key) & ((1ull << 52) - 1);
    if (!id) id = 1;
    auto it = S().channels.find(id);
    if (it != S().channels.end()) return it->second;
    if (S().channels.size() >= kMostChannels) {
        auto oldest = S().channels.begin();
        for (auto c = S().channels.begin(); c != S().channels.end(); ++c)
            if (c->second.made < oldest->second.made) oldest = c;
        S().channels.erase(oldest);
    }
    Channel c;
    c.id = id;
    c.key = key;
    c.owner = slot;
    c.made = Clock::now();
    c.word = WordOf(key);
    for (const char* field : {"HolyGrailTypeId", "RitualLevel", "FixedOrGeneral", "SubFeatureFlag"})
        c.fields[field] = IntOf(body, field);
    c.fields["WishMaterialList"] = body.contains("WishMaterialList") && body["WishMaterialList"].is_array()
                                       ? body["WishMaterialList"]
                                       : Json::array();
    Log("dungeon %llu (%s) on the room's board", static_cast<unsigned long long>(id), key.c_str());
    return S().channels.emplace(id, std::move(c)).first->second;
}

Json EntryOf(const Channel& c, int asker) {
    Json e = c.fields;
    const auto own = c.ritual.find(asker);
    const bool ritual = own != c.ritual.end() && Clock::now() - own->second < kRitualOwn;
    e["ChannelId"] = c.id;
    e["DiscernmentWord"] = c.word;
    e["Status"] = 1;
    e["ShareLevel"] = ritual ? 0 : (c.share ? c.share : 1);
    e["TurnoutLevel"] = 0;
    e["CreateUserId"] = 1000 + c.owner;
    e["CreateCharaId"] = c.owner;
    e["CreateDate"] = Stamp();
    e["LastPlayDate"] = Stamp();
    if (!c.form.empty()) {
        e["FormDataVersion"] = 0;
        e["FormData"] = c.form;
    }
    return e;
}

bool ChannelMatches(const Channel& c, const Json& body) {
    for (const char* field : {"HolyGrailTypeId", "RitualLevel", "SubFeatureFlag"}) {
        const auto it = body.find(field);
        if (it != body.end() && !it->is_null() && IntOf(body, field) != IntOf(c.fields, field)) return false;
    }
    return true;
}

std::string FixedKey(const Json& body) {
    return "fixed:" + std::to_string(IntOf(body, "HolyGrailTypeId")) + ":" + std::to_string(IntOf(body, "RitualLevel")) + ":" +
           std::to_string(IntOf(body, "SubFeatureFlag"));
}

bool WantsFixed(const Json& body) {
    const auto it = body.find("FixedOrGeneralList");
    if (it == body.end() || !it->is_array()) return false;
    for (const auto& f : *it)
        if (IntOf(f, "FixedOrGeneral") == 2) return true;
    return false;
}

Json HandleChannel(int slot, const std::string& api, const Json& body) {
    std::lock_guard lock(S().host_mu);
    if (api == "upload") {
        const std::string form = StrOf(body, "FormData");
        const bool fixed = IntOf(body, "FixedOrGeneral") == 2;
        if (!fixed && (form.empty() || form.size() > kLongestForm || form.size() % 4)) return {{"ResKind", 1}};
        Channel& c = MakeChannelLocked(fixed ? FixedKey(body) : "form:" + std::to_string(Fnv(form)), slot, body);
        if (!form.empty() && form.size() <= kLongestForm) c.form = form;
        c.ritual[slot] = Clock::now();
        return {{"ResKind", 0}, {"ChannelId", c.id}, {"DiscernmentWord", c.word}};
    }
    if (api == "search") {
        Json list = Json::array();
        if (WantsFixed(body)) {
            Channel& c = MakeChannelLocked(FixedKey(body), slot, body);
            c.fields["FixedOrGeneral"] = 2;
            c.ritual[slot] = Clock::now();
            list.push_back(EntryOf(c, slot));
            return {{"ResKind", 0}, {"ChannelList", list}};
        }
        const long long most = std::clamp<long long>(IntOf(body, "GetCount", 10), 1, 32);
        std::vector<const Channel*> found;
        for (const auto& [id, c] : S().channels)
            if (c.owner != slot && !c.form.empty() && ChannelMatches(c, body)) found.push_back(&c);
        std::sort(found.begin(), found.end(), [](const Channel* a, const Channel* b) { return a->made > b->made; });
        for (const Channel* c : found) {
            if (static_cast<long long>(list.size()) >= most) break;
            list.push_back(EntryOf(*c, slot));
        }
        return {{"ResKind", 0}, {"ChannelList", list}};
    }
    if (api == "word_search") {
        std::string word = StrOf(body, "SearchWord");
        std::transform(word.begin(), word.end(), word.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        for (const auto& [id, c] : S().channels) {
            if (c.word != word) continue;
            Json e = EntryOf(c, slot);
            e["ResKind"] = 0;
            return e;
        }
        return {{"ResKind", kNoChannel}};
    }
    if (api == "random_join") {
        const Channel* best = nullptr;
        for (const auto& [id, c] : S().channels)
            if (c.owner != slot && (!best || c.made > best->made)) best = &c;
        if (!best) return {{"ResKind", kNoChannel}};
        Json e = EntryOf(*best, slot);
        e["ResKind"] = 0;
        return e;
    }
    if (api == "get_details_info" || api == "get_info") {
        Json list = Json::array();
        const auto ids = body.find("ChannelIdList");
        if (ids != body.end() && ids->is_array()) {
            for (const auto& entry : *ids) {
                const auto it = S().channels.find(static_cast<std::uint64_t>(IntOf(entry, "ChannelId")));
                if (it == S().channels.end()) continue;
                if (api == "get_info") {
                    const Json e = EntryOf(it->second, slot);
                    list.push_back({{"ChannelId", e["ChannelId"]}, {"Status", e["Status"]}, {"ShareLevel", e["ShareLevel"]},
                                    {"TurnoutLevel", 0}});
                } else {
                    list.push_back(EntryOf(it->second, slot));
                }
            }
        }
        return {{"ResKind", 0}, {api == "get_info" ? "ChannelInfoList" : "ChannelList", list}};
    }
    if (api == "share") {
        const auto it = S().channels.find(static_cast<std::uint64_t>(IntOf(body, "ChannelId")));
        if (it != S().channels.end() && it->second.owner == slot) it->second.share = IntOf(body, "ShareLevel", 1);
        return {{"ResKind", 0}};
    }
    if (api == "add_material") return {{"ResKind", 0}, {"CompleteFlag", 0}};
    return {{"ResKind", 0}};
}

Json HandleHost(int slot, const Json& request) {
    const std::string path = StrOf(request, "p");
    const Json& body = request.contains("b") ? request["b"] : Json::object();
    if (path.rfind("/summon_messenger/", 0) == 0) return HandleSummon(slot, path.substr(18), body);
    if (path.rfind("/channel/", 0) == 0) return HandleChannel(slot, path.substr(9), body);
    return HandleSession(slot, path, body);
}

int HostSlotWait(int timeout_ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const int host = S().link.HostSlot();
        if (host && S().link.Connected()) return host;
        if (Clock::now() >= end) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool Rpc(const Json& request, Json& reply, std::string& error, int timeout_ms) {
    const bool resolve = StrOf(request, "p") == "/np/signaling/resolve";
    if (resolve) timeout_ms = std::min(timeout_ms, kResolveWaitMs);
    if (S().host) {
        const auto end = Clock::now() + std::chrono::milliseconds(std::min(timeout_ms, 10000));
        while (!S().own_slot.load() && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int own = S().own_slot.load();
        if (!own) {
            error = "not in the room yet";
            return false;
        }
        reply = HandleHost(own, request);
        Localize(reply);
        return true;
    }
    if (StrOf(request, "p") == "/mp/matching2/context_start" && request.contains("b")) {
        const std::string started_as = StrOf(request["b"], "OnlineId");
        std::lock_guard lock(S().mu);
        if (!started_as.empty()) S().online_id = started_as;
    }
    const bool start = StrOf(request, "p") == "/mp/matching2/context_start" && Env("BB_ROOM_START_WAITS_HOST") != "1";
    const int host = HostSlotWait(start || resolve ? 0 : std::min(timeout_ms, 10000));
    if (!host) {
        error = start ? "no host in the room yet; this hunter is introduced when the host comes" : "no host in the room";
        return false;
    }
    std::uint32_t id;
    {
        std::lock_guard lock(S().mu);
        id = S().next_call++;
    }
    SendJson(host, kRequest, request, id, true);
    std::unique_lock lock(S().mu);
    if (!S().cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return S().replies.count(id) > 0; })) {
        error = "the host did not answer";
        return false;
    }
    reply = std::move(S().replies[id]);
    S().replies.erase(id);
    lock.unlock();
    Localize(reply);
    return true;
}

bool PollEvent(const Json& body, Json& reply) {
    const long long wait = std::clamp<long long>(IntOf(body, "WaitMs", 0), 0, 25000);
    std::unique_lock lock(S().mu);
    S().cv.wait_for(lock, std::chrono::milliseconds(wait), [] { return !S().events.empty(); });
    const long long cursor = IntOf(body, "Cursor", 0);
    if (S().events.empty()) {
        reply = {{"HasEvent", 0}, {"NextCursor", cursor}, {"WaitMaxMs", 25000}};
        return true;
    }
    reply = std::move(S().events.front());
    S().events.pop_front();
    reply["HasEvent"] = 1;
    reply["NextCursor"] = cursor + 1;
    reply["WaitMaxMs"] = 25000;
    return true;
}

SocketHandle OpenLoopbackUdp(int* port) {
    const SocketHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return kNoSocket;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        CloseSocket(s);
        return kNoSocket;
    }
    *port = ntohs(addr.sin_port);
#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    return s;
}

void BridgeOpen(int slot) {
    std::lock_guard lock(S().bridge_mu);
    if (S().bridges.count(slot)) return;
    int port = 0;
    const SocketHandle s = OpenLoopbackUdp(&port);
    if (s == kNoSocket) return;
    S().bridges[slot] = s;
    S().bridge_ports[slot] = port;
}

void BridgeToGame(int slot, const std::uint8_t* data, std::size_t size) {
    SocketHandle s;
    {
        std::lock_guard lock(S().bridge_mu);
        const auto it = S().bridges.find(slot);
        if (it == S().bridges.end()) return;
        s = it->second;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(static_cast<std::uint16_t>(S().game_port));
    ::sendto(s, reinterpret_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&to),
             sizeof(to));
}

void SendBundle(std::map<int, std::vector<std::uint8_t>>& pending) {
    std::vector<std::uint8_t> frame;
    for (auto& [slot, bytes] : pending) {
        if (bytes.empty()) continue;
        frame.assign(1, kBundle);
        frame.push_back(static_cast<std::uint8_t>(slot));
        const auto n = static_cast<std::uint32_t>(bytes.size());
        frame.insert(frame.end(), reinterpret_cast<const std::uint8_t*>(&n), reinterpret_cast<const std::uint8_t*>(&n) + 4);
        frame.insert(frame.end(), bytes.begin(), bytes.end());
        S().link.SendGame(slot, frame.data(), frame.size());
        bytes.clear();
    }
}

void BridgeLoop() {
    std::vector<std::uint8_t> buffer(65536);
    for (;;) {
        std::map<int, SocketHandle> bridges;
        {
            std::lock_guard lock(S().bridge_mu);
            bridges = S().bridges;
        }
        if (bridges.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::map<int, std::vector<std::uint8_t>> pending;
        std::size_t held = 0;
        Clock::time_point until{};
        for (;;) {
            fd_set set;
            FD_ZERO(&set);
            SocketHandle top = 0;
            for (const auto& [slot, s] : bridges) {
                FD_SET(s, &set);
                top = std::max(top, s);
            }
            long wait_us = 50000;
            if (until != Clock::time_point{}) {
                wait_us = std::max<long>(0, long(std::chrono::duration_cast<std::chrono::microseconds>(until - Clock::now()).count()));
            }
            timeval tv{0, wait_us};
            const int ready = ::select(static_cast<int>(top + 1), &set, nullptr, nullptr, &tv);
            if (ready > 0) {
                for (const auto& [slot, s] : bridges) {
                    if (!FD_ISSET(s, &set)) continue;
                    for (;;) {
                        sockaddr_in from{};
                        socklen_t len = sizeof(from);
                        const int n = ::recvfrom(s, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                                 reinterpret_cast<sockaddr*>(&from), &len);
                        if (n <= 0) break;
                        if (BbThd::Signal::IsSignal(buffer.data(), static_cast<std::size_t>(n))) {
                            std::vector<std::uint8_t> frame(1 + static_cast<std::size_t>(n));
                            frame[0] = kDatagram;
                            std::memcpy(frame.data() + 1, buffer.data(), static_cast<std::size_t>(n));
                            S().link.Send(slot, frame.data(), frame.size());
                            continue;
                        }
                        auto& out = pending[slot];
                        if (!out.empty() && out.size() + 2 + static_cast<std::size_t>(n) > kSlotBundleBytes) {
                            held -= out.size();
                            std::map<int, std::vector<std::uint8_t>> full;
                            full[slot] = std::move(out);
                            out.clear();
                            SendBundle(full);
                        }
                        out.push_back(static_cast<std::uint8_t>(n));
                        out.push_back(static_cast<std::uint8_t>(n >> 8));
                        out.insert(out.end(), buffer.begin(), buffer.begin() + n);
                        held += 2 + static_cast<std::size_t>(n);
                    }
                }
                if (until == Clock::time_point{} && held) until = Clock::now() + BundleWindow();
            }
            if (until == Clock::time_point{}) break;
            if (Clock::now() >= until || held >= kBundleBytes) break;
        }
        if (held) SendBundle(pending);
    }
}

std::int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void OnFrame(int sender, const std::vector<std::uint8_t>& data) {
    if (data.empty()) return;
    const std::uint8_t kind = data[0];
    if (kind == kDatagram) {
        BridgeToGame(sender, data.data() + 1, data.size() - 1);
        return;
    }
    if (kind == kBundle) {
        const int own = S().own_slot.load();
        for (std::size_t at = 1; data.size() - at >= 5;) {
            const int slot = data[at];
            std::uint32_t n = 0;
            std::memcpy(&n, data.data() + at + 1, 4);
            at += 5;
            if (n > data.size() - at) return;
            for (std::size_t p = at; slot == own && at + n - p >= 2;) {
                const std::size_t m = data[p] | (std::size_t(data[p + 1]) << 8);
                p += 2;
                if (m > at + n - p) break;
                BridgeToGame(sender, data.data() + p, m);
                p += m;
            }
            at += n;
        }
        return;
    }
    if (kind == kBatch) {
        std::size_t at = 1;
        while (data.size() - at >= 2) {
            const std::size_t n = data[at] | (std::size_t(data[at + 1]) << 8);
            at += 2;
            if (n > data.size() - at) break;
            BridgeToGame(sender, data.data() + at, n);
            at += n;
        }
        return;
    }
    if (kind == kTravel) {
        if (data.size() != 5 || sender != S().link.HostSlot()) return;
        std::uint32_t lamp = 0;
        std::memcpy(&lamp, data.data() + 1, 4);
        Log("the host travels to lamp %u: following", lamp);
        AddPartyLine(PartyLineKind::HostTravels, {});
        std::lock_guard lock(S().vitals_mu);
        S().travel = lamp;
        S().travel_at = Clock::now();
        return;
    }
    if (kind == kStageTravel) {
        if (data.size() != 9 || sender != S().link.HostSlot()) return;
        std::uint32_t map = 0;
        std::int32_t point = -1;
        std::memcpy(&map, data.data() + 1, 4);
        std::memcpy(&point, data.data() + 5, 4);
        Log("the host travels to map %08x: following", map);
        AddPartyLine(PartyLineKind::HostTravels, {});
        std::lock_guard lock(S().vitals_mu);
        S().stage_map = map;
        S().stage_point = point;
        S().stage_at = Clock::now();
        return;
    }
    if (kind == kWorldFlags) {
        if (sender != S().link.HostSlot() || data.size() < 2 || data.size() > kBundleBytes) return;
        std::lock_guard lock(S().vitals_mu);
        if (S().world_flags.size() < 256) S().world_flags.emplace_back(data.begin() + 1, data.end());
        return;
    }
    if (kind == kWorldAsk) {
        if (data.size() != 1) return;
        std::lock_guard lock(S().vitals_mu);
        if (S().world_asks.size() < 16) S().world_asks.push_back(sender);
        return;
    }
    if (kind == kTravelFlags) {
        if (sender != S().link.HostSlot() || data.size() < 2 || data.size() > 1 + 4096) return;
        std::lock_guard lock(S().vitals_mu);
        S().travel_flags.assign(data.begin() + 1, data.end());
        return;
    }
    if (kind == kProgress) {
        if (data.size() < 1 || (data.size() - 1) % 4 || sender != S().link.HostSlot()) return;
        std::lock_guard lock(S().vitals_mu);
        for (std::size_t at = 1; at < data.size() && S().progress.size() < 4096; at += 4) {
            std::uint32_t id = 0;
            std::memcpy(&id, data.data() + at, 4);
            S().progress.push_back(id);
        }
        return;
    }
    if (kind == kBossLot) {
        if (data.size() != 5 || sender != S().link.HostSlot()) return;
        std::int32_t lot = 0;
        std::memcpy(&lot, data.data() + 1, 4);
        std::lock_guard lock(S().vitals_mu);
        if (S().boss_lots.size() < 64) S().boss_lots.push_back(lot);
        return;
    }
    if (kind == kBossTrophy) {
        if (data.size() != 5 || sender != S().link.HostSlot()) return;
        std::int32_t trophy = 0;
        std::memcpy(&trophy, data.data() + 1, 4);
        std::lock_guard lock(S().vitals_mu);
        if (S().boss_trophies.size() < 64) S().boss_trophies.push_back(trophy);
        return;
    }
    if (kind == kPartyCall) {
        if (data.size() != 2) return;
        std::lock_guard lock(S().vitals_mu);
        if (S().party_calls.size() < 16) S().party_calls.emplace_back(sender, PartyCall(data[1]));
        return;
    }
    if (kind == kDungeon) {
        constexpr std::size_t fixed = 1 + 8 + 4 * 43;
        if (data.size() < fixed || (data.size() - fixed) % 4 || sender != S().link.HostSlot()) return;
        Dungeon d;
        std::memcpy(&d.channel, data.data() + 1, 8);
        std::memcpy(d.record.data(), data.data() + 9, 4 * 43);
        d.flags.resize((data.size() - fixed) / 4);
        if (!d.flags.empty()) std::memcpy(d.flags.data(), data.data() + fixed, 4 * d.flags.size());
        std::lock_guard lock(S().vitals_mu);
        if (S().dungeons.size() < 8) S().dungeons.push_back(std::move(d));
        return;
    }
    if (kind == kVoice) {
        if (data.size() < 4 || data.size() > 1200) return;
        std::lock_guard lock(S().vitals_mu);
        if (S().voice.size() >= 48) S().voice.pop_front();
        S().voice.emplace_back(sender, std::vector<std::uint8_t>(data.begin() + 1, data.end()));
        return;
    }
    if (kind == kBring) {
        if (data.size() != 1 || sender != S().link.HostSlot()) return;
        {
            std::lock_guard lock(S().vitals_mu);
            S().bring_at = Clock::now();
        }
        std::lock_guard lock(S().status_mu);
        S().status.GuestBrought();
        return;
    }
    if (kind == kPing) {
        if (data.size() < 5 || data.size() > 5 + kLongestPingName) return;
        {
            std::lock_guard lock(S().status_mu);
            S().status.Pinged(sender);
            S().status.Named(sender, std::string(data.begin() + 5, data.end()), Clock::now());
        }
        std::uint8_t pong[5] = {kPong};
        std::memcpy(pong + 1, data.data() + 1, 4);
        S().link.Send(sender, pong, sizeof pong);
        return;
    }
    if (kind == kPong) {
        if (data.size() != 5) return;
        std::uint32_t stamp = 0;
        std::memcpy(&stamp, data.data() + 1, 4);
        const std::uint32_t ms = static_cast<std::uint32_t>(SteadyMs()) - stamp;
        std::lock_guard lock(S().status_mu);
        S().status.Pinged(sender);
        S().status.Rtt(sender, static_cast<int>(std::min<std::uint32_t>(ms, 0x7fffffff)));
        return;
    }
    if (kind == kWarpAsk) {
        if (data.size() != 1 || !S().host) return;
        bool member = false;
        for (const auto& [slot, name] : PartyMembers()) member = member || slot == sender;
        const bool take = member && S().warp_requests.load();
        if (take) {
            std::lock_guard lock(S().status_mu);
            if (S().status.Ask(sender, Clock::now())) Log("slot %d asks to be brought to the host", sender);
        }
        const std::uint8_t answer[2] = {kWarpAnswer, take ? std::uint8_t(kWarpGot) : std::uint8_t(kWarpRefused)};
        S().link.Send(sender, answer, sizeof answer);
        return;
    }
    if (kind == kWarpAnswer) {
        if (data.size() != 2 || sender != S().link.HostSlot()) return;
        std::lock_guard lock(S().status_mu);
        S().status.GuestAnswered(data[1], Clock::now());
        return;
    }
    if (kind == kLampAsk) {
        if (data.size() != 5 || !S().host) return;
        bool member = false;
        for (const auto& [slot, name] : PartyMembers()) member = member || slot == sender;
        std::uint32_t lamp = 0;
        std::memcpy(&lamp, data.data() + 1, 4);
        bool take = member && S().warp_requests.load();
        if (take) {
            std::lock_guard lock(S().status_mu);
            take = S().status.LampAsk(sender, lamp, Clock::now());
        }
        if (take) Log("slot %d asks the party to travel to lamp %u", sender, lamp);
        const std::uint8_t answer[2] = {kLampAnswer, take ? std::uint8_t(kLampGot) : std::uint8_t(kLampRefused)};
        S().link.Send(sender, answer, sizeof answer);
        return;
    }
    if (kind == kLampAnswer) {
        if (data.size() != 2 || sender != S().link.HostSlot()) return;
        std::lock_guard lock(S().status_mu);
        S().status.GuestLampAnswered(data[1], Clock::now());
        return;
    }
    if (kind == kPiece) {
        if (data.size() <= kPieceHead) return;
        const std::uint8_t id = data[1], index = data[2], count = data[3];
        std::vector<std::uint8_t> whole;
        {
            std::lock_guard lock(S().vitals_mu);
            Pieces& p = S().pieces[sender];
            if (index == 0) {
                p = Pieces{};
                p.id = id;
            }
            if (p.id != id || p.next != index || p.bytes.size() + data.size() > kLargestPortrait) {
                S().pieces.erase(sender);
                return;
            }
            p.bytes.insert(p.bytes.end(), data.begin() + kPieceHead, data.end());
            ++p.next;
            if (p.next < count) return;
            whole = std::move(p.bytes);
            S().pieces.erase(sender);
        }
        if (!whole.empty() && whole[0] == kPortrait) OnFrame(sender, whole);
        return;
    }
    if (kind == kPortrait) {
        if (data.size() < 2 || data.size() > kLargestPortrait) return;
        std::lock_guard lock(S().vitals_mu);
        S().portraits[sender].assign(data.begin() + 1, data.end());
        return;
    }
    if (kind == kChalices) {
        if ((data.size() - 1) % 4 || data.size() > 1 + 4 * 64) return;
        std::lock_guard lock(S().vitals_mu);
        for (std::size_t at = 1; at < data.size(); at += 4) {
            std::uint32_t id = 0;
            std::memcpy(&id, data.data() + at, 4);
            if (std::find(S().chalices.begin(), S().chalices.end(), id) == S().chalices.end()) {
                S().chalices.push_back(id);
                S().chalices_new = true;
            }
        }
        return;
    }
    if (kind == kVitals) {
        if (data.size() < kVitalsFixed) return;
        const std::size_t name_size = data[1];
        if (data.size() != kVitalsFixed + name_size) return;
        Vitals v;
        v.slot = sender;
        std::size_t at = 2;
        v.name.assign(reinterpret_cast<const char*>(data.data() + at), name_size);
        at += name_size;
        for (std::int32_t* field : {&v.hp, &v.hp_max, &v.stamina, &v.stamina_max}) {
            std::memcpy(field, data.data() + at, 4);
            at += 4;
        }
        std::memcpy(&v.revives, data.data() + at, 2);
        at += 2;
        std::memcpy(v.pos, data.data() + at, sizeof v.pos);
        at += sizeof v.pos;
        std::memcpy(&v.map, data.data() + at, 4);
        {
            std::lock_guard lock(S().status_mu);
            S().status.Named(sender, v.name, Clock::now());
            S().status.Vitals(sender, v.hp, v.hp_max, v.map, Clock::now());
        }
        std::lock_guard lock(S().vitals_mu);
        S().vitals[sender] = {std::move(v), Clock::now()};
        return;
    }
    if (kind == kRequest || kind == kReply) {
        if (data.size() < 5) return;
        std::uint32_t id = 0;
        for (int i = 0; i < 4; ++i) id |= static_cast<std::uint32_t>(data[1 + i]) << (8 * i);
        Json value = Json::parse(data.begin() + 5, data.end(), nullptr, false);
        if (value.is_discarded()) return;
        if (kind == kRequest) {
            if (!S().host) return;
            SendJson(sender, kReply, HandleHost(sender, value), id, true);
            return;
        }
        if (!id) return;
        {
            std::lock_guard lock(S().mu);
            S().replies[id] = std::move(value);
        }
        S().cv.notify_all();
        return;
    }
    if (kind == kEvent) {
        if (sender != S().link.HostSlot()) return;
        Json value = Json::parse(data.begin() + 1, data.end(), nullptr, false);
        if (!value.is_object()) return;
        NoteEvent(value);
        Localize(value);
        {
            std::lock_guard lock(S().mu);
            value["EventId"] = S().next_event++;
            S().events.push_back(std::move(value));
        }
        S().cv.notify_all();
    }
}

std::atomic<std::int64_t> g_connected_at_ms{0};

void StatusTick() {
    const auto now = Clock::now();
    {
        std::lock_guard lock(S().status_mu);
        S().status.Tick(now);
    }
    if (!S().link.Connected() || now - S().ping_at < kPingEvery) return;
    S().ping_at = now;
    const auto stamp = static_cast<std::uint32_t>(SteadyMs());
    std::vector<std::uint8_t> frame(5);
    frame[0] = kPing;
    std::memcpy(frame.data() + 1, &stamp, 4);
    const std::string name = S().player.substr(0, kLongestPingName);
    frame.insert(frame.end(), name.begin(), name.end());
    S().link.Send(0, frame.data(), frame.size());
}

void LinkLoop() {
    int last_host = 0;
    std::string refused;
    for (;;) {
        S().link.WaitEvent(500);
        for (auto& e : S().link.Drain()) {
            switch (e.kind) {
            case Event::Kind::Connected:
                refused.clear();
                if (S().own_slot.load() && S().own_slot.load() != e.slot)
                    Log("came back as slot %d (was %d)", e.slot, S().own_slot.load());
                S().own_slot = e.slot;
                if (!g_connected_at_ms.load()) g_connected_at_ms = SteadyMs();
                {
                    std::lock_guard lock(S().status_mu);
                    S().status.OwnConnected(Clock::now());
                }
                Log("in the room as slot %d%s", e.slot, S().host ? " (host)" : "");
                break;
            case Event::Kind::Joined:
                BridgeOpen(e.slot);
                Log("slot %d joined the room", e.slot);
                break;
            case Event::Kind::Left:
                Log("slot %d left the room", e.slot);
                {
                    std::lock_guard lock(S().vitals_mu);
                    S().vitals.erase(e.slot);
                    S().pieces.erase(e.slot);
                    S().portraits.erase(e.slot);
                }
                {
                    std::lock_guard lock(S().status_mu);
                    S().status.Left(e.slot, Clock::now());
                }
                if (S().host) {
                    std::lock_guard lock(S().host_mu);
                    ForgetSlotLocked(e.slot);
                }
                break;
            case Event::Kind::Away:
                Log("slot %d dropped without a leave: the room holds its slot for a reconnect", e.slot);
                {
                    std::lock_guard lock(S().status_mu);
                    S().status.Away(e.slot, Clock::now());
                }
                break;
            case Event::Kind::Host:
                if (e.slot != last_host) Log("the room's host is slot %d", e.slot);
                if (last_host && e.slot != last_host && !S().host) {
                    std::lock_guard lock(S().mu);
                    S().events.push_back({{"Name", "room_closed"}, {"RoomId", 0}, {"Reason", "host_left"},
                                          {"EventId", S().next_event++}});
                    S().cv.notify_all();
                }
                last_host = e.slot;
                if (e.slot && !S().host && !S().player.empty()) {
                    const std::string introduce = NetworkId();
                    SendJson(e.slot, kRequest,
                             Json{{"p", "/mp/matching2/context_start"}, {"b", {{"OnlineId", introduce}}}}, 0, true);
                }
                break;
            case Event::Kind::Data:
                {
                    std::lock_guard lock(S().status_mu);
                    S().status.Heard(e.slot, Clock::now());
                }
                OnFrame(e.slot, e.data);
                break;
            case Event::Kind::Disconnected:
                Log("lost the room connection; reconnecting");
                {
                    std::lock_guard lock(S().status_mu);
                    S().status.OwnLost(Clock::now());
                }
                break;
            case Event::Kind::GaveUp:
                {
                    std::lock_guard lock(S().status_mu);
                    S().status.OwnGaveUp(Clock::now());
                }
                Log("the room could not be reached for 2 minutes: seamless co-op is off until the next start");
                break;
            case Event::Kind::Refused:
                if (!e.data.empty()) {
                    const std::string reason(e.data.begin(), e.data.end());
                    if (reason != refused) Log("Refused: %s", reason.c_str());
                    refused = reason;
                    break;
                }
                Log("could not join the room (%s): it may be full (5 players), or your connection already has another "
                    "room open (one room per player); retrying",
                    S().link.LastError().empty() ? "refused" : S().link.LastError().c_str());
                break;
            case Event::Kind::Visitor:
                AddPartyLine(PartyLineKind::Visitor, {});
                break;
            case Event::Kind::Announcement:
                if (AnnouncementServer(Env("BB_ROOM_SERVER"))) {
                    const std::string words(e.data.begin(), e.data.end());
                    std::lock_guard lock(S().announce_mu);
                    if (S().announcements.Offer(e.id, words, Clock::now())) Log("server announcement: %s", CleanAnnouncement(words).c_str());
                }
                break;
            }
        }
        StatusTick();
    }
}

std::string ApiList(const std::string& path) {
    static const std::map<std::string, std::string> lists = {
        {"/blood_messenger/create", "BloodMessIdList"},
        {"/blood_messenger/message_area", "BloodMessList"},
        {"/blood_messenger/evaluation", "BloodMessEvaluationList"},
        {"/blood_messenger/exist_messages", "BloodMessEvaluationList"},
        {"/chair_messenger/get", "ChairMessList"},
        {"/tomb_messenger/message_area", "TombMessList"},
        {"/wandering_ghost/get", "WanderingGhostList"},
    };
    const auto it = lists.find(path);
    return it == lists.end() ? "" : it->second;
}

std::string GameApiAnswer(const std::string& path, const Json& body) {
    const int slot = S().own_slot.load();
    if (path == "/basic_utils/login") {
        return Json{{"ResKind", 0}, {"SessionId", "s" + std::to_string(slot)}, {"UserId", 1000 + slot},
                    {"UserStatus", 0}, {"LanguageId", 1}, {"ServerVersion", 109}}
            .dump();
    }
    if (path == "/basic_utils/sync_chara_id") {
        return Json{{"ResKind", 0}, {"PublishCharacterIdList", Json::array({{{"PublishCharaId", slot ? slot : 1}}})}}.dump();
    }
    if (path == "/basic_utils/get_normal_notice") return R"({"ResKind":0,"NoticeList":[]})";
    if (path == "/basic_utils/get_emergency_notice")
        return Json{{"ResKind", 0}, {"NoticeList", Json::array()}, {"CheckTime", Stamp()}}.dump();
    if (path == "/summon_messenger/create" && !S().host) {
        std::string introduce;
        {
            std::lock_guard lock(S().mu);
            introduce = S().online_id;
        }
        const int host = S().link.HostSlot();
        if (host && !introduce.empty())
            SendJson(host, kRequest, Json{{"p", "/mp/matching2/context_start"}, {"b", {{"OnlineId", introduce}}}}, 0, true);
    }
    if (path.rfind("/summon_messenger/", 0) == 0 || path.rfind("/channel/", 0) == 0) {
        Json reply;
        std::string error;
        const bool ok = Rpc({{"p", path}, {"b", body}}, reply, error, 8000);
        if (path.rfind("/summon_messenger/", 0) == 0) NoteSummon(path.substr(18), body, reply, ok);
        if (!ok) return "";
        return reply.dump();
    }
    Json reply = {{"ResKind", 0}};
    if (const std::string list = ApiList(path); !list.empty()) reply[list] = Json::array();
    if (path == "/blood_messenger/exist_messages") reply["LostBloodMessIdList"] = Json::array();
    if (path == "/chair_messenger/get") reply["ChairMessCount"] = 0;
    return reply.dump();
}

std::string SsInfo() {
    static const char* const apis[] = {
        "Login", "ServerTimeGet", "SyncCharaId", "NoticeNormalGet", "NoticeEmergencyGet", "UserAgreementGet",
        "BloodMessCreate", "BloodMessGetList", "BloodMessEvaluate", "BloodMessGetEvaluate", "BloodMessRemove",
        "BloodMessSearchAdd", "ChannelUpload", "ChannelShare", "ChannelSearch", "ChannelWordSearch",
        "ChannelGetDetailsInfo", "ChannelGetInfo", "ChannelRandomJoin", "ChannelAddMaterial",
        "ChannelAddMaterialCompleteNotify", "MessengerShellUpload", "MultiPlayNetError", "UserPropertiesMoveCount",
        "UserPropertiesMoveCountCheck", "SummonDataCreate", "SummonDataGetList", "SummonDataRemove",
        "SummonDataSummon", "ChairMessGetList", "ChairMessRespawnPointNotice", "TombMessCreate", "TombMessGetList",
        "DeathVisionGet", "TombMessRemove", "WanderingGhostCreate", "WanderingGhostGet"};
    static const std::pair<const char*, const char*> numbers[] = {
        {"ReloadServerStatusInfoInterval", "600"},
        {"SummonDataCreateInterval", "1"},
        {"SummonDataGetListInterval", "1"},
        {"SummonDataGetListGuardTimer", "0"},
        {"SummonDataGetListNomalCoopWaitCount", "0"},
        {"SummonDataGetListCombinationInvasionWaitCount", "0"},
        {"SummonDataGetListNaturalEnemyWaitCountFixed", "0"},
        {"SummonDataGetListAllAreaMethodWaitCount", "0"},
        {"SummonDataGetListGetCountPerSummonType", "8"},
        {"SummonDataGetListGetMaxCount", "8"},
        {"SummonDataGetListCoopGuardTimerClampTime", "0"},
        {"SummonDataGetListInvationGuardTimerClampTime", "0"},
        {"SummonDataGetListCoopGuardTimerLimitationTimeRate", "0"},
        {"SummonDataGetListCoopGuardTimerObserveTimeRate", "0"},
        {"SummonDataGetListInvationGuardTimerLimitationTimeRate", "0"},
        {"SummonDataGetListInvationGuardTimerObserveTimeRate", "0"},
        {"TombMessGetListInterval", "600"},
        {"BloodMessGetListInterval", "600"},
        {"WanderingGhostCreateInterval", "600"},
        {"WanderingGhostGetInterval", "600"},
        {"ChairMessGetListInterval", "60"},
        {"NoticeEmergencyGetInterval", "3600"},
        {"PlayLog", "0"},
        {"SummonDataCoopMatchingLevelLowerAbs", "-1000"},
        {"SummonDataCoopMatchingLevelLowerRel", "0"},
        {"SummonDataCoopMatchingLevelUpperAbs", "1000"},
        {"SummonDataCoopMatchingLevelUpperRel", "0"},
        {"SummonDataCreateNaturalEnemyPercentFixed", "0"},
        {"SummonDataBloodMadHunterPercent", "0"},
    };
    const std::string base = "http://127.0.0.1:" + std::to_string(S().http_port);
    std::string text = "<ssinfo><ss>0</ss>";
    for (int n = 0; n < 10; ++n) {
        const std::string k = std::to_string(n);
        text += "<gameurl" + k + ">";
        for (const char* api : apis) text += std::string("<api_") + api + ">" + base + "</api_" + api + ">";
        text += "</gameurl" + k + ">";
        for (const auto& [tag, value] : numbers) text += std::string("<") + tag + k + ">" + value + "</" + tag + k + ">";
    }
    text += "</ssinfo>";
    std::string encoded(4 * ((text.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                  reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
    encoded.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return encoded;
}

void HttpLoop() {
    httplib::Server server;
    server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 60) Log("game server: %s %s -> %d", req.method.c_str(), req.path.c_str(), res.status);
    });
    server.Get(".*", [](const httplib::Request& req, httplib::Response& res) {
        if (req.path.size() >= 8 && req.path.compare(req.path.size() - 8, 8, "/ss.info") == 0) {
            res.set_content(SsInfo(), "text/plain");
            return;
        }
        res.set_content(GameApiAnswer(req.path, Json::object()), "application/json");
    });
    server.Post(".*", [](const httplib::Request& req, httplib::Response& res) {
        const Json body = Json::parse(req.body, nullptr, false);
        const std::string answer = GameApiAnswer(req.path, body.is_object() ? body : Json::object());
        if (answer.empty()) {
            res.status = 503;
            return;
        }
        res.set_content(answer, "application/json");
    });
    server.Put(".*", [](const httplib::Request&, httplib::Response& res) { res.status = 200; });
    if (!server.listen("127.0.0.1", S().http_port)) Log("the game server could not listen on port %d", S().http_port);
}

} // namespace

bool Active() {
    static const bool on = Env("BB_NET_BACKEND") == "room";
    return on;
}

std::int64_t ConnectedForMs() {
    const std::int64_t at = g_connected_at_ms.load();
    return at ? SteadyMs() - at : -1;
}

bool SponsoredRelay() {
    static const bool on = Env("BB_ROOM_SERVER").find("185-244-28-175.sslip.io") != std::string::npos;
    return on;
}

bool IsHost() {
    return Env("BB_ROOM_HOST") == "1";
}

void SendVitals(const Vitals& own) {
    if (!S().started.load() || !S().link.Connected()) return;
    const std::string name = own.name.substr(0, 32);
    std::vector<std::uint8_t> frame(kVitalsFixed + name.size());
    frame[0] = kVitals;
    frame[1] = static_cast<std::uint8_t>(name.size());
    std::size_t at = 2;
    std::memcpy(frame.data() + at, name.data(), name.size());
    at += name.size();
    for (const std::int32_t field : {own.hp, own.hp_max, own.stamina, own.stamina_max}) {
        std::memcpy(frame.data() + at, &field, 4);
        at += 4;
    }
    std::memcpy(frame.data() + at, &own.revives, 2);
    at += 2;
    std::memcpy(frame.data() + at, own.pos, sizeof own.pos);
    at += sizeof own.pos;
    std::memcpy(frame.data() + at, &own.map, 4);
    S().link.Send(0, frame.data(), frame.size());
}

std::vector<Vitals> Party(int max_age_ms) {
    std::vector<Vitals> out;
    const auto now = Clock::now();
    std::lock_guard lock(S().vitals_mu);
    for (const auto& [slot, entry] : S().vitals) {
        Vitals v = entry.first;
        v.age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.second).count();
        if (v.age_ms <= max_age_ms) out.push_back(std::move(v));
    }
    return out;
}

void SendTravel(std::uint32_t lamp, const std::vector<std::uint8_t>& world_flags) {
    if (!S().started.load() || !S().link.Connected()) return;
    if (!world_flags.empty()) {
        std::vector<std::uint8_t> flags_frame(1 + world_flags.size());
        flags_frame[0] = kTravelFlags;
        std::memcpy(flags_frame.data() + 1, world_flags.data(), world_flags.size());
        S().link.Send(0, flags_frame.data(), flags_frame.size());
    }
    std::uint8_t frame[5] = {kTravel};
    std::memcpy(frame + 1, &lamp, 4);
    S().link.Send(0, frame, sizeof frame);
    Log("travelling to lamp %u: the party follows", lamp);
}

bool TakeTravel(std::uint32_t& lamp, std::vector<std::uint8_t>* world_flags) {
    std::lock_guard lock(S().vitals_mu);
    if (!S().travel) return false;
    std::vector<std::uint8_t> flags;
    flags.swap(S().travel_flags);
    if (Clock::now() - S().travel_at > std::chrono::seconds(120)) {
        S().travel = 0;
        return false;
    }
    lamp = S().travel;
    S().travel = 0;
    if (world_flags) *world_flags = std::move(flags);
    return true;
}

void SendStageTravel(std::uint32_t map, std::int32_t point, const std::vector<std::uint8_t>& world_flags) {
    if (!S().started.load() || !S().link.Connected()) return;
    if (!world_flags.empty()) {
        std::vector<std::uint8_t> flags_frame(1 + world_flags.size());
        flags_frame[0] = kTravelFlags;
        std::memcpy(flags_frame.data() + 1, world_flags.data(), world_flags.size());
        S().link.Send(0, flags_frame.data(), flags_frame.size());
    }
    std::uint8_t frame[9] = {kStageTravel};
    std::memcpy(frame + 1, &map, 4);
    std::memcpy(frame + 5, &point, 4);
    S().link.Send(0, frame, sizeof frame);
    Log("travelling to map %08x: the party follows", map);
}

bool TakeStageTravel(std::uint32_t& map, std::int32_t& point, std::vector<std::uint8_t>* world_flags) {
    std::lock_guard lock(S().vitals_mu);
    if (!S().stage_map) return false;
    std::vector<std::uint8_t> flags;
    flags.swap(S().travel_flags);
    const bool fresh = Clock::now() - S().stage_at <= std::chrono::seconds(120);
    map = S().stage_map;
    point = S().stage_point;
    S().stage_map = 0;
    if (!fresh) return false;
    if (world_flags) *world_flags = std::move(flags);
    return true;
}

void SendWorldFlags(const std::vector<std::uint8_t>& part, int target) {
    if (part.empty() || !S().started.load() || !S().link.Connected()) return;
    std::vector<std::uint8_t> frame(1 + part.size());
    frame[0] = kWorldFlags;
    std::memcpy(frame.data() + 1, part.data(), part.size());
    S().link.Send(target, frame.data(), frame.size());
}

std::vector<std::vector<std::uint8_t>> TakeWorldFlags() {
    std::lock_guard lock(S().vitals_mu);
    return std::exchange(S().world_flags, {});
}

void SendWorldAsk() {
    if (!S().started.load() || !S().link.Connected()) return;
    const int host = S().link.HostSlot();
    if (host <= 0) return;
    const std::uint8_t frame[1] = {kWorldAsk};
    S().link.Send(host, frame, sizeof frame);
}

std::vector<int> TakeWorldAsks() {
    std::lock_guard lock(S().vitals_mu);
    return std::exchange(S().world_asks, {});
}

void SendProgress(const std::vector<std::uint32_t>& flags, int target) {
    if (flags.empty() || !S().started.load() || !S().link.Connected()) return;
    std::vector<std::uint8_t> frame(1 + 4 * flags.size());
    frame[0] = kProgress;
    std::memcpy(frame.data() + 1, flags.data(), 4 * flags.size());
    S().link.Send(target, frame.data(), frame.size());
}

std::vector<std::uint32_t> TakeProgress() {
    std::lock_guard lock(S().vitals_mu);
    return std::exchange(S().progress, {});
}

void SendBossLot(std::int32_t lot, int target) {
    if (!S().started.load() || !S().link.Connected()) return;
    std::uint8_t frame[5] = {kBossLot};
    std::memcpy(frame + 1, &lot, 4);
    S().link.Send(target, frame, sizeof frame);
}

void SendBossTrophy(std::int32_t trophy, int target) {
    if (!S().started.load() || !S().link.Connected()) return;
    std::uint8_t frame[5] = {kBossTrophy};
    std::memcpy(frame + 1, &trophy, 4);
    S().link.Send(target, frame, sizeof frame);
}

void SendPartyCall(PartyCall call, int target) {
    if (!S().started.load() || !S().link.Connected()) return;
    const std::uint8_t frame[2] = {kPartyCall, std::uint8_t(call)};
    S().link.Send(target, frame, sizeof frame);
}

std::vector<std::pair<int, PartyCall>> TakePartyCalls() {
    std::lock_guard lock(S().vitals_mu);
    return std::exchange(S().party_calls, {});
}

std::vector<std::int32_t> TakeBossTrophies() {
    std::lock_guard lock(S().vitals_mu);
    return std::exchange(S().boss_trophies, {});
}

std::vector<std::int32_t> TakeBossLots() {
    std::lock_guard lock(S().vitals_mu);
    return std::exchange(S().boss_lots, {});
}

void PutDungeonForm(std::uint64_t channel, const std::vector<std::uint8_t>& data) {
    std::string form(4 * ((data.size() + 2) / 3) + 1, char(0));
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(form.data()), data.data(), static_cast<int>(data.size()));
    form.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    std::lock_guard lock(S().host_mu);
    auto it = S().channels.find(channel);
    if (it == S().channels.end()) {
        Channel c;
        c.id = channel;
        c.key = "entered:" + std::to_string(channel);
        c.word = WordOf(c.key);
        c.owner = S().own_slot.load();
        c.made = Clock::now();
        for (const char* field : {"HolyGrailTypeId", "RitualLevel", "FixedOrGeneral", "SubFeatureFlag"}) c.fields[field] = 0;
        c.fields["WishMaterialList"] = Json::array();
        it = S().channels.emplace(channel, std::move(c)).first;
    }
    if (it->second.form == form) return;
    it->second.form = form;
    Log("dungeon %llu: its layout is on the room's board", static_cast<unsigned long long>(channel));
}

void SendDungeon(const Dungeon& d, int target) {
    if (!S().started.load() || !S().link.Connected()) return;
    std::vector<std::uint8_t> frame(1 + 8 + 4 * 43 + 4 * d.flags.size());
    frame[0] = kDungeon;
    std::memcpy(frame.data() + 1, &d.channel, 8);
    std::memcpy(frame.data() + 9, d.record.data(), 4 * 43);
    if (!d.flags.empty()) std::memcpy(frame.data() + 9 + 4 * 43, d.flags.data(), 4 * d.flags.size());
    S().link.Send(target, frame.data(), frame.size());
}

bool TakeDungeon(Dungeon& out) {
    std::lock_guard lock(S().vitals_mu);
    if (S().dungeons.empty()) return false;
    out = std::move(S().dungeons.back());
    S().dungeons.clear();
    return true;
}

void SendChalices(const std::vector<std::uint32_t>& goods) {
    if (goods.empty() || goods.size() > 64 || !S().started.load() || !S().link.Connected()) return;
    std::vector<std::uint8_t> frame(1 + 4 * goods.size());
    frame[0] = kChalices;
    std::memcpy(frame.data() + 1, goods.data(), 4 * goods.size());
    S().link.Send(0, frame.data(), frame.size());
}

bool TakeChalices(std::vector<std::uint32_t>& out) {
    std::lock_guard lock(S().vitals_mu);
    if (!S().chalices_new) return false;
    S().chalices_new = false;
    out = S().chalices;
    return true;
}

void SendPortrait(const std::vector<std::uint8_t>& packed) {
    if (packed.empty() || packed.size() + 1 > kLargestPortrait || !S().started.load() || !S().link.Connected()) return;
    std::vector<std::uint8_t> frame(1 + packed.size());
    frame[0] = kPortrait;
    std::memcpy(frame.data() + 1, packed.data(), packed.size());
    const std::size_t count = (frame.size() + kPieceBytes - 1) / kPieceBytes;
    if (count > 255) return;
    const std::uint8_t id = ++S().next_pieces;
    std::vector<std::uint8_t> piece;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = i * kPieceBytes;
        const std::size_t size = std::min(kPieceBytes, frame.size() - at);
        piece.assign({kPiece, id, static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(count)});
        piece.insert(piece.end(), frame.begin() + static_cast<std::ptrdiff_t>(at), frame.begin() + static_cast<std::ptrdiff_t>(at + size));
        if (!S().link.SendPaced(0, piece.data(), piece.size())) return;
    }
}

std::vector<std::pair<int, std::vector<std::uint8_t>>> TakePortraits() {
    std::lock_guard lock(S().vitals_mu);
    std::vector<std::pair<int, std::vector<std::uint8_t>>> out(S().portraits.begin(), S().portraits.end());
    S().portraits.clear();
    return out;
}

int HostSlot() {
    return S().link.HostSlot();
}

std::string PlayerName() {
    return Env("BB_ROOM_PLAYER").substr(0, 16);
}

std::string NetworkId() {
    static const std::string id = [] {
        const std::string name = PlayerName();
        if (name.empty()) return name;
        char tag[8];
        std::snprintf(tag, sizeof tag, "-%04x", std::random_device{}() & 0xffffu);
        return name.substr(0, 16 - 5) + tag;
    }();
    return id;
}

std::string PlainName(const std::string& network_id) {
    const std::size_t n = network_id.size();
    if (n < 6 || network_id[n - 5] != '-') return network_id;
    for (std::size_t i = n - 4; i < n; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(network_id[i]))) return network_id;
    }
    return network_id.substr(0, n - 5);
}

void Shutdown() {
    static std::atomic<bool> done{false};
    if (!S().started.load() || done.exchange(true)) return;
    if (S().link.Connected()) Log("leaving the room: this game is closing");
    S().link.Stop();
    SetVoice(false);
}

void Start(int game_p2p_port) {
    BbOnlineGate::MarkLive("seamless co-op");
    if (S().started.exchange(true)) return;
    std::atexit(Shutdown);
    if (!S().game_port) S().game_port = game_p2p_port;
    S().host = IsHost();
    S().player = PlayerName();
    S().http_port = std::atoi(Env("BB_ROOM_HTTP_PORT").c_str());
    if (S().http_port <= 0 || S().http_port > 65535) S().http_port = game_p2p_port + 1;
    S().link.SetHost(S().host);
    S().link.SetVisitor(!S().host && Env("BB_ROOM_VISITOR") == "1");
    if (const int local = std::atoi(Env("BB_ROOM_LOCAL_PORT").c_str()); S().host && local > 0) {
        std::string error;
        if (!S().local_room.Start(local, Derive(Env("BB_ROOM_NAME"), Env("BB_ROOM_PASSWORD")).room_id, error)) {
            Log("direct room not opened: %s", error.c_str());
            return;
        }
        Log("direct room open on this PC, port %d: guests connect straight to this game, no server", local);
    }
    if (!S().link.Start(Env("BB_ROOM_SERVER"), Env("BB_ROOM_NAME"), Env("BB_ROOM_PASSWORD"))) {
        Log("no room server or room name: seamless co-op is off");
        return;
    }
    Log("joining room on %s as %s (%s), game server on 127.0.0.1:%d", DirectRoom() ? "a direct connection" : Env("BB_ROOM_SERVER").c_str(),
        S().player.c_str(), S().host ? "host" : "guest", S().http_port);
    std::thread(LinkLoop).detach();
    std::thread(BridgeLoop).detach();
    std::thread(HttpLoop).detach();
}

void SetGamePort(int port) {
    if (port > 0 && port <= 65535) S().game_port = port;
}

bool Call(const std::string& path, const Json& body, Json& reply, std::string& error, int timeout_ms) {
    if (path == "/np/events/poll") return PollEvent(body, reply);
    if (path == "/np/events/ack" || path == "/mp/matching2/signaling_update") {
        reply = {{"ResKind", 0}};
        return true;
    }
    const bool ok = Rpc({{"p", path}, {"b", body}}, reply, error, std::max(timeout_ms, 3000));
    if (path == "/mp/matching2/join_room") {
        const bool joined = ok && IntOf(reply, "ResKind", 1) == 0;
        Notice& n = N();
        std::unique_lock lock(n.mu);
        const std::string who = n.who;
        lock.unlock();
        Note(joined ? Stage::Arrived : Stage::Failed, joined ? who : std::string());
    }
    return ok;
}

std::vector<std::pair<int, std::string>> PartyMembers() {
    std::vector<std::pair<int, std::string>> out;
    if (!S().host) return out;
    std::lock_guard lock(S().host_mu);
    const int own = S().own_slot.load();
    for (const auto& [id, room] : S().rooms) {
        const bool mine = std::any_of(room.members.begin(), room.members.end(),
                                      [&](const Member& m) { return m.slot == own && m.member_id == 1; });
        if (!mine) continue;
        for (const Member& m : room.members) {
            if (m.slot == own) continue;
            const auto name = S().names.find(m.slot);
            out.emplace_back(m.slot, name == S().names.end() ? std::string("a hunter") : name->second);
        }
    }
    return out;
}

bool Kick(int slot) {
    if (!S().host) return false;
    std::lock_guard lock(S().host_mu);
    const bool done = KickLocked(slot, false);
    if (done) Log("the host sent slot %d home", slot);
    if (!S().link.IsVisitor(slot)) return done;
    S().link.SendAway(slot);
    Log("the host sent the visitor in slot %d away from the room", slot);
    return true;
}

bool OpenRoomAllowed() {
    return IsHost() && AnnouncementServer(Env("BB_ROOM_SERVER"));
}

void SetOpenRoom(bool on) {
    S().link.SetOpen(on && OpenRoomAllowed());
}

bool OpenRoomListed() {
    return S().link.OpenListed();
}

bool IsVisitorSlot(int slot) {
    return S().link.IsVisitor(slot);
}

bool LeaveParty() {
    if (S().host || !S().started.load()) return false;
    Json reply;
    std::string error;
    const bool ok = Rpc({{"p", "/lantern/leave_party"}, {"b", Json::object()}}, reply, error, 5000);
    if (ok) Log("left the host's world from the co-op panel");
    return ok;
}

namespace {
std::atomic<bool> g_sent_home{false};
}

bool IsDevSlot(int slot) {
    return S().link.IsDev(slot);
}

void TakeDownSign() {
    if (S().host || !S().started.load()) return;
    std::thread([] {
        Json reply;
        std::string error;
        Rpc({{"p", "/summon_messenger/delete"}, {"b", Json::object()}}, reply, error, 3000);
    }).detach();
}

void NoteSentHome() {
    g_sent_home = true;
}

bool TakeSentHome() {
    return g_sent_home.exchange(false);
}

void VoiceLoop() {
    while (S().voice_run.load()) {
        S().voice_link.WaitEvent(200);
        for (auto& e : S().voice_link.Drain()) {
            if (e.kind == Event::Kind::Connected) Log("voice: connected to the room's voice channel");
            if (e.kind != Event::Kind::Data || e.data.size() < 4 || e.data.size() > 1200 || e.data[0] != kVoice) continue;
            std::lock_guard lock(S().vitals_mu);
            if (S().voice.size() >= 48) S().voice.pop_front();
            S().voice.emplace_back(-e.slot, std::vector<std::uint8_t>(e.data.begin() + 1, e.data.end()));
        }
    }
}

void SetVoice(bool on) {
    std::lock_guard lock(S().voice_mu);
    if (on == S().voice_on || (on && !S().started.load())) return;
    S().voice_on = on;
    if (on) {
        if (!S().voice_link.Start(Env("BB_ROOM_SERVER"), Env("BB_ROOM_NAME"), Env("BB_ROOM_PASSWORD"), "voice")) {
            S().voice_on = false;
            return;
        }
        S().voice_run = true;
        S().voice_thread = std::thread(VoiceLoop);
        return;
    }
    S().voice_run = false;
    if (S().voice_thread.joinable()) S().voice_thread.join();
    S().voice_link.Stop();
    Log("voice: left the room's voice channel");
}

void SendVoice(const std::vector<std::uint8_t>& packet) {
    if (packet.empty() || packet.size() > 1100 || !S().voice_link.Connected()) return;
    std::vector<std::uint8_t> frame(1 + packet.size());
    frame[0] = kVoice;
    std::memcpy(frame.data() + 1, packet.data(), packet.size());
    S().voice_link.SendLossy(0, frame.data(), frame.size(), kMostVoiceBacklog);
}

std::vector<std::pair<int, std::vector<std::uint8_t>>> TakeVoice() {
    std::lock_guard lock(S().vitals_mu);
    std::vector<std::pair<int, std::vector<std::uint8_t>>> out(std::make_move_iterator(S().voice.begin()),
                                                                 std::make_move_iterator(S().voice.end()));
    S().voice.clear();
    return out;
}

void SendBring(int target) {
    if (!S().host || !S().started.load() || !S().link.Connected()) return;
    const std::uint8_t frame[1] = {kBring};
    S().link.Send(target, frame, sizeof frame);
    {
        std::lock_guard lock(S().status_mu);
        S().status.ClearAsk(target);
    }
    Log("the host calls %s to its side", target ? ("slot " + std::to_string(target)).c_str() : "everyone");
}

bool TakeBring() {
    std::lock_guard lock(S().vitals_mu);
    if (S().bring_at == Clock::time_point{}) return false;
    const bool fresh = Clock::now() - S().bring_at < std::chrono::seconds(10);
    S().bring_at = {};
    return fresh;
}

std::vector<PeerStatus> Connections() {
    std::lock_guard lock(S().status_mu);
    return S().status.List(Clock::now());
}

std::vector<PartyLine> PartyLines() {
    std::lock_guard lock(S().status_mu);
    return S().status.Lines(Clock::now());
}

bool CurrentReconnect(ReconnectNotice& out) {
    std::lock_guard lock(S().status_mu);
    return S().status.Reconnecting(Clock::now(), out);
}

bool LinkUp() {
    return S().link.Connected();
}

void SendWarpAsk() {
    if (S().host || !S().started.load()) return;
    const auto now = Clock::now();
    std::lock_guard lock(S().status_mu);
    if (S().status.GuestWaiting(now)) return;
    const int host = S().link.HostSlot();
    const std::uint8_t frame[1] = {kWarpAsk};
    if (!host || !S().link.Connected() || !S().link.Send(host, frame, sizeof frame)) {
        S().status.AddLine(PartyLineKind::WarpNoAnswer, {}, now);
        return;
    }
    S().status.GuestAsked(now);
    Log("asked the host to bring this hunter over");
}

bool WarpWaiting() {
    std::lock_guard lock(S().status_mu);
    return S().status.GuestWaiting(Clock::now());
}

void SetWarpRequests(bool on) {
    S().warp_requests = on;
}

void SendLampAsk(std::uint32_t lamp) {
    if (S().host || !S().started.load()) return;
    const auto now = Clock::now();
    std::lock_guard lock(S().status_mu);
    if (S().status.GuestLampWaiting(now)) return;
    const int host = S().link.HostSlot();
    std::uint8_t frame[5] = {kLampAsk};
    std::memcpy(frame + 1, &lamp, 4);
    if (!host || !S().link.Connected() || !S().link.Send(host, frame, sizeof frame)) {
        S().status.AddLine(PartyLineKind::LampNoAnswer, {}, now);
        return;
    }
    S().status.GuestLampAsked(now);
    Log("asked the host to take the party to lamp %u", lamp);
}

bool SignsWaiting() {
    if (!S().host) return false;
    std::lock_guard lock(S().host_mu);
    const int own = S().own_slot.load();
    return std::any_of(S().signs.begin(), S().signs.end(), [&](const auto& sign) { return sign.first != own; });
}

bool PendingLampAsk(int& slot, std::string& who, std::uint32_t& lamp) {
    std::lock_guard lock(S().status_mu);
    return S().status.LampAskNow(Clock::now(), slot, who, lamp);
}

void AnswerLampAsk(bool yes) {
    int slot = 0;
    std::string who;
    std::uint32_t lamp = 0;
    {
        std::lock_guard lock(S().status_mu);
        if (!S().status.LampAskNow(Clock::now(), slot, who, lamp)) return;
        S().status.LampAskDone();
    }
    const std::uint8_t answer[2] = {kLampAnswer, yes ? std::uint8_t(kLampAgreed) : std::uint8_t(kLampRefused)};
    S().link.Send(slot, answer, sizeof answer);
    Log("the host %s the party's travel to lamp %u", yes ? "accepted" : "refused", lamp);
}

void NoteTravelLost() {
    AddPartyLine(PartyLineKind::TravelLost, {});
}

void NoteStayedWithParty() {
    AddPartyLine(PartyLineKind::StayedWithParty, {});
}

void NoteBringFar() {
    AddPartyLine(PartyLineKind::BringFar, {});
}

bool CurrentAnnouncement(std::string& text, float& alpha) {
    std::lock_guard lock(S().announce_mu);
    return S().announcements.Current(Clock::now(), text, alpha);
}

bool CurrentNotice(std::string& text) {
    Notice& n = N();
    std::lock_guard lock(n.mu);
    const auto now = Clock::now();
    const long secs = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(now - n.since).count());
    const auto quiet = now - n.seen;
    char line[160];
    switch (n.stage) {
    case Stage::None:
        return false;
    case Stage::Looking:
    case Stage::Found:
    case Stage::Calling:
    case Stage::Waiting:
        return false;
    case Stage::Invited:
        if (quiet > std::chrono::seconds(15)) {
            if (quiet > std::chrono::seconds(20)) return false;
            std::snprintf(line, sizeof line, "%s's summon did not go through: ring again", n.who.c_str());
            break;
        }
        std::snprintf(line, sizeof line, "Joining %s's world... %ld s", n.who.c_str(), secs);
        break;
    case Stage::Arrived:
        if (quiet > std::chrono::seconds(4)) return false;
        std::snprintf(line, sizeof line, "In %s's world", n.who.empty() ? "the host" : n.who.c_str());
        break;
    case Stage::Home:
        if (quiet > std::chrono::seconds(4)) return false;
        std::snprintf(line, sizeof line, "Back in your own world");
        break;
    case Stage::HostOnly:
        if (quiet > std::chrono::seconds(6)) return false;
        std::snprintf(line, sizeof line, "Only the host can summon: ring the Small Resonant Bell to help");
        break;
    case Stage::Failed:
        if (quiet > std::chrono::seconds(6)) return false;
        std::snprintf(line, sizeof line, "Could not connect: ring again");
        break;
    }
    text = line;
    return true;
}

} // namespace BbRoom
