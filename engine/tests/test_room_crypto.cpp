// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gpu/bbnet/room/room_crypto.h"
#include "gpu/bbnet/room/room_host.h"
#include "gpu/bbnet/room/room_link.h"
#include "gpu/bbnet/room/room_ws.h"
#include "room_test_net.h"

using namespace BbRoom;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<std::uint8_t> Bytes(std::string_view hex) {
    std::vector<std::uint8_t> out;
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
    }
    return out;
}

bool SameKey(const Key& key, std::string_view hex) {
    const auto want = Bytes(hex);
    return want.size() == key.size() && std::equal(key.begin(), key.end(), want.begin());
}

constexpr std::string_view kHuntersId = "4344e16951b4fdc22c6d41ddc47bd7e6dd67aa9fe320abe75e253f081bd62f56";
constexpr std::string_view kHuntersKey = "7f675ac2ba70aed035849c072fae64e0f1294202fe09e93932fa6931475f2d80";
constexpr std::string_view kRoomId = "39308767506c75babdcca2cbca588b36f4d05559d3f7d12d05e325e2be58d244";
constexpr std::string_view kRoomKey = "1ca13febe1fd15a2cfd246d373cc49cab8d2f4ed3db7eaaec29b55bcfdccd944";
constexpr std::string_view kNoPasswordId = "8467550e4b1b2468342263bf4afa92f2074035373c8e364260e7188d31146aa4";
constexpr std::string_view kNoPasswordKey = "aa3da1cf3777eb66d43788d7222df23c28564bd7be1c13d71cf87e3ad848b5bf";
constexpr std::string_view kPinnedFrame =
    "5c96c47ac5edde2573555664055524655bcf3c3be87892810da9467550b6caf59df1d7b43d8fbe2dd68e08";
constexpr std::string_view kPinnedEmptyFrame = "77882b34cada41209575974bb0f651cee5ffaf884bcb6fdd35c41f6b68";
constexpr std::uint8_t kPinnedSender = 3;
constexpr std::uint8_t kPinnedEmptySender = 1;
constexpr int kShippedIterations = 200000;
constexpr int kTestIterations = 1000;
const std::vector<std::uint8_t> kPinnedPayload{'p', 'a', 'l', 'e', 'b', 'l', 'o', 'o', 'd', 0, 1, 2, 3, 0xff};

Secret Derived(const std::string& name, const std::string& password, int iterations = kTestIterations) {
    auto secret = Derive(name, password, iterations);
    Check(secret.has_value(), "the room key derives");
    return secret ? *secret : Secret{};
}

std::atomic<int> g_fills{0};
std::vector<std::uint8_t> g_fixed_nonce;

int FailingFill(unsigned char*, int) {
    ++g_fills;
    return 0;
}

int ErrorFill(unsigned char*, int) {
    ++g_fills;
    return -1;
}

int FixedFill(unsigned char* out, int size) {
    ++g_fills;
    if (static_cast<std::size_t>(size) != g_fixed_nonce.size()) return 0;
    std::memcpy(out, g_fixed_nonce.data(), g_fixed_nonce.size());
    return 1;
}

void TestPinnedDerive() {
    const auto hunters = Derived("hunters", "blood", kShippedIterations);
    Check(hunters.room_id == kHuntersId, "the shipped room id for hunters/blood");
    Check(SameKey(hunters.key, kHuntersKey), "the shipped frame key for hunters/blood");
    const auto room = Derived("room", "pass");
    Check(room.room_id == kRoomId, "the pinned room id for room/pass");
    Check(SameKey(room.key, kRoomKey), "the pinned frame key for room/pass");
    const auto open = Derived("hunters", "");
    Check(open.room_id == kNoPasswordId, "the pinned room id without a password");
    Check(SameKey(open.key, kNoPasswordKey), "the pinned frame key without a password");
}

void TestDeriveFails() {
    Check(!Derive("room", "pass", 0), "no iterations is a failed derive, not a weak key");
    Check(!Derive("room", "pass", -5), "negative iterations is a failed derive");
}

