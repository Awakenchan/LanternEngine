// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <span>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"

namespace Vulkan::EffectShaders {

inline constexpr u64 SsaoHash = 0xcbbca5dc;
inline constexpr u64 SsrHash = 0x6c2323c2;
inline constexpr u64 SsrCompositeHash = 0x3fe9e964;
inline constexpr u64 MotionBlurHash = 0x29e06868;

std::span<const u32> Replacement(const Shader::Info& info, const Shader::Profile& profile,
                                 const Shader::Backend::Bindings& start);

std::optional<std::vector<u32>> Patched(const Shader::Info& info, std::span<const u32> spv);

bool KeepRecompiled(const Shader::Info& info);
bool KeepRecompiled(const Shader::Info& info, bool modern_ssr, bool pom_cavity);
bool KeepRecompiledHash(u64 pgm_hash, bool modern_ssr, bool pom_cavity);

bool GtaoActive();
bool SsrActive();
bool SsrCompositeActive();

}
