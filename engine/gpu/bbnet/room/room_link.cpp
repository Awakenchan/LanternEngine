// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_link.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace BbRoom {

constexpr std::size_t kMostGameBacklog = 32 * 1024;
constexpr std::size_t kMemberTokenLen = 16;

namespace {

void TellSealFailedOnce() {
    static std::atomic<bool> told{false};
    if (told.exchange(true, std::memory_order_relaxed)) return;
    static_cast<void>(
        std::printf("Seamless: a frame could not be sealed (no random nonce or cipher error), so it was not sent\n"));
    static_cast<void>(std::fflush(stdout));
}

std::vector<std::uint8_t> RoutedFrame(const Key& key, int slot, int route, const std::uint8_t* data, std::size_t size) {
    auto sealed = Seal(key, static_cast<std::uint8_t>(slot), data, size);
    if (sealed.empty()) {
        TellSealFailedOnce();
        return sealed;
    }
    sealed.insert(sealed.begin(), static_cast<std::uint8_t>(route));
    return sealed;
}

} // namespace

std::string RoomUrl(const std::string& server, const std::string& room_id, const std::string& path) {
    std::string base = server;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.rfind("https://", 0) == 0) {
        base = "wss://" + base.substr(8);
    } else if (base.rfind("http://", 0) == 0) {
        base = "ws://" + base.substr(7);
    } else if (base.rfind("wss://", 0) != 0 && base.rfind("ws://", 0) != 0) {
        base = "wss://" + base;
    }
    return base + "/" + path + "/" + room_id;
}

Link::Link() = default;

Link::~Link() { Stop(); }

bool Link::Start(const std::string& server, const std::string& name, const std::string& password, const std::string& path) {
    Stop();
    SetError({});
    if (server.empty() || name.empty()) return false;
    auto secret = Derive(name, password);
    if (!secret) {
        SetError("the room key could not be derived");
        return false;
    }
    secret_ = std::move(*secret);
    WipeKey(secret->key);
    std::array<std::uint8_t, kMemberTokenLen> token{};
    if (!RandomBytes(token)) {
        SetError("no random member token");
        return false;
    }
    static const char digits[] = "0123456789abcdef";
    std::string member;
    for (const std::uint8_t byte : token) {
        member.push_back(digits[byte >> 4]);
        member.push_back(digits[byte & 15]);
    }
    url_ = RoomUrl(server, secret_.room_id, path) + "/" + member;
    if (visitor_ && path == "room") url_ += "?visitor=1";
    if (!WebSocket::SandboxAllows(url_, "room link")) {
        SetError("refused by the test sandbox");
        return false;
    }
    name_ = name;
    password_ = password;
    stop_ = false;
    welcomed_once_ = false;
    thread_ = std::thread([this] { Run(); });
    return true;
}

