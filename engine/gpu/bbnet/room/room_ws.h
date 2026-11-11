// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace BbRoom {

constexpr int kRoomProtocol = 17;
constexpr int kMaxMembers = 5;
constexpr int kCloseFull = 4003;

std::string VersionRefusal(int room, int own);
std::string FullRefusal(int most = kMaxMembers);

class WebSocket {
public:
    using Handler = std::function<void(std::string&& message, bool binary)>;

    WebSocket();
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    bool Connect(const std::string& url, int timeout_s, std::string& error);
    void Run(const Handler& handler);
    bool Send(const void* data, std::size_t size, bool binary);
    bool SendLossy(const void* data, std::size_t size, std::size_t most_backlog);
    bool SendPaced(const void* data, std::size_t size);
    std::size_t Backlog() const { return backlog_.load(); }
    void WatchClosely() { watch_closely_ = true; }
    void Close();
    bool Open() const { return open_.load(); }
    int CloseCode() const { return close_code_.load(); }
    bool VersionRefused() const { return version_refused_; }

private:
    struct Impl;
    Impl* impl_;
    std::atomic<bool> open_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> watch_closely_{false};
    std::atomic<int> close_code_{0};
    bool version_refused_ = false;
    std::mutex queue_mutex_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::deque<std::vector<std::uint8_t>> paced_;
    std::atomic<std::size_t> backlog_{0};
    void Wake();
};

} // namespace BbRoom
