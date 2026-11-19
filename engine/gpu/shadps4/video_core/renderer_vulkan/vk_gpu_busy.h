// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
}

namespace Vulkan::GpuBusy {
void Init(const Instance& instance, Scheduler& watched);
void Begin(const Scheduler* owner, vk::CommandBuffer cmdbuf);
void End(const Scheduler* owner, vk::CommandBuffer cmdbuf);
void Shutdown();
}
