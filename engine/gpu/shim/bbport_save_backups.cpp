// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/save_snapshots.h"
#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {
std::filesystem::path Utf8Path(const char* text) { return std::filesystem::path(reinterpret_cast<const char8_t*>(text)); }
bool Enabled(const std::filesystem::path& config) {
    bool enabled = !std::getenv("BB_SAVE_BACKUPS") || std::string(std::getenv("BB_SAVE_BACKUPS")) != "0";
    std::ifstream input(config);
    std::string line;
    while (std::getline(input, line)) {
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        auto key = line.substr(0, equal);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        if (key != "save_backups") continue;
        auto value = line.substr(equal + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r") + 1);
        enabled = value != "0" && value != "false" && value != "off";
    }
    return enabled;
}
std::filesystem::path active_profile;
std::unique_ptr<Lantern::BackupLock> writing;
std::unique_ptr<Lantern::BackupLock> session;
unsigned writers = 0; // mutations serialized by the runtime save-data mutex
std::atomic_uint title_requests{0};
std::condition_variable_any wake;
std::mutex wake_mutex;
std::jthread worker; // stop and join before destroying any state it uses
}

// The runtime calls these at writable mount/unmount and memory/delete boundaries.
// Holding the same file lock makes manual launcher snapshots defer too.
extern "C" int bbgpu_save_backups_write(int begin) {
    if (active_profile.empty()) return 0;
    if (!begin) { if (writers && --writers == 0) writing.reset(); return 0; }
    if (writers) { ++writers; return 0; }
    try {
        do {
            writing = std::make_unique<Lantern::BackupLock>(active_profile, ".save-write.lock");
            if (!*writing && !writing->Contended())
                throw std::runtime_error("the live save lock is inaccessible");
            if (!*writing) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (!*writing);
        ++writers;
        return 0;
    } catch (const std::exception& e) {
        std::printf("Saves: cannot coordinate snapshots: %s\n", e.what());
        writing.reset();
        return -1;
    }
}

extern "C" void bbgpu_save_backups_title(void) {
    {
        std::lock_guard lock(wake_mutex);
        ++title_requests;
    }
    wake.notify_all();
}

extern "C" int bbgpu_save_backups_start(const char* user) {
    if (worker.joinable()) return 0;
    if (!user || !*user) return -1;
    try {
        const auto input = Utf8Path(user);
        std::filesystem::create_directories(input);
        const auto source = std::filesystem::canonical(input);
        const auto folder = std::getenv("BB_SAVE_BACKUP_DIR")
            ? Utf8Path(std::getenv("BB_SAVE_BACKUP_DIR")) : Lantern::BackupFolder(source);
        const auto config = std::getenv("BB_CONFIG") ? Utf8Path(std::getenv("BB_CONFIG"))
            : std::filesystem::path("lantern.ini");
        session = std::make_unique<Lantern::BackupLock>(source, ".session.lock");
        if (!*session) throw std::runtime_error("the live save profile is in use or inaccessible");
        Lantern::RecoverSaveRestore(source);
        active_profile = source;
        const auto shown = folder.u8string();
        std::printf("Saves: automatic backups every 10 minutes and at the title menu; folder: %s\n", reinterpret_cast<const char*>(shown.c_str()));
        worker = std::jthread([source, folder, config](std::stop_token stop) {
            Lantern::SaveSnapshots snapshots(source, folder);
            const auto started = std::chrono::steady_clock::now();
            unsigned completed_title = 0;
            bool warned = false;
            while (!stop.stop_requested()) {
                const auto request = title_requests.load();
                if (Enabled(config)) {
                    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                    const bool title = request != completed_title;
                    const auto result = snapshots.Tick(seconds, title);
                    if (result != Lantern::SnapshotResult::Busy) {
                        completed_title = request;
                        warned = false;
                    }
                    if (result == Lantern::SnapshotResult::Created) {
                        std::printf("Saves: %s backup completed (newest 6 play backups kept).\n",
                            title ? "title menu" : "automatic");
                        std::fflush(stdout);
                    } else if (result == Lantern::SnapshotResult::Busy && !warned) {
                        std::printf("Saves: backup deferred: %s; retrying every 5 seconds.\n", snapshots.Error().c_str());
                        std::fflush(stdout);
                        warned = true;
                    }
                } else {
                    completed_title = request;
                }
                std::unique_lock lock(wake_mutex);
                wake.wait_for(lock, stop, std::chrono::seconds(5), [request] { return title_requests.load() != request; });
            }
        });
        return 0;
    } catch (const std::exception& e) {
        std::printf("Saves: automatic backups could not start: %s\n", e.what());
        session.reset();
        active_profile.clear();
        return -1;
    }
}
