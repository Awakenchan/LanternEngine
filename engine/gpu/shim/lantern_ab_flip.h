// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Lantern::AbFlip {
using u64 = std::uint64_t;

inline constexpr std::size_t kMaxSwitches = 16;

enum class Half : std::uint8_t { None, Before, After };

enum class Registered : std::uint8_t { Added, Off, Full, TooLate };

inline Half HalfAt(long flip_after_s, u64 since_first_frame_ns) {
    if (flip_after_s <= 0) return Half::None;
    return since_first_frame_ns >= u64(flip_after_s) * 1'000'000'000ull ? Half::After : Half::Before;
}

[[nodiscard]] Registered Register(const char* name, std::atomic<int>* value, int flipped);
bool Unregister(const std::atomic<int>* value);
Half Current();
bool Advance(u64 since_first_frame_ns);
}
