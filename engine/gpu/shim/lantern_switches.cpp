// SPDX-License-Identifier: GPL-3.0-or-later
#include "lantern_switches.h"
#include "lantern_scrub.h"
#include "lantern_shown_values.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

#ifdef _WIN32
#include <stdlib.h>
#define LANTERN_ENVIRON _environ
#else
extern char** environ;
#define LANTERN_ENVIRON environ
#endif

namespace Lantern {

namespace {

constexpr std::size_t kTextArenaBytes = 16384;
constexpr std::size_t kWarningBytes = 4096;
constexpr std::size_t kUnknownBytes = 2048;
constexpr std::size_t kReceivedBytes = 16384;
constexpr std::size_t kReceivedLineBytes = 1600;
constexpr std::size_t kReceivedValueBytes = 4096;
constexpr std::size_t kIniBytes = 65536;
constexpr std::size_t kLogLineBytes = 2048;
constexpr std::size_t kShownTextMax = 64;
constexpr std::string_view kSwitchPrefixes[] = {"BB_", "LANTERN_"};
constexpr std::pair<const char*, Scrub::Kind> kProfileNames[] = {
    {"USERPROFILE", Scrub::Kind::Home}, {"HOME", Scrub::Kind::Home}, {"USERNAME", Scrub::Kind::User},
    {"USER", Scrub::Kind::User}, {"OneDrive", Scrub::Kind::OneDrive}, {"OneDriveConsumer", Scrub::Kind::OneDrive},
    {"OneDriveCommercial", Scrub::Kind::OneDrive}};

constexpr void ApplyFields(Switches& s) {
#define LANTERN_APPLY_Bool(id) s.id = s.Number(SwitchId::id) != 0;
#define LANTERN_APPLY_Mode(id) s.id = s.ModeOf(SwitchId::id);
#define LANTERN_APPLY_Int(id) s.id = s.Number(SwitchId::id);
#define LANTERN_APPLY_Enum(id) s.id = s.Number(SwitchId::id);
#define LANTERN_APPLY_Text(id) s.id = s.Text(SwitchId::id);
#define X(id, name, kind, def, mn, mx, live, step, flags) LANTERN_APPLY_##kind(id)
#include "lantern_switches.inc"
#undef X
#undef LANTERN_APPLY_Bool
#undef LANTERN_APPLY_Mode
#undef LANTERN_APPLY_Int
#undef LANTERN_APPLY_Enum
#undef LANTERN_APPLY_Text
}

constexpr Switches Defaults() {
    Switches s{};
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        s.values.number[i] = DefaultNumber(kSwitchRows[i]);
        s.values.text[i] = kSwitchRows[i].def.data();
        s.values.source[i] = SwitchSource::Default;
    }
    ApplyFields(s);
    return s;
}

struct LoadState {
    char arena[kTextArenaBytes];
    std::size_t arena_used;
    char warnings[kWarningBytes];
    std::size_t warnings_used;
    char unknown[kUnknownBytes];
    std::size_t unknown_used;
    unsigned unknown_count;
    char received[kReceivedBytes];
    std::size_t received_used;
    unsigned received_count;
    char ini[kIniBytes];
};

LoadState g_load;
std::once_flag g_load_once;

void (*g_log_sink)(std::string_view, void*) = nullptr;
void* g_log_sink_context = nullptr;

void Append(char* buffer, std::size_t size, std::size_t& used, std::string_view text) {
    if (used >= size) return;
    const std::size_t room = size - 1 - used;
    const std::size_t n = text.size() < room ? text.size() : room;
    std::memcpy(buffer + used, text.data(), n);
    used += n;
    buffer[used] = 0;
}

void Warn(const char* format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n <= 0) return;
    Append(g_load.warnings, sizeof g_load.warnings, g_load.warnings_used,
           std::string_view(line, std::size_t(n) < sizeof line ? std::size_t(n) : sizeof line - 1));
    Append(g_load.warnings, sizeof g_load.warnings, g_load.warnings_used, "\n");
}

