// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "gpu/bbnet/room/room_crypto.h"
#include "gpu/bbnet/room/room_link.h"

using namespace BbRoom;

namespace {

void TestDerive() {
    const auto a = *Derive("hunters", "blood", 1000);
    const auto b = *Derive("hunters", "blood", 1000);
    const auto c = *Derive("hunters", "bloods", 1000);
    const auto d = *Derive("hunter", "sblood", 1000);
    assert(a.room_id.size() == 64);
    assert(a.room_id == b.room_id && a.key == b.key);
    assert(a.room_id != c.room_id && a.key != c.key);
    assert(a.room_id != d.room_id);
    for (const char ch : a.room_id) assert((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
    assert(Derive("hunters", "blood")->room_id == "4344e16951b4fdc22c6d41ddc47bd7e6dd67aa9fe320abe75e253f081bd62f56");
}

void TestSeal() {
    const auto secret = *Derive("room", "pass", 1000);
    const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5};
    const auto sealed = Seal(secret.key, 2, payload.data(), payload.size());
    assert(sealed.size() == payload.size() + kSealOverhead);
    const auto opened = Open(secret.key, 2, sealed.data(), sealed.size());
    assert(opened && *opened == payload);
    assert(!Open(secret.key, 3, sealed.data(), sealed.size()));
    auto tampered = sealed;
    tampered[kNonceLen + 2] ^= 1;
    assert(!Open(secret.key, 2, tampered.data(), tampered.size()));
    const auto other = *Derive("room", "other", 1000);
    assert(!Open(other.key, 2, sealed.data(), sealed.size()));
    const auto empty = Seal(secret.key, 1, nullptr, 0);
    const auto opened_empty = Open(secret.key, 1, empty.data(), empty.size());
    assert(opened_empty && opened_empty->empty());
    const auto again = Seal(secret.key, 2, payload.data(), payload.size());
    assert(again != sealed);
}

void TestUrl() {
    assert(RoomUrl("https://a.example.dev/", "ab") == "wss://a.example.dev/room/ab");
    assert(RoomUrl("http://127.0.0.1:8787", "ab") == "ws://127.0.0.1:8787/room/ab");
    assert(RoomUrl("a.example.dev", "ab") == "wss://a.example.dev/room/ab");
}

bool Await(Link& link, Event::Kind kind, Event* out, int timeout_ms = 15000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        link.WaitEvent(200);
        for (auto& e : link.Drain()) {
            if (e.kind == kind) {
                if (out) *out = std::move(e);
                return true;
            }
        }
    }
    return false;
}

bool AwaitAll(Link& link, std::vector<Event::Kind> kinds, int timeout_ms = 15000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!kinds.empty() && std::chrono::steady_clock::now() < end) {
        link.WaitEvent(200);
        for (const auto& e : link.Drain()) kinds.erase(std::remove(kinds.begin(), kinds.end(), e.kind), kinds.end());
    }
    return kinds.empty();
}

