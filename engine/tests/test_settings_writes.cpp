// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "gpu/shim/bbport_settings.h"
#include "gpu/shim/input/ini_bindings.h"

extern "C" void bbport_settings_flush_for_exit(void);

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int ThreadSaves = 400;
constexpr std::size_t LongLineLength = 1500;
constexpr std::chrono::milliseconds ExitLockHold{600};

void Require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "settings-writes-test: FAIL: %s\n", what);
        std::exit(1);
    }
}

std::string ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void WriteAll(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    Require(file.good(), "the test file is written");
}

void UseConfig(const fs::path& path) {
    const std::string text = path.string();
#ifdef _WIN32
    Require(_putenv_s("BB_CONFIG", text.c_str()) == 0, "BB_CONFIG is set");
#else
    Require(setenv("BB_CONFIG", text.c_str(), 1) == 0, "BB_CONFIG is set");
#endif
}

int Leftovers(const fs::path& folder) {
    int count = 0;
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (entry.path().extension() == ".tmp") {
            ++count;
        }
    }
    return count;
}

bool Has(const std::string& text, const std::string& line) {
    return text.find("\n" + line + "\n") != std::string::npos || text.rfind(line + "\n", 0) == 0;
}

void Expect(BbSettings::SaveResult got, BbSettings::SaveResult want, const char* what) {
    Require(got == want, what);
}

#ifdef _WIN32
struct HeldFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit HeldFile(const fs::path& path) {
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(handle != INVALID_HANDLE_VALUE, "the test holds the file with no sharing");
    }
    ~HeldFile() { CloseHandle(handle); }
};
#else
struct HeldFile {
    int handle = -1;
    explicit HeldFile(const fs::path& path) {
        handle = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);  // not inherited by the child engine
        Require(handle >= 0 && flock(handle, LOCK_EX) == 0, "the test locks the file");
    }
    ~HeldFile() { close(handle); }
};
#endif

bool CanMakeUnreadable(const fs::path& file) {
#ifdef _WIN32
    (void)file;
    return true;
#else
    Require(chmod(file.c_str(), 0) == 0, "the file is made unreadable");
    const bool unreadable = access(file.c_str(), R_OK) != 0;
    Require(chmod(file.c_str(), 0644) == 0, "the file is made readable again");
    return unreadable;
#endif
}

struct Unreadable {
#ifdef _WIN32
    HeldFile held;
    explicit Unreadable(const fs::path& file) : held(file) {}
#else
    fs::path path;
    explicit Unreadable(const fs::path& file) : path(file) { Require(chmod(file.c_str(), 0) == 0, "unreadable"); }
    ~Unreadable() { chmod(path.c_str(), 0644); }
#endif
};

void UnreadableKeepsContent(const fs::path& folder) {
    const fs::path file = folder / "unreadable.ini";
    const std::string original = "# mine\nupscaler=fsr3\nlauncher_only=kept\n";
    WriteAll(file, original);
    UseConfig(file);
    BbSettings::Load();
    if (!CanMakeUnreadable(file)) {
        std::printf("SKIPPED: unreadable save (the user can read any file)\n");
        return;
    }
    const auto counts = BbSettings::SaveFailureCounts();
    BbSettings::Get().show_fps = true;
    {
        const Unreadable hold(file);
        Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Unreadable, "a save over an unreadable file is refused");
    }
    Require(BbSettings::LastSaveFailed(), "a save over a file that cannot be read reports the failure");
    Require(BbSettings::SaveFailureCounts().unreadable == counts.unreadable + 1, "the unreadable save is counted");
    Require(ReadAll(file) == original, "a file that cannot be read keeps its content");
    Require(Leftovers(folder) == 0, "no temporary file is left after a refused save");

    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Written, "the next save after the file is readable is written");
    Require(!BbSettings::LastSaveFailed(), "the failure clears after a save that works");
    const std::string after = ReadAll(file);
    Require(Has(after, "# mine") && Has(after, "launcher_only=kept"), "the comment and the unknown key stay");
    Require(Has(after, "upscaler=fsr3") && Has(after, "show_fps=1"), "the kept change and the file's value are written");
    Require(ReadAll(fs::path(file.string() + ".bak")) == original, "the previous content is kept as .bak");
}

