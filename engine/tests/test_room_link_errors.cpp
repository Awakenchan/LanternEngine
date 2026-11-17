// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "gpu/bbnet/room/room_crypto.h"
#include "gpu/bbnet/room/room_host.h"
#include "gpu/bbnet/room/room_link.h"
#include "room_test_net.h"

using namespace BbRoom;

namespace {

constexpr int kReconnectWrites = 5;
constexpr int kReaders = 2;
constexpr auto kWaitLimit = std::chrono::seconds(20);
constexpr auto kPoll = std::chrono::milliseconds(10);
constexpr int kQuietMs = 400;
constexpr std::uint8_t kTokenFill = 0x5a;
constexpr std::string_view kNoKey = "no random WebSocket key";
constexpr std::string_view kBadAddress = "bad address";
constexpr std::string_view kNoToken = "no random member token";
constexpr int kWireLossyRoute = 129;
constexpr int kWirePlainRoute = 1;

static_assert(std::is_same_v<decltype(std::declval<const Link&>().LastError()), std::string>,
              "the error text is handed out as a copy, never as a reference into the link");
static_assert(std::is_same_v<decltype(std::declval<const Link&>().RoomId()), std::string>,
              "the room id is handed out as a copy");
static_assert(RouteByte(kWirePlainRoute, true) == kWireLossyRoute, "a lossy game frame to slot 1 is route 129 on the wire");
static_assert(RouteByte(kWirePlainRoute, false) == kWirePlainRoute, "a plain frame to slot 1 is route 1 on the wire");
static_assert(RouteByte(kMostGameTarget, true) == kMostRoute, "the highest game target with the lossy bit is the highest route");
constexpr std::size_t kRoomyBacklog = std::size_t{1} << 20;
constexpr int kDrainWaitMs = 20;
constexpr int kDataWaitMs = 50;

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::atomic<int> g_fills{0};

int FailingFill(unsigned char*, int) {
    ++g_fills;
    return 0;
}

int TokenThenFailFill(unsigned char* out, int size) {
    if (g_fills.fetch_add(1) > 0) return 0;
    std::memset(out, kTokenFill, static_cast<std::size_t>(size));
    return 1;
}

template <typename Done>
bool Until(Done done) {
    const auto end = std::chrono::steady_clock::now() + kWaitLimit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(kPoll);
    }
    return true;
}

int DataFrames(Link& link, int ms) {
    int frames = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        link.WaitEvent(kDrainWaitMs);
        for (const auto& e : link.Drain()) frames += e.kind == Event::Kind::Data;
    }
    return frames;
}

bool AwaitData(Link& link, Event& got) {
    const auto end = std::chrono::steady_clock::now() + kWaitLimit;
    while (std::chrono::steady_clock::now() < end) {
        link.WaitEvent(kDataWaitMs);
        for (auto& e : link.Drain()) {
            if (e.kind == Event::Kind::Data) {
                got = std::move(e);
                return true;
            }
        }
    }
    return false;
}

void TestErrorTextWhileReconnecting() {
    g_fills = 0;
    SetRandomFill(TokenThenFailFill);
    Link link;
    const bool started = link.Start("ws://127.0.0.1:1", "race-room", "secret");
    Check(started, "the link starts once the member token is drawn");
    std::atomic<bool> stop_reading{false};
    std::atomic<int> bad_reads{0};
    std::atomic<long long> reads{0};
    std::atomic<long long> seen{0};
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            while (!stop_reading.load(std::memory_order_relaxed)) {
                const std::string text = link.LastError();
                ++reads;
                if (text == kNoKey) {
                    ++seen;
                } else if (!text.empty()) {
                    ++bad_reads;
                }
            }
        });
    }
    const bool rewritten = Until([] { return g_fills.load() > kReconnectWrites; });
    stop_reading = true;
    for (auto& reader : readers) reader.join();
    link.Stop();
    SetRandomFill(nullptr);
    Check(rewritten, "the link rewrote its error on every failed reconnect");
    Check(reads.load() > 0 && seen.load() > 0, "readers saw the reconnect error while it was being written");
    Check(bad_reads.load() == 0, "every read is a whole error text");
    Check(link.LastError() == kNoKey, "the last reconnect error stays readable after the link stops");
    Check(!link.Connected(), "a link that never connected says so");
}

void TestStartRefusals() {
    Link link;
    Check(!link.Start("", "room", "secret") && link.LastError().empty(), "no server means no start and no stale reason");
    Check(!link.Start("ws://127.0.0.1:1", "", "secret") && link.LastError().empty(), "no room name means no start");
    g_fills = 0;
    SetRandomFill(FailingFill);
    const bool started = link.Start("ws://127.0.0.1:1", "room", "secret");
    SetRandomFill(nullptr);
    Check(!started && link.LastError() == kNoToken, "no random member token means no start, with that reason");
    Check(g_fills.load() == 1 && !link.Connected(), "nothing past the member token ran");
}

void TestConnectFailureReason() {
    Link link;
    g_fills = 0;
    SetRandomFill(FailingFill);
    Check(!link.Start("ws://127.0.0.1:1", "room", "secret"), "a refused start");
    SetRandomFill(nullptr);
    Check(link.Start("ws://:9", "room", "secret"), "an address without a host still starts the link");
    Check(link.LastError().empty() || link.LastError() == kBadAddress, "the earlier reason is cleared on a new start");
    Check(Until([&] { return link.LastError() == kBadAddress; }), "the link thread reports the bad address");
    Check(!link.Connected(), "a bad address never connects");
    link.Stop();
}

