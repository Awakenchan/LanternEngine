// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "start/package_switches.h"

extern "C" int lantern_mutant = 0;

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::string Value(const start::PackageSwitches& r, const std::string& name) {
    for (const auto& [n, v] : r.applied) {
        if (n == name) return v;
    }
    return "(none)";
}

std::string Reason(const start::PackageSwitches& r, const std::string& name) {
    for (const auto& [n, why] : r.refused) {
        if (n == name) return why;
    }
    return "";
}

void Reader() {
    const std::string file =
        "# test package\r\n"
        "LANTERN_JOB_LOCK=1\r\n"
        "  LANTERN_JOB_NODE_CACHE = 1  \r\n"
        "; comment\r\n"
        "\r\n"
        "LANTERN_RING_CONSTANTS=0\r\n"
        "LANTERN_RING_CONSTANTS=1\r\n"
        "LANTERN_MAIN_JOBS_BARRIER=1\n"
        "BB_BACKGROUND_FPS=0\n"
        "LANTERN_NOT_A_SWITCH=1\n"
        "PATH=C:\\x\n"
        "lantern_job_lock=1\n"
        "BB_ROOM_PASSWORD=hunter2\n"
        "BB_THD_TOKEN=tok\n"
        "BB_PLATE_HOST_TITLE=John\n"
        "LANTERN_EXTRA_VARIABLES=LANTERN_JOB_LOCK\n"
        "LANTERN_PACKAGE_SWITCHES=X\n"
        "no equals sign\n";
    const auto r = start::ReadPackageSwitches(file, "BB_BACKGROUND_FPS,LANTERN_MAIN_JOBS_BARRIER");
    Expect(Value(r, "LANTERN_JOB_LOCK") == "1" && Value(r, "LANTERN_JOB_NODE_CACHE") == "1", "table switches are applied, trimmed");
    Expect(Value(r, "LANTERN_RING_CONSTANTS") == "1", "a later line for the same switch wins");
    Expect(r.applied.size() == 3, "only the three allowed switches are applied");
    Expect(Value(r, "BB_BACKGROUND_FPS") == "(none)" && Reason(r, "BB_BACKGROUND_FPS") == "the launcher's Extra variables set it",
           "the launcher's Extra variables win over the package file");
    Expect(Value(r, "LANTERN_MAIN_JOBS_BARRIER") == "(none)", "every name in the Extra variables list wins");
    Expect(Reason(r, "LANTERN_NOT_A_SWITCH") == "not a switch this build reads", "a name outside the table is refused");
    Expect(Reason(r, "PATH") == "not a switch this build reads", "a name without the BB_ or LANTERN_ prefix is refused");
    Expect(Reason(r, "line 12") == "not a switch name", "a lower-case name is refused and named by its line");
    Expect(Reason(r, "BB_ROOM_PASSWORD") == "secret-like, set it in the launcher", "a password is refused");
    Expect(Reason(r, "BB_THD_TOKEN") == "secret-like, set it in the launcher", "a token is refused");
    Expect(Reason(r, "BB_PLATE_HOST_TITLE") == "secret-like, set it in the launcher", "a name plate is refused");
    Expect(Reason(r, "LANTERN_EXTRA_VARIABLES") == "set by the launcher and bb-start" &&
               Reason(r, "LANTERN_PACKAGE_SWITCHES") == "set by the launcher and bb-start",
           "the source lists cannot be set from the file");
    Expect(Reason(r, "line 18") == "not NAME=value", "a line without = is refused and named by its line");

    const std::string line = start::DescribePackageSwitches(r);
    Expect(line.rfind("Package switches (engine-switches.txt): 3 applied: LANTERN_JOB_LOCK=1 ", 0) == 0, "the log line lists what was applied");
    Expect(line.find("hunter2") == std::string::npos && line.find("tok") == std::string::npos && line.find("John") == std::string::npos &&
               line.find("C:\\x") == std::string::npos,
           "refused values never reach the log");
    Expect(line.find("BB_ROOM_PASSWORD (secret-like, set it in the launcher)") != std::string::npos, "refusals are logged with their reason");

    Expect(start::ShownPackageValue("LANTERN_JOB_LOCK", "1") == "1", "a short number is shown");
    Expect(start::ShownPackageValue("BB_ANISO", "C:\\Users\\x") == "<set>", "a path in a number switch is masked");
    Expect(start::ShownPackageValue("BB_ANISO", "192.168.1.2") == "<set>", "an address is masked");
    Expect(start::ShownPackageValue("BB_PC_MENU_ASSETS", "menus") == "<set>", "a text switch is masked");
    Expect(start::ShownPackageValue("BB_ANISO", "1\nFrame stats: 1") == "<set>", "a value with a line break is masked");
    Expect(start::ShownPackageName("BB_A\nFrame") == "BB_A_Frame", "control characters in a name become _");

    const auto box = start::ReadPackageSwitches("LANTERN_JOB_LOCK=1\n", "");
    Expect(Value(box, "LANTERN_JOB_LOCK") == "1", "with no Extra variables the package file applies");
    const auto empty = start::ReadPackageSwitches("", "LANTERN_JOB_LOCK");
    Expect(empty.applied.empty() && empty.refused.empty(), "an empty file applies nothing");
}

