// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <httplib.h>

#include "gpu/bbnet/room/room_link.h"
#include "gpu/bbnet/room/room_service.h"

using Json = nlohmann::json;

namespace {

int Fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    std::fflush(stdout);
    return 1;
}

bool Poll(Json& ev, const char* name, int seconds = 20) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        Json reply;
        std::string error;
        BbRoom::Call("/np/events/poll", {{"WaitMs", 1000}, {"Cursor", 0}}, reply, error, 3000);
        if (reply.value("HasEvent", 0) && reply.value("Name", "") == name) {
            ev = reply;
            return true;
        }
    }
    return false;
}

Json Game(int http_port, const std::string& path, const Json& body) {
    httplib::Client cli("127.0.0.1", http_port);
    cli.set_read_timeout(std::chrono::seconds(15));
    auto res = cli.Post(path, body.dump(), "application/json");
    if (!res || res->status != 200) return Json();
    return Json::parse(res->body, nullptr, false);
}

int OpenUdp(int port) {
    const auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
    DWORD timeout = 15000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv{15, 0};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return static_cast<int>(s);
}

bool SwapVitals(const char* own, const char* other) {
    BbRoom::Vitals v;
    v.name = own;
    v.hp = 640;
    v.hp_max = 800;
    v.stamina = 90;
    v.stamina_max = 100;
    v.revives = 3;
    v.pos[0] = 1.5f;
    v.map = 0x1d0a5a00u;
    for (int i = 0; i < 50; ++i) {
        BbRoom::SendVitals(v);
        for (const BbRoom::Vitals& got : BbRoom::Party(5000)) {
            if (got.name == other && got.hp == 640 && got.hp_max == 800 && got.revives == 3 && got.pos[0] == 1.5f &&
                got.map == 0x1d0a5a00u) {
                std::printf("%s: vitals from %s (slot %d)\n", own, other, got.slot);
                for (int k = 0; k < 8; ++k) {
                    BbRoom::SendVitals(v);
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

bool HasLine(BbRoom::PartyLineKind kind, const std::string& name) {
    for (const BbRoom::PartyLine& line : BbRoom::PartyLines())
        if (line.kind == kind && line.name == name) return true;
    return false;
}

bool WaitLine(BbRoom::PartyLineKind kind, const std::string& name, int seconds = 15) {
    for (int i = 0; i < seconds * 20; ++i) {
        if (HasLine(kind, name)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool FindPeer(const std::string& name, BbRoom::PeerStatus& out) {
    for (const BbRoom::PeerStatus& peer : BbRoom::Connections()) {
        if (peer.name != name) continue;
        out = peer;
        return true;
    }
    return false;
}

bool WaitPing(const std::string& name, BbRoom::PeerStatus& out) {
    for (int i = 0; i < 100; ++i) {
        if (FindPeer(name, out) && out.ping_ms >= 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

std::vector<std::uint8_t> OldVitals(const std::string& name) {
    std::vector<std::uint8_t> frame = {6, static_cast<std::uint8_t>(name.size())};
    frame.insert(frame.end(), name.begin(), name.end());
    frame.resize(frame.size() + 4 * 4 + 2 + 3 * 4 + 4);
    return frame;
}

int HostStatus() {
    BbRoom::PeerStatus guest;
    if (!WaitPing("GuestHunter", guest)) return Fail("no ping to the guest");
    if (guest.state != BbRoom::LinkState::Good) return Fail("the local guest's link should read good");
    std::printf("host: guest ping %d ms, good\n", guest.ping_ms);
    BbRoom::Link old;
    if (!old.Start(std::getenv("BB_ROOM_SERVER"), std::getenv("BB_ROOM_NAME"), std::getenv("BB_ROOM_PASSWORD")))
        return Fail("the older client did not start");
    const auto stop_at = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const std::vector<std::uint8_t> vitals = OldVitals("OldHunter");
    const std::uint8_t unknown[3] = {200, 1, 2};
    BbRoom::PeerStatus aged;
    while (std::chrono::steady_clock::now() < stop_at) {
        old.Send(0, vitals.data(), vitals.size());
        old.Send(0, unknown, sizeof unknown);
        old.Drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!FindPeer("OldHunter", aged)) return Fail("the older client is not listed");
    if (aged.state != BbRoom::LinkState::Unknown || aged.ping_ms != -1) return Fail("the older client should read unknown");
    std::printf("host: a client without the new frames stays in the room, ping unknown\n");
    const std::uint8_t ask[1] = {22};
    old.Send(old.HostSlot(), ask, sizeof ask);
    bool refused = false;
    for (int i = 0; i < 50 && !refused; ++i) {
        for (const BbRoom::Event& e : old.Drain()) {
            refused = refused || (e.kind == BbRoom::Event::Kind::Data && e.data.size() == 2 && e.data[0] == 23 && e.data[1] == BbRoom::kWarpRefused);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!refused) return Fail("a hunter outside the host's world should be refused");
    if (!FindPeer("OldHunter", aged) || aged.asked) return Fail("a refused request should not show");
    std::printf("host: a request from outside the world is refused\n");
    old.Stop();
    if (!WaitLine(BbRoom::PartyLineKind::Left, "OldHunter")) return Fail("no left line for the older client");
    if (FindPeer("OldHunter", aged)) return Fail("the older client is still listed");
    std::printf("host: left line for the older client\n");
    BbRoom::PeerStatus asked;
    for (int i = 0; i < 300; ++i) {
        if (FindPeer("GuestHunter", asked) && asked.asked) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!asked.asked) return Fail("the guest's request never came");
    if (!HasLine(BbRoom::PartyLineKind::WarpAsked, "GuestHunter")) return Fail("the host saw no request line");
    for (const bool yes : {false, true}) {
        int asker = 0;
        std::string who;
        std::uint32_t lamp = 0;
        for (int i = 0; i < 300 && !BbRoom::PendingLampAsk(asker, who, lamp); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (asker != asked.slot || lamp != 2412950) return Fail("the guest's lamp ask never came");
        BbRoom::AnswerLampAsk(yes);
        if (BbRoom::PendingLampAsk(asker, who, lamp)) return Fail("an answered lamp ask still waits");
    }
    std::printf("host: a guest's lamp ask refused, then accepted\n");
    if (!WaitLine(BbRoom::PartyLineKind::Died, "GuestHunter")) return Fail("no death line for the guest");
    std::printf("host: request and death lines from the guest\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    BbRoom::SendBring(asked.slot);
    BbRoom::SetWarpRequests(false);
    if (!FindPeer("GuestHunter", asked) || asked.asked) return Fail("Bring did not clear the request");
    std::printf("host: Bring accepted the request\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    if (!FindPeer("GuestHunter", asked) || asked.asked) return Fail("a request with requests off should not show");
    if (!BbRoom::Kick(asked.slot)) return Fail("the kick failed");
    if (!WaitLine(BbRoom::PartyLineKind::Kicked, "GuestHunter")) return Fail("no kicked line");
    std::printf("host: kicked line\n");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 0;
}

int GuestStatus() {
    BbRoom::PeerStatus host;
    if (!WaitPing("HostHunter", host)) return Fail("no ping to the host");
    std::printf("guest: host ping %d ms\n", host.ping_ms);
    if (BbRoom::WarpWaiting()) return Fail("waiting before asking");
    for (int i = 0; i < 300 && !HasLine(BbRoom::PartyLineKind::Left, "OldHunter"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BbRoom::SendWarpAsk();
    if (!BbRoom::WarpWaiting()) return Fail("not waiting after asking");
    if (!WaitLine(BbRoom::PartyLineKind::WarpSeen, "")) return Fail("the host did not answer the request");
    if (BbRoom::TakeBring()) return Fail("moved before the host accepted");
    BbRoom::SendLampAsk(2412950);
    if (!WaitLine(BbRoom::PartyLineKind::LampSeen, "")) return Fail("the host did not take the lamp ask");
    if (!WaitLine(BbRoom::PartyLineKind::LampRefused, "")) return Fail("the lamp ask's refusal never came");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    BbRoom::SendLampAsk(2412950);
    if (!WaitLine(BbRoom::PartyLineKind::LampAgreed, "")) return Fail("the lamp ask's acceptance never came");
    std::printf("guest: lamp ask refused, then accepted\n");
    BbRoom::Vitals v;
    v.name = "GuestHunter";
    v.hp_max = 800;
    v.map = 0x1d0a5a00u;
    v.hp = 500;
    BbRoom::SendVitals(v);
    v.hp = 0;
    BbRoom::SendVitals(v);
    bool brought = false;
    for (int i = 0; i < 300 && !brought; ++i) {
        brought = BbRoom::TakeBring();
        if (!brought) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!brought) return Fail("the host's Bring never came");
    if (BbRoom::WarpWaiting()) return Fail("still waiting after being brought");
    std::printf("guest: asked, answered, brought only after the host's Bring\n");
    BbRoom::SendWarpAsk();
    if (!WaitLine(BbRoom::PartyLineKind::WarpOff, "")) return Fail("a host with requests off should refuse");
    if (BbRoom::WarpWaiting()) return Fail("still waiting after a refusal");
    std::printf("guest: refused by a host with requests off\n");
    Json ev;
    if (!Poll(ev, "room_member_kicked", 30)) return Fail("no kick event");
    return 0;
}

constexpr std::uint64_t kEnteredDungeon = 0x8000000012345678ull;

Json FixedSearch() {
    return {{"HolyGrailTypeId", 0}, {"RitualLevel", 1}, {"SubFeatureFlag", 0}, {"FormDataVersion", 0}, {"Status", 1},
            {"FixedOrGeneralList", Json::array({{{"FixedOrGeneral", 2}}})}, {"GetCount", 1}};
}

std::string Sign() {
    return std::string(299, 'A') + "=";
}

int Host(int game_port, int http_port) {
    const int udp = OpenUdp(game_port);
    BbRoom::Start(game_port);
    Json reply;
    std::string error;
    if (!BbRoom::Call("/mp/matching2/context_start", {{"OnlineId", "HostHunter"}}, reply, error, 8000)) return Fail("host context_start");
    if (!BbRoom::Call("/mp/matching2/create_room", {{"OnlineId", "HostHunter"}, {"MaxMembers", 4}}, reply, error, 8000) ||
        reply.value("ResKind", -1) != 0)
        return Fail("create_room");
    if (reply.value("MaxMembers", 0) != 5) return Fail("the room should hold the host and four cooperators");
    const long long room = reply.value("RoomId", 0LL);
    std::printf("host: room %lld for 5 hunters\n", room);
    Json list;
    for (int i = 0; i < 60; ++i) {
        list = Game(http_port, "/summon_messenger/get", {{"SummonTypeList", Json::array({{{"SummonType", 7}}})}});
        if (list.is_object() && list["SummonDataList"].size() == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!list.is_object() || list["SummonDataList"].size() != 1) return Fail("the guest's sign never showed up");
    const long long user = list["SummonDataList"][0].value("UserId", 0LL);
    std::printf("host: sign from user %lld\n", user);
    if (!BbRoom::Call("/np/signaling/resolve", {{"OnlineId", "GuestHunter"}}, reply, error, 8000) ||
        reply.value("ResKind", -1) != 0)
        return Fail("the host cannot find the guest by name");
    std::printf("host: guest found by name\n");
    if (!BbRoom::Call("/np/signaling/resolve", {{"OnlineId", "GuestHunter-ab12"}}, reply, error, 8000) ||
        reply.value("ResKind", -1) != 0)
        return Fail("the host cannot find a guest introduced by its room name from its online id");
    if (!BbRoom::Call("/np/signaling/resolve", {{"OnlineId", "GuestHunterX-ab12"}}, reply, error, 8000) ||
        reply.value("ResKind", -1) == 0)
        return Fail("a longer name must not match");
    Game(http_port, "/summon_messenger/request", {{"TargetUserId", user}});
    Json ev;
    if (!Poll(ev, "room_member_joined")) return Fail("host saw no member join");
    std::printf("host: member %lld joined at %s:%lld\n", ev.value("MemberId", 0LL), ev.value("Addr", "").c_str(),
                ev.value("Port", 0LL));
    char buffer[64];
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const int n = ::recvfrom(udp, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n != 5 || std::string(buffer, 5) != "hello") return Fail("no P2P datagram from the guest");
    if (ntohs(from.sin_port) != ev.value("Port", 0LL)) return Fail("datagram came from another port than the member's");
    ::sendto(udp, "back!", 5, 0, reinterpret_cast<sockaddr*>(&from), len);
    std::printf("host: P2P ok\n");
    {
        std::vector<char> big(65536);
        bool signal = false, largest = false;
        for (int i = 0; i < 40 && !(signal && largest); ++i) {
            const int got = ::recvfrom(udp, big.data(), static_cast<int>(big.size()), 0, nullptr, nullptr);
            if (got <= 0) break;
            signal = signal || (got == 8 && std::memcmp(big.data(), "\xfe" "bbs" "sig!", 8) == 0);
            largest = largest || (got == 65507 && big[0] == 'L' && big[65506] == 'L');
        }
        if (!signal) return Fail("no signaling datagram from the guest");
        if (!largest) return Fail("no 65507-byte datagram from the guest");
        std::printf("host: signaling datagram and a 65507-byte datagram arrived\n");
    }
    if (!SwapVitals("HostHunter", "GuestHunter")) return Fail("no vitals from the guest");
    const Json found = Game(http_port, "/channel/search", FixedSearch());
    if (!found.is_object() || found["ChannelList"].size() != 1) return Fail("no fixed dungeon on the board");
    const auto channel = static_cast<std::uint64_t>(found["ChannelList"][0].value("ChannelId", 0LL));
    BbRoom::PutDungeonForm(channel, std::vector<std::uint8_t>(0x7c, 0x5a));
    BbRoom::PutDungeonForm(kEnteredDungeon, std::vector<std::uint8_t>(0x7c, 0x33));
    std::printf("host: fixed dungeon %llu, its layout on the board\n", static_cast<unsigned long long>(channel));
    BbRoom::Dungeon dungeon;
    dungeon.channel = channel;
    dungeon.record[3] = 0x1d0a5a00u;
    dungeon.flags = {12900166u, 72900202u};
    for (int i = 0; i < 10; ++i) {
        BbRoom::SendTravel(2412951, {0x49, 0x4e, 0x46, 0x45});
        BbRoom::SendStageTravel(0x18020000u, 2412345);
        BbRoom::SendWorldFlags({1, 2, 3});
        std::vector<std::uint32_t> flags = {12411800u, 72410201u, 1062u | 0x80000000u};
        for (std::uint32_t id = 0; flags.size() < 3600; ++id) flags.push_back(12400000u + id);
        BbRoom::SendProgress(flags);
        BbRoom::SendDungeon(dungeon);
        BbRoom::SendChalices({6109u, 6209u});
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (const int status = HostStatus()) return status;
    std::printf("HOST OK\n");
    return 0;
}

int Guest(int game_port, int http_port, bool early) {
    const int udp = OpenUdp(game_port);
    BbRoom::Start(game_port);
    Json reply;
    std::string error;
    const bool started = BbRoom::Call("/mp/matching2/context_start", {{"OnlineId", "GuestHunter"}}, reply, error, early ? 2000 : 15000);
    if (!started && !early) return Fail("guest context_start");
    if (early) std::printf("guest: context_start before the host: %s\n", started ? "answered" : error.c_str());
    const Json login = Game(http_port, "/basic_utils/login", Json::object());
    if (!login.is_object() || login.value("ResKind", -1) != 0) return Fail("login");
    Json created;
    for (int i = 0; i < 12; ++i) {
        created = Game(http_port, "/summon_messenger/create", {{"SummonData", Sign()}, {"SummonType", 7}, {"CharaId", 2}});
        if (created.is_object() && created.value("ResKind", -1) == 0) break;
    }
    if (!created.is_object() || created.value("ResKind", -1) != 0) return Fail("sign create");
    Json ev;
    if (!Poll(ev, "guest_invite")) return Fail("no invite");
    const long long room = ev.value("RoomId", 0LL);
    std::printf("guest: invited to room %lld by %s at %s:%lld\n", room, ev.value("HostOnlineId", "").c_str(),
                ev.value("HostAddr", "").c_str(), ev.value("HostPort", 0LL));
    if (!BbRoom::Call("/mp/matching2/join_room", {{"OnlineId", "GuestHunter"}, {"RoomId", room}}, reply, error, 8000) ||
        reply.value("ResKind", -1) != 0)
        return Fail("join_room");
    std::printf("guest: member %lld, %zu members\n", reply.value("MemberId", 0LL), reply["Members"].size());
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(static_cast<std::uint16_t>(ev.value("HostPort", 0LL)));
    char buffer[64];
    for (int i = 0; i < 5; ++i) {
        ::sendto(udp, "hello", 5, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        const int n = ::recvfrom(udp, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n == 5 && std::string(buffer, 5) == "back!") {
            ::sendto(udp, "\xfe" "bbs" "sig!", 8, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
            const std::vector<char> largest(65507, 'L');
            for (int k = 0; k < 3; ++k) {
                ::sendto(udp, largest.data(), static_cast<int>(largest.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!SwapVitals("GuestHunter", "HostHunter")) return Fail("no vitals from the host");
            std::uint32_t lamp = 0;
            std::vector<std::uint8_t> world_flags;
            for (int k = 0; k < 40 && !BbRoom::TakeTravel(lamp, &world_flags); ++k)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (lamp != 2412951) return Fail("no travel from the host");
            if (world_flags.size() != 4 || world_flags[0] != 0x49) return Fail("no world flags with the travel");
            std::printf("guest: the host travels to lamp %u\n", lamp);
            std::uint32_t stage_map = 0;
            std::int32_t stage_point = -1;
            for (int k = 0; k < 40 && !BbRoom::TakeStageTravel(stage_map, stage_point); ++k)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (stage_map != 0x18020000u || stage_point != 2412345) return Fail("no stage travel from the host");
            std::vector<std::vector<std::uint8_t>> world_parts;
            for (int k = 0; k < 40 && world_parts.empty(); ++k) {
                world_parts = BbRoom::TakeWorldFlags();
                if (world_parts.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (world_parts.empty() || world_parts[0] != std::vector<std::uint8_t>{1, 2, 3}) return Fail("no world flags from the host");
            std::printf("guest: the host goes to map %08x, %zu world flag part(s)\n", stage_map, world_parts.size());
            std::vector<std::uint32_t> flags;
            for (int k = 0; k < 40 && flags.empty(); ++k) {
                flags = BbRoom::TakeProgress();
                if (flags.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (flags.size() < 3600 || flags[0] != 12411800u || flags[1] != 72410201u || flags[2] != (1062u | 0x80000000u)) return Fail("no progress from the host");
            std::printf("guest: the host's progress: %zu flag(s), first %u\n", flags.size(), flags[0]);
            const Json found = Game(http_port, "/channel/search", FixedSearch());
            if (!found.is_object() || found["ChannelList"].size() != 1) return Fail("no fixed dungeon for the guest");
            const long long channel = found["ChannelList"][0].value("ChannelId", 0LL);
            Json details;
            for (int k = 0; k < 20; ++k) {
                details = Game(http_port, "/channel/get_details_info", {{"ChannelIdList", Json::array({{{"ChannelId", channel}}})}});
                if (details.is_object() && details["ChannelList"].size() == 1 && details["ChannelList"][0].contains("FormData")) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (!details.is_object() || details["ChannelList"].size() != 1 ||
                details["ChannelList"][0].value("FormData", "").size() != 168)
                return Fail("the host's dungeon layout did not reach the guest");
            std::printf("guest: the host's dungeon %lld with its layout\n", channel);
            Json entered;
            for (int k = 0; k < 20; ++k) {
                entered = Game(http_port, "/channel/get_details_info",
                               {{"ChannelIdList", Json::array({{{"ChannelId", kEnteredDungeon}}})}});
                if (entered.is_object() && entered["ChannelList"].size() == 1) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (!entered.is_object() || entered["ChannelList"].size() != 1 ||
                entered["ChannelList"][0].value("ChannelId", std::uint64_t(0)) != kEnteredDungeon ||
                entered["ChannelList"][0].value("FormData", "").size() != 168)
                return Fail("a dungeon the host entered without a ritual did not reach the guest");
            std::printf("guest: the host's entered dungeon with its layout too\n");
            BbRoom::Dungeon dungeon;
            for (int k = 0; k < 40 && !BbRoom::TakeDungeon(dungeon); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (static_cast<long long>(dungeon.channel) != channel || dungeon.record[3] != 0x1d0a5a00u || dungeon.flags.size() != 2)
                return Fail("no dungeon progress from the host");
            std::vector<std::uint32_t> chalices;
            for (int k = 0; k < 40 && !BbRoom::TakeChalices(chalices); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (chalices.size() != 2 || chalices[0] != 6109u) return Fail("no chalices from the host");
            std::printf("guest: dungeon progress and %zu chalice(s) from the host\n", chalices.size());
            if (const int status = GuestStatus()) return status;
            std::printf("GUEST OK\n");
            return 0;
        }
    }
    return Fail("no P2P answer from the host");
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) return Fail("usage: room-service-test host|guest|early-guest");
    const bool host = std::string(argv[1]) == "host";
    const int game_port = host ? 47658 : 48658;
    const int http_port = game_port + 1;
    const int result = host ? Host(game_port, http_port) : Guest(game_port, http_port, std::string(argv[1]) == "early-guest");
    std::fflush(stdout);
    std::_Exit(result);
}
