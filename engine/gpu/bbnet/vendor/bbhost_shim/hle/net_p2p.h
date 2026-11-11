// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
constexpr std::uint8_t kSignalMagic[4] = {0xfe, 'b', 'b', 's'};
void hle_net_p2p_signal(std::uint32_t addr, std::uint16_t port, const void* data, std::size_t len);
