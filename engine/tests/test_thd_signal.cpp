// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gpu/bbnet/thd/thd_signal.h"
#include "gpu/bbnet/vendor/bbhost_shim/hle/common.h"
#include "gpu/bbnet/vendor/bbhost_shim/hle/net_p2p.h"
#include "gpu/bbnet/vendor/bbhost_shim/hle/np.h"
#include "gpu/bbnet/vendor/bbhost_shim/log.h"
#include "gpu/bbnet/vendor/bbhost_shim/net/session.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;
void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct Timed {
    long long due;
    std::function<void()> fn;
};
long long g_now = 0;
std::vector<Timed> g_queue;
std::map<std::string, void*> g_functions;
std::map<std::uint16_t, net::Peer> g_peers;
struct Sent {
    std::uint8_t kind, reason;
    std::string from, to;
    std::uint64_t nonce;
};
std::vector<Sent> g_sent;
struct Event {
    unsigned ctx, conn;
    std::uint16_t event;
    int error;
    long long at;
};
std::vector<Event> g_events;
std::vector<std::string> g_m2_up;
std::vector<std::pair<std::string, int>> g_m2_dead;

void RunUntil(long long t) {
    for (;;) {
        std::size_t best = g_queue.size();
        for (std::size_t i = 0; i < g_queue.size(); ++i) {
            if (g_queue[i].due <= t && (best == g_queue.size() || g_queue[i].due < g_queue[best].due)) best = i;
        }
        if (best == g_queue.size()) break;
        Timed job = std::move(g_queue[best]);
        g_queue.erase(g_queue.begin() + static_cast<long>(best));
        if (job.due > g_now) g_now = job.due;
        job.fn();
    }
    g_now = t;
}

std::int64_t __attribute__((sysv_abi)) OnEvent(unsigned ctx, unsigned conn, std::uint16_t event, int error, void*) {
    g_events.push_back({ctx, conn, event, error, g_now});
    return 0;
}

std::vector<std::uint8_t> Packet(std::uint8_t kind, const char* from, const char* to, std::uint64_t nonce = 0,
                                 std::uint8_t reason = 1) {
    std::vector<std::uint8_t> p(46, 0);
    std::memcpy(p.data(), kSignalMagic, 4);
    p[4] = kind;
    p[5] = reason;
    std::memcpy(p.data() + 6, from, std::strlen(from));
    std::memcpy(p.data() + 22, to, std::strlen(to));
    std::memcpy(p.data() + 38, &nonce, 8);
    return p;
}

void Deliver(const std::vector<std::uint8_t>& p, std::uint32_t addr, std::uint16_t port_host) {
    const std::uint16_t nbo = static_cast<std::uint16_t>((port_host << 8) | (port_host >> 8));
    hle_np_signaling_control(p.data(), p.size(), addr, nbo);
    RunUntil(g_now);
}

bool SentKind(std::uint8_t kind, const char* to) {
    for (const Sent& s : g_sent)
        if (s.kind == kind && s.to == to) return true;
    return false;
}
const Sent* LastSent(std::uint8_t kind) {
    for (auto it = g_sent.rbegin(); it != g_sent.rend(); ++it)
        if (it->kind == kind) return &*it;
    return nullptr;
}
bool HasEvent(unsigned conn, std::uint16_t event, int error = 0) {
    for (const Event& e : g_events)
        if (e.conn == conn && e.event == event && e.error == error) return true;
    return false;
}

template <typename R, typename... A> R Call(const char* name, A... a) {
    using Fn = R(__attribute__((sysv_abi)) *)(A...);
    return reinterpret_cast<Fn>(g_functions.at(name))(a...);
}

void AddPeer(std::uint16_t member, const char* id, std::uint32_t addr, std::uint16_t port) {
    net::Peer p;
    p.member_id = member;
    p.online_id = id;
    p.addr = addr;
    p.port = port;
    g_peers[member] = p;
}

