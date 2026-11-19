// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include <SDL3/SDL.h>

#include "gpu/bbnet/room/room_host.h"
#include "gpu/bbnet/room/room_link.h"
#include "gpu/bbnet/room/room_ws.h"
#include "gpu/shim/test_sandbox_net.h"
#include "gpu/shim/voice.h"
#include "room_test_net.h"

namespace {

using LanternSandbox::Target;
using LanternSandbox::Verdict;

constexpr const char* kTest = "sandbox-test";
constexpr const char* kRemoteUrl = "ws://0.0.0.0:9/";
constexpr const char* kRemoteServer = "ws://0.0.0.0:9";
constexpr const char* kLoopbackUrl = "ws://127.0.0.1:1/";
constexpr const char* kRefusal = "refused by the test sandbox";
constexpr int kConnectSeconds = 2;

int failures = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

void SetEnv(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value) setenv(key, value, 1);
    else unsetenv(key);
#endif
    LanternSandbox::Reload();
}

unsigned Sockets() { return LanternSandbox::counters.sockets.load(); }

void Decisions() {
    Check(LanternSandbox::Decide(false, false, Target::Remote) == Verdict::Allow, "without the switch a remote host is allowed");
    Check(LanternSandbox::Decide(true, false, Target::Remote) == Verdict::Refuse, "the sandbox refuses a remote host");
    Check(LanternSandbox::Decide(true, false, Target::Loopback) == Verdict::Refuse,
          "the sandbox refuses loopback without its loopback switch");
    Check(LanternSandbox::Decide(true, true, Target::Loopback) == Verdict::Allow, "the loopback switch allows loopback");
    Check(LanternSandbox::Decide(true, true, Target::Remote) == Verdict::Refuse,
          "the loopback switch still refuses a remote host");
    Check(LanternSandbox::ClassifyHost("127.0.0.1") == Target::Loopback, "127.0.0.1 is loopback");
    Check(LanternSandbox::ClassifyHost("::1") == Target::Loopback, "::1 is loopback");
    Check(LanternSandbox::ClassifyHost("[::1]") == Target::Loopback, "[::1] is loopback");
    for (const char* remote : {"0.0.0.0", "127.0.0.2", "127.0.0.1.example", "localhost", "185.244.28.175", ""})
        Check(LanternSandbox::ClassifyHost(remote) == Target::Remote, "only 127.0.0.1 and ::1 are loopback");
    Check(LanternSandbox::HostOfUrl("wss://relay.example:443/room/ab") == "relay.example", "the host of a wss address");
    Check(LanternSandbox::HostOfUrl("http://127.0.0.1:8787") == "127.0.0.1", "the host of an http address");
    Check(LanternSandbox::HostOfUrl("http://[::1]:80/x") == "[::1]", "the host of an IPv6 address");
    Check(LanternSandbox::HostOfUrl("https://a.example") == "a.example", "the host of an address without a port");
    Check(LanternSandbox::HostOfUrl("http://u@127.0.0.1.evil.example/") == "127.0.0.1.evil.example",
          "a user part does not hide the host");
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(LanternSandbox::ClassifyAddress(reinterpret_cast<const sockaddr*>(&v4), sizeof v4) == Target::Loopback,
          "the IPv4 loopback address is loopback");
    v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK + 1);
    Check(LanternSandbox::ClassifyAddress(reinterpret_cast<const sockaddr*>(&v4), sizeof v4) == Target::Remote,
          "127.0.0.2 is not allowed");
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    reinterpret_cast<unsigned char*>(&v6.sin6_addr)[sizeof v6.sin6_addr - 1] = 1;
    Check(LanternSandbox::ClassifyAddress(reinterpret_cast<const sockaddr*>(&v6), sizeof v6) == Target::Loopback,
          "the IPv6 loopback address is loopback");
    Check(LanternSandbox::ClassifyAddress(nullptr, 0) == Target::Remote, "no address is not loopback");
}

void Voice() {
    const unsigned before = LanternSandbox::counters.voice_refused.load();
    if (LanternSandbox::AllowVoice()) {
        Check(false, "the sandbox lets voice start (BbVoice::Start not called, so no device opens)");
        return;
    }
    BbVoice::Start();
    BbVoice::Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Check(LanternSandbox::counters.voice_refused.load() >= before + 3, "BbVoice::Start refused in the sandbox");
    Check(!BbVoice::Available(), "voice is not available in the sandbox");
    Check(LanternSandbox::counters.recording_opens.load() == 0, "no recording device was opened");
    Check(SDL_WasInit(SDL_INIT_AUDIO) == 0, "SDL audio was never started");
}

void Network(bool loopback) {
    SetEnv(LanternSandbox::kLoopbackSwitch, loopback ? "1" : nullptr);
    const unsigned refused = LanternSandbox::counters.refused.load();
    const unsigned sockets = Sockets();
    std::string error;
    {
        BbRoom::WebSocket socket;
        Check(!socket.Connect(kRemoteUrl, kConnectSeconds, error), "a connect to 0.0.0.0 does not open");
        Check(error == kRefusal, "a connect to 0.0.0.0 is refused by the sandbox");
    }
    Check(Sockets() == sockets, "no socket was created for a non-loopback address");
    {
        BbRoom::Link link;
        Check(!link.Start(kRemoteServer, "sandbox-room", "secret"), "the room link does not start");
        Check(link.LastError() == kRefusal, "the room link is refused by the sandbox");
        link.Stop();
    }
    Check(Sockets() == sockets, "the room link created no socket");
    Check(LanternSandbox::counters.refused.load() >= refused + 2, "the refusals were counted");
    BbRoom::WebSocket local;
    error.clear();
    Check(!local.Connect(kLoopbackUrl, kConnectSeconds, error), "nothing listens on 127.0.0.1:1");
    if (loopback) {
        Check(error != kRefusal, "the loopback switch lets a test fake on 127.0.0.1 be reached");
        Check(Sockets() == sockets + 1, "the loopback connect made its one socket");
    } else {
        Check(error == kRefusal, "loopback is refused without its loopback switch");
        Check(Sockets() == sockets, "no socket for loopback without its loopback switch");
    }
}

