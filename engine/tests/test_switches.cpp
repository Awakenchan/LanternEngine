// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gpu/shim/lantern_switches.h"
#include "gpu/shim/engine/vertex_census.h"
#include "gpu/shim/lantern_pacing_stats.h"
#include "gpu/shim/bbport_toggles.h"
#include "gpu/shadps4/shader_recompiler/compile_stats.h"
#include "gpu/shim/engine/pipe_census.h"

extern "C" int lantern_mutant = 0;

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct FakeEnv {
    std::vector<std::pair<std::string, std::string>> vars;
    std::vector<std::string> lines;
    std::vector<const char*> pointers;
    const char* Find(const char* name) const {
        for (const auto& [n, v] : vars) {
            if (n == name) return v.c_str();
        }
        return nullptr;
    }
    const char* const* Environment() {
        lines.clear();
        pointers.clear();
        for (const auto& [n, v] : vars) lines.push_back(n + "=" + v);
        for (const auto& l : lines) pointers.push_back(l.c_str());
        pointers.push_back(nullptr);
        return pointers.data();
    }
};

const char* FakeLookup(const char* name, void* context) {
    return static_cast<FakeEnv*>(context)->Find(name);
}

const Lantern::Switches& Load(FakeEnv& env, std::string_view ini = {}) {
    Lantern::SwitchInput input;
    input.env = FakeLookup;
    input.context = &env;
    input.ini = ini;
    input.environment = env.Environment();
    Lantern::ReloadSwitches(input);
    return Lantern::Switches::Get();
}

std::vector<std::string> g_logged;
void Sink(std::string_view line, void*) {
    g_logged.emplace_back(line);
}

void ParserValues() {
    using Lantern::SwitchId;
    long v = 7;
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, 0, "0", &v) == LANTERN_PARSE_OK && v == 0, "mode 0");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, 0, "1", &v) == LANTERN_PARSE_OK && v == 1, "mode 1");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, 0, "2", &v) == LANTERN_PARSE_OK && v == 2, "mode 2");
    v = 7;
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, 0, "", &v) == LANTERN_PARSE_UNSET && v == 7, "empty is unset");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, 0, nullptr, &v) == LANTERN_PARSE_UNSET, "missing is unset");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, 0, "yes", &v) == LANTERN_PARSE_BAD, "garbage is refused");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, 0, "3", &v) == LANTERN_PARSE_BAD, "above the range is refused");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, 0, "-1", &v) == LANTERN_PARSE_BAD, "below the range is refused");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 1, 0, "2", &v) == LANTERN_PARSE_BAD,
           "2 is refused for a step without a comparison");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, 0, "verify", &v) == LANTERN_PARSE_BAD,
           "verify is not a value of an ordinary step");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, LANTERN_SW_VERIFY_ALIAS, "verify", &v) == LANTERN_PARSE_ALIAS &&
               v == 2,
           "verify is 2 where the row allows the alias");
    Expect(lantern_parse_switch(LANTERN_KIND_Bool, 0, 1, 0, "y", &v) == LANTERN_PARSE_BAD, "y is not a bool");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, LANTERN_SW_COMPARE_ALIAS, "compare", &v) == LANTERN_PARSE_ALIAS &&
               v == 2,
           "compare is 2 where the row allows that spelling");
    Expect(lantern_parse_switch(LANTERN_KIND_Mode, 0, 2, LANTERN_SW_VERIFY_ALIAS, "compare", &v) == LANTERN_PARSE_BAD,
           "compare is not the verify spelling");
    Expect(lantern_parse_switch(LANTERN_KIND_Int, 0, 16, 0, "1234567890", &v) == LANTERN_PARSE_BAD, "too many digits");

    FakeEnv env;
    env.vars = {{"LANTERN_VERTEX_MEMO", "garbage"}, {"LANTERN_PIPELINE_MEMO", "5"}, {"BB_NATIVE_CALLS", "verify"},
                {"LANTERN_LIFTER", "compare"}};
    const auto& s = Load(env);
    Expect(s.vertex_memo == Lantern::Mode::Off && s.Source(SwitchId::vertex_memo) == Lantern::SwitchSource::Default,
           "a malformed value takes the default");
    Expect(s.pipeline_memo == Lantern::Mode::Off, "an out-of-range value takes the default");
    Expect(s.native_calls == Lantern::Mode::Compare, "BB_NATIVE_CALLS=verify reads as 2");
    Expect(s.lifter == Lantern::Mode::Compare, "LANTERN_LIFTER=compare reads as 2, as lifter/bridge.cpp GetMode does");
    char warnings[4096];
    const std::string_view w(warnings, Lantern::FormatSwitchWarnings(warnings, sizeof warnings));
    Expect(w.find("LANTERN_VERTEX_MEMO=garbage not understood") != std::string_view::npos, "garbage is logged");
    Expect(w.find("LANTERN_PIPELINE_MEMO=5 not understood") != std::string_view::npos, "out of range is logged");
    Expect(w.find("BB_NATIVE_CALLS spelled verify, read as 2") != std::string_view::npos, "the alias is logged");
    Expect(w.find("LANTERN_LIFTER spelled compare, read as 2") != std::string_view::npos, "the compare spelling is logged");
}