struct Room {
    LocalRoom room;
    Link host;
    Link guest;
    bool ready = false;

    Room(const std::string& base, const std::string& password) {
        const std::string name = RoomTestNet::UniqueRoomName(base);
        const int port = RoomTestNet::FreePort();
        if (name.empty() || !port) return;
        const auto secret = Derive(name, password);
        std::string error;
        if (!secret || !room.Start(port, secret->room_id, error)) return;
        const std::string server = "ws://127.0.0.1:" + std::to_string(port);
        host.SetHost(true);
        if (!host.Start(server, name, password) || !Until([&] { return host.Connected(); })) return;
        if (!guest.Start(server, name, password) || !Until([&] { return guest.Connected(); })) return;
        DataFrames(host, kQuietMs);
        ready = true;
    }

    ~Room() {
        guest.Stop();
        host.Stop();
        room.Stop();
    }
};

void TestSendRefusals() {
    const std::vector<std::uint8_t> ping{4, 2};
    Link idle;
    Check(!idle.Send(1, ping.data(), ping.size()) && !idle.SendLossy(1, ping.data(), ping.size(), kRoomyBacklog) &&
              !idle.SendGame(1, ping.data(), ping.size()) && !idle.SendPaced(1, ping.data(), ping.size()),
          "a link that is not in a room sends nothing");
    Room room("send-room", "secret");
    Check(room.ready, "the local room, host and guest come up");
    if (!room.ready) return;
    Link& guest = room.guest;
    const int own = guest.Slot();
    const int to = room.host.Slot();
    Check(!guest.Send(own, ping.data(), ping.size()), "no frame to its own slot");
    Check(!guest.Send(-1, ping.data(), ping.size()), "no frame to a negative slot");
    Check(!guest.Send(kMostRoute + 1, ping.data(), ping.size()), "no route past one byte");
    Check(!guest.SendLossy(kMostRoute + 1, ping.data(), ping.size(), kRoomyBacklog), "no lossy route past one byte");
    Check(!guest.SendGame(kMostGameTarget + 1, ping.data(), ping.size()), "a game frame cannot carry the lossy bit itself");
    Check(!guest.SendPaced(kMostGameTarget + 1, ping.data(), ping.size()), "a paced frame stays under the lossy bit");
    Check(!guest.SendGame(own, ping.data(), ping.size()) && !guest.SendPaced(own, ping.data(), ping.size()),
          "no game or paced frame to its own slot");
    Check(DataFrames(room.host, kQuietMs) == 0, "nothing refused reached the host");
    g_fills = 0;
    SetRandomFill(FailingFill);
    const bool sent = guest.Send(to, ping.data(), ping.size());
    const bool lossy = guest.SendLossy(to, ping.data(), ping.size(), kRoomyBacklog);
    const bool game = guest.SendGame(to, ping.data(), ping.size());
    const bool paced = guest.SendPaced(to, ping.data(), ping.size());
    SetRandomFill(nullptr);
    Check(!sent && !lossy && !game && !paced, "every send kind reports a failed seal");
    Check(g_fills.load() == 4, "each send kind asked for its own nonce");
    Check(DataFrames(room.host, kQuietMs) == 0, "no unsealed frame reached the host");
    Check(guest.Connected(), "the guest stays in the room after a failed seal");
}

void TestEverySendKindArrives() {
    Room room("kinds-room", "secret");
    Check(room.ready, "the local room, host and guest come up");
    if (!room.ready) return;
    Link& guest = room.guest;
    const int to = room.host.Slot();
    Check(guest.LossyRoutes() && room.host.LossyRoutes(), "the local room's welcome turns on lossy routes");
    Check(RouteByte(to, guest.LossyRoutes()) == (to | kLossyRoute), "game frames to the host carry the lossy bit");
    using SendFn = bool (*)(Link&, int, const std::vector<std::uint8_t>&);
    const SendFn kinds[] = {
        [](Link& l, int t, const std::vector<std::uint8_t>& d) { return l.Send(t, d.data(), d.size()); },
        [](Link& l, int t, const std::vector<std::uint8_t>& d) { return l.SendLossy(t, d.data(), d.size(), kRoomyBacklog); },
        [](Link& l, int t, const std::vector<std::uint8_t>& d) { return l.SendGame(t, d.data(), d.size()); },
        [](Link& l, int t, const std::vector<std::uint8_t>& d) { return l.SendPaced(t, d.data(), d.size()); },
    };
    std::uint8_t mark = 1;
    for (const SendFn send : kinds) {
        const std::vector<std::uint8_t> frame{mark, 7, 7};
        Check(send(guest, to, frame), "the send kind is accepted");
        Event got;
        Check(AwaitData(room.host, got) && got.data == frame && got.slot == guest.Slot(),
              "the send kind arrives sealed from the guest's slot");
        ++mark;
    }
    guest.Stop();
    const std::vector<std::uint8_t> late{9};
    Check(!guest.Send(to, late.data(), late.size()) && !guest.SendGame(to, late.data(), late.size()),
          "a stopped link sends nothing");
}

} // namespace

int main() {
    RoomTestNet::StartSockets();
    TestStartRefusals();
    TestConnectFailureReason();
    TestErrorTextWhileReconnecting();
    TestSendRefusals();
    TestEverySendKindArrives();
    std::printf("room link errors: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
