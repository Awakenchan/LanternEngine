// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gpu/bbnet/room/room_crypto.h"
#include "gpu/bbnet/room/room_host.h"
#include "gpu/bbnet/room/room_link.h"
#include "gpu/bbnet/room/room_ws.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using RawSocket = SOCKET;
constexpr RawSocket kNoSocket = INVALID_SOCKET;
void CloseSocket(RawSocket s) { closesocket(s); }
void ShutSocket(RawSocket s) { shutdown(s, SD_BOTH); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using RawSocket = int;
constexpr RawSocket kNoSocket = -1;
void CloseSocket(RawSocket s) { ::close(s); }
void ShutSocket(RawSocket s) { shutdown(s, SHUT_RDWR); }
#endif

using namespace BbRoom;

namespace {

constexpr int kWaitMs = 8000;
constexpr std::size_t kMemberHexLen = 32;
const std::string kName = "wire-room";
const std::string kPassword = "wire-secret";

int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

RawSocket Listen(int& port) {
    const RawSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&at), sizeof at) != 0 || ::listen(s, 8) != 0) return kNoSocket;
    socklen_t size = sizeof at;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&at), &size);
    port = ntohs(at.sin_port);
    return s;
}

int FreePort() {
    int port = 0;
    const RawSocket s = Listen(port);
    CloseSocket(s);
    return port;
}