#ifdef _WIN32
using TestSock = SOCKET;
constexpr TestSock kNoSock = INVALID_SOCKET;
void CloseTestSock(TestSock s) { closesocket(s); }
#else
using TestSock = int;
constexpr TestSock kNoSock = -1;
void CloseTestSock(TestSock s) { ::close(s); }
#endif

bool BindsToLoopback() {
    SetEnv(LanternSandbox::kLoopbackSwitch, "1");
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    const unsigned rebinds = LanternSandbox::counters.rebinds.load();
    const bool allowed = LanternSandbox::LocalBind(reinterpret_cast<sockaddr*>(&any), sizeof any, "sandbox test bind");
    if (!allowed || ntohl(any.sin_addr.s_addr) != INADDR_LOOPBACK) {
        Check(false, "a bind to every address is moved to 127.0.0.1 before it happens (nothing was bound)");
        return false;
    }
    Check(LanternSandbox::counters.rebinds.load() == rebinds + 1, "the moved bind was counted");
    sockaddr_in6 any6{};
    any6.sin6_family = AF_INET6;
    Check(LanternSandbox::LocalBind(reinterpret_cast<sockaddr*>(&any6), sizeof any6, "sandbox test bind") &&
              reinterpret_cast<const unsigned char*>(&any6.sin6_addr)[sizeof any6.sin6_addr - 1] == 1,
          "an IPv6 bind to every address is moved to ::1");
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = htonl(0xc0000201u);
    Check(!LanternSandbox::LocalBind(reinterpret_cast<sockaddr*>(&remote), sizeof remote, "sandbox test bind"),
          "a bind to a non-loopback address is refused");
    const TestSock s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSock) {
        Check(false, "a test socket for the bind check");
        return false;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof bound;
    const bool ok = ::bind(s, reinterpret_cast<sockaddr*>(&any), sizeof any) == 0 &&
                    ::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == 0;
    CloseTestSock(s);
    Check(ok && ntohl(bound.sin_addr.s_addr) == INADDR_LOOPBACK, "getsockname shows the moved bind on 127.0.0.1");
    const TestSock unbound = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (unbound == kNoSock) {
        Check(false, "a test socket for the unbound check");
        return false;
    }
    sockaddr_in auto_bound{};
    len = sizeof auto_bound;
    const bool bound_first = LanternSandbox::BindIfUnbound(unbound, "sandbox test send") &&
                             ::getsockname(unbound, reinterpret_cast<sockaddr*>(&auto_bound), &len) == 0;
    CloseTestSock(unbound);
    Check(bound_first && ntohl(auto_bound.sin_addr.s_addr) == INADDR_LOOPBACK && auto_bound.sin_port != 0,
          "an unbound socket is bound to 127.0.0.1 before its first send or listen");
    return true;
}

void RoomHostBindsLoopback() {
    BbRoom::LocalRoom room;
    std::string error;
    const int port = RoomTestNet::FreePort();
    const unsigned rebinds = LanternSandbox::counters.rebinds.load();
    Check(room.Start(port, "sandbox-room", error), "the room host listener starts under the sandbox's loopback switch");
    Check(LanternSandbox::counters.rebinds.load() == rebinds + 1, "the room host's every-address listener was moved to 127.0.0.1");
    room.Stop();
    SetEnv(LanternSandbox::kLoopbackSwitch, nullptr);
    error.clear();
    Check(!room.Start(port, "sandbox-room", error) && error == kRefusal,
          "the room host listener is refused without the loopback switch");
}

}

int main(int argc, char** argv) {
#ifdef LANTERN_SANDBOX_MUTANTS
    if (argc != 2) {
        std::printf("FAIL: %s needs the mutant number\n", kTest);
        return 1;
    }
    LanternSandbox::mutant = std::atoi(argv[1]);
#else
    (void)argv;
    if (argc != 1) {
        std::printf("FAIL: %s takes no arguments\n", kTest);
        return 1;
    }
#endif
    SetEnv(LanternSandbox::kSwitch, "1");
    SetEnv(LanternSandbox::kLoopbackSwitch, nullptr);
    RoomTestNet::StartSockets();
    Decisions();
    Voice();
    Network(false);
    Network(true);
    if (BindsToLoopback()) RoomHostBindsLoopback();
    Check(LanternSandbox::counters.recording_opens.load() == 0, "no recording device was opened in the whole run");
    Check(SDL_WasInit(SDL_INIT_AUDIO) == 0, "SDL audio was never started in the whole run");
    if (failures) {
        std::printf("%s: %d failed\n", kTest, failures);
        return 1;
    }
    std::printf("%s: ok (voice refused, no recording device, binds moved to 127.0.0.1, %u sockets: the loopback connect and the room host listener)\n", kTest, Sockets());
    return 0;
}
