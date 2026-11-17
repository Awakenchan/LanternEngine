// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "lantern_diag_c.h"

namespace Lantern {

enum class SwitchKind : std::uint8_t {
    Bool = LANTERN_KIND_Bool,
    Mode = LANTERN_KIND_Mode,
    Int = LANTERN_KIND_Int,
    Enum = LANTERN_KIND_Enum,
    Text = LANTERN_KIND_Text,
};
enum class Liveness : std::uint8_t { StartOnly, Live };
enum class Mode : std::uint8_t { Off, On, Compare };
enum class SwitchSource : std::uint8_t { Default, Ini, Env, Verify, Batch };

enum class SwitchId : std::uint16_t {
#define X(id, name, kind, def, mn, mx, live, step, flags) id,
#include "lantern_switches.inc"
#undef X
    kCount
};
inline constexpr std::size_t kSwitchCount = std::size_t(SwitchId::kCount);
static_assert(kSwitchCount == LANTERN_SW_ID_COUNT);

struct SwitchRow {
    std::string_view name;
    SwitchKind kind;
    std::string_view def;
    long min;
    long max;
    Liveness live;
    bool step;
    unsigned flags;
};

inline constexpr bool kRowStep = true;
inline constexpr bool kRowNoStep = false;

inline constexpr SwitchRow kSwitchRows[] = {
#define X(id, name, kind, def, mn, mx, live, step, flags) \
    {name, SwitchKind::kind, def, mn, mx, Liveness::live, kRow##step, flags},
#include "lantern_switches.inc"
#undef X
};
static_assert(std::size(kSwitchRows) == kSwitchCount);

constexpr const SwitchRow& Row(SwitchId id) {
    return kSwitchRows[std::size_t(id)];
}

constexpr long DefaultNumber(const SwitchRow& row) {
    long value = 0;
    char text[16] = {};
    for (std::size_t i = 0; i < row.def.size() && i + 1 < sizeof text; ++i) text[i] = row.def[i];
    lantern_parse_switch(int(row.kind), row.min, row.max, row.flags, text, &value);
    return value;
}

constexpr bool DefaultsParse() {
    for (const SwitchRow& row : kSwitchRows) {
        if (row.def.empty() || row.kind == SwitchKind::Text) continue;
        char text[16] = {};
        for (std::size_t i = 0; i < row.def.size() && i + 1 < sizeof text; ++i) text[i] = row.def[i];
        long value = 0;
        if (lantern_parse_switch(int(row.kind), row.min, row.max, row.flags, text, &value) != LANTERN_PARSE_OK) {
            return false;
        }
    }
    return true;
}
static_assert(DefaultsParse(), "every row's default is a valid value of its own kind and range");


struct SwitchValues {
    std::array<long, kSwitchCount> number{};
    std::array<const char*, kSwitchCount> text{};
    std::array<SwitchSource, kSwitchCount> source{};
};

struct Switches {
#define LANTERN_FIELD_Bool(id) bool id = false;
#define LANTERN_FIELD_Mode(id) Mode id = Mode::Off;
#define LANTERN_FIELD_Int(id) long id = 0;
#define LANTERN_FIELD_Enum(id) long id = 0;
#define LANTERN_FIELD_Text(id) const char* id = "";
#define X(id, name, kind, def, mn, mx, live, step, flags) LANTERN_FIELD_##kind(id)
#include "lantern_switches.inc"
#undef X
#undef LANTERN_FIELD_Bool
#undef LANTERN_FIELD_Mode
#undef LANTERN_FIELD_Int
#undef LANTERN_FIELD_Enum
#undef LANTERN_FIELD_Text
    SwitchValues values{};
    unsigned set_count = 0;

    constexpr long Number(SwitchId id) const { return values.number[std::size_t(id)]; }
    constexpr const char* Text(SwitchId id) const { return values.text[std::size_t(id)]; }
    constexpr SwitchSource Source(SwitchId id) const { return values.source[std::size_t(id)]; }
    constexpr bool IsSet(SwitchId id) const {
        return Source(id) == SwitchSource::Ini || Source(id) == SwitchSource::Env;
    }
    constexpr Mode ModeOf(SwitchId id) const {
        const long v = Number(id);
        return v <= 0 ? Mode::Off : v == 1 ? Mode::On : Mode::Compare;
    }

    static const Switches& Get();
    static const Switches& Load();
};

struct SwitchInput {
    const char* (*env)(const char* name, void* context) = nullptr;
    void* context = nullptr;
    std::string_view ini;
    const char* const* environment = nullptr;
};

extern constinit Switches g_switches;

inline const Switches& Switches::Get() {
    return g_switches;
}

bool ReloadSwitches(const SwitchInput& input);
std::size_t FormatSwitchesLine(char* out, std::size_t size, const Switches& s);
std::size_t FormatSwitchValue(char* out, std::size_t size, const Switches& s, SwitchId id);
std::size_t FormatSwitchWarnings(char* out, std::size_t size);
std::size_t FormatReceivedLines(char* out, std::size_t size);
void PrintSwitchesAtStart();
const SwitchRow* FindRow(std::string_view name, SwitchId* id = nullptr);

enum class LogChannel : std::uint8_t { Always, FrameStats, Steps };
[[gnu::format(printf, 2, 3)]] void Log(LogChannel channel, const char* format, ...);
void LogLine(LogChannel channel, std::string_view line);
void LogScrubbed(LogChannel channel, std::string_view line);
bool LogChannelOn(LogChannel channel);
void SetLogSinkForTest(void (*sink)(std::string_view line, void* context), void* context);

}
