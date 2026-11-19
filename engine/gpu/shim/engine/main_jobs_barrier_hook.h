// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace BbEngine::MainJobsBarrier {
enum class [[nodiscard]] Installed : std::uint8_t { Yes, NotAsked, Refused };

Installed Install(std::uint8_t* image, std::size_t size) noexcept;
void Tick() noexcept;
bool LatchedOn() noexcept;
std::atomic<int>& Switch() noexcept;
}