void Order() {
    FakeEnv env;
    env.vars = {{"LANTERN_RING_CONSTANTS", "2"}};
    const auto& s = Load(env, "# settings\nupscaler=fsr4\nLANTERN_RING_CONSTANTS=1\nLANTERN_VERTEX_MEMO = 1\nBB_NOT_A_ROW=1\n");
    Expect(s.ring_constants == Lantern::Mode::Compare, "the environment wins over lantern.ini");
    Expect(s.vertex_memo == Lantern::Mode::On, "lantern.ini sets what the environment leaves unset");
    Expect(s.Source(Lantern::SwitchId::vertex_memo) == Lantern::SwitchSource::Ini, "the source is lantern.ini");
    Expect(s.upscaler_no_blur == false, "an ordinary setting line is not a switch");
    char warnings[4096];
    const std::string_view w(warnings, Lantern::FormatSwitchWarnings(warnings, sizeof warnings));
    Expect(w.find("BB_NOT_A_ROW in lantern.ini is not read") != std::string_view::npos, "an unknown ini name is logged");
}

void FrameStatsEverywhere() {
    FakeEnv env;
    env.vars = {{"BB_FRAME_STATS", "0"}, {"BB_FAULT_SITES", "0"}};
    const auto& s = Load(env);
    Expect(!s.frame_stats, "BB_FRAME_STATS=0 is off in the switches object");
    Expect(!BbEngine::VertexCensus::On(), "vertex census reads it as off");
    Expect(!LanternPacingStats::Enabled(), "pacing stats read it as off");
    Expect(!BbStats::enabled, "BbStats reads it as off");
    Expect(!Shader::CompileStats::Enabled(), "compile stats read it as off");
    Expect(!BbEngine::PipeCensus::On(), "the pipe census reads it as off");
    Expect(!Lantern::LogChannelOn(Lantern::LogChannel::FrameStats), "the frame statistics log channel is off");
    Expect(lantern_switch_c(LANTERN_SW_ID_frame_stats) == 0, "the C reader reads it as off");
    env.vars = {{"BB_FRAME_STATS", "1"}};
    Load(env);
    Expect(BbEngine::VertexCensus::On() && LanternPacingStats::Enabled() && BbStats::enabled &&
               Shader::CompileStats::Enabled() && BbEngine::PipeCensus::On(),
           "BB_FRAME_STATS=1 is on at every reader");
    Expect(lantern_switch_c(LANTERN_SW_ID_frame_stats) == 1, "the C reader reads 1");
    env.vars = {{"BB_FRAME_STATS", "y"}};
    Expect(!Load(env).frame_stats, "BB_FRAME_STATS=y is not understood and takes the default");
}

void GroupModes() {
    FakeEnv env;
    env.vars = {{"LANTERN_VERIFY", "1"}, {"LANTERN_VERTEX_MEMO", "0"}};
    const auto& s = Load(env);
    Expect(s.pipeline_memo == Lantern::Mode::Compare, "LANTERN_VERIFY=1 compares an unset step");
    Expect(s.ring_constants == Lantern::Mode::Compare, "LANTERN_VERIFY=1 reaches ring constants too");
    Expect(s.vertex_memo == Lantern::Mode::Off, "a step's own 0 wins over LANTERN_VERIFY");
    Expect(s.native_shaders == Lantern::Mode::On, "a step without a comparison is turned on, not compared");
    env.vars = {{"LANTERN_BATCH", "1"}};
    const auto& b = Load(env);
    Expect(b.pipeline_memo == Lantern::Mode::On && b.gx_textures == Lantern::Mode::On, "LANTERN_BATCH=1 turns steps on");
    Expect(!b.frame_stats, "LANTERN_BATCH leaves rows that are not steps alone");
}

