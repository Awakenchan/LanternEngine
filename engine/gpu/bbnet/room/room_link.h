// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "room_counters.h"
#include "room_crypto.h"
#include "room_ws.h"

namespace BbRoom {

constexpr int kLossyRoute = 0x80;
constexpr int kMostRoute = 0xff;
constexpr int kMostGameTarget = kLossyRoute - 1;

constexpr int RouteByte(int target, bool lossy) { return lossy ? (target | kLossyRoute) : target; }

struct Event {
    enum class Kind { Connected, Joined, Left, Host, Data, Disconnected, Refused, GaveUp, Announcement, Visitor, Away };
    Kind kind = Kind::Data;
    int slot = 0;
    std::vector<std::uint8_t> data;
    std::uint64_t id = 0;
};

class Link {
public:
    Link();
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    bool Start(const std::string& server, const std::string& name, const std::string& password,
               const std::string& path = "room");
    void Stop();
    bool Send(int target, const std::uint8_t* data, std::size_t size);
    bool SendLossy(int target, const std::uint8_t* data, std::size_t size, std::size_t most_backlog);
    bool SendGame(int target, const std::uint8_t* data, std::size_t size);
    bool SendPaced(int target, const std::uint8_t* data, std::size_t size);
    void SetHost(bool host);
    void SetVisitor(bool visitor) { visitor_ = visitor; }
    void SetCounters(NetCounts::Counters* counters) { counters_ = counters; }
    void SetOpen(bool open);
    bool OpenListed() const { return open_listed_.load(); }
    bool IsVisitor(int slot) const;
    void SendAway(int slot);
    std::vector<Event> Drain();
    bool WaitEvent(int timeout_ms);

    bool Connected() const { return connected_.load(); }
    int Slot() const { return slot_.load(); }
    bool IsDev(int slot) const;
    int HostSlot() const { return host_.load(); }
    std::vector<int> Members() const;
    std::string RoomId() const { return secret_.room_id; }
    bool LossyRoutes() const { return lossy_targets_.load(); }
    std::string LastError() const;

private:
    enum class Delivery : std::uint8_t { Reliable, Lossy, Paced };

    bool SendFrame(int target, int most_target, bool lossy_route, Delivery delivery, std::size_t most_backlog,
                   const std::uint8_t* data, std::size_t size);
    bool SendText(const std::string& text);
    std::shared_ptr<WebSocket> CurrentSocket();
    void SetError(std::string error);
    void Run();
    bool Session();
    void Handle(const std::string& text);
    void Push(Event event);
    void SendOpen(WebSocket& socket);
    bool Counted(NetCounts::Kind kind, std::size_t bytes);

    std::string url_;
    Secret secret_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int> slot_{0};
    std::atomic<int> host_{0};
    std::atomic<bool> want_host_{false};
    std::atomic<bool> lossy_targets_{false};
    std::atomic<bool> visitor_{false};
    std::atomic<bool> want_open_{false};
    std::atomic<bool> open_listed_{false};
    std::atomic<bool> full_{false};
    std::atomic<int> full_most_{kMaxMembers};
    std::atomic<NetCounts::Counters*> counters_{nullptr};
    bool welcomed_once_ = false;
    std::string name_;
    std::string password_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Event> events_;
    std::vector<int> members_;
    std::vector<int> devs_;
    std::vector<int> visitors_;
    std::string last_error_;
    std::mutex socket_mutex_;
    std::shared_ptr<WebSocket> socket_;
};

std::string RoomUrl(const std::string& server, const std::string& room_id, const std::string& path = "room");

} // namespace BbRoom
