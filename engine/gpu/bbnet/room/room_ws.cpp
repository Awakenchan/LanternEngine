// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include "room_ws.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <httplib.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace BbRoom {

static std::string DevHeader() {
    const char* key = std::getenv("BB_ROOM_DEV_KEY");
    return key && *key ? std::string("\r\nX-Lantern-Dev: ") + key : std::string();
}
namespace {

#ifdef _WIN32
using Sock = SOCKET;
constexpr Sock kBad = INVALID_SOCKET;
void CloseSock(Sock s) { closesocket(s); }
bool WouldBlock() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
void NonBlocking(Sock s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
#else
using Sock = int;
constexpr Sock kBad = -1;
void CloseSock(Sock s) { close(s); }
bool WouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
void NonBlocking(Sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

using Clock = std::chrono::steady_clock;

constexpr auto kPingEvery = std::chrono::seconds(10);
constexpr auto kDeadAfter = std::chrono::seconds(35);
constexpr auto kClosePingEvery = std::chrono::seconds(2);
constexpr auto kCloseDeadAfter = std::chrono::seconds(9);
constexpr std::size_t kPacedBytesPerSecond = 12000;
constexpr std::size_t kMostPacedQueued = 512;
#ifdef _WIN32
constexpr int kSendBuffer = 16384;
#else
constexpr int kSendBuffer = 8192;
#endif

struct Url {
    bool tls = true;
    std::string host;
    std::string port;
    std::string path;
};

bool ParseUrl(const std::string& text, Url& url) {
    std::string rest;
    if (text.rfind("wss://", 0) == 0) {
        rest = text.substr(6);
    } else if (text.rfind("ws://", 0) == 0) {
        url.tls = false;
        rest = text.substr(5);
    } else {
        return false;
    }
    const auto slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    url.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto colon = authority.rfind(':');
    url.host = colon == std::string::npos ? authority : authority.substr(0, colon);
    url.port = colon == std::string::npos ? (url.tls ? "443" : "80") : authority.substr(colon + 1);
    return !url.host.empty();
}

bool WaitSock(Sock s, bool write, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return ::select(static_cast<int>(s + 1), write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv) > 0;
}

std::string Base64(const std::uint8_t* data, std::size_t size) {
    std::string out(4 * ((size + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
    out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return out;
}

void Frame(std::vector<std::uint8_t>& out, std::uint8_t opcode, const std::uint8_t* data, std::size_t size) {
    out.push_back(static_cast<std::uint8_t>(0x80 | opcode));
    if (size < 126) {
        out.push_back(static_cast<std::uint8_t>(0x80 | size));
    } else if (size < 65536) {
        out.push_back(0x80 | 126);
        out.push_back(static_cast<std::uint8_t>(size >> 8));
        out.push_back(static_cast<std::uint8_t>(size));
    } else {
        out.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(size) >> (8 * i)));
    }
    std::uint8_t mask[4];
    RAND_bytes(mask, sizeof mask);
    out.insert(out.end(), mask, mask + 4);
    const std::size_t at = out.size();
    out.resize(at + size);
    for (std::size_t i = 0; i < size; ++i) out[at + i] = data[i] ^ mask[i & 3];
}

int AnswerProtocol(const std::string& head) {
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string wanted = "\r\nx-lantern-room:";
    const auto at = lower.find(wanted);
    if (at == std::string::npos) return 0;
    return std::atoi(head.c_str() + at + wanted.size());
}

} // namespace

std::string VersionRefusal(int room, int own) {
    const std::string versions = room > 0 ? "protocol " + std::to_string(room) + ", yours " + std::to_string(own) : "yours " + std::to_string(own);
    const char* side = room > own ? ", so update your Lantern" : room > 0 ? ", so the host has to update Lantern" : ", so update to the newest beta";
    return "This room runs a different Lantern version (" + versions + "): everyone in a seamless room needs the same Lantern version" + side;
}

std::string FullRefusal(int most) {
    return "This room is full (" + std::to_string(most) + " of " + std::to_string(most) + " hunters)";
}

struct WebSocket::Impl {
    Sock sock = kBad;
    Sock wake = kBad;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    std::vector<std::uint8_t> in;
    std::vector<std::uint8_t> out;
    std::size_t out_at = 0;
    std::string message;
    std::uint8_t message_opcode = 0;

    ~Impl() {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
        if (sock != kBad) CloseSock(sock);
        if (wake != kBad) CloseSock(wake);
    }

    int Read(std::uint8_t* buffer, int size, bool& closed) {
        if (ssl) {
            ERR_clear_error();
            const int n = SSL_read(ssl, buffer, size);
            if (n > 0) return n;
            const int err = SSL_get_error(ssl, n);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) closed = true;
            return 0;
        }
        const int n = static_cast<int>(::recv(sock, reinterpret_cast<char*>(buffer), size, 0));
        if (n > 0) return n;
        if (n == 0 || !WouldBlock()) closed = true;
        return 0;
    }

    int Write(const std::uint8_t* data, int size, bool& closed) {
        if (ssl) {
            ERR_clear_error();
            const int n = SSL_write(ssl, data, size);
            if (n > 0) return n;
            const int err = SSL_get_error(ssl, n);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) closed = true;
            return 0;
        }
        const int n = static_cast<int>(::send(sock, reinterpret_cast<const char*>(data), size, 0));
        if (n > 0) return n;
        if (!WouldBlock()) closed = true;
        return 0;
    }
};

WebSocket::WebSocket() : impl_(new Impl) {}

WebSocket::~WebSocket() { delete impl_; }

bool WebSocket::Connect(const std::string& text, int timeout_s, std::string& error) {
    Url url;
    if (!ParseUrl(text, url)) {
        error = "bad address";
        return false;
    }
    const auto deadline = Clock::now() + std::chrono::seconds(timeout_s);
    const auto left = [&] {
        return static_cast<int>(std::max<long long>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
    };
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &list) != 0 || !list) {
        error = "cannot find " + url.host;
        return false;
    }
    for (addrinfo* a = list; a && impl_->sock == kBad; a = a->ai_next) {
        const Sock s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kBad) continue;
        NonBlocking(s);
        const int r = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
        int soerr = 0;
        socklen_t len = sizeof soerr;
        if ((r == 0 || (WouldBlock() && WaitSock(s, true, left()))) &&
            ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len) == 0 && soerr == 0) {
            impl_->sock = s;
        } else {
            CloseSock(s);
        }
    }
    freeaddrinfo(list);
    if (impl_->sock == kBad) {
        error = "cannot reach " + url.host;
        return false;
    }
    int on = 1;
    ::setsockopt(impl_->sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
    ::setsockopt(impl_->sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&kSendBuffer), sizeof kSendBuffer);

    if (url.tls) {
        impl_->ctx = SSL_CTX_new(TLS_client_method());
        if (!impl_->ctx) {
            error = "no TLS";
            return false;
        }
        httplib::tls::load_system_certs(impl_->ctx);
        SSL_CTX_set_verify(impl_->ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_mode(impl_->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        impl_->ssl = SSL_new(impl_->ctx);
        SSL_set_tlsext_host_name(impl_->ssl, url.host.c_str());
        SSL_set1_host(impl_->ssl, url.host.c_str());
        SSL_set_fd(impl_->ssl, static_cast<int>(impl_->sock));
        for (;;) {
            ERR_clear_error();
            const int r = SSL_connect(impl_->ssl);
            if (r == 1) break;
            const int err = SSL_get_error(impl_->ssl, r);
            if ((err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) && left() > 0 &&
                WaitSock(impl_->sock, err == SSL_ERROR_WANT_WRITE, std::min(left(), 1000)))
                continue;
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                if (left() > 0) continue;
                error = "TLS handshake timed out";
            } else {
                const long verify = SSL_get_verify_result(impl_->ssl);
                error = verify != X509_V_OK ? std::string("certificate not trusted: ") + X509_verify_cert_error_string(verify)
                                            : "TLS handshake failed";
            }
            return false;
        }
    }

    std::uint8_t key[16];
    RAND_bytes(key, sizeof key);
    const std::string request = "GET " + url.path + " HTTP/1.1\r\nHost: " + url.host +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + Base64(key, sizeof key) +
                                "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: LanternEngine\r\nX-Lantern-Room: " +
                                std::to_string(kRoomProtocol) + DevHeader() + "\r\n\r\n";
    std::size_t sent = 0;
    bool closed = false;
    while (sent < request.size()) {
        sent += static_cast<std::size_t>(impl_->Write(reinterpret_cast<const std::uint8_t*>(request.data()) + sent,
                                                      static_cast<int>(request.size() - sent), closed));
        if (closed || left() == 0) {
            error = "the server closed the connection";
            return false;
        }
        if (sent < request.size()) WaitSock(impl_->sock, true, std::min(left(), 200));
    }
    std::string head;
    std::uint8_t buffer[4096];
    for (;;) {
        const int n = impl_->Read(buffer, sizeof buffer, closed);
        if (n > 0) {
            head.append(reinterpret_cast<char*>(buffer), static_cast<std::size_t>(n));
            const auto end = head.find("\r\n\r\n");
            if (end != std::string::npos) {
                impl_->in.assign(head.begin() + static_cast<std::ptrdiff_t>(end + 4), head.end());
                head.resize(end);
                break;
            }
            if (head.size() > 16384) closed = true;
            continue;
        }
        if (closed || left() == 0) {
            error = "no answer from the server";
            return false;
        }
        if (!(impl_->ssl && SSL_pending(impl_->ssl) > 0)) WaitSock(impl_->sock, false, std::min(left(), 200));
    }
    if (head.find(" 101") == std::string::npos) {
        const std::string status = head.substr(0, head.find("\r\n"));
        if (status.find(" 426") != std::string::npos) {
            version_refused_ = true;
            error = VersionRefusal(AnswerProtocol(head), kRoomProtocol);
        } else {
            error = "the server refused the room (" + status + ")";
        }
        return false;
    }

    impl_->wake = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t self_len = sizeof self;
    if (impl_->wake == kBad || ::bind(impl_->wake, reinterpret_cast<sockaddr*>(&self), sizeof self) != 0 ||
        ::getsockname(impl_->wake, reinterpret_cast<sockaddr*>(&self), &self_len) != 0 ||
        ::connect(impl_->wake, reinterpret_cast<sockaddr*>(&self), sizeof self) != 0) {
        error = "no wake socket";
        return false;
    }
    NonBlocking(impl_->wake);
    open_ = true;
    return true;
}

