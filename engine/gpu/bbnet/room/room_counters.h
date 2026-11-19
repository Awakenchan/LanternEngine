// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#ifdef LANTERN_NET_COUNTS_MUTANTS
namespace BbRoom::NetCounts {
inline int g_mutant = 0;
}
#define LANTERN_NET_COUNTS_PICK(n, wrong, right) (::BbRoom::NetCounts::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_NET_COUNTS_PICK(n, wrong, right) (right)
#endif

namespace BbRoom::NetCounts {

enum class Kind : std::size_t {
    BundleSent,
    BundleLost,
    BacklogRefused,
    Offline,
    SendFailed,
    Undecryptable,
    BadBundle,
    BridgeMissing,
    BridgeSendFailed,
    BridgeRecvError,
    OpenFailed,
    Reconnect,
    Count
};

constexpr std::size_t kKinds = static_cast<std::size_t>(Kind::Count);

inline const char* Name(Kind kind) {
    static constexpr std::array<const char*, kKinds> names = {
        "bundle_sent",    "bundle_lost",    "backlog_refused",  "offline",        "send_failed", "undecryptable",
        "bad_bundle",     "bridge_missing", "bridge_send_failed", "bridge_recv_error", "open_failed", "reconnects"};
    return names[static_cast<std::size_t>(kind)];
}

inline bool HasBytes(Kind kind) {
    return kind != Kind::BridgeRecvError && kind != Kind::OpenFailed && kind != Kind::Reconnect;
}

struct Tally {
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
    bool operator==(const Tally&) const = default;
};

struct Snapshot {
    std::array<Tally, kKinds> tally{};
    std::uint64_t backlog_peak = 0;
    bool operator==(const Snapshot&) const = default;
    const Tally& operator[](Kind kind) const { return tally[static_cast<std::size_t>(kind)]; }
};

class Counters {
public:
    void Add(Kind kind, std::size_t bytes = 0) {
        const auto i = static_cast<std::size_t>(kind);
        counts_[i].fetch_add(LANTERN_NET_COUNTS_PICK(1, 0, 1), std::memory_order_relaxed);
        bytes_[i].fetch_add(LANTERN_NET_COUNTS_PICK(2, 0, bytes), std::memory_order_relaxed);
    }
    void Backlog(std::size_t now) {
        std::uint64_t peak = peak_.load(std::memory_order_relaxed);
        while (LANTERN_NET_COUNTS_PICK(3, now < peak, now > peak) &&
               !peak_.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
        }
    }
    Snapshot Take() const {
        Snapshot out;
        for (std::size_t i = 0; i < kKinds; ++i) {
            out.tally[i].count = counts_[i].load(std::memory_order_relaxed);
            out.tally[i].bytes = bytes_[i].load(std::memory_order_relaxed);
        }
        out.backlog_peak = peak_.load(std::memory_order_relaxed);
        return out;
    }

private:
    std::array<std::atomic<std::uint64_t>, kKinds> counts_{};
    std::array<std::atomic<std::uint64_t>, kKinds> bytes_{};
    std::atomic<std::uint64_t> peak_{0};
};

inline std::string Line(const Snapshot& s) {
    std::string out;
    char part[96];
    for (std::size_t i = 0; i < kKinds; ++i) {
        const auto kind = static_cast<Kind>(i);
        const Tally& t = s.tally[i];
        if (LANTERN_NET_COUNTS_PICK(4, false, t.count == 0)) continue;
        if (HasBytes(kind)) {
            std::snprintf(part, sizeof part, "%s%s=%llu/%lluB", out.empty() ? "" : " ", Name(kind),
                          static_cast<unsigned long long>(t.count),
                          static_cast<unsigned long long>(LANTERN_NET_COUNTS_PICK(5, t.count, t.bytes)));
        } else {
            std::snprintf(part, sizeof part, "%s%s=%llu", out.empty() ? "" : " ", Name(kind),
                          static_cast<unsigned long long>(t.count));
        }
        out += part;
    }
    if (s.backlog_peak) {
        std::snprintf(part, sizeof part, "%sbacklog_peak=%lluB", out.empty() ? "" : " ",
                      static_cast<unsigned long long>(s.backlog_peak));
        out += part;
    }
    return out.empty() ? std::string("nothing sent or dropped") : out;
}

class Reporter {
public:
    using Clock = std::chrono::steady_clock;

    explicit Reporter(Clock::duration every = std::chrono::seconds(30)) : every_(every) {}

    std::optional<std::string> Tick(const Snapshot& now, Clock::time_point at, bool in_room) {
        if (!in_room) {
            next_ = {};
            return std::nullopt;
        }
        joined_ = true;
        if (next_ == Clock::time_point{}) {
            next_ = at + every_;
            return std::nullopt;
        }
        if (LANTERN_NET_COUNTS_PICK(6, false, at < next_)) return std::nullopt;
        next_ = at + every_;
        if (LANTERN_NET_COUNTS_PICK(7, false, now == last_)) return std::nullopt;
        last_ = now;
        return "net: " + Line(now);
    }

    std::optional<std::string> Leave(const Snapshot& now) {
        if (LANTERN_NET_COUNTS_PICK(8, false, !joined_ || left_)) return std::nullopt;
        left_ = true;
        return "net on leaving the room: " + Line(now);
    }

private:
    Clock::duration every_;
    Clock::time_point next_{};
    Snapshot last_{};
    bool joined_ = false;
    bool left_ = false;
};

enum class RecvError { None, WouldBlock, Transient, Fatal };
enum class RecvStep { Forward, Skip, Drained, Stop };

constexpr int kMostRecvErrorsInRow = 16;

inline RecvStep AfterRecv(long n, RecvError error, int errors_in_row) {
    if (n > 0) return RecvStep::Forward;
    if (n < 0 && error == RecvError::WouldBlock) return RecvStep::Drained;
    if (errors_in_row >= LANTERN_NET_COUNTS_PICK(10, kMostRecvErrorsInRow + 1000, kMostRecvErrorsInRow))
        return RecvStep::Stop;
    if (n == 0 || LANTERN_NET_COUNTS_PICK(9, error == RecvError::Fatal, error == RecvError::Transient)) return RecvStep::Skip;
    return LANTERN_NET_COUNTS_PICK(11, RecvStep::Skip, RecvStep::Stop);
}

inline bool CountsAsRecvError(RecvError error) {
    return LANTERN_NET_COUNTS_PICK(12, error != RecvError::Fatal, error == RecvError::Transient || error == RecvError::Fatal);
}

#ifdef LANTERN_NET_COUNTS_MUTANTS
inline constexpr int kMutants = 12;
#endif

}
