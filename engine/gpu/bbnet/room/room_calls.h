// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <utility>
#include <vector>

#ifdef LANTERN_ROOM_CALLS_MUTANTS
namespace BbRoom::Calls {
inline int g_mutant = 0;
inline constexpr int kMutants = 10;
}
#define LANTERN_ROOM_CALLS_PICK(n, wrong, right) (::BbRoom::Calls::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_ROOM_CALLS_PICK(n, wrong, right) (right)
#endif

namespace BbRoom::Calls {

inline constexpr std::size_t kRecentCalls = 64;
inline constexpr std::size_t kRecentBytes = 1024 * 1024;

inline bool Resendable(const std::string& path) {
    static const char* const kSafe[] = {
        "/mp/matching2/context_start", "/np/signaling/resolve",   "/mp/matching2/join_room",
        "/mp/matching2/leave_room",    "/mp/matching2/heartbeat", "/lantern/leave_party",
        "/summon_messenger/create",    "/summon_messenger/get",   "/summon_messenger/delete",
    };
    if (LANTERN_ROOM_CALLS_PICK(1, true, false)) return true;
    if (path.rfind("/channel/", 0) == 0) return true;
    for (const char* safe : kSafe)
        if (path == safe) return true;
    return false;
}

inline std::uint64_t Hash(const std::uint8_t* data, std::size_t size) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

struct Owed {
    std::uint32_t id = 0;
    int host = 0;
    std::vector<std::uint8_t> frame;
};

class Pending {
public:
    Pending() : next_(static_cast<std::uint32_t>(std::random_device{}())) {}
    explicit Pending(std::uint32_t first) : next_(first) {}

    std::uint32_t NextId() {
        if (!next_) ++next_;
        return next_++;
    }

    void Add(std::uint32_t id, int host, std::vector<std::uint8_t> frame, bool resend) {
        calls_.push_back({id, host, std::move(frame), resend, false});
    }

    bool Answer(std::uint32_t id) {
        for (auto it = calls_.begin(); it != calls_.end(); ++it) {
            if (it->id != id) continue;
            if (LANTERN_ROOM_CALLS_PICK(2, false, true)) calls_.erase(it);
            return true;
        }
        return LANTERN_ROOM_CALLS_PICK(3, true, false);
    }

    void Drop(std::uint32_t id) {
        for (auto it = calls_.begin(); it != calls_.end(); ++it) {
            if (it->id != id) continue;
            if (LANTERN_ROOM_CALLS_PICK(4, false, true)) calls_.erase(it);
            return;
        }
    }

    void Unsent(std::uint32_t id) {
        for (Call& c : calls_)
            if (c.id == id) c.owed = true;
    }

    void Lost() {
        for (Call& c : calls_) c.owed = LANTERN_ROOM_CALLS_PICK(5, c.owed, true);
    }

    std::vector<Owed> Back(int host) {
        std::vector<Owed> out;
        for (Call& c : calls_) {
            if (!c.owed || !LANTERN_ROOM_CALLS_PICK(6, true, c.resend) || LANTERN_ROOM_CALLS_PICK(7, false, c.host != host)) continue;
            c.owed = LANTERN_ROOM_CALLS_PICK(8, true, false);
            out.push_back({c.id, c.host, c.frame});
        }
        return out;
    }

    std::size_t Size() const { return calls_.size(); }

private:
    struct Call {
        std::uint32_t id;
        int host;
        std::vector<std::uint8_t> frame;
        bool resend;
        bool owed;
    };
    std::uint32_t next_;
    std::deque<Call> calls_;
};

class Recent {
public:
    Recent() = default;
    Recent(std::size_t most, std::size_t most_bytes) : most_(most), most_bytes_(most_bytes) {}

    const std::string* Find(int slot, std::uint32_t id, const std::uint8_t* request, std::size_t size) const {
        if (!id) return nullptr;
        const std::uint64_t hash = Hash(request, size);
        for (const Entry& e : entries_)
            if (e.slot == slot && e.id == id && LANTERN_ROOM_CALLS_PICK(9, true, e.hash == hash)) return &e.reply;
        return nullptr;
    }

    void Keep(int slot, std::uint32_t id, const std::uint8_t* request, std::size_t size, std::string reply) {
        if (!id) return;
        const std::uint64_t hash = Hash(request, size);
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->slot != slot || it->id != id) continue;
            bytes_ -= it->reply.size();
            entries_.erase(it);
            break;
        }
        if (reply.size() > most_bytes_) return;
        bytes_ += reply.size();
        entries_.push_back({slot, id, hash, std::move(reply)});
        while (LANTERN_ROOM_CALLS_PICK(10, false, entries_.size() > most_ || bytes_ > most_bytes_)) {
            bytes_ -= entries_.front().reply.size();
            entries_.pop_front();
        }
    }

    void Forget(int slot) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->slot != slot) {
                ++it;
                continue;
            }
            bytes_ -= it->reply.size();
            it = entries_.erase(it);
        }
    }

    std::size_t Size() const { return entries_.size(); }
    std::size_t Bytes() const { return bytes_; }

private:
    struct Entry {
        int slot;
        std::uint32_t id;
        std::uint64_t hash;
        std::string reply;
    };
    std::size_t most_ = kRecentCalls;
    std::size_t most_bytes_ = kRecentBytes;
    std::size_t bytes_ = 0;
    std::deque<Entry> entries_;
};

}