void UnreadableLoadRefusesSaves(const fs::path& folder) {
    const fs::path file = folder / "load.ini";
    const std::string original = "preset=1\nsharpen=0\nupscaler=fsr3\n";
    WriteAll(file, original);
    UseConfig(file);
    if (!CanMakeUnreadable(file)) {
        std::printf("SKIPPED: unreadable load (the user can read any file)\n");
        return;
    }
    const auto counts = BbSettings::SaveFailureCounts();
    {
        const Unreadable hold(file);
        BbSettings::Load();
    }
    Require(BbSettings::SaveFailureCounts().load_unreadable == counts.load_unreadable + 1, "the unreadable load is counted");
    Require(BbSettings::LastSaveFailed(), "an unreadable load shows the save warning");
    BbSettings::Get().show_fps = true;
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::LoadUnreadable, "a save after an unreadable load is refused");
    BbSettings::Save();
    Require(BbSettings::FlushSaves(std::chrono::seconds(10)), "the save worker finishes");
    Require(ReadAll(file) == original, "the defaults of an unreadable load are never written over the file");
    BbSettings::Load();
    Require(BbSettings::Get().preset == 1 && !BbSettings::Get().sharpen, "a readable load takes the file's values");
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Written, "a readable load saves again, adding the missing keys");
    Require(Has(ReadAll(file), "preset=1") && Has(ReadAll(file), "sharpen=0"), "the file's values stay");
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Unchanged, "a save with no change leaves the file as it is");
}

void LoadReadsWholeLines(const fs::path& folder) {
    const fs::path file = folder / "lines.ini";
    const std::string filler = "comment_key=" + std::string(255 - std::string("comment_key=").size(), 'x');
    WriteAll(file, "\xEF\xBB\xBFupscaler=fsr3\n" + filler + "show_fps=1\n");
    UseConfig(file);
    BbSettings::Get().show_fps = false;
    BbSettings::Load();
    Require(BbSettings::Get().upscaler == BbSettings::UpscalerFsr3, "a key on the first line after a BOM is read");
    Require(!BbSettings::Get().show_fps, "the tail of a long line is not read as a key of its own");
    Require(BbSettings::SavedValue("comment_key", "") == filler.substr(std::string("comment_key=").size()) + "show_fps=1",
            "SavedValue reads the whole line");
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Written, "the save is written");
    const std::string after = ReadAll(file);
    Require(after.rfind("upscaler=fsr3\n", 0) == 0, "the BOM is not written back and the first key stays first");
    Require(Has(after, filler + "show_fps=1"), "the long line stays whole");
}

void LauncherEditSurvives(const fs::path& folder) {
    const fs::path file = folder / "edit.ini";
    WriteAll(file, "upscaler=fsr3\nshow_fps=0\nsharpen=0\n");
    UseConfig(file);
    BbSettings::Load();
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Written, "the first save adds the missing keys");
    std::string edited = ReadAll(file);
    edited.replace(edited.find("show_fps=0"), 10, "show_fps=1");
    WriteAll(file, edited);
    auto& s = BbSettings::Get();
    s.sharpen = true;
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Written, "the engine's change is written");
    const std::string after = ReadAll(file);
    Require(Has(after, "show_fps=1"), "a key the launcher changed while the engine ran keeps the launcher's value");
    Require(Has(after, "sharpen=1"), "the key the engine changed is written");
    Require(Has(after, "upscaler=fsr3"), "the other keys stay");
}

std::string g_late_binding = "Space";

void LateKeysKeepLauncherEdits(const fs::path& folder) {
    const fs::path file = folder / "late.ini";
    WriteAll(file, "bind.jump=Space\nupscaler=fsr3\n");
    UseConfig(file);
    BbInputConfig::append_bindings = nullptr;
    BbSettings::Load();
    BbInputConfig::append_bindings = [](BbInputConfig::Items& lines) { lines.emplace_back("bind.jump", g_late_binding); };
    std::string edited = ReadAll(file);
    edited.replace(edited.find("bind.jump=Space"), 15, "bind.jump=F");
    WriteAll(file, edited);
    BbSettings::Get().show_fps = !BbSettings::Get().show_fps;
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Written, "the save is written");
    Require(Has(ReadAll(file), "bind.jump=F"), "a binding the launcher changed keeps its value when the engine adds its bindings after the load");
    g_late_binding = "G";
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Written, "the changed binding is written");
    Require(Has(ReadAll(file), "bind.jump=G"), "a binding the engine changed is written");
    BbInputConfig::append_bindings = nullptr;
}

