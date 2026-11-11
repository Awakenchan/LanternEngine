// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <type_traits>

template <typename T> std::int64_t bbhost_guest_arg(T value) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<std::int64_t>(value);
    } else {
        return static_cast<std::int64_t>(value);
    }
}

template <typename R, typename... A> R hle_call_guest(void* fn, A... a) {
    using Fn = std::int64_t(__attribute__((sysv_abi)) *)(decltype(bbhost_guest_arg(a))...);
    if (!fn) return R{};
    return static_cast<R>(reinterpret_cast<Fn>(fn)(bbhost_guest_arg(a)...));
}
