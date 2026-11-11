// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Lantern {
namespace fs = std::filesystem;
enum class SnapshotResult { NotDue, Busy, Empty, Unchanged, Created };

inline fs::path BackupFolder(const fs::path& user) {
    auto path = fs::absolute(user).lexically_normal();
    if (path.filename().empty()) path = path.parent_path();
    if (path.parent_path().filename() == "profiles")
        return path.parent_path().parent_path() / "launcher-backups" / path.filename();
    return path / "launcher-backups/Main";
}

// A process-wide and cross-process guard shared with the launcher's FileShare.None lock.
class BackupLock {
#ifdef _WIN32
    HANDLE handle_;
    DWORD error_;
public:
    explicit BackupLock(const fs::path& folder, const char* name = ".backup.lock") : handle_(CreateFileW((folder / name).c_str(),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)), error_(GetLastError()) {}
    ~BackupLock() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }
    bool Contended() const { return error_ == ERROR_SHARING_VIOLATION || error_ == ERROR_LOCK_VIOLATION; }
#else
    int fd_;
    int error_ = 0;
public:
    explicit BackupLock(const fs::path& folder, const char* name = ".backup.lock") : fd_(open((folder / name).c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600)) {
        if (fd_ < 0) error_ = errno;
        else if (flock(fd_, LOCK_EX | LOCK_NB)) { error_ = errno; close(fd_); fd_ = -1; }
    }
    ~BackupLock() { if (fd_ >= 0) close(fd_); }
    explicit operator bool() const { return fd_ >= 0; }
    bool Contended() const { return error_ == EWOULDBLOCK || error_ == EAGAIN; }
#endif
    BackupLock(const BackupLock&) = delete;
    BackupLock& operator=(const BackupLock&) = delete;
};

inline bool IsPlayBackup(const std::string& name) {
    if (name.size() != 24 || name.substr(19) != "_play") return false;
    std::tm time{};
    std::istringstream in(name.substr(0, 19));
    in >> std::get_time(&time, "%Y-%m-%d_%H-%M-%S");
    return !in.fail();
}

inline void PrunePlayBackups(const fs::path& folder) {
    std::vector<fs::path> automatic;
    for (const auto& entry : fs::directory_iterator(folder))
        if (!entry.is_symlink() && entry.is_directory() && IsPlayBackup(entry.path().filename().string()))
            automatic.push_back(entry.path());
    std::sort(automatic.begin(), automatic.end(), std::greater<>());
    for (std::size_t i = 6; i < automatic.size(); ++i) {
        std::error_code ignored;
        fs::remove_all(automatic[i], ignored);
    }
}

class SaveSnapshots {
    struct File {
        fs::path name;
        std::uintmax_t size;
        fs::file_time_type modified;
        bool operator==(const File&) const = default;
    };
    fs::path source_, folder_, previous_;
    std::string error_;
    double due_ = 600;

    static bool Ends(const std::string& name, const char* suffix) { return name.ends_with(suffix); }
    static std::vector<File> Files(const fs::path& root) {
        std::vector<File> files;
        if (!fs::exists(root)) return files;
        for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
            const auto name = it->path().filename().string();
            if (Ends(name, ".lantern-work") || Ends(name, ".lantern-commit"))
                throw std::runtime_error("the game is writing its saves");
            if (Ends(name, ".importing")) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
            if (it->is_symlink()) throw std::runtime_error("a save path is a symbolic link");
            if (it->is_regular_file()) files.push_back({it->path().lexically_relative(root), it->file_size(), it->last_write_time()});
        }
        std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.name < b.name; });
        return files;
    }

    static bool Equal(const fs::path& a, const fs::path& b) {
        std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
        if (!left || !right) return false;
        std::array<char, 65536> x{}, y{};
        do {
            left.read(x.data(), x.size()); right.read(y.data(), y.size());
            if (left.bad() || right.bad() || left.gcount() != right.gcount() ||
                !std::equal(x.begin(), x.begin() + left.gcount(), y.begin())) return false;
        } while (!left.eof());
        return right.eof();
    }

    bool Matches(const fs::path& other, const std::vector<File>& files) const {
        const auto saved = Files(other);
        if (saved.size() != files.size()) return false;
        for (std::size_t i = 0; i < files.size(); ++i)
            if (files[i].name != saved[i].name || files[i].size != saved[i].size ||
                !Equal(source_ / files[i].name, other / files[i].name)) return false;
        return true;
    }
