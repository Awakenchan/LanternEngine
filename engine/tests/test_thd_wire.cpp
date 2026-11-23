// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbnet_test_symbols.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
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
#define _dup dup
#define _dup2 dup2
#define _fileno fileno
using RawSocket = int;
constexpr RawSocket kNoSocket = -1;
void CloseSocket(RawSocket s) { ::close(s); }
void ShutSocket(RawSocket s) { shutdown(s, SHUT_RDWR); }
#endif

extern "C" void bbnet_register(void);
extern "C" void bbnet_initialize(void);

namespace BbThd {
bool RefusalNotice(std::string& text);
}

#define GUEST __attribute__((sysv_abi))

namespace {

constexpr int kWaitMs = 15000;
constexpr const char* kToken = "test-token-0123456789";
constexpr const char* kOnlineId = "TestHunter";
constexpr std::uint64_t kRoomId = 4242;

struct Request {
    std::string step;
    std::string method, target;
    std::vector<std::string> headers;
    std::string body;
};

class FakeServer {
public:
    bool Start() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kNoSocket) return false;
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        at.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&at), sizeof at) != 0 || ::listen(listener_, 16) != 0) return false;
        socklen_t size = sizeof at;
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&at), &size) != 0) return false;
        port_ = ntohs(at.sin_port);
        accept_ = std::thread([this] { Accept(); });
        return true;
    }

    void Stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        // Linux: closing the listener does not wake the accept() blocked on it; shutting it down does.
        ShutSocket(listener_);
        CloseSocket(listener_);
        if (accept_.joinable()) accept_.join();
        std::vector<std::thread> handlers;
        {
            std::lock_guard lock(mutex_);
            handlers.swap(handlers_);
        }
        for (auto& t : handlers) t.join();
    }

    int Port() const { return port_; }

    void SetStep(const std::string& step) {
        std::lock_guard lock(mutex_);
        step_ = step;
    }

    bool WaitFor(const std::string& path, std::size_t count) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(kWaitMs), [&] { return CountLocked(path) >= count; });
    }

    std::vector<Request> Requests() {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    void RefuseContextStart() {
        std::lock_guard lock(mutex_);
        refuse_ = true;
    }

    std::size_t RequestsAfterRefusal() {
        std::lock_guard lock(mutex_);
        return refused_sent_ ? requests_.size() - first_refusal_at_ : 0;
    }

    bool RefusalSent() {
        std::lock_guard lock(mutex_);
        return refused_sent_;
    }

private:
    std::size_t CountLocked(const std::string& path) const {
        std::size_t n = 0;
        for (const Request& r : requests_)
            if (r.target == path) ++n;
        return n;
    }

    void Accept() {
        for (;;) {
            const RawSocket peer = ::accept(listener_, nullptr, nullptr);
            if (peer == kNoSocket) return;
            std::lock_guard lock(mutex_);
            if (stopping_) {
                CloseSocket(peer);
                return;
            }
            handlers_.emplace_back([this, peer] { Serve(peer); });
        }
    }

    static bool ReadRequest(RawSocket peer, std::string& head, std::string& body) {
        std::string data;
        char buffer[4096];
        std::size_t end = std::string::npos;
        while ((end = data.find("\r\n\r\n")) == std::string::npos) {
            const int n = ::recv(peer, buffer, sizeof buffer, 0);
            if (n <= 0) return false;
            data.append(buffer, static_cast<std::size_t>(n));
        }
        head = data.substr(0, end);
        body = data.substr(end + 4);
        std::size_t length = 0;
        std::istringstream lines(head);
        std::string line;
        while (std::getline(lines, line)) {
            std::string lower = line;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower.rfind("content-length:", 0) == 0) length = std::strtoull(line.c_str() + 15, nullptr, 10);
        }
        while (body.size() < length) {
            const int n = ::recv(peer, buffer, sizeof buffer, 0);
            if (n <= 0) return false;
            body.append(buffer, static_cast<std::size_t>(n));
        }
        return true;
    }

    bool Refuses(const std::string& target) {
        if (target != "/mp/matching2/context_start" && target != "/np/events/poll") return false;
        std::unique_lock lock(mutex_);
        if (!refuse_) return false;
        if (target == "/mp/matching2/context_start")
            cv_.wait_for(lock, std::chrono::milliseconds(kWaitMs), [&] { return stopping_ || CountLocked("/np/events/poll") >= 1; });
        else
            cv_.wait_for(lock, std::chrono::milliseconds(kWaitMs), [&] { return stopping_ || refused_sent_; });
        return true;
    }

    std::string Answer(const std::string& target) {
        if (target == "/auth/whoami") return R"({"ResKind":0,"OnlineId":"TestHunter"})";
        if (target == "/mp/matching2/context_start") return R"({"ResKind":0})";
        if (target == "/mp/matching2/create_room")
            return R"({"ResKind":0,"RoomId":4242,"SessionId":"session-host","MemberId":1,"MaxMembers":4})";
        if (target == "/mp/matching2/join_room")
            return R"({"ResKind":0,"SessionId":"session-guest","MemberId":2,"OwnerMemberId":1,"MaxMembers":4,)"
                   R"("Members":[{"MemberId":1,"OnlineId":"HostHunter","Addr":"127.0.0.1","Port":9,"LocalAddr":"127.0.0.1","LocalPort":9}]})";
        if (target == "/mp/matching2/leave_room") return R"({"ResKind":0})";
        if (target == "/mp/matching2/heartbeat") return R"({"ResKind":0,"InRoom":1})";
        if (target == "/np/signaling/resolve") return R"({"ResKind":0,"Addr":"127.0.0.1","Port":9})";
        if (target == "/np/events/poll") {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_; });
            return R"({"ResKind":0,"HasEvent":0,"NextCursor":0})";
        }
        return R"({"ResKind":0,"Game":"answer"})";
    }

    void Serve(RawSocket peer) {
        std::string head, body;
        if (!ReadRequest(peer, head, body)) {
            CloseSocket(peer);
            return;
        }
        Request r;
        std::istringstream lines(head);
        std::string line;
        std::getline(lines, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto space = line.find(' ');
        const auto second = line.find(' ', space + 1);
        r.method = line.substr(0, space);
        r.target = line.substr(space + 1, second - space - 1);
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            r.headers.push_back(line);
        }
        r.body = body;
        {
            std::lock_guard lock(mutex_);
            r.step = r.target == "/np/events/poll" || r.target == "/mp/matching2/heartbeat" ? "background" : step_;
            requests_.push_back(r);
        }
        cv_.notify_all();
        const bool refused = Refuses(r.target);
        const std::string reply = refused ? R"({"ResKind":1,"Message":"expired"})" : Answer(r.target);
        const std::string status = refused ? "401 Unauthorized" : "200 OK";
        const std::string answer = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                                   std::to_string(reply.size()) + "\r\nConnection: close\r\n\r\n" + reply;
        if (refused) {
            {
                std::lock_guard lock(mutex_);
                if (!refused_sent_) {
                    refused_sent_ = true;
                    first_refusal_at_ = requests_.size();
                }
            }
            cv_.notify_all();
        }
        ::send(peer, answer.data(), static_cast<int>(answer.size()), 0);
        CloseSocket(peer);
    }

    RawSocket listener_ = kNoSocket;
    int port_ = 0;
    std::thread accept_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::thread> handlers_;
    std::vector<Request> requests_;
    std::string step_ = "start";
    bool stopping_ = false;
    bool refuse_ = false;
    bool refused_sent_ = false;
    std::size_t first_refusal_at_ = 0;
};