const char* Keep(std::string_view text) {
    if (g_load.arena_used + text.size() + 1 > sizeof g_load.arena) {
        Warn("Switches: no room left to keep a text value of %zu bytes; the default is used", text.size());
        return nullptr;
    }
    char* at = g_load.arena + g_load.arena_used;
    std::memcpy(at, text.data(), text.size());
    at[text.size()] = 0;
    g_load.arena_used += text.size() + 1;
    return at;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool HasSwitchPrefix(std::string_view name) {
    for (std::string_view prefix : kSwitchPrefixes) {
        if (name.substr(0, prefix.size()) == prefix) return true;
    }
    return false;
}

void Take(Switches& s, std::size_t i, std::string_view raw, SwitchSource source) {
    const SwitchRow& row = kSwitchRows[i];
    const char* kept = Keep(raw);
    if (!kept) return;
    long value = 0;
    const int status = lantern_parse_switch(int(row.kind), row.min, row.max, row.flags, kept, &value);
    if (status == LANTERN_PARSE_UNSET) return;
    if (status == LANTERN_PARSE_BAD) {
        Warn("Switches: %.*s=%s not understood (expected %ld to %ld); the default %.*s is used",
             int(row.name.size()), row.name.data(), Shown::Plain(kept, false) ? kept : "<set>", row.min,
             row.max, int(row.def.size()), row.def.data());
        return;
    }
    if (status == LANTERN_PARSE_ALIAS) {
        Warn("Switches: %.*s spelled %s, read as 2", int(row.name.size()), row.name.data(), kept);
    }
    if (row.kind != SwitchKind::Text) s.values.number[i] = value;
    s.values.text[i] = kept;
    s.values.source[i] = source;
}

void ReadIni(Switches& s, std::string_view ini) {
    while (!ini.empty()) {
        const std::size_t end = ini.find('\n');
        std::string_view line = Trim(ini.substr(0, end));
        ini.remove_prefix(end == std::string_view::npos ? ini.size() : end + 1);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view name = Trim(line.substr(0, equals));
        if (!HasSwitchPrefix(name)) continue;
        SwitchId id{};
        if (!FindRow(name, &id)) {
            Warn("Switches: %.*s in lantern.ini is not read by this build", int(name.size()), name.data());
            continue;
        }
        Take(s, std::size_t(id), Trim(line.substr(equals + 1)), SwitchSource::Ini);
    }
}

void ReadEnvironment(Switches& s, const SwitchInput& input) {
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        char name[96] = {};
        const std::string_view n = kSwitchRows[i].name;
        std::memcpy(name, n.data(), n.size() < sizeof name - 1 ? n.size() : sizeof name - 1);
        const char* value = input.env(name, input.context);
        if (value && value[0]) Take(s, i, value, SwitchSource::Env);
    }
}

void AppendPrintable(char* buffer, std::size_t size, std::size_t& used, std::string_view text) {
    for (char c : text) {
        const char shown = Shown::Printable(c);
        Append(buffer, size, used, std::string_view(&shown, 1));
    }
}

void NoteUnknown(const char* const* environment) {
    if (!environment) return;
    for (const char* const* entry = environment; *entry; ++entry) {
        const std::string_view text = *entry;
        const std::string_view name = text.substr(0, text.find('='));
        if (!HasSwitchPrefix(name) || FindRow(name)) continue;
        if (g_load.unknown_count++) Append(g_load.unknown, sizeof g_load.unknown, g_load.unknown_used, ", ");
        AppendPrintable(g_load.unknown, sizeof g_load.unknown, g_load.unknown_used, name.substr(0, Shown::kNameMax));
    }
}

void AppendName(std::string_view name) {
    AppendPrintable(g_load.received, sizeof g_load.received, g_load.received_used, name.substr(0, Shown::kNameMax));
    if (name.size() > Shown::kNameMax) Append(g_load.received, sizeof g_load.received, g_load.received_used, "...");
}

void AppendShownText(std::string_view value) {
    char clean[kReceivedValueBytes];
    const std::size_t n = Scrub::Text(value, clean, sizeof clean);
    const std::string_view scrubbed(clean, n);
    AppendPrintable(g_load.received, sizeof g_load.received, g_load.received_used, scrubbed.substr(0, kShownTextMax));
    if (scrubbed.size() > kShownTextMax) Append(g_load.received, sizeof g_load.received, g_load.received_used, "...");
}