void TestPinnedFrames() {
    const auto room = Derived("room", "pass");
    const auto frame = Bytes(kPinnedFrame);
    const auto opened = Open(room.key, kPinnedSender, frame.data(), frame.size());
    Check(opened && *opened == kPinnedPayload, "a frame sealed by the shipped code opens");
    Check(!Open(room.key, kPinnedSender + 1, frame.data(), frame.size()), "the pinned frame names its sender");
    const auto empty = Bytes(kPinnedEmptyFrame);
    const auto opened_empty = Open(room.key, kPinnedEmptySender, empty.data(), empty.size());
    Check(opened_empty && opened_empty->empty(), "an empty frame sealed by the shipped code opens");
}

void TestSameBytesAsShipped() {
    const auto room = Derived("room", "pass");
    const auto frame = Bytes(kPinnedFrame);
    const auto empty = Bytes(kPinnedEmptyFrame);
    SetRandomFill(FixedFill);
    g_fixed_nonce.assign(frame.begin(), frame.begin() + kNonceLen);
    const auto again = Seal(room.key, kPinnedSender, kPinnedPayload.data(), kPinnedPayload.size());
    g_fixed_nonce.assign(empty.begin(), empty.begin() + kNonceLen);
    const auto empty_again = Seal(room.key, kPinnedEmptySender, nullptr, 0);
    SetRandomFill(nullptr);
    Check(again == frame, "with the shipped nonce the frame is byte for byte the shipped frame");
    Check(empty_again == empty, "with the shipped nonce the empty frame is byte for byte the shipped one");
}

void TestOpenEdges() {
    const auto room = Derived("room", "pass");
    const auto frame = Bytes(kPinnedFrame);
    Check(!Open(room.key, kPinnedSender, nullptr, frame.size()), "no data opens nothing");
    Check(!Open(room.key, kPinnedSender, frame.data(), kSealOverhead - 1), "a short box opens nothing");
    auto flipped = frame;
    flipped.back() ^= 1;
    Check(!Open(room.key, kPinnedSender, flipped.data(), flipped.size()), "a flipped tag bit opens nothing");
    auto nonce = frame;
    nonce[0] ^= 0x80;
    Check(!Open(room.key, kPinnedSender, nonce.data(), nonce.size()), "a flipped nonce bit opens nothing");
}

void TestSealRefusesWithoutRandom() {
    const auto room = Derived("room", "pass");
    for (const RandomFill fill : {FailingFill, ErrorFill}) {
        g_fills = 0;
        SetRandomFill(fill);
        const auto sealed = Seal(room.key, 2, kPinnedPayload.data(), kPinnedPayload.size());
        const auto empty = Seal(room.key, 2, nullptr, 0);
        std::array<std::uint8_t, 4> token{};
        const bool token_made = RandomBytes(token);
        SetRandomFill(nullptr);
        Check(g_fills.load() == 3, "the seal asks the random source for its nonce");
        Check(sealed.empty() && empty.empty(), "no random nonce means no frame");
        Check(!token_made, "the random source failure is reported");
    }
    const auto sealed = Seal(room.key, 2, kPinnedPayload.data(), kPinnedPayload.size());
    const auto opened = Open(room.key, 2, sealed.data(), sealed.size());
    Check(opened && *opened == kPinnedPayload, "the system random source seals again");
    const auto other = Seal(room.key, 2, kPinnedPayload.data(), kPinnedPayload.size());
    Check(other.size() == sealed.size() && !std::equal(other.begin(), other.begin() + kNonceLen, sealed.begin()),
          "fresh nonces differ");
}

bool WaitConnected(Link& link) {
    for (int i = 0; i < 200 && !link.Connected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return link.Connected();
}

int DataFrames(Link& link, int ms) {
    int frames = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        link.WaitEvent(20);
        for (const auto& e : link.Drain()) frames += e.kind == Event::Kind::Data;
    }
    return frames;
}

bool AwaitData(Link& link, Event& got) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        link.WaitEvent(50);
        for (auto& e : link.Drain()) {
            if (e.kind == Event::Kind::Data) {
                got = std::move(e);
                return true;
            }
        }
    }
    return false;
}

