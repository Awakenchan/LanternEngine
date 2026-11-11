// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace BbOnlineGate {
template <typename Get> const char* ConfiguredBy(Get get) {
    const auto set = [&](const char* key) {
        const char* v = get(key);
        return v && v[0];
    };
    if (const char* online = get("BB_ONLINE"); online && online[0] == '1') return "online play (BB_ONLINE=1)";
    if (const char* backend = get("BB_NET_BACKEND"); backend && backend[0]) {
        if (!std::strcmp(backend, "room")) return "seamless co-op (BB_NET_BACKEND=room)";
        if (!std::strcmp(backend, "thd")) return "The Hunter's Dream (BB_NET_BACKEND=thd)";
        return "an online backend (BB_NET_BACKEND)";
    }
    for (const char* key : {"BB_ROOM_SERVER", "BB_ROOM_NAME"})
        if (set(key)) return "seamless co-op room settings";
    for (const char* key : {"BB_THD_SERVER", "BB_THD_TOKEN"})
        if (set(key)) return "The Hunter's Dream settings";
    for (const char* key : {"BB_SHADNET_SERVER", "BB_SHADNET_NPID"})
        if (set(key)) return "shadNet settings";
    return nullptr;
}

inline std::atomic<const char*>& LiveReason() {
    static std::atomic<const char*> reason{nullptr};
    return reason;
}

inline void MarkLive(const char* what) {
    const char* none = nullptr;
    LiveReason().compare_exchange_strong(none, what);
}

inline const char* Configured() {
    static const char* const reason = ConfiguredBy([](const char* key) { return std::getenv(key); });
    return reason;
}

inline const char* Reason() {
    if (const char* r = Configured()) return r;
    return LiveReason().load(std::memory_order_acquire);
}

inline int Allowed(int value, int off, const char* reason) { return reason ? off : value; }
}