void AppendReceivedValue(std::string_view name, std::string_view value, const SwitchRow* row) {
    const bool upper = Shown::UpperName(name);
    if (value.empty()) {
        Append(g_load.received, sizeof g_load.received, g_load.received_used, "(empty)");
    } else if (!upper || Shown::SecretName(name) || (row && (row->flags & LANTERN_SW_SECRET))) {
        Append(g_load.received, sizeof g_load.received, g_load.received_used, "<set>");
    } else if (row && row->kind == SwitchKind::Text) {
        if (row->flags & LANTERN_SW_SHOW) AppendShownText(value);
        else Append(g_load.received, sizeof g_load.received, g_load.received_used, "<set>");
    } else if (Shown::Plain(value, !row)) {
        Append(g_load.received, sizeof g_load.received, g_load.received_used, value);
    } else {
        Append(g_load.received, sizeof g_load.received, g_load.received_used, "<set>");
    }
    if (!upper) Append(g_load.received, sizeof g_load.received, g_load.received_used, "(not upper-case)");
    else if (!row) Append(g_load.received, sizeof g_load.received, g_load.received_used, "(unknown)");
}

std::string_view EnvironmentValue(const char* const* environment, std::string_view name) {
    for (const char* const* entry = environment; *entry; ++entry) {
        const std::string_view text = *entry;
        if (text.size() > name.size() && text.substr(0, name.size()) == name && text[name.size()] == '=') {
            return text.substr(name.size() + 1);
        }
    }
    return {};
}

bool InNameList(std::string_view list, std::string_view name) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        if (list.substr(0, comma) == name) return true;
        list.remove_prefix(comma == std::string_view::npos ? list.size() : comma + 1);
    }
    return false;
}

void NoteReceived(const char* const* environment) {
    if (!environment) return;
    const std::string_view box = EnvironmentValue(environment, "LANTERN_EXTRA_VARIABLES");
    const std::string_view package = EnvironmentValue(environment, "LANTERN_PACKAGE_SWITCHES");
    for (const char* const* entry = environment; *entry; ++entry) {
        const std::string_view text = *entry;
        const std::size_t equals = text.find('=');
        if (equals == std::string_view::npos || equals == 0) continue;
        const std::string_view name = text.substr(0, equals);
        if (!Shown::HasPrefix(name, "BB_") && !Shown::HasPrefix(name, "LANTERN_")) continue;
        if (g_load.received_count++) Append(g_load.received, sizeof g_load.received, g_load.received_used, " ");
        AppendName(name);
        Append(g_load.received, sizeof g_load.received, g_load.received_used, "=");
        AppendReceivedValue(name, text.substr(equals + 1), Shown::UpperName(name) ? FindRow(name) : nullptr);
        if (InNameList(box, name)) Append(g_load.received, sizeof g_load.received, g_load.received_used, "[box]");
        else if (InNameList(package, name)) Append(g_load.received, sizeof g_load.received, g_load.received_used, "[package]");
    }
}

void ApplyGroupModes(Switches& s) {
    const bool verify = s.Number(SwitchId::verify) == 1;
    const bool batch = s.Number(SwitchId::batch) == 1;
    if (!verify && !batch) return;
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        const SwitchRow& row = kSwitchRows[i];
        if (!row.step || row.kind != SwitchKind::Mode || s.values.source[i] != SwitchSource::Default) continue;
        if (verify && row.max == 2) {
            s.values.number[i] = 2;
            s.values.source[i] = SwitchSource::Verify;
        } else if (batch || verify) {
            s.values.number[i] = 1;
            s.values.source[i] = batch ? SwitchSource::Batch : SwitchSource::Verify;
        }
    }
}

const char* ProcessEnv(const char* name, void*) {
    return std::getenv(name);
}

std::string_view ReadIniFile() {
    const char* config = std::getenv("BB_CONFIG");
    const char* path = config && config[0] ? config : "lantern.ini";
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return {};
    const std::size_t n = std::fread(g_load.ini, 1, sizeof g_load.ini - 1, file);
    const bool failed = std::ferror(file) != 0;
    if (std::fclose(file) != 0 || failed) {
        Warn("Switches: lantern.ini could not be read; only the environment is used");
        return {};
    }
    if (n == sizeof g_load.ini - 1) Warn("Switches: lantern.ini is larger than %zu bytes; the rest is not read", n);
    g_load.ini[n] = 0;
    return std::string_view(g_load.ini, n);
}

