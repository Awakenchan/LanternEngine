// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include "video_core/renderer_vulkan/vk_breadcrumbs_arming.h"

namespace Vulkan::Breadcrumbs::Arming {

inline constexpr int kNoteReadAttempts = 3;
inline constexpr std::chrono::milliseconds kNoteRetryWait{20};

enum class RemoveResult : std::uint8_t { Removed, Absent, Failed };
enum class NoteUpdate : std::uint8_t { None, Written, Removed, Failed };
enum class ArmResult : std::uint8_t { Written, WrittenToFallback, Failed };

struct StartResult {
    Note note;
    Decision decision;
    NoteUpdate update = NoteUpdate::None;
    bool from_fallback = false;
};

[[nodiscard]] std::filesystem::path NotePath(const char* log_file, const char* user_dir);
[[nodiscard]] Note ReadNote(const std::filesystem::path& path);
[[nodiscard]] bool WriteNote(const std::filesystem::path& path, std::uint32_t starts);
[[nodiscard]] RemoveResult RemoveNote(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path FallbackNotePath(const char* user_dir, const std::filesystem::path& primary);
[[nodiscard]] ArmResult Arm(const std::filesystem::path& primary, const std::filesystem::path& fallback);
[[nodiscard]] StartResult Start(Override override_value, const std::filesystem::path& primary,
                                const std::filesystem::path& fallback);

} // namespace Vulkan::Breadcrumbs::Arming