void StartLine() {
    FakeEnv env;
    const auto& d = Load(env);
    char line[4096];
    std::string text(line, Lantern::FormatSwitchesLine(line, sizeof line, d));
    const std::string expected = "Switches: " + std::to_string(Lantern::kSwitchCount) + " known, 0 set";
    Expect(text == expected, "with nothing set the start line names only the table size");
    env.vars = {{"BB_FRAME_STATS", "1"}, {"BB_ROOM_PASSWORD", "hunter2"}, {"BB_GAME_DIR", "C:\\Users\\Jane\\Game"},
                {"BB_FOO", "1"}, {"LANTERN_STEPS_OFF", "group:gx"}, {"BB_GUEST_IN_PLACE", "1"}, {"BB_P2P_PORT", "424242"},
                {"BB_ANISO", "8"}};
    const auto& s = Load(env);
    text.assign(line, Lantern::FormatSwitchesLine(line, sizeof line, s));
    Expect(text.find("BB_FRAME_STATS=1") != std::string::npos, "a set bool is shown");
    Expect(text.find("LANTERN_STEPS_OFF=group:gx") != std::string::npos, "the bisect line is shown");
    Expect(text.find("hunter2") == std::string::npos && text.find("BB_ROOM_PASSWORD=<set>") != std::string::npos,
           "a secret is never printed");
    Expect(text.find("Jane") == std::string::npos && text.find("BB_GAME_DIR=<set>") != std::string::npos,
           "a path is never printed");
    Expect(text.find("BB_P2P_PORT=<set>") != std::string::npos && text.find("424242") == std::string::npos,
           "a text row's value is never printed, digits only or not");
    Expect(text.find("BB_GUEST_IN_PLACE=1") != std::string::npos, "a parsed bool is shown");
    Expect(text.find("BB_ANISO=8") != std::string::npos, "a parsed number is shown");
    env.vars = {{"LANTERN_STEPS_OFF", "vertex-memo,C:\\Users\\Jane Doe\\x"}, {"BB_ANISO", "C:\\Users\\Jane Doe\\y"},
                {"USERNAME", "Jane Doe"}};
    Load(env);
    g_logged.clear();
    Lantern::SetLogSinkForTest(Sink, nullptr);
    Lantern::PrintSwitchesAtStart();
    Lantern::SetLogSinkForTest(nullptr, nullptr);
    std::string all;
    for (const auto& l : g_logged) all += l + "\n";
    Expect(all.find("Jane") == std::string::npos, "the start line and its warnings pass through the scrubber");
    Expect(all.find("LANTERN_STEPS_OFF=vertex-memo,C:\\Users\\<user>") != std::string::npos,
           "free text in LANTERN_STEPS_OFF is scrubbed, the step names stay");
    Expect(all.find("BB_ANISO=<set> not understood") != std::string::npos && all.find("not understood") != std::string::npos,
           "a malformed number is reported, scrubbed");
    Expect(text.find("unknown names set (not read by this build): BB_FOO") != std::string::npos, "unknown names are listed");
    Expect(text.find("BB_FOO=") == std::string::npos, "an unknown name's value is not printed");
}

std::string Printed(FakeEnv& env) {
    Load(env);
    g_logged.clear();
    Lantern::SetLogSinkForTest(Sink, nullptr);
    Lantern::PrintSwitchesAtStart();
    Lantern::SetLogSinkForTest(nullptr, nullptr);
    std::string all;
    bool only_switches = true;
    for (const auto& l : g_logged) {
        only_switches = only_switches && l.rfind("Switches", 0) == 0;
        all += l + "\n";
    }
    Expect(only_switches, "every start line begins with Switches, whatever the environment holds");
    return all;
}

