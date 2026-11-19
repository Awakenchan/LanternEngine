// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include "gpu/shadps4/video_core/renderer_vulkan/driver_cache_save.h"

namespace {
using namespace Vulkan::DriverCacheSave;

constexpr std::uint64_t Start = 1000 * NsPerSecond;

struct Suite {
    int failures = 0;
    void Expect(bool ok, const char* what) {
        if (!ok) {
            ++failures;
            std::printf("  failed: %s\n", what);
        }
    }
};

Inputs Due() {
    return {.now_ns = Start + MinIntervalNs,
            .last_save_ns = Start,
            .last_new_pipeline_ns = Start + MinIntervalNs - CompileQuietNs,
            .new_pipelines = MinNewPipelines,
            .closed = false};
}

void Decisions(Suite& t) {
    t.Expect(Decide(Due()) == Verdict::Save, "due save at the exact interval, quiet and minimum");
    auto in = Due();
    in.closed = true;
    t.Expect(Decide(in) == Verdict::Closed, "closed refuses a due save");
    in = Due();
    in.new_pipelines = 0;
    t.Expect(Decide(in) == Verdict::NothingNew, "no new pipeline is nothing new");
    in = Due();
    in.new_pipelines = MinNewPipelines - 1;
    t.Expect(Decide(in) == Verdict::TooFew, "one under the minimum is too few");
    in = Due();
    in.now_ns = Start + MinIntervalNs - 1;
    in.last_new_pipeline_ns = Start;
    t.Expect(Decide(in) == Verdict::TooSoon, "one ns before the interval is too soon");
    in.now_ns = Start + MinIntervalNs * 3 / 4;
    t.Expect(Decide(in) == Verdict::TooSoon, "three quarters of the interval is too soon");
    in = Due();
    in.last_new_pipeline_ns = in.now_ns - CompileQuietNs + 1;
    t.Expect(Decide(in) == Verdict::Compiling, "a pipeline inside the quiet time is compiling");
    in.last_new_pipeline_ns = in.now_ns - 1;
    t.Expect(Decide(in) == Verdict::Compiling, "a pipeline 1 ns ago is compiling");
    in = Due();
    in.new_pipelines = 100000;
    in.now_ns = Start + 10 * MinIntervalNs;
    t.Expect(Decide(in) == Verdict::Save, "many new pipelines long after the last save");
    in = Due();
    in.compiles_unfinished = 1;
    t.Expect(Decide(in) == Verdict::Compiling, "a pipeline still compiling on a compile thread is compiling");
    in.compiles_unfinished = 0;
    t.Expect(Decide(in) == Verdict::Save, "no compile left: due save");
}

void ExitAndWrite(Suite& t) {
    t.Expect(WriteAllowed(false, false), "a play save writes while the game runs");
    t.Expect(!WriteAllowed(false, true), "a play save does not write once the exit began");
    t.Expect(!WriteAllowed(true, false), "a play save does not write after the exit save");
    t.Expect(ExitSaveNeeded(false, 0, 0), "without saves during play the exit save always runs");
    t.Expect(ExitSaveNeeded(false, 5, 3), "without saves during play the exit save runs with work pending");
    t.Expect(!ExitSaveNeeded(true, 0, 0), "nothing new since the play save: the exit save is skipped");
    t.Expect(ExitSaveNeeded(true, 1, 0), "one new pipeline since the play save: the exit save runs");
    t.Expect(ExitSaveNeeded(true, 0, 1), "a pipeline still compiling: the exit save runs");
}

void Temporaries(Suite& t) {
    const std::string_view target = "CUSA03173.vkcache";
    t.Expect(TemporaryOwner("CUSA03173.vkcache.tmp.4242.7", target) == 4242u, "owner of a temporary");
    t.Expect(TemporaryOwner("CUSA03173.vkcache.tmp.0.0", target) == 0u, "pid 0 parses");
    t.Expect(TemporaryOwner("CUSA03173.vkcache.tmp.4294967295.1", target) == 4294967295u,
             "largest process id parses");
    t.Expect(!TemporaryOwner("CUSA03173.vkcache.tmp.4294967296.1", target), "process id over 32 bits");
    t.Expect(!TemporaryOwner("CUSA03173.vkcache.tmp.99999999999999999999999.1", target),
             "process id over 64 bits");
    t.Expect(!TemporaryOwner("CUSA03173.vkcache", target), "the cache file itself");
    t.Expect(!TemporaryOwner("CUSA03173.vkcache.tmp.4242", target), "no sequence");
    t.Expect(!TemporaryOwner("CUSA03173.vkcache.tmp.4242.", target), "empty sequence");
    t.Expect(!TemporaryOwner("CUSA03173.vkcache.tmp.4242.x", target), "sequence not a number");
    t.Expect(!TemporaryOwner("CUSA03173.vkcache.tmp..1", target), "empty process id");
    t.Expect(!TemporaryOwner("CUSA03173.vkcache.tmp.42a.1", target), "process id not a number");
    t.Expect(!TemporaryOwner("CUSA00900.vkcache.tmp.4242.7", target), "another title's temporary");
    t.Expect(!TemporaryOwner("CUSA", target), "a shorter name");
    t.Expect(!TemporaryOwner("CUSA03173.vkcache.bak.4242.7", target), "another suffix");
    t.Expect(!TemporaryOwner("CUSA03173.vkcache.tmp.4242.7", ""), "no target name");
    t.Expect(RemoveTemporary(7, 9, false), "a dead process's temporary goes");
    t.Expect(!RemoveTemporary(7, 9, true), "a live process's temporary stays");
    t.Expect(!RemoveTemporary(9, 9, false), "this process's temporary stays");
}

struct FakeLock {
    bool held = false;
    void lock() { held = true; }
    void unlock() { held = false; }
};

struct FakeHost {
    FakeLock mutex;
    Inputs inputs = Due();
    bool lost = false, lost_in_snapshot = false, closed = false, shutting = false, shut_in_snapshot = false;
    bool write_ok = true, snapshotted = false, wrote = false, dropped = false, recorded = false;
    bool snapshot_locked = false, write_locked = false, record_locked = false, report_locked = false;
    FakeLock& SaveMutex() { return mutex; }
    bool DeviceLost() { return lost; }
    bool Closed() { return closed; }
    bool ShuttingDown() { return shutting; }
    Inputs Read() { return inputs; }
    void Skipped(Verdict) {}
    int Snapshot() {
        snapshotted = true;
        snapshot_locked = mutex.held;
        shutting = shutting || shut_in_snapshot;
        lost = lost || lost_in_snapshot;
        return 7;
    }
    void Dropped() { dropped = true; }
    bool Write(int) {
        wrote = true;
        write_locked = mutex.held;
        return write_ok;
    }
    void Record(Step, const Inputs&) {
        recorded = true;
        record_locked = mutex.held;
    }
    void Report(Step, const Inputs&, int) { report_locked = mutex.held; }
};

void PlaySaves(Suite& t) {
    FakeHost due;
    t.Expect(PlaySave(due) == Step::Saved && due.wrote && due.recorded, "a due play save writes and records");
    t.Expect(!due.snapshot_locked, "the snapshot runs without the save lock");
    t.Expect(due.write_locked && due.record_locked, "the write and its record run under the save lock");
    t.Expect(!due.report_locked && !due.mutex.held, "the report runs after the save lock is released");
    FakeHost exiting;
    exiting.shut_in_snapshot = true;
    t.Expect(PlaySave(exiting) == Step::Dropped && exiting.dropped && !exiting.wrote,
             "an exit begun during the snapshot drops the play save");
    FakeHost closed;
    closed.inputs.closed = true;
    t.Expect(PlaySave(closed) == Step::Waited && !closed.snapshotted, "a closed cache takes no snapshot");
    FakeHost lost;
    lost.lost = true;
    t.Expect(PlaySave(lost) == Step::Stop && !lost.snapshotted, "after a device loss the saver stops");
    FakeHost lost_late;
    lost_late.lost_in_snapshot = true;
    t.Expect(PlaySave(lost_late) == Step::Stop && !lost_late.wrote, "a device lost during the snapshot writes nothing");
    FakeHost failing;
    failing.write_ok = false;
    t.Expect(PlaySave(failing) == Step::Failed && failing.recorded && !failing.mutex.held, "a failed write is recorded");
}

std::string Source(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void Wiring(Suite& t) {
    const std::string serialization = Source("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    const auto sync = serialization.find("void PipelineCache::Sync() {");
    const auto end = serialization.find("\n}", sync);
    const auto in_sync = [&](const char* what) {
        const auto at = serialization.find(what, sync);
        return sync != std::string::npos && at < end ? at : std::string::npos;
    };
    const auto store = in_sync("driver_cache_shutdown.store(true");
    const auto stop = in_sync("StopDriverCacheSaver();");
    const auto lock = in_sync("DriverCacheSaveMutex()");
    const auto save = in_sync("SaveNativeCache();");
    t.Expect(store != std::string::npos && stop != std::string::npos && lock != std::string::npos &&
                 save != std::string::npos && store < stop && stop < lock && lock < save,
             "Sync marks the exit, joins the play saver, then takes the save lock and saves");
    const std::string cache = Source("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");
    t.Expect(cache.find("return DriverCacheSave::PlaySave(host);") != std::string::npos,
             "the play saver goes through PlaySave");
    t.Expect(cache.find("bool DeviceLost() { return Breadcrumbs::DeviceLost(); }") != std::string::npos,
             "the play saver reads the device-lost flag");
    t.Expect(cache.find("if (TrySaveDriverCache() == DriverCacheSave::Step::Stop) {") != std::string::npos,
             "the play saver's loop ends on a stop");
    const std::string crumbs = Source("gpu/shadps4/video_core/renderer_vulkan/vk_breadcrumbs.cpp");
    const auto report = crumbs.find("void ReportDeviceLost(const char* where) {");
    t.Expect(report != std::string::npos &&
                 crumbs.find("device_lost.store(true", report) < crumbs.find("reported.test_and_set()", report),
             "every device-lost report sets the flag first");
}

int RunSuite() {
    Suite t;
    Decisions(t);
    ExitAndWrite(t);
    Temporaries(t);
    PlaySaves(t);
    return t.failures;
}
} // namespace

int main() {
    Suite wiring;
    Wiring(wiring);
    const int failures = RunSuite() + wiring.failures;
    if (failures != 0) {
        std::printf("driver-cache-save-test: %d failed\n", failures);
        return 1;
    }
#ifdef LANTERN_DECISION_MUTANTS
    static constexpr const char* names[MutantCount + 1] = {
        "",
        "closed ignored",
        "no zero check",
        "minimum one lower",
        "half the interval",
        "quiet time ignored",
        "no overflow check",
        "target prefix ignored",
        "temporary without a sequence accepted",
        "sequence not checked",
        "own process removed",
        "unfinished compiles ignored",
        "play save writes during the exit",
        "exit save skipped with compiles pending",
        "device loss ignored",
        "snapshot under the save lock",
        "exit not rechecked before the write",
    };
    int survived = 0;
    for (int id = 1; id <= MutantCount; ++id) {
        active_mutant = id;
        std::printf("mutant %d (%s):\n", id, names[id]);
        if (RunSuite() == 0) {
            ++survived;
            std::printf("  SURVIVED\n");
        }
    }
    active_mutant = 0;
    if (survived != 0) {
        std::printf("driver-cache-save-test: %d of %d mutants survived\n", survived, MutantCount);
        return 1;
    }
    std::printf("driver-cache-save-test: passed, all %d mutants killed\n", MutantCount);
#else
    std::printf("driver-cache-save-test: passed\n");
#endif
    return 0;
}