class StunCounter {
public:
    bool Start() {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == kNoSocket) return false;
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&at), sizeof at) != 0) return false;
        socklen_t size = sizeof at;
        if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&at), &size) != 0) return false;
        port_ = ntohs(at.sin_port);
        reader_ = std::thread([this] {
            char buffer[2048];
            for (;;) {
                const int n = ::recv(socket_, buffer, sizeof buffer, 0);
                if (n < 0 || (n == 4 && std::memcmp(buffer, "stop", 4) == 0)) return;
                datagrams_.fetch_add(1);
            }
        });
        return true;
    }

    void Stop() {
        const RawSocket poke = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        at.sin_port = htons(static_cast<std::uint16_t>(port_));
        ::sendto(poke, "stop", 4, 0, reinterpret_cast<sockaddr*>(&at), sizeof at);
        CloseSocket(poke);
        if (reader_.joinable()) reader_.join();
        CloseSocket(socket_);
    }

    int Port() const { return port_; }
    std::size_t Datagrams() const { return datagrams_.load(); }

private:
    RawSocket socket_ = kNoSocket;
    int port_ = 0;
    std::thread reader_;
    std::atomic<std::size_t> datagrams_{0};
};

struct Callbacks {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> seen;

    void Note(const std::string& text) {
        {
            std::lock_guard lock(mutex);
            seen.push_back(text);
        }
        cv.notify_all();
    }

