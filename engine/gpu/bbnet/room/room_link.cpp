// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_link.h"

#include <algorithm>
#include <chrono>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

namespace BbRoom {

constexpr int kLossyTarget = 0x80;
constexpr std::size_t kMostGameBacklog = 128 * 1024;

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
    if (server.empty() || name.empty()) return false;
    secret_ = Derive(name, password);
    std::uint8_t token[16];
    RAND_bytes(token, sizeof token);
    static const char digits[] = "0123456789abcdef";
    std::string member;
    for (const std::uint8_t byte : token) {
        member.push_back(digits[byte >> 4]);
        member.push_back(digits[byte & 15]);
    }
    url_ = RoomUrl(server, secret_.room_id, path) + "/" + member;
    if (visitor_ && path == "room") url_ += "?visitor=1";
    name_ = name;
    password_ = password;
    stop_ = false;
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

bool Link::Send(int target, const std::uint8_t* data, std::size_t size) {
    const int slot = slot_.load();
    if (!connected_ || !slot || target == slot || target < 0 || target > 255) return false;
    auto sealed = Seal(secret_.key, static_cast<std::uint8_t>(slot), data, size);
    if (sealed.empty()) return false;
    sealed.insert(sealed.begin(), static_cast<std::uint8_t>(target));
    std::shared_ptr<WebSocket> socket;
    {
        std::lock_guard lock(socket_mutex_);
        socket = socket_;
    }
    return socket && socket->Send(sealed.data(), sealed.size(), true);
}

bool Link::SendLossy(int target, const std::uint8_t* data, std::size_t size, std::size_t most_backlog) {
    const int slot = slot_.load();
    if (!connected_ || !slot || target == slot || target < 0 || target > 255) return false;
    auto sealed = Seal(secret_.key, static_cast<std::uint8_t>(slot), data, size);
    if (sealed.empty()) return false;
    sealed.insert(sealed.begin(), static_cast<std::uint8_t>(target));
    std::shared_ptr<WebSocket> socket;
    {
        std::lock_guard lock(socket_mutex_);
        socket = socket_;
    }
    return socket && socket->SendLossy(sealed.data(), sealed.size(), most_backlog);
}

bool Link::SendGame(int target, const std::uint8_t* data, std::size_t size) {
    const int slot = slot_.load();
    if (!connected_ || !slot || target == slot || target < 0 || target > 127) return false;
    auto sealed = Seal(secret_.key, static_cast<std::uint8_t>(slot), data, size);
    if (sealed.empty()) return false;
    const int flagged = lossy_targets_.load() ? target | kLossyTarget : target;
    sealed.insert(sealed.begin(), static_cast<std::uint8_t>(flagged));
    std::shared_ptr<WebSocket> socket;
    {
        std::lock_guard lock(socket_mutex_);
        socket = socket_;
    }
    return socket && socket->SendLossy(sealed.data(), sealed.size(), kMostGameBacklog);
}

bool Link::SendPaced(int target, const std::uint8_t* data, std::size_t size) {
    const int slot = slot_.load();
    if (!connected_ || !slot || target == slot || target < 0 || target > 127) return false;
    auto sealed = Seal(secret_.key, static_cast<std::uint8_t>(slot), data, size);
    if (sealed.empty()) return false;
    sealed.insert(sealed.begin(), static_cast<std::uint8_t>(target));
    std::shared_ptr<WebSocket> socket;
    {
        std::lock_guard lock(socket_mutex_);
        socket = socket_;
    }
    return socket && socket->SendPaced(sealed.data(), sealed.size());
}

void Link::SetHost(bool host) {
    want_host_ = host;
    std::shared_ptr<WebSocket> socket;
    {
        std::lock_guard lock(socket_mutex_);
        socket = socket_;
    }
    const std::string text = host ? R"({"t":"host"})" : R"({"t":"unhost"})";
    if (socket && connected_) socket->Send(text.data(), text.size(), false);
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
    std::shared_ptr<WebSocket> socket;
    {
        std::lock_guard lock(socket_mutex_);
        socket = socket_;
    }
    if (socket && connected_) SendOpen(*socket);
}

void Link::SendAway(int slot) {
    std::shared_ptr<WebSocket> socket;
    {
        std::lock_guard lock(socket_mutex_);
        socket = socket_;
    }
    const std::string text = nlohmann::json{{"t", "away"}, {"slot", slot}}.dump();
    if (socket && connected_) socket->Send(text.data(), text.size(), false);
}

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
        last_error_ = error;
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
                if (lossy_targets_.load()) socket->WatchClosely();
                if (want_host_) {
                    const std::string text = R"({"t":"host"})";
                    socket->Send(text.data(), text.size(), false);
                    if (want_open_) SendOpen(*socket);
                }
            }
            return;
        }
        if (message.size() < 1 + kSealOverhead) return;
        const auto sender = static_cast<std::uint8_t>(message[0]);
        auto plain = Open(secret_.key, sender, reinterpret_cast<const std::uint8_t*>(message.data()) + 1, message.size() - 1);
        if (!plain) return;
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
