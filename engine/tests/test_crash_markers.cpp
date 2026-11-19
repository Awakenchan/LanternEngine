// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include "video_core/renderer_vulkan/vk_breadcrumbs_note.h"

using namespace Vulkan::Breadcrumbs::Arming;
namespace fs = std::filesystem;

namespace {

int failures = 0;
bool quiet = false;

void Check(bool ok, const char* what, int line) {
    if (!ok) {
        ++failures;
        if (!quiet) {
            std::printf("FAIL line %d: %s\n", line, what);
        }
    }
}

#define CHECK(cond) Check((cond), #cond, __LINE__)

Note Read(std::uint32_t starts) {
    return Note{.status = NoteStatus::Read, .valid = true, .starts = starts};
}

void TestOverride() {
    CHECK(ParseOverride(nullptr) == Override::None);
    CHECK(ParseOverride("") == Override::None);
    CHECK(ParseOverride("0") == Override::ForcedOff);
    CHECK(ParseOverride("1") == Override::ForcedOn);
    CHECK(ParseOverride("2") == Override::ForcedOn);
}

void TestDecide() {
    const Decision fresh = Decide(Override::None, Note{});
    CHECK(!fresh.on && fresh.reason == Reason::Default && fresh.action == NoteAction::Leave);

    const Decision first = Decide(Override::None, Read(kArmedStarts));
    CHECK(first.on && first.reason == Reason::Armed);
    CHECK(first.action == NoteAction::Write && first.starts_left == kArmedStarts - 1);

    const Decision last = Decide(Override::None, Read(1));
    CHECK(last.on && last.action == NoteAction::Remove && last.starts_left == 0);

    const Decision spent = Decide(Override::None, Read(0));
    CHECK(!spent.on && spent.action == NoteAction::Remove);

    const Decision off = Decide(Override::ForcedOff, Read(kArmedStarts));
    CHECK(!off.on && off.reason == Reason::ForcedOff);
    CHECK(off.action == NoteAction::Write && off.starts_left == kArmedStarts - 1);

    const Decision on = Decide(Override::ForcedOn, Note{});
    CHECK(on.on && on.reason == Reason::ForcedOn && on.action == NoteAction::Leave);

    const Decision unreadable = Decide(Override::None, NoteOf(NoteStatus::Unreadable));
    CHECK(unreadable.on && unreadable.reason == Reason::NoteUnreadable);
    CHECK(unreadable.action == NoteAction::Leave);

    const Decision damaged = Decide(Override::None, NoteOf(NoteStatus::Read));
    CHECK(damaged.on && damaged.reason == Reason::NoteDamaged && damaged.action == NoteAction::Remove);
}

void TestSequence() {
    Note note = Read(kArmedStarts);
    std::uint32_t on_starts = 0;
    for (int start = 0; start < 6; ++start) {
        const Decision d = Decide(Override::None, note);
        on_starts += d.on ? 1 : 0;
        if (d.action == NoteAction::Write) {
            note = Read(d.starts_left);
        } else if (d.action == NoteAction::Remove) {
            note = Note{};
        }
    }
    CHECK(on_starts == kArmedStarts);
    CHECK(note.status == NoteStatus::Absent);
}

void TestParse() {
    const Note two = ParseNote("starts 2\n");
    CHECK(two.status == NoteStatus::Read && two.valid && two.starts == 2);
    const Note crlf = ParseNote("starts 1\r\n");
    CHECK(crlf.valid && crlf.starts == 1);
    const Note large = ParseNote("starts 99\n");
    CHECK(large.valid && large.starts == kArmedStarts);
    CHECK(!ParseNote("").valid);
    CHECK(!ParseNote("starts \n").valid);
    CHECK(!ParseNote("starts 2x\n").valid);
    CHECK(!ParseNote("start 2\n").valid);
    CHECK(!ParseNote("starts -1\n").valid);
    char text[kNoteMaxBytes]{};
    const std::size_t size = FormatNote(kArmedStarts, text, sizeof(text));
    CHECK(std::string(text, size) == "starts 3\n");
    const Note round = ParseNote(std::string_view(text, size));
    CHECK(round.valid && round.starts == kArmedStarts);
    CHECK(FormatNote(1, text, 4) == 0);
}

void TestTriggers() {
    CHECK(ArmsNextStarts(Trigger::DeviceLost));
    CHECK(ArmsNextStarts(Trigger::GpuStuck));
}

void RunPure() {
    TestTriggers();
    TestOverride();
    TestDecide();
    TestSequence();
    TestParse();
}

std::string Contents(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool OnlyFile(const fs::path& folder, const fs::path& name) {
    std::error_code error;
    int count = 0;
    bool found = false;
    for (const auto& entry : fs::directory_iterator(folder, error)) {
        ++count;
        found = found || entry.path().filename() == name;
    }
    return !error && count == 1 && found;
}

void TestFiles(const fs::path& root) {
    CHECK(NotePath(nullptr, nullptr) == fs::path("logs") / kNoteName);
    CHECK(NotePath("", "") == fs::path("logs") / kNoteName);
    CHECK(NotePath("session.log", nullptr) == fs::path("logs") / kNoteName);
    CHECK(NotePath("a/b/last_session.log", "port/user") == fs::path("a/b") / kNoteName);
    CHECK(NotePath(nullptr, "port/user") == fs::path("port/logs") / kNoteName);
    CHECK(NotePath(nullptr, "port/user/") == fs::path("port/logs") / kNoteName);
    CHECK(NotePath(nullptr, "C:/Games/Lantern Engine/user") == fs::path("C:/Games/Lantern Engine/logs") / kNoteName);
    CHECK(NotePath(nullptr, "elsewhere/gpu") == fs::path("logs") / kNoteName);

    const fs::path folder = root / "logs";
    const fs::path note = folder / kNoteName;
    CHECK(ReadNote(note).status == NoteStatus::Absent);
    CHECK(RemoveNote(note) == RemoveResult::Absent);

    CHECK(WriteNote(note, kArmedStarts));
    CHECK(Contents(note) == "starts 3\n");
    CHECK(OnlyFile(folder, kNoteName));
    const Note read = ReadNote(note);
    CHECK(read.status == NoteStatus::Read && read.valid && read.starts == kArmedStarts);

    CHECK(WriteNote(note, 1));
    CHECK(Contents(note) == "starts 1\n");
    CHECK(OnlyFile(folder, kNoteName));

    {
        std::ofstream damaged(note, std::ios::binary | std::ios::trunc);
        damaged << "garbage";
    }
    const Note bad = ReadNote(note);
    CHECK(bad.status == NoteStatus::Read && !bad.valid);

    CHECK(RemoveNote(note) == RemoveResult::Removed);
    CHECK(ReadNote(note).status == NoteStatus::Absent);

    std::error_code error;
    fs::create_directories(note, error);
    CHECK(!error);
    CHECK(ReadNote(note).status == NoteStatus::Unreadable);
    CHECK(!WriteNote(note, kArmedStarts));
    CHECK(fs::is_directory(note));
    CHECK(OnlyFile(folder, kNoteName));
    const StartResult blocked = Start(Override::None, note, {});
    CHECK(blocked.decision.on && blocked.decision.reason == Reason::NoteUnreadable);
    CHECK(blocked.update == NoteUpdate::None && fs::is_directory(note));
    fs::remove_all(note, error);
}

void TestStarts(const fs::path& root) {
    const fs::path note = root / "starts" / kNoteName;
    CHECK(Start(Override::None, note, {}).decision.on == false);
    CHECK(WriteNote(note, kArmedStarts));
    const bool expected[] = {true, true, true, false, false};
    const NoteUpdate updates[] = {NoteUpdate::Written, NoteUpdate::Written, NoteUpdate::Removed,
                                  NoteUpdate::None, NoteUpdate::None};
    for (int start = 0; start < 5; ++start) {
        const StartResult r = Start(Override::None, note, {});
        CHECK(r.decision.on == expected[start]);
        CHECK(r.update == updates[start]);
    }
    CHECK(!fs::exists(note));

    CHECK(WriteNote(note, kArmedStarts));
    const StartResult forced = Start(Override::ForcedOff, note, {});
    CHECK(!forced.decision.on && forced.update == NoteUpdate::Written);
    CHECK(Contents(note) == "starts 2\n");
    std::error_code error;
    fs::remove(note, error);
}

void TestFallback(const fs::path& root) {
    CHECK(FallbackNotePath(nullptr, "x") == fs::path());
    CHECK(FallbackNotePath("port/user", "logs/a") == fs::path("port/user") / kNoteName);
    CHECK(FallbackNotePath("port/user", fs::path("port/user") / kNoteName) == fs::path());

    const fs::path blocked_folder = root / "blocked";
    {
        std::ofstream file(blocked_folder, std::ios::binary);
        file << "a file where the logs folder should be";
    }
    const fs::path primary = blocked_folder / kNoteName;
    const fs::path fallback = root / "user" / kNoteName;
    CHECK(Arm(primary, {}) == ArmResult::Failed);
    CHECK(Arm(primary, fallback) == ArmResult::WrittenToFallback);
    CHECK(Contents(fallback) == "starts 3\n");
    const bool expected[] = {true, true, true, false};
    for (int start = 0; start < 4; ++start) {
        const StartResult r = Start(Override::None, primary, fallback);
        CHECK(r.decision.on == expected[start]);
        CHECK(r.from_fallback == expected[start]);
    }
    CHECK(!fs::exists(fallback));

    const fs::path good = root / "logs2" / kNoteName;
    CHECK(Arm(good, fallback) == ArmResult::Written);
    CHECK(!fs::exists(fallback));
    const StartResult first = Start(Override::None, good, fallback);
    CHECK(first.decision.on && !first.from_fallback && first.update == NoteUpdate::Written);
}

int RunAll(const fs::path& root) {
    failures = 0;
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    if (error) {
        std::printf("FAIL: cannot reset the test folder\n");
        std::exit(1);
    }
    RunPure();
    TestFiles(root);
    TestStarts(root);
    TestFallback(root);
    return failures;
}

} // namespace

int main() {
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = getpid();
#endif
    std::error_code error;
    const fs::path root = fs::current_path(error) / "out" / "test-tmp" / ("crash-markers-" + std::to_string(pid));
    if (error || !fs::is_directory(root.parent_path().parent_path())) {
        std::printf("FAIL: run from the engine folder (out/ not found)\n");
        return 1;
    }
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    if (error) {
        std::printf("FAIL: cannot create out/test-tmp for the note files\n");
        return 1;
    }
    int result = 0;
#if defined(BB_MARKER_MUTANTS)
    constexpr int kMutants = 6;
    marker_mutant = 0;
    if (RunAll(root) != 0) {
        std::printf("FAIL: the unmutated header fails the checks\n");
        result = 1;
    }
    quiet = true;
    for (int mutant = 1; mutant <= kMutants; ++mutant) {
        marker_mutant = mutant;
        const int caught = RunAll(root);
        std::printf("mutant %d: %s (%d checks failed)\n", mutant, caught ? "caught" : "SURVIVED", caught);
        result |= caught ? 0 : 1;
    }
    marker_mutant = 0;
    std::printf("crash-markers mutants: %s\n", result ? "FAILED" : "all caught");
#else
    result = RunAll(root) ? 1 : 0;
    std::printf("crash-markers test: %s\n", result ? "FAILED" : "ok");
#endif
    fs::remove_all(root, error);
    return result;
}