std::string HexOf(const std::uint8_t* data, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

class Recorder {
public:
    Recorder(int room_port, const Key& key) : room_port_(room_port), key_(key) {}

    bool Start() {
        listener_ = Listen(port_);
        if (listener_ == kNoSocket) return false;
        accept_ = std::thread([this] { Accept(); });
        return true;
    }

    void Stop() {
        // Linux: closing the listener does not wake the accept() blocked on it; shutting it down does.
        ShutSocket(listener_);
        CloseSocket(listener_);
        if (accept_.joinable()) accept_.join();
        std::vector<std::thread> pumps;
        {
            std::lock_guard lock(mutex_);
            for (const RawSocket s : sockets_) ShutSocket(s);
            pumps.swap(pumps_);
        }
        for (auto& t : pumps) t.join();
        std::lock_guard lock(mutex_);
        for (const RawSocket s : sockets_) CloseSocket(s);
        sockets_.clear();
    }

    int Port() const { return port_; }

    std::string Text() {
        std::lock_guard lock(mutex_);
        std::string out;
        for (const auto& [stream, lines] : streams_) {
            out += stream + ":\n";
            for (const auto& line : lines) out += "  " + line + "\n";
        }
        return out;
    }

private:
    void Accept() {
        for (int connection = 1;; ++connection) {
            const RawSocket client = ::accept(listener_, nullptr, nullptr);
            if (client == kNoSocket) return;
            const RawSocket room = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            to.sin_port = htons(static_cast<std::uint16_t>(room_port_));
            if (::connect(room, reinterpret_cast<sockaddr*>(&to), sizeof to) != 0) {
                CloseSocket(client);
                CloseSocket(room);
                continue;
            }
            const std::string who = "connection " + std::to_string(connection);
            std::lock_guard lock(mutex_);
            sockets_.push_back(client);
            sockets_.push_back(room);
            pumps_.emplace_back([this, client, room, who] { Pump(client, room, who + " client to room", who, true); });
            pumps_.emplace_back([this, client, room, who] { Pump(room, client, who + " room to client", who, false); });
        }
    }

    void Note(const std::string& stream, const std::string& line) {
        std::lock_guard lock(mutex_);
        streams_[stream].push_back(line);
    }

    std::string Head(const std::string& head) {
        std::istringstream lines(head);
        std::string line, out;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("Sec-WebSocket-Key: ", 0) == 0) line = "Sec-WebSocket-Key: <nonce>";
            if (line.rfind("Sec-WebSocket-Accept: ", 0) == 0) line = "Sec-WebSocket-Accept: <nonce>";
            if (line.rfind("Host: 127.0.0.1:", 0) == 0) line = "Host: 127.0.0.1:<port>";
            if (line.rfind("GET ", 0) == 0) {
                const auto end = line.find(' ', 4);
                std::string path = line.substr(4, end - 4);
                const auto slash = path.rfind('/');
                if (slash != std::string::npos && path.size() - slash - 1 == kMemberHexLen) path = path.substr(0, slash + 1) + "<member>";
                line = "GET " + path + line.substr(end);
            }
            out += (out.empty() ? "" : " | ") + line;
        }
        return out;
    }

    void Pump(RawSocket from, RawSocket to, const std::string& stream, const std::string& who, bool from_client) {
        std::string data;
        bool upgraded = false;
        char buffer[65536];
        for (;;) {
            const int n = ::recv(from, buffer, sizeof buffer, 0);
            if (n <= 0) break;
            int sent = 0;
            while (sent < n) {
                const int m = ::send(to, buffer + sent, n - sent, 0);
                if (m <= 0) break;
                sent += m;
            }
            data.append(buffer, static_cast<std::size_t>(n));
            if (!upgraded) {
                const auto end = data.find("\r\n\r\n");
                if (end == std::string::npos) continue;
                Note(stream, "handshake: " + Head(data.substr(0, end)));
                data.erase(0, end + 4);
                upgraded = true;
            }
            while (Frame(data, stream, who, from_client)) {
            }
        }
        Note(stream, "end of stream");
        ShutSocket(to);
    }

    bool Frame(std::string& data, const std::string& stream, const std::string& who, bool from_client) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(data.data());
        if (data.size() < 2) return false;
        const bool fin = b[0] & 0x80;
        const int opcode = b[0] & 0x0f;
        const bool masked = b[1] & 0x80;
        std::size_t size = b[1] & 0x7f, at = 2;
        if (size == 126) {
            if (data.size() < 4) return false;
            size = std::size_t(b[2]) << 8 | b[3];
            at = 4;
        } else if (size == 127) {
            if (data.size() < 10) return false;
            size = 0;
            for (int i = 0; i < 8; ++i) size = size << 8 | b[2 + i];
            at = 10;
        }
        std::uint8_t mask[4] = {};
        if (masked) {
            if (data.size() < at + 4) return false;
            std::memcpy(mask, b + at, 4);
            at += 4;
        }
        if (data.size() < at + size) return false;
        std::vector<std::uint8_t> payload(b + at, b + at + size);
        for (std::size_t i = 0; masked && i < size; ++i) payload[i] ^= mask[i & 3];
        data.erase(0, at + size);
        Describe(stream, who, from_client, fin, opcode, masked, payload);
        return true;
    }

    void Describe(const std::string& stream, const std::string& who, bool from_client, bool fin, int opcode, bool masked,
                  const std::vector<std::uint8_t>& payload) {
        if (opcode == 9 || opcode == 10) return;
        std::string line = std::string(fin ? "fin " : "part ") + (masked ? "masked " : "plain ");
        if (opcode == 1) {
            line += "text " + std::string(payload.begin(), payload.end());
            if (!from_client) {
                const std::string text(payload.begin(), payload.end());
                const auto slot_at = text.find("\"slot\":");
                if (text.find("\"t\":\"welcome\"") != std::string::npos && slot_at != std::string::npos) {
                    std::lock_guard lock(mutex_);
                    slots_[who] = std::atoi(text.c_str() + slot_at + 7);
                }
            }
        } else if (opcode == 2 && payload.size() >= 1 + kSealOverhead) {
            int sender = payload[0];
            if (from_client) {
                std::lock_guard lock(mutex_);
                sender = slots_[who];
            }
            const auto plain = Open(key_, static_cast<std::uint8_t>(sender), payload.data() + 1, payload.size() - 1);
            line += std::string(from_client ? "binary route " : "binary from ") + std::to_string(payload[0]) + " nonce <nonce> sealed " +
                    std::to_string(payload.size() - 1) + " bytes";
            line += plain ? " opens as " + HexOf(plain->data(), plain->size()) : " DOES NOT OPEN with the room key";
        } else if (opcode == 8) {
            line += "close " + (payload.size() >= 2 ? std::to_string(payload[0] << 8 | payload[1]) : std::string("no code"));
        } else {
            line += "opcode " + std::to_string(opcode) + " " + HexOf(payload.data(), payload.size());
        }
        Note(stream, line);
    }

    int room_port_;
    Key key_;
    RawSocket listener_ = kNoSocket;
    int port_ = 0;
    std::thread accept_;
    std::mutex mutex_;
    std::vector<std::thread> pumps_;
    std::vector<RawSocket> sockets_;
    std::map<std::string, std::vector<std::string>> streams_;
    std::map<std::string, int> slots_;
};

enum class Leftovers { Kept, DroppedAfterMatch, SkippedKept };

bool TakeFirst(std::deque<Event>& queue, Event::Kind kind, Event* out, Leftovers mode = Leftovers::Kept) {
    std::deque<Event> skipped;
    while (!queue.empty()) {
        Event e = std::move(queue.front());
        queue.pop_front();
        if (e.kind == kind) {
            if (out) *out = std::move(e);
            if (mode == Leftovers::DroppedAfterMatch) queue.clear();
            if (mode == Leftovers::SkippedKept) queue.insert(queue.begin(), skipped.begin(), skipped.end());
            return true;
        }
        if (mode == Leftovers::SkippedKept) skipped.push_back(std::move(e));
    }
    if (mode == Leftovers::SkippedKept) queue = std::move(skipped);
    return false;
}

std::map<const Link*, std::deque<Event>> g_leftovers;

bool Await(Link& link, Event::Kind kind, Event* out = nullptr) {
    auto& queue = g_leftovers[&link];
    if (TakeFirst(queue, kind, out)) return true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (std::chrono::steady_clock::now() < end) {
        link.WaitEvent(50);
        for (auto& e : link.Drain()) queue.push_back(std::move(e));
        if (TakeFirst(queue, kind, out)) return true;
    }
    return false;
}

