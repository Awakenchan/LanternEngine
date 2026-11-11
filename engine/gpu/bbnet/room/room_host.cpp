// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_host.h"
#include "room_ws.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace BbRoom {
namespace {

#ifdef _WIN32
using Sock = SOCKET;
constexpr Sock kBad = INVALID_SOCKET;
void CloseSock(Sock s) { closesocket(s); }
void NonBlocking(Sock s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
int Poll(pollfd* fds, std::size_t count, int timeout_ms) { return WSAPoll(fds, static_cast<ULONG>(count), timeout_ms); }
#else
using Sock = int;
constexpr Sock kBad = -1;
void CloseSock(Sock s) { ::close(s); }
void NonBlocking(Sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
int Poll(pollfd* fds, std::size_t count, int timeout_ms) { return ::poll(fds, static_cast<nfds_t>(count), timeout_ms); }
#endif

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxFrame = 65545;
constexpr std::size_t kMaxPayload = 1 << 20;
constexpr std::size_t kMaxHead = 8192;
constexpr std::size_t kMaxSockets = 32;
constexpr std::size_t kMaxQueued = 32u << 20;
constexpr std::size_t kVoiceBacklog = 32768;
constexpr std::size_t kLossyBacklog = 128 * 1024;
constexpr std::uint8_t kLossyTarget = 0x80;
constexpr auto kHandshakeTime = std::chrono::seconds(5);
constexpr auto kAwayTime = std::chrono::seconds(120);
constexpr auto kPingEvery = std::chrono::seconds(30);
constexpr auto kSilentFor = std::chrono::seconds(90);
constexpr auto kFloodTime = std::chrono::seconds(5);
constexpr int kTickMs = 100;

struct Budget {
    double rate = 0, burst = 0, level = 0;
    Clock::time_point at{};

    void Set(double per_second, double most) {
        rate = per_second;
        burst = most;
        level = most;
        at = Clock::now();
    }
    bool Take(double amount, Clock::time_point now) {
        level = std::min(burst, level + std::chrono::duration<double>(now - at).count() * rate);
        at = now;
        if (level < amount) return false;
        level -= amount;
        return true;
    }
};

struct Peer {
    Sock sock = kBad;
    std::string in, out;
    std::size_t sent = 0;
    bool upgraded = false, voice = false, dead = false;
    int slot = 0;
    std::string token;
    Clock::time_point opened{}, heard{}, pinged{}, flooding{}, dropped{};
    Budget messages, bytes;

    std::size_t Queued() const { return out.size() - sent; }
};

struct Away {
    std::string token;
    Clock::time_point since{}, until{};
};

const char* CloseReason(int code) {
    switch (code) {
    case kCloseFull: return "the room is full";
    case 4004: return "voice for a room nobody is in";
    case 4008: return "it sent too much for too long";
    case 1002: return "a broken websocket frame";
    case 1009: return "a message over the size limit";
    default: return "closed by the room";
    }
}

void Note(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Note(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    std::printf("Seamless: room: %s\n", text);
    std::fflush(stdout);
}

std::string AcceptKey(const std::string& key) {
    const std::string joined = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(joined.data()), joined.size(), digest);
    char text[64];
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(text), digest, SHA_DIGEST_LENGTH);
    return std::string(text, n > 0 ? static_cast<std::size_t>(n) : 0);
}

std::string Header(const std::string& head, const char* name) {
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string wanted = std::string("\r\n") + name + ":";
    const auto at = lower.find(wanted);
    if (at == std::string::npos) return "";
    auto start = at + wanted.size();
    const auto end = head.find("\r\n", start);
    while (start < end && head[start] == ' ') ++start;
    return head.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool IsHex(const std::string& text, std::size_t size) {
    return text.size() == size && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

} // namespace

struct LocalRoom::Impl {
    Sock listener = kBad;
    std::string room_id;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::list<Peer> peers;
    std::map<int, Peer*> members, voices;
    std::map<int, Away> away;
    int host = 0;
    int most = kMaxMembers;

    bool Alive() const { return !members.empty() || !away.empty(); }

    void Flush(Peer& peer) {
        while (!peer.dead && peer.Queued()) {
            const auto n = ::send(peer.sock, peer.out.data() + peer.sent, static_cast<int>(std::min<std::size_t>(peer.Queued(), 1 << 20)), 0);
            if (n > 0) {
                peer.sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && WouldBlock()) break;
            Drop(peer);
        }
        if (peer.sent && peer.sent == peer.out.size()) {
            peer.out.clear();
            peer.sent = 0;
        }
    }

    void Queue(Peer& peer, std::uint8_t opcode, const void* data, std::size_t size) {
        if (peer.dead) return;
        if (peer.Queued() + size > kMaxQueued) {
            Drop(peer);
            return;
        }
        std::uint8_t head[10] = {static_cast<std::uint8_t>(0x80 | opcode)};
        std::size_t used = 2;
        if (size < 126) {
            head[1] = static_cast<std::uint8_t>(size);
        } else if (size < 65536) {
            head[1] = 126;
            head[2] = static_cast<std::uint8_t>(size >> 8);
            head[3] = static_cast<std::uint8_t>(size);
            used = 4;
        } else {
            head[1] = 127;
            for (int i = 0; i < 8; ++i) head[2 + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(size) >> (56 - 8 * i));
            used = 10;
        }
        peer.out.append(reinterpret_cast<const char*>(head), used);
        peer.out.append(static_cast<const char*>(data), size);
        Flush(peer);
    }

    void Text(Peer& peer, const nlohmann::json& message) {
        const std::string text = message.dump();
        Queue(peer, 1, text.data(), text.size());
    }

    void Broadcast(const nlohmann::json& message, int except) {
        for (auto& [slot, member] : members)
            if (slot != except) Text(*member, message);
    }

    void CloseWith(Peer& peer, int code) {
        if (peer.slot) Note("slot %d closed (%d): %s", peer.slot, code, CloseReason(code));
        else Note("a hunter was closed before joining (%d): %s", code, CloseReason(code));
        const std::uint8_t body[2] = {static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code)};
        Queue(peer, 8, body, sizeof body);
        Drop(peer);
    }

    void Gone(int slot) {
        nlohmann::json left = {{"t", "leave"}, {"slot", slot}};
        Broadcast(left, slot);
        if (host == slot) {
            host = 0;
            Broadcast({{"t", "host"}, {"slot", 0}}, slot);
        }
    }

    void Leave(Peer& peer) {
        const int slot = peer.slot;
        if (!slot) return;
        peer.slot = 0;
        if (peer.voice) {
            const auto it = voices.find(slot);
            if (it != voices.end() && it->second == &peer) voices.erase(it);
            return;
        }
        const auto it = members.find(slot);
        if (it != members.end() && it->second == &peer) members.erase(it);
        if (!peer.token.empty()) {
            const auto now = Clock::now();
            away[slot] = {peer.token, now, now + kAwayTime};
            Note("slot %d dropped without a leave: held %lld s for its reconnect", slot,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kAwayTime).count()));
            Broadcast({{"t", "away"}, {"slot", slot}}, slot);
            return;
        }
        Note("slot %d left", slot);
        Gone(slot);
    }