void Signaling() {
    hle_register_np_signaling();
    Expect(g_functions.size() == 7, "seven sceNpSignaling functions registered");
    Call<int>("sceNpSignalingInitialize", std::uint64_t{0}, 0, 0, std::uint64_t{0});
    unsigned ctx = 0;
    Expect(Call<int>("sceNpSignalingCreateContext", static_cast<const void*>(nullptr), reinterpret_cast<void*>(&OnEvent),
                     static_cast<void*>(nullptr), &ctx) == 0 && ctx,
           "a context");
    AddPeer(2, "playerB", 0x0100007f, 4000);

    char npid[36] = {};
    std::memcpy(npid, "playerB", 7);
    unsigned conn = 0;
    Expect(Call<int>("sceNpSignalingActivateConnection", ctx, static_cast<const void*>(npid), &conn) == 0 && conn,
           "activate gives a connection");
    Expect(conn >= 1 && conn <= 0x7fff, "the connection id is in 1..0x7fff");
    RunUntil(g_now);
    Expect(SentKind(0x01, "playerB") && SentKind(0x11, "playerB"), "ActivationRequest and Offer go out");
    int status = -1;
    Call<int>("sceNpSignalingGetConnectionStatus", ctx, conn, &status, static_cast<std::uint32_t*>(nullptr),
              static_cast<std::uint16_t*>(nullptr));
    Expect(status == 1, "pending during the handshake");

    Deliver(Packet(0x12, "playerB", "playerA"), 0x0100007f, 4000);
    const Sent* check = LastSent(0x13);
    Expect(check != nullptr, "Accept is answered with Check");
    Deliver(Packet(0x14, "playerB", "playerA", check ? check->nonce : 0), 0x0100007f, 4000);
    Expect(SentKind(0x04, "playerB"), "CheckAck: Established goes to the peer");
    Expect(HasEvent(conn, 0x01), "ESTABLISHED for the game");
    Call<int>("sceNpSignalingGetConnectionStatus", ctx, conn, &status, static_cast<std::uint32_t*>(nullptr),
              static_cast<std::uint16_t*>(nullptr));
    Expect(status == 2, "active once up");
    RunUntil(g_now + 1000);
    Expect(!g_m2_up.empty() && g_m2_up.back() == "playerB", "Matching2 hears the transport is up after 1 s");

    Deliver(Packet(0x03, "playerB", "playerA", 0, 1), 0x0100007f, 4000);
    Expect(HasEvent(conn, 0x0b), "a peer's deactivate is PEER_DEACTIVATED");

    Expect(Call<int>("sceNpSignalingDeactivateConnection", ctx, conn) == 0, "deactivate");
    RunUntil(g_now);
    Expect(HasEvent(conn, 0x00, BbThd::kNpUtilErrorNotMatch) || HasEvent(conn, 0x00, static_cast<int>(0x80552718)),
           "DEAD, terminated by myself");
    Expect(Call<int>("sceNpSignalingDeactivateConnection", ctx, conn) == static_cast<int>(0x8055270E),
           "a second deactivate: connection not found");

    AddPeer(3, "playerC", 0x0200007f, 4001);
    char npid_c[36] = {};
    std::memcpy(npid_c, "playerC", 7);
    unsigned silent = 0;
    Call<int>("sceNpSignalingActivateConnection", ctx, static_cast<const void*>(npid_c), &silent);
    RunUntil(g_now + 31000);
    Expect(HasEvent(silent, 0x00, static_cast<int>(0x80552711)), "no answer: DEAD with timeout after 30 s");
    bool m2_dead = false;
    for (const auto& [who, error] : g_m2_dead) m2_dead |= who == "playerC" && error == static_cast<int>(0x80550E11);
    Expect(m2_dead, "and Matching2 hears the timeout");

    g_sent.clear();
    AddPeer(4, "playerD", 0x0300007f, 4002);
    Deliver(Packet(0x01, "playerD", "playerA"), 0x0300007f, 4002);
    Expect(SentKind(0x02, "playerD"), "a peer's ActivationRequest is acknowledged");
    Deliver(Packet(0x11, "playerD", "playerA"), 0x0300007f, 4002);
    Expect(SentKind(0x12, "playerD"), "its Offer is answered with Accept");

    g_sent.clear();
    Deliver(Packet(0x11, "stranger", "playerA"), 0x0400007f, 4003);
    Expect(g_sent.empty(), "an Offer from someone with no transport is ignored");
}