void WebSocket::Wake() {
    if (impl_->wake != kBad) ::send(impl_->wake, "w", 1, 0);
}

bool WebSocket::Send(const void* data, std::size_t size, bool binary) {
    if (!open_) return false;
    std::vector<std::uint8_t> frame;
    frame.reserve(size + 14);
    Frame(frame, binary ? 2 : 1, static_cast<const std::uint8_t*>(data), size);
    {
        std::lock_guard lock(queue_mutex_);
        backlog_ += frame.size();
        queue_.push_back(std::move(frame));
    }
    Wake();
    return true;
}

bool WebSocket::SendLossy(const void* data, std::size_t size, std::size_t most_backlog) {
    if (!open_ || backlog_.load() >= most_backlog) return false;
    return Send(data, size, true);
}

bool WebSocket::SendPaced(const void* data, std::size_t size) {
    if (!open_) return false;
    std::vector<std::uint8_t> frame;
    frame.reserve(size + 14);
    Frame(frame, 2, static_cast<const std::uint8_t*>(data), size);
    {
        std::lock_guard lock(queue_mutex_);
        if (paced_.size() >= kMostPacedQueued) return false;
        paced_.push_back(std::move(frame));
    }
    Wake();
    return true;
}

void WebSocket::Close() {
    closing_ = true;
    Wake();
}

