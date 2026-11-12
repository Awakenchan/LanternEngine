// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <functional>
#include <memory>
#include <cstdio>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "gpu/bbnet/room/room_crypto.h"
#include "gpu/bbnet/room/room_host.h"
#include "gpu/bbnet/room/room_link.h"
#include "gpu/bbnet/room/room_ws.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace BbRoom;

namespace {

int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool Await(Link& link, Event::Kind kind, Event* out = nullptr, int timeout_ms = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        link.WaitEvent(50);
        for (auto& e : link.Drain()) {
            if (e.kind == kind) {
                if (out) *out = std::move(e);
                return true;
            }
        }
    }
    return false;
}

bool AwaitData(Link& link, const std::vector<std::uint8_t>& wanted, int from) {
    Event e;
    return Await(link, Event::Kind::Data, &e) && e.data == wanted && e.slot == from;
}

std::string Text(const Event& e) { return std::string(e.data.begin(), e.data.end()); }

#ifdef _WIN32
using RawSock = SOCKET;
void CloseRaw(RawSock s) { closesocket(s); }
#else
using RawSock = int;
void CloseRaw(RawSock s) { ::close(s); }
#endif

std::string Exchange(int port, const std::string& request) {
    const RawSock s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(static_cast<std::uint16_t>(port));
    std::string answer;
    if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
        ::send(s, request.data(), static_cast<int>(request.size()), 0);
        char buffer[1024];
        for (;;) {
            const int n = ::recv(s, buffer, sizeof buffer, 0);
            if (n <= 0) break;
            answer.append(buffer, static_cast<std::size_t>(n));
        }
    }
    CloseRaw(s);
    return answer;
}

void OtherVersionServer(int port, int protocol, std::atomic<bool>& ready) {
    const RawSock listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifndef _WIN32
    const int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    at.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(listener, reinterpret_cast<sockaddr*>(&at), sizeof at) != 0 || ::listen(listener, 1) != 0) {
        std::fprintf(stderr, "version-test listener could not open port %d\n", port);
        CloseRaw(listener);
        return;
    }
    ready = true;
    fd_set pending;
    FD_ZERO(&pending);
    FD_SET(listener, &pending);
    timeval wait{10, 0};
    if (::select(static_cast<int>(listener + 1), &pending, nullptr, nullptr, &wait) <= 0) {
        CloseRaw(listener);
        return;
    }
    const RawSock peer = ::accept(listener, nullptr, nullptr);
    std::string head;
    char buffer[1024];
    while (head.find("\r\n\r\n") == std::string::npos) {
        const int n = ::recv(peer, buffer, sizeof buffer, 0);
        if (n <= 0) break;
        head.append(buffer, static_cast<std::size_t>(n));
    }
    const std::string answer = "HTTP/1.1 426 Upgrade Required\r\nX-Lantern-Room: " + std::to_string(protocol) + "\r\nConnection: close\r\n\r\n";
    ::send(peer, answer.data(), static_cast<int>(answer.size()), 0);
    CloseRaw(peer);
    CloseRaw(listener);
}

std::vector<Event> Gather(Link& link, int ms) {
    std::vector<Event> out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        link.WaitEvent(20);
        for (auto& e : link.Drain()) out.push_back(std::move(e));
    }
    return out;
}

bool Has(const std::vector<Event>& events, Event::Kind kind, int slot) {
    return std::any_of(events.begin(), events.end(), [&](const Event& e) { return e.kind == kind && e.slot == slot; });
}

std::size_t Where(const std::vector<Event>& events, Event::Kind kind, int slot) {
    for (std::size_t i = 0; i < events.size(); ++i)
        if (events[i].kind == kind && events[i].slot == slot) return i;
    return events.size();
}

