// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx_whole_hook_sites.h"
#include <cstddef>
#include <cstdint>

namespace BbEngine::GxWhole::Hooks {
bool On();
std::uint64_t ThreadCalls(Flush flush);
bool InstallSites(std::uint8_t* image, std::size_t size);
bool Install(std::uint8_t* image, std::size_t size, Mode requested, Mode accepted);
}