    bool WaitFor(const std::string& text) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(kWaitMs), [&] {
            for (const auto& s : seen)
                if (s == text) return true;
            return false;
        });
    }
};
Callbacks g_callbacks;

std::string Hex(std::int64_t value) {
    char text[32];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(static_cast<std::uint32_t>(value)));
    return text;
}

GUEST std::int64_t RequestDone(std::int64_t, std::int64_t req, std::int64_t event, std::int64_t error, std::int64_t,
                                std::int64_t) {
    g_callbacks.Note("request " + std::to_string(req) + " event " + Hex(event) + " error " + Hex(error));
    return 0;
}
GUEST std::int64_t ContextEvent(std::int64_t, std::int64_t event, std::int64_t, std::int64_t error, std::int64_t) {
    g_callbacks.Note("context event " + Hex(event) + " error " + Hex(error));
    return 0;
}
GUEST std::int64_t Ignored(std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t) {
    return 0;
}

struct RequestOptParam {
    void* cbFunc;
    void* cbFuncArg;
    std::uint32_t timeout;
    std::uint16_t appReqId;
    std::uint16_t pad;
};

int failures = 0;
void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

template <typename F>
F Fn(const char* nid) {
    const std::uint64_t address = BbNetTest::Find(nid);
    if (!address) {
        std::printf("FAIL: %s is not registered\n", nid);
        std::exit(1);
    }
    return reinterpret_cast<F>(address);
}

void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::string ReadAll(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    std::string out;
    for (const char c : text.str())
        if (c != '\r') out.push_back(c);
    return out;
}

std::string Normalised(std::string line, int port) {
    const std::string own = "127.0.0.1:" + std::to_string(port);
    for (std::size_t at = line.find(own); at != std::string::npos; at = line.find(own, at))
        line.replace(at, own.size(), "127.0.0.1:<port>");
    return line;
}

std::string Describe(const Request& r, int port) {
    std::string out = r.method + " " + r.target + "\n";
    // Sorted: the order of the headers is the HTTP client's map order, which differs between
    // standard libraries (libc++ on Windows, libstdc++ on Linux) and means nothing on the wire.
    std::vector<std::string> headers;
    for (const auto& h : r.headers) headers.push_back(Normalised(h, port));
    std::sort(headers.begin(), headers.end());
    for (const auto& h : headers) out += "  " + h + "\n";
    out += "  body: " + Normalised(r.body, port) + "\n";
    return out;
}

void DriveNp() {
    using GetState = std::int32_t(GUEST*)(int, int*);
    using GetOnlineId = std::int32_t(GUEST*)(int, void*);
    int state = 0;
    Check(Fn<GetState>("eQH7nWPcAgc")(1, &state) == 0 && state == 2, "sceNpGetState reads signed in");
    char id[20] = {};
    Check(Fn<GetOnlineId>("XDncXQIJUSk")(1, id) == 0 && std::strcmp(id, kOnlineId) == 0, "sceNpGetOnlineId is the account");
    g_callbacks.Note("np state " + std::to_string(state) + " online id " + id);
}

void DriveSignaling() {
    using Init = std::int32_t(GUEST*)(std::uint64_t, int, int, std::uint64_t);
    using Create = std::int32_t(GUEST*)(const void*, void*, void*, unsigned*);
    using Activate = std::int32_t(GUEST*)(unsigned, const void*, unsigned*);
    Check(Fn<Init>("3KOuC4RmZZU")(0x10000, 0, 0, 0) == 0, "sceNpSignalingInitialize");
    std::uint8_t self[36] = {}, peer[36] = {};
    std::memcpy(self, kOnlineId, std::strlen(kOnlineId));
    std::memcpy(peer, "SummonedHunter", 14);
    unsigned ctx = 0, conn = 0;
    Check(Fn<Create>("5yYjEdd4t8Y")(self, reinterpret_cast<void*>(&Ignored), nullptr, &ctx) == 0, "sceNpSignalingCreateContext");
    Check(Fn<Activate>("0UvTFeomAUM")(ctx, peer, &conn) == 0 && conn != 0, "sceNpSignalingActivateConnection");
}

}

std::size_t CountLines(const std::string& text, const std::string& part) {
    std::size_t n = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
        if (line.find(part) != std::string::npos) ++n;
    return n;
}

