// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BbThd {
inline constexpr int kNpUtilErrorNotMatch = static_cast<int>(0x80550609);
inline constexpr std::size_t kMaxDatagram = 65507;

namespace Signal {
inline bool IsSignal(const std::uint8_t* data, std::size_t size) {
    static constexpr std::uint8_t kMagic[4] = {0xfe, 'b', 'b', 's'};
    return size >= sizeof(kMagic) && std::memcmp(data, kMagic, sizeof(kMagic)) == 0;
}
}

inline int CompareOnlineId(const void* a, const void* b) {
    return std::strncmp(static_cast<const char*>(a), static_cast<const char*>(b), 16) != 0 ? kNpUtilErrorNotMatch : 0;
}

inline int CompareNpId(const void* a, const void* b) {
    const auto* x = static_cast<const std::uint8_t*>(a);
    const auto* y = static_cast<const std::uint8_t*>(b);
    if (CompareOnlineId(x, y) != 0) return kNpUtilErrorNotMatch;
    if (std::memcmp(x + 20, y + 20, 8) != 0) return kNpUtilErrorNotMatch;
    if (std::memcmp(x + 28, y + 28, 8) != 0) return kNpUtilErrorNotMatch;
    return 0;
}
}
