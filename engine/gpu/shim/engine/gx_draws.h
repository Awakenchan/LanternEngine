// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::GxDraws {
constexpr std::uint32_t kBindTag = 0xbb5e0000u;
bool On();
std::uint64_t BoundState();
void ForgetBound();
std::uint64_t SpillArea(std::uint64_t state, int stage);
std::uint64_t SharedBlockCopy(std::uint64_t state);
void Report();
bool Install(std::uint8_t* image, std::size_t size);
}