void Allowlist() {
    const std::string file =
        "BB_THD_VERIFY_TLS=0\n"
        "BB_ONLINE=1\n"
        "BB_THD_STUN=x\n"
        "BB_TOGGLE_FILE=C:\\x\n"
        "LANTERN_JOB_LOCK=1\n"
        "LANTERN_RING_CONSTANTS=2\n"
        "BB_GUEST_IN_PLACE=1\n"
        "BB_BACKGROUND_FPS=0\n"
        "BB_SEAMLESS_DEATHS=1\n"
        "LANTERN_AB_FLIP=1.2.3\n"
        "LANTERN_STEPS_OFF=group:gx\n"
        "C:\\Users\\Jane Doe\\secret\n"
        "bb_room_password=hunter2\n";
    const auto r = start::ReadPackageSwitches(file, "");
    Expect(Value(r, "LANTERN_JOB_LOCK") == "1" && Value(r, "LANTERN_RING_CONSTANTS") == "2" && Value(r, "BB_GUEST_IN_PLACE") == "1" &&
               Value(r, "BB_BACKGROUND_FPS") == "0" && Value(r, "LANTERN_STEPS_OFF") == "group:gx",
           "engine switches, the two launcher rows a test package needs and the shown text row are applied");
    Expect(r.applied.size() == 5, "nothing else is applied");
    for (const char* name : {"BB_THD_VERIFY_TLS", "BB_ONLINE", "BB_THD_STUN"}) {
        Expect(Reason(r, name) == "online, room or server switch, set it in the launcher", name);
    }
    Expect(Reason(r, "BB_TOGGLE_FILE") == "a path or text switch, set it in the launcher", "a path switch is refused");
    Expect(Reason(r, "BB_SEAMLESS_DEATHS") == "set on the launcher's pages", "a launcher page switch stays the launcher's");
    Expect(Reason(r, "LANTERN_AB_FLIP") == "the value is not a plain number or word", "a value that is not plain is refused");
    Expect(Reason(r, "line 12") == "not NAME=value" && Reason(r, "line 13") == "not a switch name",
           "a line whose name is not a switch name is named by its number");
    const std::string line = start::DescribePackageSwitches(r);
    Expect(line.find("Jane") == std::string::npos && line.find("Users") == std::string::npos && line.find("hunter2") == std::string::npos &&
               line.find("bb_room") == std::string::npos,
           "a refused line's text never reaches the log");
    Expect(line.find("line 12 (not NAME=value)") != std::string::npos, "the refusal names the line");
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--mutants") == 0) {
        std::printf("1 2 3 4 5\n");
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--mutant") == 0) lantern_mutant = std::atoi(argv[2]);
    Reader();
    Allowlist();
    if (failures) {
        std::printf("package-switches-test: %d failed\n", failures);
        return 1;
    }
    std::printf("package-switches-test: ok\n");
    return 0;
}
