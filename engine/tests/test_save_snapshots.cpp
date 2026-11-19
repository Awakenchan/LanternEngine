#include "gpu/shim/engine/save_snapshots.h"
#include <cassert>
#include <fstream>
#include <iostream>
extern "C" int bbgpu_save_backups_start(const char* user);
extern "C" void bbgpu_save_backups_write(int begin);

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc == 3 && std::string(argv[1]) == "--try-lock") {
        Lantern::BackupLock lock{fs::path(argv[2])};
        return lock ? 0 : 2;
    }
    if (argc == 3 && std::string(argv[1]) == "--transactions") {
        const auto user = fs::absolute(argv[2]);
        fs::create_directories(user / "savedata");
        std::ofstream(user / "savedata/userdata0000") << "partial transaction";
        const auto folder = Lantern::BackupFolder(user);
        fs::create_directories(folder);
        {
            Lantern::BackupLock restoring(folder, ".session.lock");
            assert(restoring);
            assert(bbgpu_save_backups_start(argv[2]) != 0);
        }
        assert(bbgpu_save_backups_start(argv[2]) == 0);
        Lantern::SaveSnapshots snapshots(user, folder);
        bbgpu_save_backups_write(1);
        bbgpu_save_backups_write(1);
        assert(snapshots.Tick(600) == Lantern::SnapshotResult::Busy);
        bbgpu_save_backups_write(0);
        assert(snapshots.Tick(605) == Lantern::SnapshotResult::Busy);
        std::ofstream(user / "savedata/userdata0000") << "complete transaction";
        bbgpu_save_backups_write(0);
        assert(snapshots.Tick(610) == Lantern::SnapshotResult::Created);
        std::cout << "PASS: native runtime defers partial and nested save transactions, then backs up completed progress\n";
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
    fs::remove_all(root);
    std::cout << "PASS: engine deadline, retry, duplicate avoidance, profiles, staging and named retention\n";
}