void WebSocket::Run(const Handler& handler) {
    Impl& s = *impl_;
    auto last_rx = Clock::now();
    auto last_ping = Clock::now();
    auto next_paced = Clock::now();
    std::vector<std::uint8_t> buffer(65536);
    bool closed = false;
    while (!closed && !closing_) {
        bool paced_waiting = false;
        {
            std::lock_guard lock(queue_mutex_);
            if (s.out_at == s.out.size()) {
                s.out.clear();
                s.out_at = 0;
            }
            while (!queue_.empty()) {
                s.out.insert(s.out.end(), queue_.front().begin(), queue_.front().end());
                queue_.pop_front();
            }
            if (!paced_.empty() && s.out_at == s.out.size() && Clock::now() >= next_paced) {
                s.out.insert(s.out.end(), paced_.front().begin(), paced_.front().end());
                next_paced = Clock::now() + std::chrono::microseconds(paced_.front().size() * 1000000 / kPacedBytesPerSecond);
                backlog_ += paced_.front().size();
                paced_.pop_front();
            }
            paced_waiting = !paced_.empty();
        }
        while (s.out_at < s.out.size() && !closed) {
            const int n = s.Write(s.out.data() + s.out_at, static_cast<int>(s.out.size() - s.out_at), closed);
            if (n <= 0) break;
            s.out_at += static_cast<std::size_t>(n);
            backlog_ -= std::min<std::size_t>(backlog_.load(), static_cast<std::size_t>(n));
        }
        if (closed) break;

        const bool pending = s.ssl && SSL_pending(s.ssl) > 0;
        if (!pending) {
            fd_set reads, writes;
            FD_ZERO(&reads);
            FD_ZERO(&writes);
            FD_SET(s.sock, &reads);
            FD_SET(s.wake, &reads);
            if (s.out_at < s.out.size()) FD_SET(s.sock, &writes);
            timeval tv{paced_waiting ? 0 : 1, paced_waiting ? 20000 : 0};
            ::select(static_cast<int>(std::max(s.sock, s.wake) + 1), &reads, &writes, nullptr, &tv);
            if (FD_ISSET(s.wake, &reads)) {
                char drain[64];
                while (::recv(s.wake, drain, sizeof drain, 0) > 0) {
                }
            }
        }

        for (;;) {
            const int n = s.Read(buffer.data(), static_cast<int>(buffer.size()), closed);
            if (n <= 0) break;
            s.in.insert(s.in.end(), buffer.begin(), buffer.begin() + n);
            last_rx = Clock::now();
        }

        std::size_t at = 0;
        while (s.in.size() - at >= 2) {
            const std::uint8_t b0 = s.in[at], b1 = s.in[at + 1];
            std::size_t head = 2;
            std::uint64_t len = b1 & 0x7f;
            if (len == 126) {
                if (s.in.size() - at < 4) break;
                len = (std::uint64_t(s.in[at + 2]) << 8) | s.in[at + 3];
                head = 4;
            } else if (len == 127) {
                if (s.in.size() - at < 10) break;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | s.in[at + 2 + i];
                head = 10;
            }
            const bool masked = b1 & 0x80;
            if (masked) head += 4;
            if (len > (1u << 24)) {
                closed = true;
                break;
            }
            if (s.in.size() - at < head + len) break;
            std::uint8_t* payload = s.in.data() + at + head;
            if (masked) {
                const std::uint8_t* key = payload - 4;
                for (std::uint64_t i = 0; i < len; ++i) payload[i] ^= key[i & 3];
            }
            const std::uint8_t opcode = b0 & 0x0f;
            const bool fin = b0 & 0x80;
            at += head + len;
            if (opcode == 9) {
                std::vector<std::uint8_t> pong;
                Frame(pong, 10, payload, static_cast<std::size_t>(len));
                s.out.insert(s.out.end(), pong.begin(), pong.end());
            } else if (opcode == 8) {
                if (len >= 2) close_code_ = (int(payload[0]) << 8) | payload[1];
                closed = true;
                break;
            } else if (opcode == 1 || opcode == 2 || opcode == 0) {
                if (opcode != 0) {
                    s.message.clear();
                    s.message_opcode = opcode;
                }
                s.message.append(reinterpret_cast<char*>(payload), static_cast<std::size_t>(len));
                if (fin) handler(std::move(s.message), s.message_opcode == 2), s.message.clear();
            }
        }
        if (at) s.in.erase(s.in.begin(), s.in.begin() + static_cast<std::ptrdiff_t>(at));

        const auto now = Clock::now();
        const bool close_watch = watch_closely_.load();
        const auto ping_every = close_watch ? std::chrono::steady_clock::duration(kClosePingEvery) : std::chrono::steady_clock::duration(kPingEvery);
        if (now - last_rx > ping_every && now - last_ping > ping_every) {
            Frame(s.out, 9, nullptr, 0);
            last_ping = now;
        }
        if (now - last_rx > (close_watch ? kCloseDeadAfter : kDeadAfter)) closed = true;
    }
    if (!closed) {
        {
            std::lock_guard lock(queue_mutex_);
            while (!queue_.empty()) {
                s.out.insert(s.out.end(), queue_.front().begin(), queue_.front().end());
                queue_.pop_front();
            }
        }
        const auto until = Clock::now() + std::chrono::seconds(2);
        while (s.out_at < s.out.size() && !closed && Clock::now() < until) {
            const int n = s.Write(s.out.data() + s.out_at, static_cast<int>(s.out.size() - s.out_at), closed);
            if (n > 0) s.out_at += static_cast<std::size_t>(n);
            else WaitSock(s.sock, true, 100);
        }
        if (s.ssl) SSL_shutdown(s.ssl);
    }
    open_ = false;
}

} // namespace BbRoom
