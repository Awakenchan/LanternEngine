// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include "video_core/cache_file.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs_note.h"

namespace Vulkan::Breadcrumbs::Arming {

namespace {

constexpr std::string_view kLogFolder = "logs";
constexpr std::string_view kUserFolder = "user";

std::filesystem::path FromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

enum class ReadOnce : std::uint8_t { Read, Absent, Failed };

ReadOnce TryRead(const std::filesystem::path& path, Note& note) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return ReadOnce::Absent;
    }
    if (error || status.type() != std::filesystem::file_type::regular) {
        return ReadOnce::Failed;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return ReadOnce::Failed;
    }
    std::array<char, kNoteMaxBytes + 1> text{};
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (file.bad()) {
        return ReadOnce::Failed;
    }
    const auto size = static_cast<std::size_t>(file.gcount());
    note = size > kNoteMaxBytes ? NoteOf(NoteStatus::Read)
                                : ParseNote(std::string_view(text.data(), size));
    return ReadOnce::Read;
}

} // namespace

std::filesystem::path UserFolder(const char* user_dir) {
    if (user_dir == nullptr || user_dir[0] == '\0') {
        return {};
    }
    std::filesystem::path folder = FromUtf8(user_dir).lexically_normal();
    if (!folder.has_filename()) {
        folder = folder.parent_path();
    }
    return folder;
}

std::filesystem::path NotePath(const char* log_file, const char* user_dir) {
    if (log_file != nullptr && log_file[0] != '\0') {
        const std::filesystem::path folder = FromUtf8(log_file).parent_path();
        if (!folder.empty()) {
            return folder / kNoteName;
        }
    }
    const std::filesystem::path user = UserFolder(user_dir);
    if (user.filename() == kUserFolder && user.has_parent_path()) {
        return user.parent_path() / kLogFolder / kNoteName;
    }
    return std::filesystem::path(kLogFolder) / kNoteName;
}

Note ReadNote(const std::filesystem::path& path) {
    for (int attempt = 0; attempt < kNoteReadAttempts; ++attempt) {
        Note note;
        switch (TryRead(path, note)) {
        case ReadOnce::Read:
            return note;
        case ReadOnce::Absent:
            return NoteOf(NoteStatus::Absent);
        case ReadOnce::Failed:
            break;
        }
        if (attempt + 1 < kNoteReadAttempts) {
            std::this_thread::sleep_for(kNoteRetryWait);
        }
    }
    return NoteOf(NoteStatus::Unreadable);
}

bool WriteNote(const std::filesystem::path& path, std::uint32_t starts) {
    std::array<char, kNoteMaxBytes> text{};
    const std::size_t size = FormatNote(starts, text.data(), text.size());
    if (size == 0) {
        return false;
    }
    std::error_code error;
    if (const auto folder = path.parent_path(); !folder.empty()) {
        std::filesystem::create_directories(folder, error);
        if (error) {
            return false;
        }
    }
    std::array<u8, kNoteMaxBytes> bytes{};
    std::memcpy(bytes.data(), text.data(), size);
    return Storage::ReplaceCacheFile(path, std::span<const u8>(bytes.data(), size));
}

RemoveResult RemoveNote(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::remove(path, error)) {
        return RemoveResult::Removed;
    }
    return error ? RemoveResult::Failed : RemoveResult::Absent;
}

std::filesystem::path FallbackNotePath(const char* user_dir, const std::filesystem::path& primary) {
    const std::filesystem::path user = UserFolder(user_dir);
    if (user.empty()) {
        return {};
    }
    std::filesystem::path fallback = user / kNoteName;
    return fallback == primary ? std::filesystem::path{} : fallback;
}

ArmResult Arm(const std::filesystem::path& primary, const std::filesystem::path& fallback) {
    if (WriteNote(primary, kArmedStarts)) {
        return ArmResult::Written;
    }
    if (!fallback.empty() && WriteNote(fallback, kArmedStarts)) {
        return ArmResult::WrittenToFallback;
    }
    return ArmResult::Failed;
}

StartResult Start(Override override_value, const std::filesystem::path& primary,
                  const std::filesystem::path& fallback) {
    StartResult result;
    result.note = ReadNote(primary);
    const std::filesystem::path* used = &primary;
    if (result.note.status == NoteStatus::Absent && !fallback.empty()) {
        if (const Note other = ReadNote(fallback); other.status != NoteStatus::Absent) {
            result.note = other;
            result.from_fallback = true;
            used = &fallback;
        }
    }
    const std::filesystem::path& path = *used;
    result.decision = Decide(override_value, result.note);
    switch (result.decision.action) {
    case NoteAction::Leave:
        break;
    case NoteAction::Write:
        result.update = WriteNote(path, result.decision.starts_left) ? NoteUpdate::Written
                                                                     : NoteUpdate::Failed;
        break;
    case NoteAction::Remove:
        result.update = RemoveNote(path) == RemoveResult::Failed ? NoteUpdate::Failed
                                                                 : NoteUpdate::Removed;
        break;
    }
    return result;
}

} // namespace Vulkan::Breadcrumbs::Arming
