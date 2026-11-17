// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "test_sandbox.h"
#include <cstdint>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace LanternSandbox {

inline Target ClassifyAddress(const sockaddr* address, std::size_t size) {
#ifdef LANTERN_SANDBOX_MUTANTS
    if (Is(Mutant::AnyHostIsLoopback)) return Target::Loopback;
#endif
    if (!address) return Target::Remote;
    if (address->sa_family == AF_INET && size >= sizeof(sockaddr_in)) {
        sockaddr_in v4{};
        std::memcpy(&v4, address, sizeof v4);
        return ntohl(v4.sin_addr.s_addr) == INADDR_LOOPBACK ? Target::Loopback : Target::Remote;
    }
    if (address->sa_family == AF_INET6 && size >= sizeof(sockaddr_in6)) {
        sockaddr_in6 v6{};
        std::memcpy(&v6, address, sizeof v6);
        static constexpr std::uint8_t kLoopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return std::memcmp(&v6.sin6_addr, kLoopback6, sizeof kLoopback6) == 0 ? Target::Loopback : Target::Remote;
    }
    return Target::Remote;
}

inline bool AllowAddress(const sockaddr* address, std::size_t size, const char* site) {
    if (!On()) return true;
    const Target target = ClassifyAddress(address, size);
    char text[64] = "an address that is not IPv4 or IPv6";
    if (address && address->sa_family == AF_INET && size >= sizeof(sockaddr_in)) {
        sockaddr_in v4{};
        std::memcpy(&v4, address, sizeof v4);
        inet_ntop(AF_INET, &v4.sin_addr, text, sizeof text);
    } else if (address && address->sa_family == AF_INET6 && size >= sizeof(sockaddr_in6)) {
        sockaddr_in6 v6{};
        std::memcpy(&v6, address, sizeof v6);
        inet_ntop(AF_INET6, &v6.sin6_addr, text, sizeof text);
    }
    return Allow(target, site, text);
}

inline bool AnyAddress(const sockaddr* address, std::size_t size) {
    if (address && address->sa_family == AF_INET && size >= sizeof(sockaddr_in)) {
        sockaddr_in v4{};
        std::memcpy(&v4, address, sizeof v4);
        return v4.sin_addr.s_addr == htonl(INADDR_ANY);
    }
    if (address && address->sa_family == AF_INET6 && size >= sizeof(sockaddr_in6)) {
        sockaddr_in6 v6{};
        std::memcpy(&v6, address, sizeof v6);
        static constexpr std::uint8_t kAny6[16] = {};
        return std::memcmp(&v6.sin6_addr, kAny6, sizeof kAny6) == 0;
    }
    return false;
}

inline bool LocalBind(sockaddr* address, std::size_t size, const char* site) {
    if (!On()) return true;
#ifdef LANTERN_SANDBOX_MUTANTS
    if (Is(Mutant::KeepAnyAddress)) return true;
#endif
    if (AnyAddress(address, size)) {
        if (address->sa_family == AF_INET) {
            sockaddr_in v4{};
            std::memcpy(&v4, address, sizeof v4);
            v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            std::memcpy(address, &v4, sizeof v4);
        } else {
            sockaddr_in6 v6{};
            std::memcpy(&v6, address, sizeof v6);
            static constexpr std::uint8_t kLoopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            std::memcpy(&v6.sin6_addr, kLoopback6, sizeof kLoopback6);
            std::memcpy(address, &v6, sizeof v6);
        }
        counters.rebinds.fetch_add(1, std::memory_order_relaxed);
        if (counters.lines.fetch_add(1, std::memory_order_relaxed) < kMostRefusalLines)
            std::printf("Sandbox: %s binds to loopback, not every address: %s=1\n", site, kSwitch);
        return true;
    }
    return AllowAddress(address, size, site);
}

template <typename Socket> bool BindIfUnbound(Socket socket, const char* site) {
    if (!On()) return true;
    sockaddr_storage found{};
    socklen_t found_size = sizeof found;
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&found), &found_size) == 0) {
        if (found.ss_family == AF_INET) {
            sockaddr_in v4{};
            std::memcpy(&v4, &found, sizeof v4);
            if (v4.sin_port != 0) return true;
        } else if (found.ss_family == AF_INET6) {
            sockaddr_in6 v6{};
            std::memcpy(&v6, &found, sizeof v6);
            if (v6.sin6_port != 0) return true;
        } else {
            return true;
        }
    }
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&v4), sizeof v4) == 0) {
        counters.rebinds.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    reinterpret_cast<unsigned char*>(&v6.sin6_addr)[sizeof v6.sin6_addr - 1] = 1;
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&v6), sizeof v6) == 0) {
        counters.rebinds.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    counters.refused.fetch_add(1, std::memory_order_relaxed);
    if (counters.lines.fetch_add(1, std::memory_order_relaxed) < kMostRefusalLines)
        std::printf("Sandbox: %s refused (an unbound socket could not be bound to loopback): %s=1\n", site, kSwitch);
    return false;
}

}