Scrub::Kind SecretKind(std::string_view name) {
    if (name.find("ROOM") != std::string_view::npos) return Scrub::Kind::Room;
    if (name.find("SERVER") != std::string_view::npos || name.find("HOST") != std::string_view::npos) {
        return Scrub::Kind::Server;
    }
    if (name.find("USER_NAME") != std::string_view::npos || name.find("PLAYER") != std::string_view::npos) {
        return Scrub::Kind::Account;
    }
    return Scrub::Kind::Secret;
}

void RegisterSecrets(const SwitchInput& input, const Switches& s) {
    Scrub::ClearSecretsForTest();
    for (const auto& [name, kind] : kProfileNames) {
        if (const char* value = input.env(name, input.context); value && value[0]) Scrub::AddSecret(value, kind);
    }
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        if (!(kSwitchRows[i].flags & LANTERN_SW_SECRET) || !s.IsSet(SwitchId(i))) continue;
        Scrub::AddSecret(s.values.text[i], SecretKind(kSwitchRows[i].name));
    }
}

void Fill(const SwitchInput& input) {
    Switches s = Defaults();
    ReadIni(s, input.ini);
    ReadEnvironment(s, input);
    ApplyGroupModes(s);
    NoteUnknown(input.environment);
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        s.set_count += s.values.source[i] == SwitchSource::Ini || s.values.source[i] == SwitchSource::Env;
    }
    ApplyFields(s);
    RegisterSecrets(input, s);
    NoteReceived(input.environment);
    g_switches = s;
}

}

constinit Switches g_switches = Defaults();

const SwitchRow* FindRow(std::string_view name, SwitchId* id) {
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        if (kSwitchRows[i].name == name) {
            if (id) *id = SwitchId(i);
            return &kSwitchRows[i];
        }
    }
    return nullptr;
}

const Switches& Switches::Load() {
    std::call_once(g_load_once, [] {
        SwitchInput input;
        input.env = ProcessEnv;
        input.ini = ReadIniFile();
        input.environment = LANTERN_ENVIRON;
        Fill(input);
    });
    return g_switches;
}

bool ReloadSwitches(const SwitchInput& input) {
    if (!input.env) return false;
    std::call_once(g_load_once, [] {});
    g_load.arena_used = 0;
    g_load.warnings_used = 0;
    g_load.warnings[0] = 0;
    g_load.unknown_used = 0;
    g_load.unknown[0] = 0;
    g_load.unknown_count = 0;
    g_load.received_used = 0;
    g_load.received[0] = 0;
    g_load.received_count = 0;
    Fill(input);
    return true;
}

std::size_t FormatSwitchValue(char* out, std::size_t size, const Switches& s, SwitchId id) {
    const SwitchRow& row = Row(id);
    std::size_t used = 0;
    if (size == 0) return 0;
    out[0] = 0;
    if (row.flags & LANTERN_SW_SECRET) {
        Append(out, size, used, "<set>");
        return used;
    }
    if (row.kind != SwitchKind::Text) {
        char number[24];
        const int n = std::snprintf(number, sizeof number, "%ld", s.Number(id));
        Append(out, size, used, std::string_view(number, n > 0 ? std::size_t(n) : 0));
        return used;
    }
    const std::string_view text = s.Text(id);
    if (!s.IsSet(id)) {
        Append(out, size, used, "(not set)");
    } else if (row.flags & LANTERN_SW_SHOW) {
        char clean[kReceivedValueBytes];
        const std::string_view scrubbed(clean, Scrub::Text(text, clean, sizeof clean));
        for (char c : scrubbed.substr(0, kShownTextMax)) {
            const char shown = static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? '_' : c;
            Append(out, size, used, std::string_view(&shown, 1));
        }
    } else {
        Append(out, size, used, "<set>");
    }
    return used;
}

std::size_t FormatSwitchesLine(char* out, std::size_t size, const Switches& s) {
    std::size_t used = 0;
    if (size == 0) return 0;
    out[0] = 0;
    char head[64];
    const int n = std::snprintf(head, sizeof head, "Switches: %zu known, %u set", kSwitchCount, s.set_count);
    Append(out, size, used, std::string_view(head, n > 0 ? std::size_t(n) : 0));
    bool first = true;
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        if (!s.IsSet(SwitchId(i))) continue;
        Append(out, size, used, first ? ": " : " ");
        first = false;
        Append(out, size, used, kSwitchRows[i].name);
        Append(out, size, used, "=");
        char value[kShownTextMax + 8];
        const std::size_t v = FormatSwitchValue(value, sizeof value, s, SwitchId(i));
        Append(out, size, used, std::string_view(value, v));
    }
    if (g_load.unknown_count) {
        Append(out, size, used, "; unknown names set (not read by this build): ");
        Append(out, size, used, std::string_view(g_load.unknown, g_load.unknown_used));
    }
    return used;
}