void TestSocketWithoutRandom(const std::string& server, const std::string& room_id) {
    const std::string url = RoomUrl(server, room_id) + "/" + std::string(32, 'e');
    WebSocket refused;
    std::string error;
    g_fills = 0;
    SetRandomFill(FailingFill);
    const bool opened = refused.Connect(url, 5, error);
    SetRandomFill(nullptr);
    Check(!opened && !error.empty() && g_fills.load() == 1, "no random WebSocket key means no handshake, with a reason");
    WebSocket socket;
    error.clear();
    Check(socket.Connect(url, 5, error), "the handshake works once the random source does");
    const std::vector<std::uint8_t> bytes{1, 2, 3};
    SetRandomFill(FailingFill);
    const bool sent = socket.Send(bytes.data(), bytes.size(), true);
    const bool lossy = socket.SendLossy(bytes.data(), bytes.size(), 1 << 20);
    const bool paced = socket.SendPaced(bytes.data(), bytes.size());
    SetRandomFill(nullptr);
    Check(!sent && !lossy && !paced, "no random mask means no WebSocket frame");
    Check(socket.Backlog() == 0, "nothing was queued without a mask");
    Check(socket.Send(bytes.data(), bytes.size(), true) && socket.Backlog() > 0, "a masked frame queues again");
    socket.Close();
}

void TestLinkSendsNothingUnsealed() {
    const std::string name = RoomTestNet::UniqueRoomName("seal-room"), password = "secret";
    const int port = RoomTestNet::FreePort();
    Check(!name.empty() && port != 0, "a free local port and a room name of its own");
    const std::string server = "ws://127.0.0.1:" + std::to_string(port);
    LocalRoom room;
    std::string error;
    Check(room.Start(port, Derived(name, password, kShippedIterations).room_id, error), "the local room opens");
    Link host, guest;
    host.SetHost(true);
    host.Start(server, name, password);
    Check(WaitConnected(host), "the host joins");
    guest.Start(server, name, password);
    Check(WaitConnected(guest), "the guest joins");
    DataFrames(host, 300);
    const std::vector<std::uint8_t> ping{7, 7, 7};
    const int to = host.Slot();
    SetRandomFill(FailingFill);
    const bool sent = guest.Send(to, ping.data(), ping.size());
    const bool lossy = guest.SendLossy(to, ping.data(), ping.size(), 1 << 20);
    const bool game = guest.SendGame(to, ping.data(), ping.size());
    const bool paced = guest.SendPaced(to, ping.data(), ping.size());
    const bool broadcast = guest.Send(0, ping.data(), ping.size());
    SetRandomFill(nullptr);
    Check(!sent && !lossy && !game && !paced && !broadcast, "every send reports a failed seal");
    Check(DataFrames(host, 700) == 0, "nothing reached the host while the seal failed");
    Check(guest.Connected(), "the guest stays in the room after a failed seal");
    Check(guest.Send(to, ping.data(), ping.size()), "the guest sends again once the seal works");
    Event got;
    Check(AwaitData(host, got) && got.data == ping && got.slot == guest.Slot(), "the next sealed frame arrives");
    TestSocketWithoutRandom(server, host.RoomId());
    guest.Stop();
    host.Stop();
    room.Stop();
}

void TestLinkRefusesWithoutRandom() {
    Link link;
    SetRandomFill(FailingFill);
    const bool started = link.Start("ws://127.0.0.1:1", "seal-room", "secret");
    SetRandomFill(nullptr);
    Check(!started && !link.LastError().empty(), "no random member token means no join, with a reason");
}

} // namespace

int main() {
    RoomTestNet::StartSockets();
    TestPinnedDerive();
    TestDeriveFails();
    TestPinnedFrames();
    TestSameBytesAsShipped();
    TestOpenEdges();
    TestSealRefusesWithoutRandom();
    TestLinkRefusesWithoutRandom();
    TestLinkSendsNothingUnsealed();
    std::printf("room crypto: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