Event Made(Event::Kind kind, const std::string& data = "") {
    Event e;
    e.kind = kind;
    e.data.assign(data.begin(), data.end());
    return e;
}

bool OneBatchReadsLikeTwo(Leftovers mode) {
    std::deque<Event> batch;
    batch.push_back(Made(Event::Kind::Connected));
    batch.push_back(Made(Event::Kind::Host));
    if (!TakeFirst(batch, Event::Kind::Connected, nullptr, mode) || !TakeFirst(batch, Event::Kind::Host, nullptr, mode)) return false;
    batch.push_back(Made(Event::Kind::Data, "before"));
    batch.push_back(Made(Event::Kind::Joined));
    batch.push_back(Made(Event::Kind::Data, "after"));
    batch.push_back(Made(Event::Kind::Data, "last"));
    Event first, second;
    if (!TakeFirst(batch, Event::Kind::Joined, nullptr, mode) || !TakeFirst(batch, Event::Kind::Data, &first, mode) ||
        !TakeFirst(batch, Event::Kind::Data, &second, mode))
        return false;
    return std::string(first.data.begin(), first.data.end()) == "after" && std::string(second.data.begin(), second.data.end()) == "last" &&
           batch.empty();
}

bool AwaitData(Link& link, const std::string& wanted, int from) {
    Event e;
    return Await(link, Event::Kind::Data, &e) && std::string(e.data.begin(), e.data.end()) == wanted && e.slot == from;
}

const std::uint8_t* Bytes(const std::string& text) { return reinterpret_cast<const std::uint8_t*>(text.data()); }

std::string ReadAll(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    std::string out;
    for (const char c : text.str())
        if (c != '\r') out.push_back(c);
    return out;
}

}

int main(int argc, char** argv) {
    const std::string golden = argc > 1 ? argv[1] : "tests/golden/room-wire.txt";
    const bool record = argc > 2 && std::strcmp(argv[2], "--record") == 0;
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    Check(OneBatchReadsLikeTwo(Leftovers::Kept), "Connected and Host in one batch are both seen, and a later Data wait matches the frame after the match");
    Check(!OneBatchReadsLikeTwo(Leftovers::DroppedAfterMatch), "mutant caught: dropping what follows the match loses Host");
    Check(!OneBatchReadsLikeTwo(Leftovers::SkippedKept), "mutant caught: keeping the skipped events changes which Data frame matches");
    const auto secret = Derive(kName, kPassword);
    if (!secret) {
        std::printf("FAIL: no room key\n");
        return 1;
    }
    const int room_port = FreePort();
    LocalRoom room;
    std::string error;
    Check(room.Start(room_port, secret->room_id, error), "the local room opens");
    Recorder recorder(room_port, secret->key);
    Check(recorder.Start(), "the recorder listens");
    const std::string server = "ws://127.0.0.1:" + std::to_string(recorder.Port());

    Link host, guest;
    host.SetHost(true);
    host.Start(server, kName, kPassword);
    Check(Await(host, Event::Kind::Connected), "the host is welcomed");
    Check(Await(host, Event::Kind::Host), "the room names the host");
    guest.Start(server, kName, kPassword);
    Check(Await(guest, Event::Kind::Connected), "the guest is welcomed");
    Check(Await(host, Event::Kind::Joined), "the host hears the guest join");

    const std::string to_guest = "host-to-guest", to_host = "guest-to-host", game = "game-bundle", all = "host-to-all",
                      paced = "guest-paced", lossy = "guest-lossy";
    Check(host.Send(guest.Slot(), Bytes(to_guest), to_guest.size()) && AwaitData(guest, to_guest, host.Slot()), "host to guest");
    Check(guest.Send(host.Slot(), Bytes(to_host), to_host.size()) && AwaitData(host, to_host, guest.Slot()), "guest to host");
    Check(guest.SendGame(host.Slot(), Bytes(game), game.size()) && AwaitData(host, game, guest.Slot()), "a game bundle");
    Check(host.Send(0, Bytes(all), all.size()) && AwaitData(guest, all, host.Slot()), "host to all");
    Check(guest.SendPaced(host.Slot(), Bytes(paced), paced.size()) && AwaitData(host, paced, guest.Slot()), "a paced frame");
    Check(guest.SendLossy(host.Slot(), Bytes(lossy), lossy.size(), 2048) && AwaitData(host, lossy, guest.Slot()), "a lossy frame");
    guest.Stop();
    Check(Await(host, Event::Kind::Left), "the host hears the guest leave");
    host.Stop();
    room.Stop();
    recorder.Stop();

    const std::string text = recorder.Text();
    if (record) {
        std::ofstream out(golden, std::ios::binary);
        out << text;
        std::printf("room-wire-test: recorded\n");
        return failures ? 1 : 0;
    }
    if (ReadAll(golden) != text) {
        std::printf("FAIL the room frames differ from %s\n%s", golden.c_str(), text.c_str());
        ++failures;
    }
    if (failures) return 1;
    std::printf("room-wire-test: ok\n");
    return 0;
}
