// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(BB_MARKER_MUTANTS)
namespace Vulkan::Breadcrumbs::Arming {
inline int marker_mutant = 0;
}
#define BB_MARKER_MUTANT(n, wrong, right)                                                         \
    (::Vulkan::Breadcrumbs::Arming::marker_mutant == (n) ? (wrong) : (right))
#else
#define BB_MARKER_MUTANT(n, wrong, right) (right)
#endif

namespace Vulkan::Breadcrumbs::Arming {

inline constexpr std::uint32_t kArmedStarts = 3;
inline constexpr std::string_view kNoteName = "gpu-markers.armed";
inline constexpr std::string_view kNoteKey = "starts ";
inline constexpr std::size_t kNoteMaxBytes = 64;

enum class Override : std::uint8_t { None, ForcedOff, ForcedOn };
enum class NoteStatus : std::uint8_t { Absent, Read, Unreadable };
enum class Reason : std::uint8_t { Default, Armed, NoteDamaged, NoteUnreadable, ForcedOn, ForcedOff };
enum class NoteAction : std::uint8_t { Leave, Write, Remove };
enum class Trigger : std::uint8_t { DeviceLost, GpuStuck };

struct Note {
    NoteStatus status = NoteStatus::Absent;
    bool valid = false;
    std::uint32_t starts = 0;
};

[[nodiscard]] constexpr Note NoteOf(NoteStatus status) noexcept {
    Note note;
    note.status = status;
    return note;
}

struct Decision {
    bool on = false;
    Reason reason = Reason::Default;
    NoteAction action = NoteAction::Leave;
    std::uint32_t starts_left = 0;
};

[[nodiscard]] constexpr Override ParseOverride(const char* value) noexcept {
    if (value == nullptr || BB_MARKER_MUTANT(5, false, value[0] == '\0')) {
        return Override::None;
    }
    return value[0] == '0' ? Override::ForcedOff : Override::ForcedOn;
}

[[nodiscard]] inline Note ParseNote(std::string_view text) noexcept {
    Note note = NoteOf(NoteStatus::Read);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (!text.starts_with(kNoteKey)) {
        return note;
    }
    text.remove_prefix(kNoteKey.size());
    std::uint32_t starts = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), starts);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return note;
    }
    note.valid = true;
    note.starts = BB_MARKER_MUTANT(4, starts, starts > kArmedStarts ? kArmedStarts : starts);
    return note;
}

[[nodiscard]] constexpr Decision Decide(Override override_value, const Note& note) noexcept {
    Decision d;
    bool armed = false;
    if (note.status == NoteStatus::Unreadable) {
        armed = BB_MARKER_MUTANT(3, false, true);
        d.reason = Reason::NoteUnreadable;
        d.action = NoteAction::Leave;
    } else if (note.status == NoteStatus::Read && !note.valid) {
        armed = true;
        d.reason = Reason::NoteDamaged;
        d.action = NoteAction::Remove;
    } else if (note.status == NoteStatus::Read && note.starts > 0) {
        armed = true;
        d.reason = Reason::Armed;
        d.starts_left = BB_MARKER_MUTANT(1, note.starts, note.starts - 1);
        d.action = d.starts_left > 0 ? NoteAction::Write : NoteAction::Remove;
    } else if (note.status == NoteStatus::Read) {
        d.action = NoteAction::Remove;
    }
    if (override_value == Override::ForcedOn) {
        d.on = true;
        d.reason = Reason::ForcedOn;
    } else if (override_value == Override::ForcedOff) {
        d.on = BB_MARKER_MUTANT(2, armed, false);
        d.reason = Reason::ForcedOff;
    } else {
        d.on = armed;
    }
    return d;
}

[[nodiscard]] constexpr bool ArmsNextStarts(Trigger trigger) noexcept {
    switch (trigger) {
    case Trigger::DeviceLost:
        return true;
    case Trigger::GpuStuck:
        return BB_MARKER_MUTANT(6, false, true);
    }
    return false;
}

[[nodiscard]] inline std::size_t FormatNote(std::uint32_t starts, char* out, std::size_t room) noexcept {
    if (room < kNoteKey.size() + 2) {
        return 0;
    }
    std::size_t used = 0;
    for (const char c : kNoteKey) {
        out[used++] = c;
    }
    const auto [end, error] = std::to_chars(out + used, out + room - 1, starts);
    if (error != std::errc{}) {
        return 0;
    }
    used = static_cast<std::size_t>(end - out);
    out[used++] = '\n';
    return used;
}

[[nodiscard]] constexpr const char* ReasonText(Reason reason) noexcept {
    switch (reason) {
    case Reason::Default:
        return "default";
    case Reason::Armed:
        return "armed by a device loss";
    case Reason::NoteDamaged:
        return "armed: the device-loss note was damaged and is removed";
    case Reason::NoteUnreadable:
        return "armed: the device-loss note could not be read and is left as it is";
    case Reason::ForcedOn:
        return "BB_BREADCRUMBS forces them on";
    case Reason::ForcedOff:
        return "BB_BREADCRUMBS=0 forces them off";
    }
    return "?";
}

} // namespace Vulkan::Breadcrumbs::Arming