public:
    SaveSnapshots(fs::path user, fs::path folder) : source_(std::move(user) / "savedata"), folder_(std::move(folder)) {}
    const std::string& Error() const { return error_; }

    // Elapsed monotonic seconds, injected by tests; a busy attempt never advances the deadline.
    SnapshotResult Tick(double elapsed) {
        if (elapsed < due_) return SnapshotResult::NotDue;
        fs::path staging;
        std::unique_ptr<BackupLock> guard;
        try {
            fs::create_directories(folder_);
            guard = std::make_unique<BackupLock>(folder_);
            if (!*guard) { error_ = "a save transaction or another backup is in progress"; return SnapshotResult::Busy; }
            const auto files = Files(source_);
            if (files.empty()) return SnapshotResult::Empty;
            if (previous_.empty() || !fs::exists(previous_)) {
                std::vector<fs::path> existing;
                for (const auto& entry : fs::directory_iterator(folder_)) {
                    const auto name = entry.path().filename().string();
                    // Timestamp-only and labeled snapshots; never partial directories.
                    if (entry.is_directory() && !entry.is_symlink() && !name.ends_with(".copying") &&
                        fs::is_directory(entry.path() / "savedata")) existing.push_back(entry.path());
                }
                // Modification time is independent of a custom label's alphabetic order.
                std::sort(existing.begin(), existing.end(), [](const auto& a, const auto& b) {
                    return fs::last_write_time(a) > fs::last_write_time(b);
                });
                if (!existing.empty()) previous_ = existing.front() / "savedata";
            }
            if (!previous_.empty() && fs::exists(previous_) && Matches(previous_, files) && Files(source_) == files) {
                due_ = elapsed + 600;
                return SnapshotResult::Unchanged;
            }
            const auto now = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            std::ostringstream name;
            name << std::put_time(&local, "%Y-%m-%d_%H-%M-%S") << "_play";
            const auto target = folder_ / name.str();
            if (fs::exists(target)) return SnapshotResult::Busy;
            staging = folder_ / (name.str() + ".copying");
            // Only this writer owns this staging path under the exclusive profile lock.
            fs::remove_all(staging);
            for (const auto& file : files) {
                const auto to = staging / "savedata" / file.name;
                fs::create_directories(to.parent_path());
                std::ifstream input(source_ / file.name, std::ios::binary);
                std::ofstream output(to, std::ios::binary);
                if (!input || !output) throw std::runtime_error("a save file cannot be opened");
                std::array<char, 65536> buffer;
                while (input) {
                    input.read(buffer.data(), buffer.size());
                    output.write(buffer.data(), input.gcount());
                }
                output.close();
                if (!input.eof() || input.bad() || !output) throw std::runtime_error("a save file cannot be copied");
                fs::last_write_time(to, file.modified);
            }
            if (Files(source_) != files || !Matches(staging / "savedata", files) || Files(source_) != files)
                throw std::runtime_error("saves changed during the snapshot");
            fs::rename(staging, target);
            staging.clear();
            previous_ = target / "savedata";
            due_ = elapsed + 600;
            // Cleanup failure must not turn a successfully published backup into a failed attempt.
            try { PrunePlayBackups(folder_); } catch (const std::exception&) { }
            return SnapshotResult::Created;
        } catch (const std::exception& e) {
            error_ = e.what();
            if (!staging.empty()) { std::error_code ignored; fs::remove_all(staging, ignored); }
            return SnapshotResult::Busy;
        }
    }
};
}
