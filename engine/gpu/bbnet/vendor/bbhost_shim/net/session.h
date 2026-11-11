// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>
namespace net {
enum class Prio { Context = 0, Request = 1, Signaling = 2, RoomEvent = 3 };
struct Peer {
    std::uint16_t member_id = 0;
    std::string online_id;
    std::uint32_t addr = 0;
    std::uint16_t port = 0;
};
void dispatch_after(int delay_ms, Prio prio, std::function<void()> fn);
bool peers_get(std::uint16_t member_id, Peer* out);
bool peers_find_online(const std::string& id, Peer* out);
void peers_set_conn(std::uint16_t member_id, unsigned conn_id, int status);
Peer peers_provisional(const std::string& id, std::uint32_t addr, std::uint16_t port);
bool server_signaling_resolve(const std::string& id, std::uint32_t* addr, std::uint16_t* port, std::string& error);
std::string online_id();
}
