#include "gpu/shim/engine/save_snapshots.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
extern "C" int bbgpu_save_backups_start(const char* user);
extern "C" int bbgpu_save_backups_write(int begin);
extern "C" unsigned bbgpu_save_backups_title(void);
extern "C" int bbgpu_save_backups_hold_for_exit(unsigned title);
extern "C" void bbgpu_save_backups_cancel_exit(void);

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc == 3 && std::string(argv[1]) == "--try-lock") {
        Lantern::BackupLock lock{fs::path(argv[2])};
        return lock ? 0 : 2;
    }
    if (argc == 3 && std::string(argv[1]) == "--session") {
        if (bbgpu_save_backups_start(argv[2])) return 2;
        std::cout << "READY" << std::endl;
        std::string stop;
        std::getline(std::cin, stop);
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--transactions") {
        const auto user = fs::absolute(argv[2]);
        fs::create_directories(user / "savedata");
        std::ofstream(user / "savedata/userdata0000") << "partial transaction";
        const auto folder = Lantern::BackupFolder(user);
        fs::create_directories(folder);
        {
            Lantern::BackupLock restoring(user, ".session.lock");
            assert(restoring);
            assert(bbgpu_save_backups_start(argv[2]) != 0);
        }
        assert(bbgpu_save_backups_start(argv[2]) == 0);
        Lantern::SaveSnapshots snapshots(user, folder);
        assert(bbgpu_save_backups_write(1) == 0);
        assert(bbgpu_save_backups_write(1) == 0);
        assert(snapshots.Tick(600) == Lantern::SnapshotResult::Busy);
        bbgpu_save_backups_write(0);
        assert(snapshots.Tick(605) == Lantern::SnapshotResult::Busy);
        std::ofstream(user / "savedata/userdata0000") << "complete transaction";
        bbgpu_save_backups_write(0);
        assert(snapshots.Tick(610) == Lantern::SnapshotResult::Created);
        std::cout << "PASS: native runtime defers partial and nested save transactions, then backs up completed progress\n";
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--title-worker") {
        const auto user = fs::absolute(argv[2]);
        fs::create_directories(user / "savedata");
        const auto folder = Lantern::BackupFolder(user);
        assert(bbgpu_save_backups_start(argv[2]) == 0);
        assert(bbgpu_save_backups_write(1) == 0);
        std::ofstream(user / "savedata/userdata0000") << "partial title progress";
        const auto title = bbgpu_save_backups_title();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(!fs::exists(folder));
        assert(!bbgpu_save_backups_hold_for_exit(title));
        std::ofstream(user / "savedata/userdata0000") << "finished title progress";
        assert(bbgpu_save_backups_write(0) == 0);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < until) {
            if (fs::exists(folder)) for (const auto& entry : fs::directory_iterator(folder)) {
                if (!Lantern::IsPlayBackup(entry.path().filename().string())) continue;
                std::ifstream saved(entry.path() / "savedata/userdata0000");
                std::string contents;
                std::getline(saved, contents);
                assert(contents == "finished title progress");
                while (!bbgpu_save_backups_hold_for_exit(title) && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                assert(bbgpu_save_backups_hold_for_exit(title));
                Lantern::BackupLock other(user, ".save-write.lock");
                assert(!other && other.Contended());
                bbgpu_save_backups_cancel_exit();
                Lantern::BackupLock released(user, ".save-write.lock");
                assert(released);
                std::cout << "PASS: title request waits for a write, publishes completed progress and holds the quit guard\n";
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        assert(!"title backup worker did not publish a completed snapshot");
    }
    if (argc == 3 && std::string(argv[1]) == "--quit-without-backups") {
        const auto user = fs::absolute(argv[2]);
        fs::create_directories(user / "savedata");
        std::ofstream(user / "savedata/userdata0000") << "latest save";
        assert(bbgpu_save_backups_start(argv[2]) == 0);
        assert(bbgpu_save_backups_write(1) == 0);
        const auto ticket = bbgpu_save_backups_title();
        assert(!bbgpu_save_backups_hold_for_exit(ticket));
        assert(bbgpu_save_backups_write(0) == 0);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!bbgpu_save_backups_hold_for_exit(ticket) && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        assert(bbgpu_save_backups_hold_for_exit(ticket));
        assert(!fs::exists(Lantern::BackupFolder(user)));
        bbgpu_save_backups_cancel_exit();
        std::cout << "PASS: quit waits for the live write even with automatic backups disabled\n";
        return 0;
    }
    const auto root = fs::temp_directory_path() / ("lantern-snapshots-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "user/savedata");
    auto write = [](const fs::path& p, const char* value) { std::ofstream(p) << value; };
    write(root / "user/savedata/userdata0000", "complete progress");
    Lantern::SaveSnapshots session(root / "user", root / "backups");
    assert(session.Tick(599) == Lantern::SnapshotResult::NotDue);
    fs::create_directory(root / "user/savedata/busy.lantern-work");
    assert(session.Tick(600) == Lantern::SnapshotResult::Busy);
    fs::remove(root / "user/savedata/busy.lantern-work");
    assert(session.Tick(605) == Lantern::SnapshotResult::Created);
    Lantern::SaveSnapshots restarted(root / "user", root / "backups");
    assert(restarted.Tick(600) == Lantern::SnapshotResult::Unchanged);
    assert(session.Tick(1205) == Lantern::SnapshotResult::Unchanged);
    assert(Lantern::BackupFolder(root / "user") == root / "user/launcher-backups/Main");
    assert(Lantern::BackupFolder(root / "user/profiles/Alex") == root / "user/launcher-backups/Alex");
    assert(Lantern::BackupFolder(root / "user/profiles/Alex/") == root / "user/launcher-backups/Alex");
    const auto named = root / "backups/Before boss_2020-01-01_00-00-00_play/savedata";
    fs::create_directories(named);
    write(named / "userdata0000", "keep forever");
    for (int i = 0; i < 9; ++i) {
        auto old = root / "backups" / ("2020-01-01_00-0" + std::to_string(i) + "-00_play") / "savedata";
        fs::create_directories(old);
        write(old / "userdata0000", "old progress");
    }
    Lantern::PrunePlayBackups(root / "backups");
    assert(fs::exists(named / "userdata0000"));
    int automatic = 0;
    for (const auto& e : fs::directory_iterator(root / "backups"))
        if (Lantern::IsPlayBackup(e.path().filename().string())) ++automatic;
    assert(automatic == 6);
    fs::create_directories(root / "user/savedata/junk.importing");
    write(root / "user/savedata/junk.importing/ignored", "not a save");
    assert(session.Tick(1805) == Lantern::SnapshotResult::Unchanged);
    write(root / "user/savedata/userdata0000", "progress at title");
    Lantern::SaveSnapshots title(root / "user", root / "title-backups");
    assert(title.Tick(120, true) == Lantern::SnapshotResult::Created);
    assert(title.Tick(121, true) == Lantern::SnapshotResult::Unchanged);
    assert(title.Tick(599) == Lantern::SnapshotResult::NotDue);
    assert(title.Tick(600) == Lantern::SnapshotResult::Unchanged);
    fs::rename(root / "user/savedata", root / "user/savedata.restore-previous");
    fs::create_directory(root / "user/savedata.restoring");
    write(root / "user/savedata.restoring/userdata0000", "partial staging");
    Lantern::RecoverSaveRestore(root / "user");
    std::ifstream recovered(root / "user/savedata/userdata0000");
    std::string restored;
    std::getline(recovered, restored);
    recovered.close();
    assert(restored == "progress at title" && !fs::exists(root / "user/savedata.restore-previous"));
    fs::remove_all(root);
    std::cout << "PASS: engine deadlines, retry, deduplication, retention, title backups and interrupted restore recovery\n";
}