void Link::Stop() {
    stop_ = true;
    {
        std::lock_guard lock(socket_mutex_);
        if (socket_) {
            const std::string text = R"({"t":"leave"})";
            socket_->Send(text.data(), text.size(), false);
            socket_->Close();
        }
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    connected_ = false;
    slot_ = 0;
    host_ = 0;
    open_listed_ = false;
    std::lock_guard lock(mutex_);
    members_.clear();
    visitors_.clear();
}

std::shared_ptr<WebSocket> Link::CurrentSocket() {
    std::lock_guard lock(socket_mutex_);
    return socket_;
}

bool Link::SendFrame(int target, int most_target, bool lossy_route, Delivery delivery, std::size_t most_backlog,
                     const std::uint8_t* data, std::size_t size) {
    const int slot = slot_.load();
    if (!connected_ || !slot) return Counted(NetCounts::Kind::Offline, size);
    if (target == slot || target < 0 || target > most_target) return Counted(NetCounts::Kind::SendFailed, size);
    const auto sealed = RoutedFrame(secret_.key, slot, RouteByte(target, lossy_route), data, size);
    if (sealed.empty()) return Counted(NetCounts::Kind::SendFailed, size);
    const auto socket = CurrentSocket();
    if (!socket) return Counted(NetCounts::Kind::Offline, size);
    bool sent = false;
    switch (delivery) {
    case Delivery::Reliable:
        sent = socket->Send(sealed.data(), sealed.size(), true);
        break;
    case Delivery::Lossy:
        sent = socket->SendLossy(sealed.data(), sealed.size(), most_backlog);
        break;
    case Delivery::Paced:
        sent = socket->SendPaced(sealed.data(), sealed.size());
        break;
    }
    if (auto* counters = counters_.load(std::memory_order_relaxed)) counters->Backlog(socket->Backlog());
    if (sent) return true;
    if (!socket->Open()) return Counted(NetCounts::Kind::Offline, size);
    const bool full = delivery == Delivery::Lossy && socket->Backlog() >= most_backlog;
    return Counted(full ? NetCounts::Kind::BacklogRefused : NetCounts::Kind::SendFailed, size);
}

bool Link::Counted(NetCounts::Kind kind, std::size_t bytes) {
    if (auto* counters = counters_.load(std::memory_order_relaxed)) counters->Add(kind, bytes);
    return false;
}

bool Link::SendText(const std::string& text) {
    const auto socket = CurrentSocket();
    return socket && connected_ && socket->Send(text.data(), text.size(), false);
}

bool Link::Send(int target, const std::uint8_t* data, std::size_t size) {
    return SendFrame(target, kMostRoute, false, Delivery::Reliable, 0, data, size);
}

bool Link::SendLossy(int target, const std::uint8_t* data, std::size_t size, std::size_t most_backlog) {
    return SendFrame(target, kMostRoute, false, Delivery::Lossy, most_backlog, data, size);
}

bool Link::SendGame(int target, const std::uint8_t* data, std::size_t size) {
    return SendFrame(target, kMostGameTarget, lossy_targets_.load(), Delivery::Lossy, kMostGameBacklog, data, size);
}

bool Link::SendPaced(int target, const std::uint8_t* data, std::size_t size) {
    return SendFrame(target, kMostGameTarget, false, Delivery::Paced, 0, data, size);
}

void Link::SetHost(bool host) {
    want_host_ = host;
    SendText(host ? R"({"t":"host"})" : R"({"t":"unhost"})");
}

void Link::SendOpen(WebSocket& socket) {
    const std::string text = want_open_
        ? nlohmann::json{{"t", "open"}, {"room", name_}, {"password", password_}}.dump()
        : std::string(R"({"t":"unopen"})");
    socket.Send(text.data(), text.size(), false);
}

void Link::SetOpen(bool open) {
    if (want_open_.exchange(open) == open) return;
    if (!open) open_listed_ = false;
    const auto socket = CurrentSocket();
    if (socket && connected_) SendOpen(*socket);
}

void Link::SendAway(int slot) { SendText(nlohmann::json{{"t", "away"}, {"slot", slot}}.dump()); }

bool Link::IsVisitor(int slot) const {
    std::lock_guard lock(mutex_);
    return std::find(visitors_.begin(), visitors_.end(), slot) != visitors_.end();
}

std::vector<Event> Link::Drain() {
    std::lock_guard lock(mutex_);
    std::vector<Event> out(std::make_move_iterator(events_.begin()), std::make_move_iterator(events_.end()));
    events_.clear();
    return out;
}

bool Link::WaitEvent(int timeout_ms) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !events_.empty() || stop_; }) &&
           !events_.empty();
}

std::string Link::LastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

void Link::SetError(std::string error) {
    std::lock_guard lock(mutex_);
    last_error_ = std::move(error);
}

std::vector<int> Link::Members() const {
    std::lock_guard lock(mutex_);
    return members_;
}

void Link::Push(Event event) {
    {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
    }
    cv_.notify_all();
}

constexpr auto kGiveUpAfter = std::chrono::minutes(10);

void Link::Run() {
    int backoff_ms = 100;
    auto reached = std::chrono::steady_clock::now();
    while (!stop_) {
        const bool welcomed = Session();
        if (stop_) break;
        const auto now = std::chrono::steady_clock::now();
        if (welcomed) reached = now;
        if (now - reached >= kGiveUpAfter) {
            Push({Event::Kind::GaveUp, 0, {}});
            break;
        }
        backoff_ms = welcomed ? 100 : std::min(backoff_ms * 2, 8000);
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(backoff_ms), [this] { return stop_.load(); });
    }
}