void SaveReplacesTheFile(const fs::path& folder) {
    const fs::path file = folder / "link.ini";
    const fs::path link = folder / "link-other-name.ini";
    const std::string original = "upscaler=fsr3\n";
    WriteAll(file, original);
    std::error_code error;
    fs::create_hard_link(file, link, error);
    if (error) {
        std::printf("SKIPPED: replace check (no hard links here: %s)\n", error.message().c_str());
        return;
    }
    UseConfig(file);
    BbSettings::Load();
    BbSettings::Get().show_fps = !BbSettings::Get().show_fps;
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Written, "the save is written");
    Require(ReadAll(link) == original, "the save replaces the file instead of writing into it");
    Require(ReadAll(file) != original, "the file has the new content");
}

void WorkerSavesAndRetries(const fs::path& folder) {
    const fs::path file = folder / "worker.ini";
    WriteAll(file, "upscaler=fsr3\n");
    UseConfig(file);
    BbSettings::Load();
    auto& s = BbSettings::Get();
    s.show_fps = true;
    BbSettings::Save();
    Require(BbSettings::FlushSaves(std::chrono::seconds(10)), "the save worker finishes");
    Require(Has(ReadAll(file), "show_fps=1"), "a save asked of the worker is written");

    const auto counts = BbSettings::SaveFailureCounts();
    {
        const HeldFile lock(fs::path(file.string() + ".lock"));
        s.sharpness = 0.37f;
        BbSettings::Save();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (BbSettings::SaveFailureCounts().not_locked == counts.not_locked &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Require(BbSettings::SaveFailureCounts().not_locked > counts.not_locked, "a save that finds the file locked reports it");
        Require(!Has(ReadAll(file), "sharpness=0.37"), "nothing is written while another program holds the lock");
    }
    Require(BbSettings::FlushSaves(std::chrono::seconds(10)), "the save worker finishes after the lock is free");
    Require(Has(ReadAll(file), "sharpness=0.37"), "a save that found the file locked is tried again and written");
    const auto after = BbSettings::SaveFailureCounts();
    Require(after.not_locked > counts.not_locked && after.dropped == counts.dropped, "the locked tries are counted and nothing is dropped");
    Require(fs::exists(file.string() + ".lock"), "the lock file stays, so every program locks the same file");
}

void UnknownAndLongLinesStay(const fs::path& folder) {
    const fs::path file = folder / "long.ini";
    const std::string long_line = "pad.long=" + std::string(LongLineLength, 'x');
    WriteAll(file, "# comment\r\nupscaler=fsr3\r\n" + long_line + "\r\nmystery = 7\r\nlast_without_newline=1");
    UseConfig(file);
    BbSettings::Load();
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Written, "the save is written");
    const std::string after = ReadAll(file);
    Require(Has(after, "# comment"), "a comment stays");
    Require(Has(after, long_line), "a line longer than any read buffer stays whole");
    Require(Has(after, "mystery = 7"), "a key the engine does not know stays as written");
    Require(Has(after, "last_without_newline=1"), "a last line without a newline stays");
    Require(after.find('\r') == std::string::npos, "the file is written with plain newlines");
    Require(Leftovers(folder) == 0, "no temporary file is left");
}

void AbsentFileGetsHeader(const fs::path& folder) {
    const fs::path file = folder / "absent.ini";
    UseConfig(file);
    BbSettings::Load();
    Expect(BbSettings::SaveNow(), BbSettings::SaveResult::Written, "the save is written");
    const std::string after = ReadAll(file);
    Require(after.rfind("# Lantern Engine settings", 0) == 0, "a new file starts with the header");
    Require(after.find("\nupscaler=") != std::string::npos, "a new file has the engine keys");
    Require(!fs::exists(file.string() + ".bak"), "a new file has no .bak");
}