void ReceivedLine() {
    FakeEnv env;
    env.vars = {{"BB_GUEST_IN_PLACE", "1"},        {"LANTERN_RING_CONSTANTS", "1"},   {"LANTERN_JOB_LOCK", "1"},
                {"LANTERN_JOB_NODE_CACHE", "1"},   {"LANTERN_MAIN_JOBS_BARRIER", "1"}, {"BB_ROOM_PASSWORD", "hunter2"},
                {"BB_ROOM_HOST_PASSWORD", "hostpw"}, {"BB_THD_TOKEN", "tok123"},       {"BB_THD_ONLINE_ID", "jane77"},
                {"BB_SOME_ACCOUNT", "acct9"},      {"LANTERN_NOT_A_ROW", "2"},         {"PATH", "C:\\Windows"}};
    const auto& s = Load(env);
    Expect(s.ring_constants == Lantern::Mode::On && s.job_lock == Lantern::Mode::On && s.job_node_cache == Lantern::Mode::On &&
               s.main_jobs_barrier == Lantern::Mode::On && s.guest_in_place,
           "the launcher's extra variables reach their switches");
    char line[8192];
    std::string text(line, Lantern::FormatSwitchesLine(line, sizeof line, s));
    Expect(text.find("LANTERN_JOB_LOCK=1") != std::string::npos && text.find("LANTERN_MAIN_JOBS_BARRIER=1") != std::string::npos,
           "the start line lists the job switches the environment set");
    text = Printed(env);
    Expect(text.find("Switches received: 11 BB_ and LANTERN_ variables in the environment ([box] Extra variables, [package] engine-switches.txt): ") != std::string::npos,
           "the received line counts every BB_ and LANTERN_ variable, and only those");
    for (const char* item : {"BB_GUEST_IN_PLACE=1", "LANTERN_RING_CONSTANTS=1", "LANTERN_JOB_LOCK=1", "LANTERN_JOB_NODE_CACHE=1",
                             "LANTERN_MAIN_JOBS_BARRIER=1", "LANTERN_NOT_A_ROW=2(unknown)", "BB_ROOM_PASSWORD=<",
                             "BB_ROOM_HOST_PASSWORD=<", "BB_THD_TOKEN=<", "BB_THD_ONLINE_ID=<set>", "BB_SOME_ACCOUNT=<set>"}) {
        Expect(text.find(item) != std::string::npos, item);
    }
    for (const char* secret : {"hunter2", "hostpw", "tok123", "jane77", "acct9", "PATH="}) {
        Expect(text.find(secret) == std::string::npos, secret);
    }

    const std::string long_value(80, 'q');
    env.vars = {{"BB_PC_MENU_ASSETS", "C:\\Users\\x\\OneDrive - Contoso\\Games\\pc-menus"},
                {"BB_PLATE_HOST_TITLE", "John Smith"},
                {"LANTERN_X_IP", "192.168.1.20"},
                {"LANTERN_X_URL", "https://example.org/room"},
                {"LANTERN_X_DRIVE", "E:\\JohnSmith-games"},
                {"LANTERN_X_LONG", long_value},
                {"LANTERN_X_PLAIN", "abc_1.5"},
                {"BB_Thd_Online_Id", "jane77"},
                {"BB_FRAME_STATS", "1\nFrame stats: 999.0 FPS"},
                {"LANTERN_X_LINE", "a\nMemory: game 1"},
                {"LANTERN_STEPS_OFF", "vertex-memo\nFrame stats: 1"},
                {"BB_ANISO", "C:\\Users\\x\\y"}};
    text = Printed(env);
    for (const char* leak : {"Contoso", "OneDrive", "John", "Smith", "192.168", "example.org", "JohnSmith", "qqqqqqqqqqqqqqqqq",
                             "jane77", "999.0", "Memory: game", "\nFrame stats"}) {
        Expect(text.find(leak) == std::string::npos, leak);
    }
    for (const char* item : {"BB_PC_MENU_ASSETS=<set>", "BB_PLATE_HOST_TITLE=<set>", "LANTERN_X_IP=<set>(unknown)",
                             "LANTERN_X_URL=<set>(unknown)", "LANTERN_X_DRIVE=<set>(unknown)", "LANTERN_X_LONG=<set>(unknown)",
                             "LANTERN_X_PLAIN=abc_1.5(unknown)", "BB_Thd_Online_Id=<set>(not upper-case)", "BB_FRAME_STATS=<set>",
                             "LANTERN_X_LINE=<set>(unknown)", "LANTERN_STEPS_OFF=vertex-memo_Frame_stats:_1", "BB_ANISO=<set>"}) {
        Expect(text.find(item) != std::string::npos, item);
    }
    env.vars = {{"LANTERN_JOB_LOCK", "1"}, {"LANTERN_RING_CONSTANTS", "1"}, {"BB_GUEST_IN_PLACE", "1"},
                {"LANTERN_EXTRA_VARIABLES", "BB_FOO,LANTERN_JOB_LOCK"}, {"LANTERN_PACKAGE_SWITCHES", "LANTERN_RING_CONSTANTS,LANTERN_JOB"}};
    text = Printed(env);
    Expect(text.find("LANTERN_JOB_LOCK=1[box]") != std::string::npos, "a variable from the launcher's Extra variables says so");
    Expect(text.find("LANTERN_RING_CONSTANTS=1[package]") != std::string::npos, "a variable from the package file says so");
    Expect(text.find("BB_GUEST_IN_PLACE=1 ") != std::string::npos,
           "a variable from neither has no source mark");
    env.vars = {{"LANTERN_" + std::string(200, 'N'), "1"}};
    text = Printed(env);
    Expect(text.find("LANTERN_" + std::string(88, 'N') + "...=") != std::string::npos && text.find(std::string(89, 'N')) == std::string::npos,
           "a name is cut at 96 characters");

    env.vars.clear();
    for (int i = 0; i < 200; ++i) env.vars.emplace_back("LANTERN_FILLER_" + std::to_string(i), "x");
    Printed(env);
    std::size_t received = 0;
    bool fits = true;
    std::string all;
    for (const auto& l : g_logged) {
        if (l.rfind("Switches received", 0) != 0) continue;
        ++received;
        fits = fits && l.size() < 2000;
        all += l + " ";
    }
    Expect(received > 1 && fits, "a long environment is split over several received lines that each fit a log line");
    Expect(all.find("LANTERN_FILLER_0=") != std::string::npos && all.find("LANTERN_FILLER_199=") != std::string::npos,
           "the split lines keep every variable");
}