void Timing() {
    unsigned ctx = 0;
    Call<int>("sceNpSignalingCreateContext", static_cast<const void*>(nullptr), reinterpret_cast<void*>(&OnEvent),
              static_cast<void*>(nullptr), &ctx);
    AddPeer(5, "playerE", 0x0500007f, 3658);
    char npid[36] = {};
    std::memcpy(npid, "playerE", 7);
    g_sent.clear();
    const long long start = g_now;
    unsigned conn = 0;
    Call<int>("sceNpSignalingActivateConnection", ctx, static_cast<const void*>(npid), &conn);
    RunUntil(g_now + 40000);
    int offers = 0;
    bool reason_one = true;
    for (const Sent& s : g_sent) {
        if (s.to != "playerE") continue;
        offers += s.kind == 0x11;
        reason_one &= s.reason == 1;
    }
    Expect(reason_one, "byte 5 is 1 on every kind sent");
    Expect(offers == 61, "61 Offers before giving up");
    long long dead_at = -1;
    for (const Event& e : g_events)
        if (e.conn == conn && e.event == 0x00) dead_at = e.at;
    Expect(dead_at - start == 30500, "DEAD at +30500 ms");

    AddPeer(6, "playerF", 0x0600007f, 3658);
    char npid_f[36] = {};
    std::memcpy(npid_f, "playerF", 7);
    unsigned conn_f = 0;
    Call<int>("sceNpSignalingActivateConnection", ctx, static_cast<const void*>(npid_f), &conn_f);
    RunUntil(g_now);
    Deliver(Packet(0x12, "playerF", "playerA"), 0x0600007f, 3658);
    g_sent.clear();
    Deliver(Packet(0x11, "playerF", "playerA"), 0x0600007f, 3658);
    bool accept = false;
    for (const Sent& s : g_sent) accept |= s.kind == 0x12 && s.to == "playerF";
    Expect(accept, "a duplicate Offer during the check is answered with Accept again");
    const Sent* check = nullptr;
    Deliver(Packet(0x13, "playerF", "playerA", 77), 0x0600007f, 3658);
    for (const Sent& s : g_sent)
        if (s.kind == 0x14 && s.to == "playerF") check = &s;
    Expect(check && check->nonce == 77, "a Check is answered with CheckAck carrying its nonce");
    Deliver(Packet(0x14, "playerF", "playerA", 1), 0x0600007f, 3658);
    int status = 0;
    std::uint32_t addr = 0;
    std::uint16_t port = 0;
    Call<int>("sceNpSignalingGetConnectionStatus", ctx, conn_f, &status, &addr, &port);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&port);
    Expect(status == 2 && addr == 0x0600007f && bytes[0] == 0x0E && bytes[1] == 0x4A,
           "status 2 with the address and port 3658 as 0E 4A");
}

void Compare() {
    std::uint8_t a[36] = {}, b[36] = {};
    std::memcpy(a, "hunter", 6);
    std::memcpy(b, "hunter", 6);
    a[10] = 0x55;
    Expect(BbThd::CompareNpId(a, b) == 0, "bytes after the name are ignored");
    b[20] = 1;
    Expect(BbThd::CompareNpId(a, b) == BbThd::kNpUtilErrorNotMatch, "opt differs");
    b[20] = 0;
    b[30] = 1;
    Expect(BbThd::CompareNpId(a, b) == BbThd::kNpUtilErrorNotMatch, "reserved differs");
}