    void Drop(Peer& peer) {
        if (peer.dead) return;
        peer.dead = true;
        Leave(peer);
    }

    int FreeSlot(bool voice) const {
        for (int slot = 1; slot <= most; ++slot) {
            if (voice ? voices.count(slot) != 0 : (members.count(slot) != 0 || away.count(slot) != 0)) continue;
            return slot;
        }
        return 0;
    }

    void Join(Peer& peer) {
        int slot = 0;
        bool back = false, returned = false;
        if (!peer.token.empty()) {
            for (auto it = away.begin(); it != away.end(); ++it) {
                if (it->second.token == peer.token) {
                    slot = it->first;
                    returned = true;
                    away.erase(it);
                    Note("slot %d came back within its hold", slot);
                    break;
                }
            }
            for (auto it = members.begin(); !slot && it != members.end(); ++it) {
                if (it->second == &peer || it->second->token != peer.token) continue;
                slot = it->first;
                back = true;
                Peer& old = *it->second;
                old.slot = 0;
                members.erase(it);
                Drop(old);
                Note("slot %d reconnected over its old socket", slot);
                break;
            }
        }
        if (!slot) slot = FreeSlot(false);
        if (!slot && !away.empty()) {
            const auto oldest = std::min_element(away.begin(), away.end(), [](const auto& a, const auto& b) { return a.second.since < b.second.since; });
            slot = oldest->first;
            away.erase(oldest);
            Note("slot %d was held for a hunter who dropped: given to a new hunter, leave sent first", slot);
            Gone(slot);
        }
        if (!slot) {
            Note("a hunter was turned away: the room is full (%d of %d hunters)", most, most);
            Text(peer, {{"t", "full"}, {"most", most}});
            CloseWith(peer, kCloseFull);
            return;
        }
        std::vector<int> others;
        for (const auto& [other, member] : members) others.push_back(other);
        peer.slot = slot;
        members[slot] = &peer;
        peer.messages.Set(400, 800);
        peer.bytes.Set(4 << 20, 8 << 20);
        Text(peer, {{"t", "welcome"}, {"slot", slot}, {"host", host}, {"members", others}, {"lossy", true}});
        if (!back) Broadcast({{"t", "join"}, {"slot", slot}}, slot);
        if (!back && !returned) Note("slot %d joined", slot);
    }