void DriveAfterSignIn() {
    using M2Init = std::int32_t(GUEST*)(const void*);
    using M2Create = std::int32_t(GUEST*)(const void*, unsigned*);
    using M2RegisterContext = std::int32_t(GUEST*)(void*, void*);
    using M2Start = std::int32_t(GUEST*)(unsigned, unsigned);
    using M2Request = std::int32_t(GUEST*)(unsigned, const void*, const RequestOptParam*, unsigned*);
    using HttpInit = std::int32_t(GUEST*)(int, int, std::uint64_t);
    using HttpTemplate = std::int32_t(GUEST*)(int, const char*, int, int);
    using HttpConnection = std::int32_t(GUEST*)(int, const char*, bool);
    using HttpRequest = std::int32_t(GUEST*)(int, std::int32_t, const char*, std::uint64_t);
    using HttpSend = std::int32_t(GUEST*)(int, const void*, std::uint64_t);
    Fn<M2Init>("10t3e5+JPnU")(nullptr);
    unsigned ctx = 0;
    Fn<M2Create>("YfmpW719rMo")(nullptr, &ctx);
    Fn<M2RegisterContext>("fQQfP87I7hs")(reinterpret_cast<void*>(&ContextEvent), nullptr);
    Fn<M2Start>("7vjNQ6Z1op0")(ctx, 0);
    g_callbacks.WaitFor("context event 0x6f02 error 0x0");
    using SignalingInit = std::int32_t(GUEST*)(std::uint64_t, int, int, std::uint64_t);
    using SignalingCreate = std::int32_t(GUEST*)(const void*, void*, void*, unsigned*);
    using SignalingActivate = std::int32_t(GUEST*)(unsigned, const void*, unsigned*);
    Fn<SignalingInit>("3KOuC4RmZZU")(0x10000, 0, 0, 0);
    std::uint8_t self[36] = {}, peer[36] = {};
    std::memcpy(self, kOnlineId, std::strlen(kOnlineId));
    std::memcpy(peer, "SummonedHunter", 14);
    unsigned signaling = 0, connection = 0;
    Fn<SignalingCreate>("5yYjEdd4t8Y")(self, reinterpret_cast<void*>(&Ignored), nullptr, &signaling);
    Fn<SignalingActivate>("0UvTFeomAUM")(signaling, peer, &connection);
    const RequestOptParam opt{reinterpret_cast<void*>(&RequestDone), nullptr, 0, 0, 0};
    unsigned req = 0;
    const std::uint16_t max_slot = 4;
    Fn<M2Request>("zCWZmXXN600")(ctx, &max_slot, &opt, &req);
    const std::uint64_t room = kRoomId;
    Fn<M2Request>("CSIMDsVjs-g")(ctx, &room, &opt, &req);
    Fn<M2Request>("BD6kfx442Do")(ctx, nullptr, &opt, &req);
    const char* url = "https://ss4.scej-network.jp:20443/fs4/login.spd";
    const std::string post = "game-body-0123";
    const int http = Fn<HttpInit>("A9cVMUtEp4Y")(1, 1, 0x40000);
    const int tmpl = Fn<HttpTemplate>("0gYjPTR-6cY")(http, "Bloodborne/1.09", 2, 1);
    const int conn = Fn<HttpConnection>("qgxDBjorUxs")(tmpl, url, true);
    const int hreq = Fn<HttpRequest>("Aeu5wVKkF9w")(conn, 1, url, post.size());
    Fn<HttpSend>("1e2BNwI-XzE")(hreq, post.data(), post.size());
}