void BigDatagram() {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    const auto receiver = socket(AF_INET, SOCK_DGRAM, 0);
    const auto sender = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Expect(bind(receiver, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0, "binds");
    socklen_t length = sizeof(at);
    getsockname(receiver, reinterpret_cast<sockaddr*>(&at), &length);
    std::vector<char> payload(5 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i * 31);
    const auto sent = sendto(sender, payload.data(), static_cast<int>(payload.size()), 0, reinterpret_cast<sockaddr*>(&at), sizeof(at));
    std::vector<char> buffer(BbThd::kMaxDatagram + 64);
    const auto got = recvfrom(receiver, buffer.data(), static_cast<int>(buffer.size()), 0, nullptr, nullptr);
    Expect(sent == static_cast<decltype(sent)>(payload.size()) && got == static_cast<decltype(got)>(payload.size()) &&
               std::memcmp(buffer.data(), payload.data(), payload.size()) == 0,
           "a 5 KiB datagram goes through whole");
#ifdef _WIN32
    closesocket(receiver);
    closesocket(sender);
    WSACleanup();
#else
    close(receiver);
    close(sender);
#endif
}
}

void host_log(const char* format, ...) {
    if (!std::getenv("THD_TEST_LOG")) return;
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
}
void register_hle_fn(const char* name, void* fn) { g_functions[name] = fn; }
std::string hle_np_npid_text(const void* npid) {
    std::string s(static_cast<const char*>(npid), 16);
    s.resize(std::strlen(s.c_str()));
    return s;
}
void hle_net_p2p_signal(std::uint32_t, std::uint16_t, const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    if (len < 46) return;
    Sent s;
    s.kind = p[4];
    s.reason = p[5];
    s.from.assign(reinterpret_cast<const char*>(p + 6), strnlen(reinterpret_cast<const char*>(p + 6), 16));
    s.to.assign(reinterpret_cast<const char*>(p + 22), strnlen(reinterpret_cast<const char*>(p + 22), 16));
    std::memcpy(&s.nonce, p + 38, 8);
    g_sent.push_back(s);
}
void hle_np_matching2_transport_up(const std::string& online_id) { g_m2_up.push_back(online_id); }
void hle_np_matching2_transport_dead(const std::string& online_id, int error) { g_m2_dead.emplace_back(online_id, error); }
void hle_np_matching2_peer_closed(const std::string&) {}

namespace net {
void dispatch_after(int delay_ms, Prio, std::function<void()> fn) { g_queue.push_back({g_now + delay_ms, std::move(fn)}); }
bool peers_get(std::uint16_t member_id, Peer* out) {
    const auto it = g_peers.find(member_id);
    if (it == g_peers.end()) return false;
    *out = it->second;
    return true;
}
bool peers_find_online(const std::string& id, Peer* out) {
    for (const auto& [m, p] : g_peers)
        if (p.online_id == id) {
            *out = p;
            return true;
        }
    return false;
}
void peers_set_conn(std::uint16_t, unsigned, int) {}
Peer peers_provisional(const std::string& id, std::uint32_t addr, std::uint16_t port) {
    Peer p;
    p.member_id = 0xff00;
    p.online_id = id;
    p.addr = addr;
    p.port = port;
    g_peers[p.member_id] = p;
    return p;
}
bool server_signaling_resolve(const std::string&, std::uint32_t*, std::uint16_t*, std::string& error) {
    error = "test";
    return false;
}
std::string online_id() { return "playerA"; }
}

int main() {
    Signaling();
    Timing();
    Compare();
    BigDatagram();
    if (failures) {
        std::printf("thd-signal-test: %d failed\n", failures);
        return 1;
    }
    std::printf("thd-signal-test: ok\n");
    return 0;
}