bool Waits(Link& link, int ms = 3000) {
    for (int i = 0; i < ms / 20 && !link.Connected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return link.Connected();
}

bool Contains(const std::vector<int>& slots, int slot) { return std::find(slots.begin(), slots.end(), slot) != slots.end(); }

struct Member {
    std::unique_ptr<WebSocket> socket = std::make_unique<WebSocket>();
    std::thread run;
    std::atomic<int> slot{0};
    std::atomic<bool> full{false};

    bool Join(const std::string& url) {
        std::string error;
        if (!socket->Connect(url, 5, error)) return false;
        run = std::thread([this] {
            socket->Run([this](std::string&& message, bool binary) {
                if (binary) return;
                const auto json = nlohmann::json::parse(message, nullptr, false);
                if (!json.is_object()) return;
                if (json.value("t", "") == "welcome") slot = json.value("slot", 0);
                if (json.value("t", "") == "full") full = true;
            });
        });
        for (int i = 0; i < 150 && !slot.load() && !full.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return slot.load() != 0;
    }

    void Drop() {
        socket->Close();
        if (run.joinable()) run.join();
        socket = std::make_unique<WebSocket>();
        slot = 0;
        full = false;
    }
};

void TestDroppedHunters() {
    const std::string name = "dropped-room", password = "secret";
    const int port = 38767;
    const std::string server = "ws://127.0.0.1:" + std::to_string(port);
    const std::string base = RoomUrl(server, Derive(name, password).room_id) + "/";
    LocalRoom room;
    std::string error;
    Check(room.Start(port, Derive(name, password).room_id, error), "the dropped-hunter room opens");
    Link host, first;
    host.SetHost(true);
    host.Start(server, name, password);
    Check(Waits(host) && host.Slot() == 1, "the host is in slot 1");
    first.Start(server, name, password);
    Check(Waits(first) && first.Slot() == 2, "a guest is in slot 2");
    Member third, fourth;
    Check(third.Join(base + std::string(32, 'c')) && third.slot.load() == 3, "a third hunter is in slot 3");
    Check(fourth.Join(base + std::string(32, 'd')) && fourth.slot.load() == 4, "a fourth hunter is in slot 4: four in the room");
    Gather(host, 300);
    Gather(first, 100);

    third.Drop();
    fourth.Drop();
    const auto dropped = Gather(host, 1000);
    Check(Has(dropped, Event::Kind::Away, 3) && Has(dropped, Event::Kind::Away, 4), "two hunters drop without a leave and the room says they are away");
    Check(!Has(dropped, Event::Kind::Left, 3) && !Has(dropped, Event::Kind::Left, 4), "their slots are held, not freed");

    Link fifth, sixth;
    fifth.Start(server, name, password);
    Check(Waits(fifth) && fifth.Slot() == 5, "the first newcomer takes the free slot");
    Check(!Contains(fifth.Members(), 3) && !Contains(fifth.Members(), 4), "held slots are not listed to a newcomer");
    sixth.Start(server, name, password);
    Check(Waits(sixth), "the second newcomer gets in although every slot was taken or held");
    const int given = sixth.Slot();
    Check(given == 3, "it gets the slot held longest");
    const auto handed = Gather(host, 800);
    Check(Where(handed, Event::Kind::Left, 3) < Where(handed, Event::Kind::Joined, 3) && Where(handed, Event::Kind::Joined, 3) < handed.size(),
          "the others hear a leave for that slot before the newcomer's join");
    const auto staying5 = Gather(fifth, 1500);
    const auto staying6 = Gather(sixth, 10);
    Check(fifth.Connected() && sixth.Connected() && fifth.Slot() == 5 && sixth.Slot() == 3, "both newcomers stay");
    Check(!Has(staying5, Event::Kind::Disconnected, 0) && !Has(staying5, Event::Kind::Refused, 0) &&
              !Has(staying6, Event::Kind::Disconnected, 0) && !Has(staying6, Event::Kind::Refused, 0),
          "neither newcomer is closed or refused");

    Check(fourth.Join(base + std::string(32, 'd')) && fourth.slot.load() == 4, "a dropped hunter back within its hold gets its own slot again");
    const auto returned = Gather(host, 500);
    Check(Has(returned, Event::Kind::Joined, 4), "the others hear it join again");
    Member stray;
    stray.Join(base + std::string(32, 'c'));
    Check(stray.slot.load() == 0 && stray.full.load(), "a hunter whose held slot went to a newcomer finds the room full");

    stray.Drop();
    const std::string leave = R"({"t":"leave"})";
    fourth.socket->Send(leave.data(), leave.size(), false);
    fourth.Drop();
    sixth.Stop();
    fifth.Stop();
    first.Stop();
    host.Stop();
    room.Stop();
}

void TestSevenHunters() {
    const std::string name = "seven-room", password = "secret";
    const int port = 38768;
    const std::string server = "ws://127.0.0.1:" + std::to_string(port);
    LocalRoom room;
    std::string error;
    Check(room.Start(port, Derive(name, password).room_id, error, 7), "a room for seven opens");
    std::vector<std::unique_ptr<Link>> hunters;
    for (int i = 0; i < 7; ++i) {
        hunters.push_back(std::make_unique<Link>());
        hunters.back()->Start(server, name, password);
        Check(Waits(*hunters.back()) && hunters.back()->Slot() == i + 1, "each of seven hunters gets the next slot");
    }
    Link eighth;
    eighth.Start(server, name, password);
    Event refused;
    Check(Await(eighth, Event::Kind::Refused, &refused) && Text(refused) == "This room is full (7 of 7 hunters)", "the eighth is refused with the room's own limit");
    eighth.Stop();
    hunters.clear();
    room.Stop();
}

} // namespace

int main() {
#ifndef _WIN32
    // Match the standalone relay: a peer closing a socket is not a fatal signal.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    const std::string name = "direct-room", password = "secret";
    const int port = 38765;
    const std::string server = "ws://127.0.0.1:" + std::to_string(port);
    LocalRoom room;
    std::string error;
    Check(room.Start(port, Derive(name, password).room_id, error), "the room opens its port");
    LocalRoom second;
    Check(!second.Start(port, Derive(name, password).room_id, error) && !error.empty(), "a port in use is refused with a reason");

    Link host, guest, other;
    host.SetHost(true);
    host.Start(server, name, password);
    Check(Await(host, Event::Kind::Connected), "the host is welcomed");
    Check(host.Slot() == 1, "the host gets slot 1");
    guest.Start(server, name, password);
    Check(Await(guest, Event::Kind::Connected), "a guest is welcomed");
    Check(guest.Slot() == 2, "the guest gets slot 2");
    Check(Await(host, Event::Kind::Joined), "the host sees the guest join");
    Check(guest.HostSlot() == 1, "the guest learns the host's slot");
    other.Start(server, name, password);
    Check(Await(other, Event::Kind::Connected) && other.Slot() == 3, "a third hunter is welcomed");
    Await(host, Event::Kind::Joined, nullptr, 500);
    Await(guest, Event::Kind::Joined, nullptr, 500);

    const std::vector<std::uint8_t> small{1, 2, 3};
    Check(guest.Send(host.Slot(), small.data(), small.size()), "a guest can send");
    Check(AwaitData(host, small, guest.Slot()), "a frame reaches the slot it names, from the sender's slot");
    Check(!Await(other, Event::Kind::Data, nullptr, 300), "a targeted frame reaches nobody else");
    Check(host.Send(0, small.data(), small.size()), "the host can send to all");
    Check(AwaitData(guest, small, host.Slot()) && AwaitData(other, small, host.Slot()), "a frame for all reaches everyone");
    Check(!Await(host, Event::Kind::Data, nullptr, 300), "a sender does not get its own frame");
    const std::vector<std::uint8_t> big(60000, 7);
    Check(guest.Send(host.Slot(), big.data(), big.size()), "a large frame can be sent");
    Check(AwaitData(host, big, guest.Slot()), "a large frame arrives whole");
    const std::vector<std::uint8_t> biggest(1 + 1 + 4 + 2 + 65507, 9);
    Check(guest.SendGame(host.Slot(), biggest.data(), biggest.size()), "a bundle holding the largest datagram can be sent");
    Check(AwaitData(host, biggest, guest.Slot()), "the room passes a bundle holding the largest datagram");

    Check(guest.SendGame(host.Slot(), small.data(), small.size()), "game packets can be sent");
    Check(AwaitData(host, small, guest.Slot()), "a droppable frame still reaches the slot it names");
    Check(!Await(other, Event::Kind::Data, nullptr, 300), "and nobody else");

    const std::string member = std::string(32, 'a');
    const std::string own_url = RoomUrl(server, Derive(name, password).room_id) + "/" + member;
    std::atomic<int> first_slot{0}, next_slot{0};
    WebSocket original, replacement;
    Check(original.Connect(own_url, 5, error), "a member connects with its token");
    std::thread original_run([&] {
        original.Run([&](std::string&& message, bool binary) {
            if (!binary && message.find("welcome") != std::string::npos) first_slot = nlohmann::json::parse(message).value("slot", 0);
        });
    });
    Await(host, Event::Kind::Joined, nullptr, 1000);
    Await(guest, Event::Kind::Joined, nullptr, 200);
    Await(other, Event::Kind::Joined, nullptr, 200);
    Check(replacement.Connect(own_url, 5, error), "the same member connects again before the room saw it drop");
    std::thread replacement_run([&] {
        replacement.Run([&](std::string&& message, bool binary) {
            if (!binary && message.find("welcome") != std::string::npos) next_slot = nlohmann::json::parse(message).value("slot", 0);
        });
    });
    for (int i = 0; i < 100 && !next_slot.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(first_slot.load() && first_slot.load() == next_slot.load(), "it keeps its slot");
    Check(!Await(host, Event::Kind::Joined, nullptr, 300), "and the others see no new member");
    original_run.join();
    const std::string leave = R"({"t":"leave"})";
    replacement.Send(leave.data(), leave.size(), false);
    replacement.Close();
    replacement_run.join();
    Check(Await(host, Event::Kind::Left, nullptr, 2000), "a clean leave after the takeover frees the slot");
    Await(guest, Event::Kind::Left, nullptr, 500);
    Await(other, Event::Kind::Left, nullptr, 500);

    Link wrong;
    wrong.Start(server, name, "not the password");
    Check(!Await(wrong, Event::Kind::Connected, nullptr, 1500) && !wrong.Connected(), "a wrong password is never let in");
    wrong.Stop();

    Link voice_a, voice_b;
    voice_a.Start(server, name, password, "voice");
    voice_b.Start(server, name, password, "voice");
    Check(Await(voice_a, Event::Kind::Connected) && Await(voice_b, Event::Kind::Connected), "voice members are welcomed");
    Await(voice_a, Event::Kind::Joined, nullptr, 200);
    Check(voice_a.SendLossy(voice_b.Slot(), small.data(), small.size(), 2048), "voice can send");
    Check(AwaitData(voice_b, small, voice_a.Slot()), "voice reaches a voice member");
    Check(!Await(host, Event::Kind::Data, nullptr, 200), "voice stays out of the game room");
    voice_a.Stop();
    voice_b.Stop();

    Check(FullRefusal() == "This room is full (5 of 5 hunters)", "the full room's line names the real limit");
    Link fourth, sixth;
    fourth.Start(server, name, password);
    Check(Await(fourth, Event::Kind::Connected), "a fourth hunter is welcomed");
    const std::string fifth_url = RoomUrl(server, Derive(name, password).room_id) + "/" + std::string(32, 'b');
    std::atomic<int> fifth_slot{0}, back_slot{0};
    std::atomic<bool> back_refused{false};
    WebSocket fifth;
    Check(fifth.Connect(fifth_url, 5, error), "a fifth hunter connects with its token");
    std::thread fifth_run([&] {
        fifth.Run([&](std::string&& message, bool binary) {
            if (!binary && message.find("welcome") != std::string::npos) fifth_slot = nlohmann::json::parse(message).value("slot", 0);
        });
    });
    for (int i = 0; i < 100 && !fifth_slot.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(fifth_slot.load() != 0, "the room holds five");
    sixth.Start(server, name, password);
    Event refused;
    Check(Await(sixth, Event::Kind::Refused, &refused) && Text(refused) == "This room is full (5 of 5 hunters)",
          "the sixth hunter is refused as the room is full");
    WebSocket back;
    Check(back.Connect(fifth_url, 5, error), "the fifth hunter reconnects while the room is full");
    std::thread back_run([&] {
        back.Run([&](std::string&& message, bool binary) {
            if (binary) return;
            if (message.find("welcome") != std::string::npos) back_slot = nlohmann::json::parse(message).value("slot", 0);
            if (message.find("full") != std::string::npos) back_refused = true;
        });
    });
    for (int i = 0; i < 100 && !back_slot.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(back_slot.load() == fifth_slot.load() && !back_refused.load() && back.CloseCode() == 0, "a member coming back to a full room keeps its slot");
    fifth_run.join();
    sixth.Stop();
    const std::string fifth_leave = R"({"t":"leave"})";
    back.Send(fifth_leave.data(), fifth_leave.size(), false);
    back.Close();
    back_run.join();
    fourth.Stop();

    const std::string old_ask = "GET /room/" + Derive(name, password).room_id +
                                " HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nX-Lantern-Room: ";
    const std::string old_answer = Exchange(port, old_ask + std::to_string(kRoomProtocol - 1) + "\r\n\r\n");
    Check(old_answer.rfind("HTTP/1.1 426", 0) == 0 && old_answer.find("X-Lantern-Room: " + std::to_string(kRoomProtocol) + "\r\n") != std::string::npos,
          "an older Lantern is refused and told the room's protocol");
    Check(Exchange(port, old_ask + std::to_string(kRoomProtocol + 1) + "\r\n\r\n").rfind("HTTP/1.1 426", 0) == 0, "a newer Lantern is refused too");
    Check(kRoomProtocol == 17, "the room protocol is 17");
    Check(VersionRefusal(17, 16).rfind("This room runs a different Lantern version (protocol 17, yours 16): everyone in a seamless room needs the same Lantern version", 0) == 0 &&
              VersionRefusal(17, 16).find("update your Lantern") != std::string::npos &&
              VersionRefusal(16, 17).find("the host has to update") != std::string::npos,
          "the version line says which side has to update");
    const int other_port = port + 1;
    std::atomic<bool> other_ready{false};
    std::thread other_server(OtherVersionServer, other_port, kRoomProtocol + 1, std::ref(other_ready));
    for (int i = 0; i < 100 && !other_ready.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(other_ready.load(), "the version-test listener opens its port");
    Link outdated;
    outdated.Start("ws://127.0.0.1:" + std::to_string(other_port), name, password);
    Event version;
    Check(Await(outdated, Event::Kind::Refused, &version) && Text(version) == VersionRefusal(kRoomProtocol + 1, kRoomProtocol),
          "a room on another version comes back as one clear refusal");
    outdated.Stop();
    other_server.join();

    other.Stop();
    Check(Await(guest, Event::Kind::Left), "a clean leave reaches the others");
    guest.Stop();
    Check(Await(host, Event::Kind::Left), "a clean leave reaches the host");
    host.Stop();
    room.Stop();
    LocalRoom again;
    Check(again.Start(port, Derive(name, password).room_id, error), "the port opens again after the room stops");
    again.Stop();

    TestDroppedHunters();
    TestSevenHunters();

    if (failures) return 1;
    std::printf("room-host-test: ok\n");
    return 0;
}
