// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx_whole.h"
#include <cstddef>
#include <cstdint>

namespace AmdGpu {
union Regs;
}
namespace Vulkan {
class PipelineCache;
class GraphicsPipeline;
struct PreparedDraw;
}

namespace BbEngine::GxWholeRoute {
void Start(GxWhole::Mode mode, bool counting);
bool On();
bool Disabled();
bool MarksPrepared();
void NoteMarked(std::uint32_t draws);
const Vulkan::GraphicsPipeline* Select(Vulkan::PipelineCache& cache, AmdGpu::Regs& regs, const Vulkan::PreparedDraw* prepared,
                                       std::uint64_t gx_record, bool indexed);
bool Line(char* out, std::size_t size, bool* suspect);
}