bool Link::Session() {
    auto socket = std::make_shared<WebSocket>();
    std::string error;
    const bool opened = socket->Connect(url_, 10, error);
    if (!opened) {
        Counted(NetCounts::Kind::OpenFailed, 0);
        SetError(error);
        if (socket->VersionRefused() && !stop_) Push({Event::Kind::Refused, 0, {error.begin(), error.end()}});
        return false;
    }
    full_ = false;
    full_most_ = kMaxMembers;
    {
        std::lock_guard lock(socket_mutex_);
        if (stop_) return false;
        socket_ = socket;
    }
    bool welcomed = false;
    socket->Run([&](std::string&& message, bool binary) {
        if (!binary) {
            const bool was = connected_.load();
            Handle(message);
            if (!was && connected_) {
                welcomed = true;
                if (welcomed_once_) Counted(NetCounts::Kind::Reconnect, 0);
                welcomed_once_ = true;
                if (lossy_targets_.load()) socket->WatchClosely();
                if (want_host_) {
                    const std::string text = R"({"t":"host"})";
                    socket->Send(text.data(), text.size(), false);
                    if (want_open_) SendOpen(*socket);
                }
            }
            return;
        }
        if (message.size() < 1 + kSealOverhead) {
            Counted(NetCounts::Kind::Undecryptable, message.size());
            return;
        }
        const auto sender = static_cast<std::uint8_t>(message[0]);
        auto plain = Open(secret_.key, sender, reinterpret_cast<const std::uint8_t*>(message.data()) + 1, message.size() - 1);
        if (!plain) {
            Counted(NetCounts::Kind::Undecryptable, message.size());
            return;
        }
        Push({Event::Kind::Data, sender, std::move(*plain)});
    });
    {
        std::lock_guard lock(socket_mutex_);
        socket_.reset();
    }
    if (connected_.exchange(false)) {
        {
            std::lock_guard lock(mutex_);
            members_.clear();
            visitors_.clear();
        }
        slot_ = 0;
        host_ = 0;
        open_listed_ = false;
        Push({Event::Kind::Disconnected, 0, {}});
    } else if (!welcomed && !stop_) {
        const std::string reason = full_ || socket->CloseCode() == kCloseFull ? FullRefusal(full_most_.load()) : std::string();
        Push({Event::Kind::Refused, 0, {reason.begin(), reason.end()}});
    }
    return welcomed;
}

void Link::Handle(const std::string& text) {
    const auto message = nlohmann::json::parse(text, nullptr, false);
    if (!message.is_object()) return;
    const std::string type = message.value("t", "");
    const int slot = message.value("slot", 0);
    if (type == "full") {
        full_ = true;
        const int most = message.value("most", 0);
        if (most > 0 && most < 128) full_most_ = most;
    } else if (type == "welcome") {
        std::vector<int> members;
        if (message.contains("members") && message["members"].is_array()) {
            for (const auto& m : message["members"]) {
                if (m.is_number_integer()) members.push_back(m.get<int>());
            }
        }
        {
            std::lock_guard lock(mutex_);
            members_ = members;
            devs_.clear();
            if (message.contains("devs") && message["devs"].is_array()) {
                for (const auto& d : message["devs"]) {
                    if (d.is_number_integer()) devs_.push_back(d.get<int>());
                }
            }
            visitors_.clear();
            if (message.contains("visitors") && message["visitors"].is_array()) {
                for (const auto& v : message["visitors"]) {
                    if (v.is_number_integer()) visitors_.push_back(v.get<int>());
                }
            }
        }
        slot_ = slot;
        host_ = message.value("host", 0);
        lossy_targets_ = message.value("lossy", false);
        connected_ = true;
        Push({Event::Kind::Connected, slot, {}});
        for (const int m : members) Push({Event::Kind::Joined, m, {}});
        if (host_) Push({Event::Kind::Host, host_.load(), {}});
    } else if (type == "join") {
        {
            std::lock_guard lock(mutex_);
            if (std::find(members_.begin(), members_.end(), slot) == members_.end()) members_.push_back(slot);
        }
        Push({Event::Kind::Joined, slot, {}});
    } else if (type == "dev") {
        std::lock_guard lock(mutex_);
        devs_.push_back(slot);
    } else if (type == "leave") {
        {
            std::lock_guard lock(mutex_);
            members_.erase(std::remove(members_.begin(), members_.end(), slot), members_.end());
            devs_.erase(std::remove(devs_.begin(), devs_.end(), slot), devs_.end());
            visitors_.erase(std::remove(visitors_.begin(), visitors_.end(), slot), visitors_.end());
        }
        Push({Event::Kind::Left, slot, {}});
    } else if (type == "away") {
        if (slot > 0) Push({Event::Kind::Away, slot, {}});
    } else if (type == "visitor") {
        {
            std::lock_guard lock(mutex_);
            if (std::find(visitors_.begin(), visitors_.end(), slot) == visitors_.end()) visitors_.push_back(slot);
        }
        Push({Event::Kind::Visitor, slot, {}});
    } else if (type == "opened") {
        open_listed_ = message.value("on", false);
    } else if (type == "host") {
        host_ = slot;
        Push({Event::Kind::Host, slot, {}});
    } else if (type == "announce" && message.contains("text") && message["text"].is_string() &&
               message.contains("id") && message["id"].is_number_unsigned()) {
        const std::string words = message["text"].get<std::string>();
        Event event{Event::Kind::Announcement, 0, {words.begin(), words.end()}};
        event.id = message["id"].get<std::uint64_t>();
        Push(std::move(event));
    }
}

bool Link::IsDev(int slot) const {
    std::lock_guard lock(mutex_);
    return std::find(devs_.begin(), devs_.end(), slot) != devs_.end();
}

} // namespace BbRoom