int RunRefused(FakeServer& server, StunCounter& stun, bool before_start, const std::filesystem::path& scratch) {
    using GetState = std::int32_t(GUEST*)(int, int*);
    const std::filesystem::path log_path = scratch / (before_start ? "refused-start.log" : "refused-in-game.log");
    std::FILE* log = std::fopen(log_path.string().c_str(), "wb");
    if (!log) {
        std::printf("FAIL: the log file %s did not open\n", log_path.string().c_str());
        return 1;
    }
    std::fflush(stdout);
    const int saved = _dup(_fileno(stdout));
    if (saved < 0 || _dup2(_fileno(log), _fileno(stdout)) < 0) {
        std::printf("FAIL: the engine's output could not be captured\n");
        return 1;
    }
    if (before_start) {
        SetEnvironment("BB_THD_REFUSED", "expired");
    } else {
        server.RefuseContextStart();
    }
    BbNetTest::ClearSymbols();
    bbnet_register();
    bbnet_initialize();
    int first_state = 0, state = 0;
    Fn<GetState>("eQH7nWPcAgc")(1, &first_state);
    DriveAfterSignIn();
    std::size_t stun_before = 0;
    if (!before_start) {
        server.WaitFor("/mp/matching2/context_start", 1);
        for (int i = 0; i < kWaitMs / 10 && !server.RefusalSent(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        stun_before = stun.Datagrams();
    }
    std::string notice;
    bool shown = BbThd::RefusalNotice(notice);
    for (int i = 0; !before_start && !shown && i < kWaitMs / 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        shown = BbThd::RefusalNotice(notice);
    }
    if (!before_start) DriveAfterSignIn();
    std::this_thread::sleep_for(std::chrono::milliseconds(17000));
    Fn<GetState>("eQH7nWPcAgc")(1, &state);
    if (before_start) shown = BbThd::RefusalNotice(notice);
    std::fflush(stdout);
    _dup2(saved, _fileno(stdout));
    std::fclose(log);
    server.Stop();
    stun.Stop();
    const std::size_t stun_after = stun.Datagrams() - stun_before;
    const std::string text = ReadAll(log_path.string());
    std::fwrite(text.data(), 1, text.size(), stdout);
    const auto requests = server.Requests();
    std::map<std::string, std::size_t> per_target;
    for (const Request& r : requests) ++per_target[r.target];
    const std::size_t refused_lines = CountLines(text, "THD: the server refused this PC's sign-in as TestHunter (expired): sign in again");
    const std::size_t start_lines = CountLines(text, "THD: The Hunter's Dream did not take this PC's sign-in before the start (expired)");
    Check(text.find(kToken) == std::string::npos, "nothing logs the token");
    Check(state == 1, "sceNpGetState reads signed out after the refusal");
    if (before_start) {
        Check(first_state == 1, "sceNpGetState reads signed out from the start");
        Check(requests.empty(), "a start the launcher found refused sends nothing to The Hunter's Dream");
        Check(stun.Datagrams() == 0, "a start the launcher found refused sends no STUN");
        Check(start_lines == 1, "the start says once that the sign-in was refused");
        Check(refused_lines == 0, "no in-game refusal line for the launcher to prompt about again");
        Check(!shown, "no in-game banner: the launcher said it before the start");
    } else {
        Check(first_state == 2, "sceNpGetState reads signed in before the refusal");
        Check(requests.size() == 2 && per_target["/mp/matching2/context_start"] == 1 && per_target["/np/events/poll"] == 1,
              "context_start and the one poll beside it, nothing else");
        Check(server.RequestsAfterRefusal() == 0, "nothing reaches the server after the first 401");
        Check(stun_before > 0, "STUN was asked before the refusal, so the count below can see it");
        Check(stun_after == 0, "no STUN after the first 401, past the 15 s keepalive");
        Check(refused_lines == 1, "the refusal is logged once, in the line the launcher reads");
        Check(shown && notice == "Sign-in expired: sign in again on the launcher's Online page", "the in-game banner says what to do");
    }
    std::string counts;
    for (const auto& [target, n] : per_target) counts += " " + target + "=" + std::to_string(n);
    std::printf("thd-wire-test %s: %s (%zu requests:%s; STUN datagrams %zu before the refusal, %zu after)\n",
                before_start ? "refused at start" : "refused in game", failures ? "FAIL" : "ok", requests.size(), counts.c_str(),
                stun_before, stun_after);
    std::fflush(stdout);
    std::error_code gone;
    std::filesystem::remove_all(scratch, gone);
    std::_Exit(failures ? 1 : 0);
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 && std::strncmp(argv[1], "--refused", 9) == 0 ? argv[1] : "";
    const std::string golden = argc > 1 && mode.empty() ? argv[1] : "tests/golden/thd-wire.txt";
    const bool record = argc > 2 && std::strcmp(argv[2], "--record") == 0;
    if (!mode.empty() && mode != "--refused-start" && mode != "--refused-in-game") {
        std::printf("FAIL: unknown mode %s\n", mode.c_str());
        return 1;
    }
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    FakeServer server;
    if (!server.Start()) {
        std::printf("FAIL: the fake server did not start\n");
        return 1;
    }
    std::error_code made;
    const std::filesystem::path scratch = std::filesystem::absolute("out/test-tmp/thd-wire-" + std::to_string(server.Port()), made);
    if (made || (std::filesystem::create_directories(scratch, made), made)) {
        std::printf("FAIL: the scratch folder out/test-tmp could not be made\n");
        return 1;
    }
    const std::string base = "http://127.0.0.1:" + std::to_string(server.Port());
    const std::filesystem::path overrides = scratch / "overrides.json";
    {
        std::ofstream out(overrides, std::ios::binary);
        out << "{\n  \"*.scej-network.jp\": \"" << base << "\"\n}\n";
        if (!out) {
            std::printf("FAIL: %s could not be written\n", overrides.string().c_str());
            return 1;
        }
    }
    SetEnvironment("BB_ONLINE", "1");
    SetEnvironment("BB_NET_BACKEND", "thd");
    SetEnvironment("BB_THD_SERVER", base);
    StunCounter stun;
    if (!mode.empty() && !stun.Start()) {
        std::printf("FAIL: the fake STUN port did not open\n");
        return 1;
    }
    SetEnvironment("BB_THD_STUN", mode.empty() ? std::string("off") : "127.0.0.1:" + std::to_string(stun.Port()));
    SetEnvironment("BB_THD_VERIFY_TLS", "0");
    SetEnvironment("BB_CLIENT_VERSION", "shadPS4 Awakenchan/v0.20.0");
    SetEnvironment("BB_THD_TOKEN", kToken);
    SetEnvironment("BB_THD_ONLINE_ID", kOnlineId);
    SetEnvironment("BB_THD_RULESET", "vanilla");
    SetEnvironment("SHADPS4_HTTP_HOST_OVERRIDES_JSON", overrides.string());
    if (!mode.empty()) return RunRefused(server, stun, mode == "--refused-start", scratch);

    BbNetTest::ClearSymbols();
    bbnet_register();

    server.SetStep("game start: bbnet_initialize");
    bbnet_initialize();

    server.SetStep("in game: sceNpGetState, sceNpGetOnlineId");
    DriveNp();

    using M2Init = std::int32_t(GUEST*)(const void*);
    using M2Create = std::int32_t(GUEST*)(const void*, unsigned*);
    using M2RegisterContext = std::int32_t(GUEST*)(void*, void*);
    using M2RegisterEvent = std::int32_t(GUEST*)(unsigned, void*, void*);
    using M2Start = std::int32_t(GUEST*)(unsigned, unsigned);
    using M2Request = std::int32_t(GUEST*)(unsigned, const void*, const RequestOptParam*, unsigned*);
    using M2Terminate = std::int32_t(GUEST*)();
    server.SetStep("in game: Matching2 Initialize, CreateContext, ContextStart");
    Check(Fn<M2Init>("10t3e5+JPnU")(nullptr) == 0, "sceNpMatching2Initialize");
    unsigned ctx = 0;
    Check(Fn<M2Create>("YfmpW719rMo")(nullptr, &ctx) == 0 && ctx != 0, "sceNpMatching2CreateContext");
    Fn<M2RegisterContext>("fQQfP87I7hs")(reinterpret_cast<void*>(&ContextEvent), nullptr);
    Fn<M2RegisterEvent>("p+2EnxmaAMM")(ctx, reinterpret_cast<void*>(&Ignored), nullptr);
    Fn<M2RegisterEvent>("0UMeWRGnZKA")(ctx, reinterpret_cast<void*>(&Ignored), nullptr);
    Check(Fn<M2Start>("7vjNQ6Z1op0")(ctx, 0) == 0, "sceNpMatching2ContextStart");
    Check(server.WaitFor("/mp/matching2/context_start", 1), "context_start reaches the server");
    Check(g_callbacks.WaitFor("context event 0x6f02 error 0x0"), "the game hears CONTEXT_STARTED");
    Check(server.WaitFor("/np/events/poll", 1), "the event poll starts");

    server.SetStep("in game: signaling activate (a peer outside the room)");
    DriveSignaling();
    Check(server.WaitFor("/np/signaling/resolve", 1), "the peer's address is asked for");

    const RequestOptParam opt{reinterpret_cast<void*>(&RequestDone), nullptr, 0, 0, 0};
    unsigned req = 0;
    server.SetStep("in game: Matching2 CreateJoinRoom");
    const std::uint16_t max_slot = 4;
    Check(Fn<M2Request>("zCWZmXXN600")(ctx, &max_slot, &opt, &req) == 0, "sceNpMatching2CreateJoinRoom");
    Check(g_callbacks.WaitFor("request " + std::to_string(req) + " event 0x101 error 0x0"), "the room is created");

    server.SetStep("in game: Matching2 SearchRoom");
    Check(Fn<M2Request>("VqZX7POg2Mk")(ctx, nullptr, &opt, &req) == 0, "sceNpMatching2SearchRoom");
    Check(g_callbacks.WaitFor("request " + std::to_string(req) + " event 0x106 error 0x80550c28"), "SearchRoom answers nothing found");

    server.SetStep("in game: Matching2 LeaveRoom (host)");
    Check(Fn<M2Request>("BD6kfx442Do")(ctx, nullptr, &opt, &req) == 0, "sceNpMatching2LeaveRoom");
    Check(g_callbacks.WaitFor("request " + std::to_string(req) + " event 0x103 error 0x0"), "the host leaves");
    Check(server.WaitFor("/mp/matching2/leave_room", 1), "leave_room reaches the server");

    server.SetStep("in game: Matching2 JoinRoom");
    const std::uint64_t room = kRoomId;
    Check(Fn<M2Request>("CSIMDsVjs-g")(ctx, &room, &opt, &req) == 0, "sceNpMatching2JoinRoom");
    Check(g_callbacks.WaitFor("request " + std::to_string(req) + " event 0x102 error 0x0"), "the room is joined");

    server.SetStep("in game: Matching2 LeaveRoom (guest)");
    Check(Fn<M2Request>("BD6kfx442Do")(ctx, nullptr, &opt, &req) == 0, "sceNpMatching2LeaveRoom");
    Check(g_callbacks.WaitFor("request " + std::to_string(req) + " event 0x103 error 0x0"), "the guest leaves");
    Check(server.WaitFor("/mp/matching2/leave_room", 2), "the second leave_room reaches the server");

    server.SetStep("in game: one WebAPI request");
    using WebInit = std::int32_t(GUEST*)(int, std::uint64_t);
    using WebContext = std::int32_t(GUEST*)(int, const void*);
    using WebCreate = std::int32_t(GUEST*)(int, const char*, const char*, int, const void*, std::int64_t*);
    using WebSend = std::int32_t(GUEST*)(std::int64_t, const void*, std::uint64_t);
    using WebStatus = std::int32_t(GUEST*)(std::int64_t, int*);
    using WebRead = std::int32_t(GUEST*)(std::int64_t, void*, std::uint64_t);
    using WebDelete = std::int32_t(GUEST*)(std::int64_t);
    const int web = Fn<WebInit>("G3AnLNdRBjE")(1, 0x10000);
    std::uint8_t self[36] = {};
    std::memcpy(self, kOnlineId, std::strlen(kOnlineId));
    const int web_ctx = Fn<WebContext>("x1Y7yiYSk7c")(web, self);
    std::int64_t web_req = 0;
    Check(Fn<WebCreate>("rdgs5Z1MyFw")(web_ctx, "userProfile", "/v1/users/me/friendList", 0, nullptr, &web_req) == 0,
          "sceNpWebApiCreateRequest");
    Check(Fn<WebSend>("kVbL4hL3K7w")(web_req, nullptr, 0) == 0, "sceNpWebApiSendRequest");
    int web_status = 0;
    char web_body[128] = {};
    Fn<WebStatus>("k210oKgP80Y")(web_req, &web_status);
    const int read = Fn<WebRead>("CQtPRSF6Ds8")(web_req, web_body, sizeof web_body - 1);
    Check(web_status == 200 && read > 0, "WebAPI answers here");
    g_callbacks.Note("webapi status " + std::to_string(web_status) + " body " + web_body);
    Fn<WebDelete>("noQgleu+KLE")(web_req);

    server.SetStep("in game: one sceHttp request");
    using HttpInit = std::int32_t(GUEST*)(int, int, std::uint64_t);
    using HttpTemplate = std::int32_t(GUEST*)(int, const char*, int, int);
    using HttpConnection = std::int32_t(GUEST*)(int, const char*, bool);
    using HttpRequest = std::int32_t(GUEST*)(int, std::int32_t, const char*, std::uint64_t);
    using HttpHeader = std::int32_t(GUEST*)(int, const char*, const char*, std::int32_t);
    using HttpSend = std::int32_t(GUEST*)(int, const void*, std::uint64_t);
    using HttpStatus = std::int32_t(GUEST*)(int, int*);
    using HttpDelete = std::int32_t(GUEST*)(int);
    const char* url = "https://ss4.scej-network.jp:20443/fs4/login.spd";
    const std::string post = "game-body-0123";
    const int http = Fn<HttpInit>("A9cVMUtEp4Y")(1, 1, 0x40000);
    const int tmpl = Fn<HttpTemplate>("0gYjPTR-6cY")(http, "Bloodborne/1.09", 2, 1);
    const int conn = Fn<HttpConnection>("qgxDBjorUxs")(tmpl, url, true);
    const int hreq = Fn<HttpRequest>("Aeu5wVKkF9w")(conn, 1, url, post.size());
    Check(http > 0 && tmpl > 0 && conn > 0 && hreq > 0, "sceHttp template, connection and request");
    Check(Fn<HttpHeader>("EY28T2bkN7k")(hreq, "Content-Type", "application/octet-stream", 0) == 0, "sceHttpAddRequestHeader");
    Fn<HttpSend>("1e2BNwI-XzE")(hreq, post.data(), post.size());
    Check(server.WaitFor("/fs4/login.spd", 1), "the game's request reaches the game server");
    int status = 0;
    for (int i = 0; i < kWaitMs / 10 && Fn<HttpStatus>("0a2TBNfE3BU")(hreq, &status) != 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(status == 200, "the game's request is answered");
    Fn<HttpDelete>("qe7oZ+v4PWA")(hreq);
    Fn<HttpDelete>("P6A3ytpsiYc")(conn);
    Fn<HttpDelete>("4I8vEpuEhZ8")(tmpl);

    server.SetStep("in game: Matching2 Terminate");
    Fn<M2Terminate>("Mqp3lJ+sjy4")();
    server.SetStep("after the script");
    server.Stop();
    std::filesystem::remove_all(scratch);

    std::string main_text, background_text;
    std::map<std::string, std::size_t> per_step;
    std::vector<std::string> step_order;
    std::size_t polls = 0, heartbeats = 0, whoami_at_start = 0;
    std::map<std::string, std::string> background_shapes;
    for (const Request& r : server.Requests()) {
        if (r.step == "background") {
            (r.target == "/np/events/poll" ? polls : heartbeats) += 1;
            background_shapes[r.target] = Describe(r, server.Port());
            continue;
        }
        if (!per_step.count(r.step)) step_order.push_back(r.step);
        ++per_step[r.step];
        if (r.step.rfind("game start", 0) == 0 && r.target == "/auth/whoami") ++whoami_at_start;
        main_text += "[" + r.step + "] " + Describe(r, server.Port());
    }
    Check(polls == 1, "one held event poll");
    std::string text = "requests per phase (background poll and heartbeat counted apart)\n";
    for (const auto& step : step_order) text += "  " + step + ": " + std::to_string(per_step[step]) + "\n";
    text += "  game start: POST /auth/whoami: " + std::to_string(whoami_at_start) + "\n";
    text += "  background: event polls: " + std::to_string(polls) + "\n";
    text += "\nrequests in order\n" + main_text;
    text += "\nbackground request shapes (heartbeat only when a 5 s beat fell inside a room)\n";
    for (const auto& [target, shape] : background_shapes)
        if (target == "/np/events/poll") text += shape;
    text += "\ngame-visible results\n";
    {
        std::lock_guard lock(g_callbacks.mutex);
        for (const auto& s : g_callbacks.seen) text += "  " + s + "\n";
    }
    std::string heartbeat_text = background_shapes.count("/mp/matching2/heartbeat") ? background_shapes["/mp/matching2/heartbeat"] : "";

    if (record) {
        std::ofstream out(golden, std::ios::binary);
        out << text;
        std::printf("thd-wire-test: recorded %zu requests (%zu heartbeats seen)\n", server.Requests().size(), heartbeats);
    } else {
        if (ReadAll(golden) != text) {
            std::printf("FAIL the request sequence differs from %s\n%s", golden.c_str(), text.c_str());
            ++failures;
        }
        if (!heartbeat_text.empty() && heartbeat_text.find("POST /mp/matching2/heartbeat\n") != 0) {
            std::printf("FAIL a heartbeat has another shape\n%s", heartbeat_text.c_str());
            ++failures;
        }
        if (!failures) std::printf("thd-wire-test: ok (%zu requests, %zu heartbeats)\n", server.Requests().size(), heartbeats);
    }
    std::fflush(stdout);
    std::_Exit(failures ? 1 : 0);
}