void SpeedDefaults() {
    FakeEnv none;
    const auto& on = Load(none);
    Expect(on.ring_constants == Lantern::Mode::On && on.job_lock == Lantern::Mode::On &&
               on.job_node_cache == Lantern::Mode::On && on.main_jobs_barrier == Lantern::Mode::On,
           "the speed switches are on with nothing set");
    FakeEnv off;
    off.vars = {{"LANTERN_RING_CONSTANTS", "0"}, {"LANTERN_JOB_LOCK", "0"}, {"LANTERN_JOB_NODE_CACHE", "0"},
                {"LANTERN_MAIN_JOBS_BARRIER", "0"}};
    const auto& s = Load(off);
    Expect(s.ring_constants == Lantern::Mode::Off && s.job_lock == Lantern::Mode::Off &&
               s.job_node_cache == Lantern::Mode::Off && s.main_jobs_barrier == Lantern::Mode::Off,
           "each speed switch at 0 turns it off");
}

void LogChannels() {
    Lantern::SetLogSinkForTest(Sink, nullptr);
    FakeEnv env;
    Load(env);
    g_logged.clear();
    Lantern::Log(Lantern::LogChannel::FrameStats, "statistics %d", 1);
    Expect(g_logged.empty(), "an off channel writes nothing");
    Lantern::Log(Lantern::LogChannel::Always, "line %d", 2);
    Expect(g_logged.size() == 1 && g_logged[0] == "line 2", "a line is written whole");
    Lantern::SetLogSinkForTest(nullptr, nullptr);
}

void Table() {
    for (const Lantern::SwitchRow& row : Lantern::kSwitchRows) {
        if (!row.step || row.kind != Lantern::SwitchKind::Mode) continue;
        Expect(row.min == 0 && (row.max == 1 || row.max == 2), "a step's mode row is 0..1 or 0..2");
        const auto& step_row = Lantern::Row(Lantern::SwitchId::lifter);
        Expect(step_row.max == 2 && (step_row.flags & LANTERN_SW_COMPARE_ALIAS), "the lifter row takes compare");
    }
    Expect(Lantern::FindRow("BB_FRAME_STATS") && !Lantern::FindRow("BB_NOPE"), "rows are found by name");
    bool unique = true;
    for (const Lantern::SwitchRow& row : Lantern::kSwitchRows) {
        Lantern::SwitchId id{};
        unique = unique && Lantern::FindRow(row.name, &id) == &row;
    }
    Expect(unique, "every switch name has one row");
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--mutants") == 0) {
        std::printf("1\n");
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--mutant") == 0) lantern_mutant = std::atoi(argv[2]);
    ParserValues();
    Order();
    FrameStatsEverywhere();
    GroupModes();
    StartLine();
    ReceivedLine();
    SpeedDefaults();
    LogChannels();
    Table();
    if (failures) {
        std::printf("switches-test: %d failed\n", failures);
        return 1;
    }
    std::printf("switches-test: ok\n");
    return 0;
}
