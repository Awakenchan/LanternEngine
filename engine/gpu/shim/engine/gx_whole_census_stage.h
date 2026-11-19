// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace AmdGpu {
union Regs;
}
namespace Vulkan {
class Rasterizer;
}

namespace BbEngine::GxWholeCensus {
void OnPacket(const std::uint32_t* packet, bool token_draw);
void OnTokenDraw(const AmdGpu::Regs& regs, std::uint32_t predicate, std::uint64_t record, Vulkan::Rasterizer& rasterizer);
void OnPacketTicks(std::uint64_t ticks, bool draw_packet);
void OnFlip();
void OnSubmissionEnd();
void OnConstantEngine(std::size_t dwords);
}
