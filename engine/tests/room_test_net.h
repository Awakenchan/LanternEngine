// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "gpu/bbnet/room/room_crypto.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace RoomTestNet {

constexpr std::size_t kRunTagLen = 8;

inline void StartSockets() {
#ifdef _WIN32
    WSADATA wsa{};
    static_cast<void>(WSAStartup(MAKEWORD(2, 2), &wsa));
#endif
}

inline int FreePort() {
#ifdef _WIN32
    using Sock = SOCKET;
    constexpr Sock kBad = INVALID_SOCKET;
    const auto close_sock = [](Sock s) { closesocket(s); };
#else
    using Sock = int;
    constexpr Sock kBad = -1;
    const auto close_sock = [](Sock s) { ::close(s); };
#endif
    const Sock s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBad) return 0;
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof at;
    const bool bound = ::bind(s, reinterpret_cast<sockaddr*>(&at), sizeof at) == 0 &&
                       ::getsockname(s, reinterpret_cast<sockaddr*>(&at), &size) == 0;
    close_sock(s);
    return bound ? ntohs(at.sin_port) : 0;
}

inline std::string UniqueRoomName(const std::string& base) {
    std::array<std::uint8_t, kRunTagLen> tag{};
    if (!BbRoom::RandomBytes(tag)) return {};
    static const char digits[] = "0123456789abcdef";
    std::string name = base + "-";
    for (const std::uint8_t byte : tag) {
        name.push_back(digits[byte >> 4]);
        name.push_back(digits[byte & 15]);
    }
    return name;
}

} // namespace RoomTestNet