void TwoWritersKeepEveryKey(const fs::path& folder) {
    const fs::path file = folder / "two.ini";
    WriteAll(file, "# two writers\nlauncher_only=kept\nupscaler=fsr3\n");
    UseConfig(file);
    BbSettings::Load();
    const auto counts = BbSettings::SaveFailureCounts();
    const auto writer = [](bool queued) {
        for (int i = 0; i < ThreadSaves; ++i) {
            BbSettings::Get().menu_x = float(i % 100) / 1000.0f;
            BbSettings::Get().menu_y = 0.5f;
            if (queued) {
                BbSettings::Save();
            } else {
                Require(BbSettings::SaveNow() != BbSettings::SaveResult::NotReplaced, "a save in one process is never refused");
            }
        }
    };
    std::thread first(writer, true);
    std::thread second(writer, false);
    first.join();
    second.join();
    Require(BbSettings::FlushSaves(std::chrono::seconds(10)), "the save worker finishes");
    const auto after_counts = BbSettings::SaveFailureCounts();
    Require(after_counts.unreadable == counts.unreadable && after_counts.not_replaced == counts.not_replaced &&
                after_counts.not_locked == counts.not_locked,
            "two writers in one process never fail a save");
    const std::string after = ReadAll(file);
    Require(Has(after, "# two writers") && Has(after, "launcher_only=kept"), "two writers keep the other keys");
    Require(Has(after, "upscaler=fsr3") && after.find("\nmenu_pos=") != std::string::npos, "two writers keep the engine keys");
    Require(Leftovers(folder) == 0, "two writers leave no temporary file");
}

[[noreturn]] void ExitSaveChild(const char* path) {
    UseConfig(path);
    BbSettings::Load();
    BbSettings::Get().preset = 2;
    BbSettings::Save();
    bbport_settings_flush_for_exit();
    std::_Exit(0);
}

bool ExitPathKeepsSave(const fs::path& folder, const char* self) {
    const fs::path file = folder / "exit.ini";
    WriteAll(file, "preset=1\n");
    fs::path program = fs::absolute(fs::path(self));
    program.make_preferred();
    std::string command = "\"" + program.string() + "\" --exit-save \"" + file.string() + "\"";
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    auto held = std::make_unique<HeldFile>(fs::path(file.string() + ".lock"));
    std::thread holder([held = std::move(held)]() mutable {
        std::this_thread::sleep_for(ExitLockHold);
        held.reset();
    });
    const int status = std::system(command.c_str());
    holder.join();
    Require(status == 0, "the exiting engine ends cleanly");
    return Has(ReadAll(file), "preset=2");
}

int Writer(const char* path, int cycles) {
    UseConfig(path);
    BbSettings::Load();
    auto& s = BbSettings::Get();
    for (int i = 0; i < cycles; ++i) {
        s.menu_x = float(i % 1000) / 1000.0f;
        s.menu_y = 0.5f;
        BbSettings::Save();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool flushed = BbSettings::FlushSaves(std::chrono::seconds(30));
    const auto counts = BbSettings::SaveFailureCounts();
    std::printf("engine-writer cycles=%d unreadable=%u not_replaced=%u not_locked=%u dropped=%u flushed=%d\n", cycles,
                unsigned(counts.unreadable), unsigned(counts.not_replaced), unsigned(counts.not_locked),
                unsigned(counts.dropped), flushed ? 1 : 0);
    return flushed && counts.dropped == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--writer") {
        return Writer(argv[2], std::atoi(argv[3]));
    }
    if (argc == 3 && std::string(argv[1]) == "--exit-save") {
        ExitSaveChild(argv[2]);
    }
    const fs::path folder = fs::temp_directory_path() /
                            ("settings-writes-test-" +
                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);
    if (argc == 2 && std::string(argv[1]) == "--exit-mutant") {
        const bool kept = ExitPathKeepsSave(folder, argv[0]);
        std::error_code ignored;
        fs::remove_all(folder, ignored);
        Require(!kept, "the build without the exit flush loses the save made just before exit");
        std::printf("settings-writes-test: exit flush mutant caught\n");
        return 0;
    }
    Require(ExitPathKeepsSave(folder, argv[0]), "a save made just before the engine exits reaches lantern.ini");
    UnreadableKeepsContent(folder);
    UnreadableLoadRefusesSaves(folder);
    LoadReadsWholeLines(folder);
    LauncherEditSurvives(folder);
    LateKeysKeepLauncherEdits(folder);
    SaveReplacesTheFile(folder);
    WorkerSavesAndRetries(folder);
    UnknownAndLongLinesStay(folder);
    AbsentFileGetsHeader(folder);
    TwoWritersKeepEveryKey(folder);
    std::error_code ignored;
    fs::remove_all(folder, ignored);
    std::printf("settings-writes-test: ok\n");
    return 0;
}