std::size_t FormatReceivedLines(char* out, std::size_t size) {
    std::size_t used = 0;
    if (size == 0) return 0;
    out[0] = 0;
    char head[192];
    const int n = std::snprintf(head, sizeof head, "Switches received: %u BB_ and LANTERN_ variables in the environment ([box] Extra variables, [package] engine-switches.txt)",
                                g_load.received_count);
    Append(out, size, used, std::string_view(head, n > 0 ? std::size_t(n) : 0));
    constexpr std::string_view more = "\nSwitches received (more):";
    std::string_view rest(g_load.received, g_load.received_used);
    std::size_t line = used;
    bool first = true;
    while (!rest.empty()) {
        const std::size_t end = rest.find(' ');
        const std::string_view item = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (!first && line + 1 + item.size() > kReceivedLineBytes) {
            Append(out, size, used, more);
            line = more.size() - 1;
        }
        Append(out, size, used, first ? ": " : " ");
        Append(out, size, used, item);
        line += 1 + item.size();
        first = false;
    }
    return used;
}

std::size_t FormatSwitchWarnings(char* out, std::size_t size) {
    std::size_t used = 0;
    if (size == 0) return 0;
    out[0] = 0;
    Append(out, size, used, std::string_view(g_load.warnings, g_load.warnings_used));
    return used;
}

void LogScrubbed(LogChannel channel, std::string_view line) {
    char clean[kLogLineBytes];
    LogLine(channel, std::string_view(clean, Scrub::Text(line, clean, sizeof clean)));
}

void PrintSwitchesAtStart() {
    char line[kLogLineBytes];
    const std::size_t n = FormatSwitchesLine(line, sizeof line, Switches::Get());
    LogScrubbed(LogChannel::Always, std::string_view(line, n));
    static char received[kReceivedBytes + 512];
    std::string_view lines(received, FormatReceivedLines(received, sizeof received));
    while (!lines.empty()) {
        const std::size_t end = lines.find('\n');
        LogScrubbed(LogChannel::Always, lines.substr(0, end));
        lines.remove_prefix(end == std::string_view::npos ? lines.size() : end + 1);
    }
    std::string_view warnings(g_load.warnings, g_load.warnings_used);
    while (!warnings.empty()) {
        const std::size_t end = warnings.find('\n');
        LogScrubbed(LogChannel::Always, warnings.substr(0, end));
        warnings.remove_prefix(end == std::string_view::npos ? warnings.size() : end + 1);
    }
}

bool LogChannelOn(LogChannel channel) {
    return channel != LogChannel::FrameStats || Switches::Get().frame_stats;
}

void SetLogSinkForTest(void (*sink)(std::string_view, void*), void* context) {
    g_log_sink = sink;
    g_log_sink_context = context;
}

void LogLine(LogChannel channel, std::string_view line) {
    if (!LogChannelOn(channel)) return;
    char buffer[kLogLineBytes];
    const std::size_t n = line.size() < sizeof buffer - 1 ? line.size() : sizeof buffer - 1;
    std::memcpy(buffer, line.data(), n);
    buffer[n] = '\n';
    if (g_log_sink) {
        g_log_sink(std::string_view(buffer, n), g_log_sink_context);
        return;
    }
    std::fwrite(buffer, 1, n + 1, stdout);
}

void Log(LogChannel channel, const char* format, ...) {
    if (!LogChannelOn(channel)) return;
    char buffer[kLogLineBytes];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    if (n < 0) return;
    LogLine(channel, std::string_view(buffer, std::size_t(n) < sizeof buffer ? std::size_t(n) : sizeof buffer - 1));
}

}

extern "C" long lantern_switch_c(int id) {
    if (id < 0 || std::size_t(id) >= Lantern::kSwitchCount) return 0;
    return Lantern::Switches::Load().Number(Lantern::SwitchId(id));
}