    void JoinVoice(Peer& peer) {
        if (!Alive()) {
            CloseWith(peer, 4004);
            return;
        }
        if (!peer.token.empty()) {
            for (auto it = voices.begin(); it != voices.end(); ++it) {
                if (it->second == &peer || it->second->token != peer.token) continue;
                Peer& old = *it->second;
                old.slot = 0;
                voices.erase(it);
                Drop(old);
                break;
            }
        }
        const int slot = FreeSlot(true);
        if (!slot) {
            CloseWith(peer, 4003);
            return;
        }
        std::vector<int> others;
        for (const auto& [other, member] : voices) others.push_back(other);
        peer.slot = slot;
        voices[slot] = &peer;
        peer.messages.Set(30, 60);
        peer.bytes.Set(4096, 8192);
        Text(peer, {{"t", "welcome"}, {"slot", slot}, {"host", 0}, {"members", others}});
    }

    void Refuse(Peer& peer, const char* status, const std::string& extra = {}) {
        const std::string answer = std::string("HTTP/1.1 ") + status + "\r\n" + extra + "Connection: close\r\nContent-Length: 0\r\n\r\n";
        peer.out += answer;
        Flush(peer);
        Drop(peer);
    }

    void Handshake(Peer& peer) {
        const auto end = peer.in.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (peer.in.size() > kMaxHead) Refuse(peer, "431 Request Header Fields Too Large");
            return;
        }
        const std::string head = peer.in.substr(0, end + 2);
        peer.in.erase(0, end + 4);
        const auto line_end = head.find("\r\n");
        const std::string line = head.substr(0, line_end);
        if (line.rfind("GET /", 0) != 0) {
            Refuse(peer, "404 Not Found");
            return;
        }
        const auto path_end = line.find(' ', 4);
        std::string path = line.substr(5, path_end == std::string::npos ? std::string::npos : path_end - 5);
        const bool voice = path.rfind("voice/", 0) == 0;
        if (!voice && path.rfind("room/", 0) != 0) {
            Refuse(peer, "404 Not Found");
            return;
        }
        path.erase(0, voice ? 6 : 5);
        const auto slash = path.find('/');
        const std::string id = path.substr(0, slash);
        const std::string token = slash == std::string::npos ? "" : path.substr(slash + 1);
        if (!IsHex(id, 64) || id.size() != room_id.size() || CRYPTO_memcmp(id.data(), room_id.data(), id.size()) != 0 || (!token.empty() && !IsHex(token, 32))) {
            Refuse(peer, "404 Not Found");
            return;
        }
        const int protocol = std::atoi(Header(head, "x-lantern-room").c_str());
        if (protocol != kRoomProtocol) {
            std::printf("Seamless: a hunter was turned away: their Lantern speaks room protocol %d, this room %d\n", protocol, kRoomProtocol);
            std::fflush(stdout);
            Refuse(peer, "426 Upgrade Required", "X-Lantern-Room: " + std::to_string(kRoomProtocol) + "\r\n");
            return;
        }
        const std::string key = Header(head, "sec-websocket-key");
        if (key.empty()) {
            Refuse(peer, "400 Bad Request");
            return;
        }
        peer.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + AcceptKey(key) + "\r\n\r\n";
        peer.upgraded = true;
        peer.voice = voice;
        peer.token = token;
        peer.heard = peer.pinged = Clock::now();
        Flush(peer);
        if (peer.dead) return;
        if (voice) JoinVoice(peer);
        else Join(peer);
    }

    bool WithinBudget(Peer& peer, std::size_t size, Clock::time_point now) {
        if (peer.messages.Take(1, now) && peer.bytes.Take(static_cast<double>(size), now)) {
            if (peer.flooding != Clock::time_point{} && now - peer.dropped > std::chrono::seconds(1)) peer.flooding = {};
            return true;
        }
        peer.dropped = now;
        if (peer.flooding == Clock::time_point{}) peer.flooding = now;
        else if (now - peer.flooding >= kFloodTime) CloseWith(peer, 4008);
        return false;
    }

    void Route(Peer& peer, std::vector<std::uint8_t>& frame) {
        if (frame.size() < 2 || frame.size() > kMaxFrame) return;
        const int target = frame[0] & ~kLossyTarget;
        const bool lossy = (frame[0] & kLossyTarget) != 0;
        const int sender = peer.slot;
        frame[0] = static_cast<std::uint8_t>(sender);
        auto& group = peer.voice ? voices : members;
        for (auto& [slot, member] : group) {
            if (slot == sender || (target != 0 && slot != target)) continue;
            if (peer.voice && member->Queued() > kVoiceBacklog) continue;
            if (lossy && member->Queued() > kLossyBacklog) continue;
            Queue(*member, 2, frame.data(), frame.size());
        }
    }

    void Control(Peer& peer, const std::string& text) {
        if (text.size() > 256) return;
        const auto message = nlohmann::json::parse(text, nullptr, false);
        if (!message.is_object()) return;
        const std::string type = message.value("t", "");
        if (type == "ping") {
            Text(peer, {{"t", "pong"}});
        } else if (type == "leave") {
            peer.token.clear();
            Drop(peer);
        } else if (!peer.voice && type == "host") {
            host = peer.slot;
            Broadcast({{"t", "host"}, {"slot", peer.slot}}, 0);
        } else if (!peer.voice && type == "unhost" && host == peer.slot) {
            host = 0;
            Broadcast({{"t", "host"}, {"slot", 0}}, 0);
        }
    }

    void Frames(Peer& peer, Clock::time_point now) {
        while (!peer.dead) {
            const auto* data = reinterpret_cast<const std::uint8_t*>(peer.in.data());
            const std::size_t have = peer.in.size();
            if (have < 2) return;
            const bool final_part = (data[0] & 0x80) != 0;
            const std::uint8_t opcode = data[0] & 0x0f;
            if (!final_part || !(data[1] & 0x80)) {
                CloseWith(peer, 1002);
                return;
            }
            std::uint64_t size = data[1] & 0x7f;
            std::size_t at = 2;
            if (size == 126) {
                if (have < 4) return;
                size = (std::uint64_t(data[2]) << 8) | data[3];
                at = 4;
            } else if (size == 127) {
                if (have < 10) return;
                size = 0;
                for (int i = 0; i < 8; ++i) size = (size << 8) | data[2 + i];
                at = 10;
            }
            if (size > kMaxPayload) {
                CloseWith(peer, 1009);
                return;
            }
            if (have < at + 4 + size) return;
            const std::uint8_t* mask = data + at;
            at += 4;
            std::vector<std::uint8_t> payload(static_cast<std::size_t>(size));
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = data[at + i] ^ mask[i & 3];
            peer.in.erase(0, at + payload.size());
            peer.heard = now;
            if (opcode == 8) {
                Drop(peer);
            } else if (opcode == 9) {
                Queue(peer, 10, payload.data(), payload.size());
            } else if (opcode == 2 && peer.slot) {
                if (WithinBudget(peer, payload.size(), now)) Route(peer, payload);
            } else if (opcode == 1 && peer.slot) {
                Control(peer, std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
            }
        }
    }

    void Read(Peer& peer, Clock::time_point now) {
        char buffer[65536];
        for (;;) {
            const auto n = ::recv(peer.sock, buffer, sizeof buffer, 0);
            if (n == 0) {
                Drop(peer);
                return;
            }
            if (n < 0) {
                if (!WouldBlock()) Drop(peer);
                break;
            }
            peer.in.append(buffer, static_cast<std::size_t>(n));
            if (peer.in.size() > 2 * kMaxPayload) {
                Drop(peer);
                return;
            }
            if (static_cast<std::size_t>(n) < sizeof buffer) break;
        }
        if (!peer.upgraded) Handshake(peer);
        if (peer.upgraded) Frames(peer, now);
    }

    void Accept(Clock::time_point now) {
        for (;;) {
            const Sock sock = ::accept(listener, nullptr, nullptr);
            if (sock == kBad) return;
            if (peers.size() >= kMaxSockets) {
                CloseSock(sock);
                continue;
            }
            NonBlocking(sock);
            const int on = 1;
            ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
            Peer& peer = peers.emplace_back();
            peer.sock = sock;
            peer.opened = peer.heard = now;
        }
    }

    void Timers(Clock::time_point now) {
        for (auto it = away.begin(); it != away.end();) {
            if (now < it->second.until) {
                ++it;
                continue;
            }
            const int slot = it->first;
            it = away.erase(it);
            Note("slot %d did not come back within its hold: leave sent", slot);
            Gone(slot);
        }
        for (Peer& peer : peers) {
            if (!peer.upgraded) {
                if (now - peer.opened > kHandshakeTime) Drop(peer);
                continue;
            }
            if (now - peer.heard > kSilentFor) {
                Drop(peer);
            } else if (now - peer.pinged > kPingEvery) {
                peer.pinged = now;
                Queue(peer, 9, nullptr, 0);
            }
        }
    }

    void Run() {
        std::vector<pollfd> fds;
        std::vector<Peer*> order;
        auto timers = Clock::now();
        while (!stop.load()) {
            fds.clear();
            order.clear();
            pollfd head{};
            head.fd = listener;
            head.events = POLLIN;
            fds.push_back(head);
            for (Peer& peer : peers) {
                pollfd entry{};
                entry.fd = peer.sock;
                entry.events = static_cast<short>(POLLIN | (peer.Queued() ? POLLOUT : 0));
                fds.push_back(entry);
                order.push_back(&peer);
            }
            const int ready = Poll(fds.data(), fds.size(), kTickMs);
            const auto now = Clock::now();
            if (ready > 0) {
                for (std::size_t i = 0; i < order.size(); ++i) {
                    Peer& peer = *order[i];
                    const short got = fds[i + 1].revents;
                    if (peer.dead) continue;
                    if (got & POLLOUT) Flush(peer);
                    if (!peer.dead && (got & (POLLIN | POLLERR | POLLHUP))) Read(peer, now);
                }
                if (fds[0].revents & POLLIN) Accept(now);
            }
            if (now - timers >= std::chrono::milliseconds(kTickMs)) {
                timers = now;
                Timers(now);
            }
            for (auto it = peers.begin(); it != peers.end();) {
                if (!it->dead) {
                    ++it;
                    continue;
                }
                CloseSock(it->sock);
                it = peers.erase(it);
            }
        }
        for (Peer& peer : peers) CloseSock(peer.sock);
        peers.clear();
        members.clear();
        voices.clear();
        away.clear();
    }
};

LocalRoom::LocalRoom() = default;

LocalRoom::~LocalRoom() { Stop(); }

bool LocalRoom::Start(int port, const std::string& room_id, std::string& error, int most) {
    Stop();
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    auto impl = std::make_unique<Impl>();
    impl->room_id = room_id;
    impl->most = std::clamp(most, 1, 127);
    impl->listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl->listener == kBad) {
        error = "no socket";
        return false;
    }
#ifndef _WIN32
    const int reuse = 1;
    ::setsockopt(impl->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(impl->listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(impl->listener, 16) != 0) {
        error = "port " + std::to_string(port) + " cannot be opened: another program may be using it";
        CloseSock(impl->listener);
        return false;
    }
    NonBlocking(impl->listener);
    Impl* raw = impl.get();
    impl->thread = std::thread([raw] { raw->Run(); });
    impl_ = std::move(impl);
    return true;
}

void LocalRoom::Stop() {
    if (!impl_) return;
    impl_->stop = true;
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->listener != kBad) CloseSock(impl_->listener);
    impl_.reset();
}

} // namespace BbRoom