bool Until(const std::function<bool()>& check, int timeout_ms = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (check()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return check();
}

int TestVisitors(const char* server) {
    const std::string name = "lantern-open-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    Link host, shut;
    host.Start(server, name, "secret");
    Event e;
    if (!Await(host, Event::Kind::Connected, &e)) return 1;
    host.SetHost(true);
    shut.SetVisitor(true);
    shut.Start(server, name, "secret");
    if (Await(shut, Event::Kind::Connected, &e, 1500)) {
        std::printf("live: a visitor joined a room that did not opt in\n");
        return 1;
    }
    shut.Stop();
    host.SetOpen(true);
    if (!Until([&] { return host.OpenListed(); })) {
        std::printf("live: the relay did not list the room\n");
        return 1;
    }
    Link friend_link;
    friend_link.Start(server, name, "secret");
    if (!Await(friend_link, Event::Kind::Connected, &e)) return 1;
    Link visitor;
    visitor.SetVisitor(true);
    visitor.Start(server, name, "secret");
    if (!AwaitAll(visitor, {Event::Kind::Connected, Event::Kind::Announcement})) {
        std::printf("live: the visitor got no welcome or no line\n");
        return 1;
    }
    const int slot = visitor.Slot();
    if (!visitor.IsVisitor(slot)) {
        std::printf("live: the visitor is not tagged\n");
        return 1;
    }
    if (!AwaitAll(host, {Event::Kind::Visitor, Event::Kind::Announcement}) || !host.IsVisitor(slot)) {
        std::printf("live: the host was not told of the visitor\n");
        return 1;
    }
    if (!AwaitAll(friend_link, {Event::Kind::Visitor, Event::Kind::Announcement}) || !friend_link.IsVisitor(slot)) {
        std::printf("live: the guest did not see the visitor's line\n");
        return 1;
    }
    host.SendAway(slot);
    if (!Await(visitor, Event::Kind::Disconnected, &e) || !Await(host, Event::Kind::Left, &e) || e.slot != slot || host.IsVisitor(slot)) {
        std::printf("live: the visitor was not sent away\n");
        return 1;
    }
    if (Await(visitor, Event::Kind::Connected, &e, 1500)) {
        std::printf("live: a visitor came back after being sent away\n");
        return 1;
    }
    visitor.Stop();
    host.SetOpen(false);
    if (!Until([&] { return !host.OpenListed(); })) return 1;
    return 0;
}

int TestLive(const char* server) {
    const std::string name = "lantern-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    Link a, b;
    a.Start(server, name, "secret");
    Event e;
    if (!Await(a, Event::Kind::Connected, &e)) {
        std::printf("live: no welcome from %s\n", server);
        return 1;
    }
    a.SetHost(true);
    b.Start(server, name, "secret");
    if (!Await(b, Event::Kind::Connected, &e)) {
        std::printf("live: second member got no welcome\n");
        return 1;
    }
    if (!Await(a, Event::Kind::Joined, &e) || e.slot != b.Slot()) {
        std::printf("live: host did not see the join\n");
        return 1;
    }
    const std::vector<std::uint8_t> ping{7, 7, 7};
    b.Send(a.Slot(), ping.data(), ping.size());
    if (!Await(a, Event::Kind::Data, &e) || e.slot != b.Slot() || e.data != ping) {
        std::printf("live: frame did not arrive\n");
        return 1;
    }
    const std::vector<std::uint8_t> all{9};
    a.Send(0, all.data(), all.size());
    if (!Await(b, Event::Kind::Data, &e) || e.slot != a.Slot() || e.data != all) {
        std::printf("live: broadcast did not arrive\n");
        return 1;
    }
    std::printf("live: host slot %d, member slot %d, room host %d\n", a.Slot(), b.Slot(), b.HostSlot());
    if (b.HostSlot() != a.Slot()) return 1;
    Link wrong;
    wrong.Start(server, name, "wrong");
    Await(wrong, Event::Kind::Connected, nullptr);
    if (Await(a, Event::Kind::Joined, nullptr, 3000)) {
        std::printf("live: another password reached the same room\n");
        return 1;
    }
    b.Stop();
    if (!Await(a, Event::Kind::Left, &e)) {
        std::printf("live: leave not seen\n");
        return 1;
    }
    wrong.Stop();
    a.Stop();
    return TestVisitors(server);
}

int Soak(const char* server, int seconds) {
    const std::string name = "lantern-soak-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    Link a, b;
    a.Start(server, name, "secret");
    Event e;
    if (!Await(a, Event::Kind::Connected, &e)) return 1;
    b.Start(server, name, "secret");
    if (!Await(b, Event::Kind::Connected, &e)) return 1;
    int drops = 0;
    long long got = 0, sent = 0;
    const std::vector<std::uint8_t> frame(300, 5);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        for (int i = 0; i < 5; ++i) {
            sent += a.Send(b.Slot(), frame.data(), frame.size());
            sent += b.Send(a.Slot(), frame.data(), frame.size());
        }
        for (Link* l : {&a, &b}) {
            for (auto& ev : l->Drain()) {
                if (ev.kind == Event::Kind::Data) ++got;
                if (ev.kind == Event::Kind::Disconnected) ++drops;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::printf("soak: %d s, sent %lld, got %lld, drops %d\n", seconds, sent, got, drops);
    return drops ? 1 : 0;
}

} // namespace

int main() {
    TestDerive();
    TestSeal();
    TestUrl();
    std::printf("room crypto: ok\n");
    if (const char* soak = std::getenv("BB_ROOM_SOAK"); soak && *soak) {
        return Soak(std::getenv("BB_ROOM_TEST_SERVER"), std::atoi(soak));
    }
    if (const char* server = std::getenv("BB_ROOM_TEST_SERVER"); server && *server) {
        const int live = TestLive(server);
        std::printf("room live: %s\n", live ? "FAILED" : "ok");
        return live;
    }
    std::printf("room live: SKIPPED: needs BB_ROOM_TEST_SERVER\n");
    return 0;
}
