// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/main_jobs_barrier.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace J = BbEngine::MainJobsBarrier;
namespace L = BbEngine::MainJobsBarrier::Layout;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;

namespace {
int g_failed = 0;
bool g_quiet = false;

enum class ModelMutant : int { None = 0, StealMovesAfterUnlock, PopRemovesAfterUnlock, kCount };
ModelMutant g_model_mutant = ModelMutant::None;
bool ModelIs(ModelMutant m) { return g_model_mutant == m; }

void Check(bool ok, const char* what) {
    if (ok) return;
    if (!g_quiet) std::printf("FAILED: %s\n", what);
    ++g_failed;
}

constexpr u64 kStateAt = 0x528;
constexpr u32 kIdle = 2, kRunning = 3;
constexpr u64 kSystemBytes = 64;
constexpr u64 kFarTsc = 0x1234567890abcdefull;
std::chrono::seconds kHangBound{20};
[[maybe_unused]] constexpr int kMutantRounds = 30;

struct Arena {
    explicit Arena(u32 n) : bytes(std::make_unique<std::uint8_t[]>(kSystemBytes + n * L::kParticipantBytes + 64)) {
        std::memset(bytes.get(), 0, kSystemBytes + n * L::kParticipantBytes + 64);
        const u64 raw = reinterpret_cast<u64>(bytes.get());
        system = (raw + 63) & ~u64(63);
        base = system + kSystemBytes;
        Put64(system + L::kParticipants, base);
        Put32(system + L::kCount, n);
        for (u32 i = 0; i < n; ++i) {
            Put64(At(i) + L::kSystem, system);
            Put32(At(i) + L::kIndex, i);
            Put32(At(i) + kStateAt, kIdle);
        }
    }
    u64 At(u32 i) const { return base + u64(i) * L::kParticipantBytes; }
    static void Put64(u64 at, u64 v) { __atomic_store_n(reinterpret_cast<u64*>(at), v, __ATOMIC_RELAXED); }
    static void Put32(u64 at, u32 v) { __atomic_store_n(reinterpret_cast<u32*>(at), v, __ATOMIC_RELAXED); }
    static u64 Get64(u64 at) { return __atomic_load_n(reinterpret_cast<const u64*>(at), __ATOMIC_RELAXED); }
    static u32 Get32(u64 at) { return __atomic_load_n(reinterpret_cast<const u32*>(at), __ATOMIC_RELAXED); }
    std::unique_ptr<std::uint8_t[]> bytes;
    u64 system = 0, base = 0;
};

struct Call {
    char what;
    u64 a, b;
    bool operator==(const Call&) const = default;
};

struct ScriptOps {
    explicit ScriptOps(Arena* a) : arena(a) {}
    Arena* arena;
    std::vector<Call> log;
    std::vector<bool> steal_results;
    std::size_t next_steal = 0;
    u64 own_count_after_step = 0;
    int grow_count_after_steal = -1;
    u64 tsc = kFarTsc;
    u64 Load64(u64 at) const { return Arena::Get64(at); }
    u32 Load32(u64 at) const { return Arena::Get32(at); }
    bool Steal(u64 victim, u64 thief) {
        log.push_back({'s', victim, thief});
        if (grow_count_after_steal >= 0 && next_steal == std::size_t(grow_count_after_steal))
            Arena::Put32(arena->system + L::kCount, Arena::Get32(arena->system + L::kCount) + 1);
        const bool r = next_steal < steal_results.size() && steal_results[next_steal];
        ++next_steal;
        return r;
    }
    void RunGameStep(u64 self) {
        log.push_back({'g', self, 0});
        Arena::Put64(self + L::kDequeCount, own_count_after_step);
    }
    u64 Tsc() {
        log.push_back({'t', tsc, 0});
        return tsc;
    }
};

std::vector<Call> GameSweepOrder(const Arena& a, u32 self_index, u64 tsc, const std::vector<bool>& results, int grow_after = -1) {
    std::vector<Call> out;
    u32 n = Arena::Get32(a.system + L::kCount);
    const u64 self = a.At(self_index);
    const u64 start = (u64(self_index) + tsc) % n;
    std::size_t k = 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 at = u32(start + i) % n;
        if (at != self_index) {
            out.push_back({'s', a.At(at) + L::kDeque, self + L::kDeque});
            const bool stole = k < results.size() && results[k];
            if (grow_after >= 0 && k == std::size_t(grow_after)) ++n;
            ++k;
            if (stole) break;
        }
    }
    return out;
}

void TestShape() {
    Arena a(4);
    J::Shape s{a.At(0), a.system, a.base, 4, 0};
    Check(J::CheckShape(s) == J::Check::Ok, "participant 0 of 4 passes");
    s = {a.At(3), a.system, a.base, 4, 3};
    Check(J::CheckShape(s) == J::Check::Ok, "the last participant passes");
    s = {a.At(0), 0, a.base, 4, 0};
    Check(J::CheckShape(s) == J::Check::NoSystem, "no system");
    s = {a.At(0), a.system, 0, 4, 0};
    Check(J::CheckShape(s) == J::Check::NoParticipants, "no participant array");
    s = {a.At(0), a.system, a.base, 0, 0};
    Check(J::CheckShape(s) == J::Check::CountOutOfRange, "zero participants");
    s = {a.At(0), a.system, a.base, J::kMostParticipants + 1, 0};
    Check(J::CheckShape(s) == J::Check::CountOutOfRange, "more than the most participants");
    s = {a.At(0), a.system, a.base, J::kMostParticipants, 0};
    Check(J::CheckShape(s) == J::Check::Ok, "exactly the most participants");
    s = {a.At(4), a.system, a.base, 4, 4};
    Check(J::CheckShape(s) == J::Check::NotInArray, "one past the array");
    s = {a.base - L::kParticipantBytes, a.system, a.base, 4, 0};
    Check(J::CheckShape(s) == J::Check::NotInArray, "before the array");
    s = {a.At(1) + 8, a.system, a.base, 4, 1};
    Check(J::CheckShape(s) == J::Check::NotOnBoundary, "inside a participant");
    s = {a.At(2), a.system, a.base, 4, 1};
    Check(J::CheckShape(s) == J::Check::IndexDiffers, "index differs from place");
    ScriptOps ops{&a};
    const J::Shape read = J::ReadShape(ops, a.At(2));
    Check(read.system == a.system && read.base == a.base && read.count == 4 && read.index == 2 && read.self == a.At(2),
          "ReadShape reads the game's offsets");
    Arena::Put64(a.At(1) + L::kSystem, 0);
    const J::Shape none = J::ReadShape(ops, a.At(1));
    Check(none.system == 0 && none.base == 0 && J::CheckShape(none) == J::Check::NoSystem, "ReadShape stops at no system");
}

void TestChooseAndLatch() {
    Check(J::Choose(false, J::Check::Ok) == J::Route::Game, "off runs the game's step");
    Check(J::Choose(true, J::Check::Ok) == J::Route::Ours, "on with a good shape runs ours");
    for (J::Check c : {J::Check::NoSystem, J::Check::NoParticipants, J::Check::CountOutOfRange, J::Check::NotInArray,
                       J::Check::NotOnBoundary, J::Check::IndexDiffers})
        Check(J::Choose(true, c) == J::Route::Game, "a failed check runs the game's step");
    Check(J::FrameLatch(true, false), "on and not refused latches on");
    Check(!J::FrameLatch(false, false), "off latches off");
    Check(!J::FrameLatch(true, true), "a refusal keeps it off for the session");
}

void TestStart() {
    Check(J::SweepStart(0, 7, 3) == 1, "start is tsc mod count");
    Check(J::SweepStart(2, 7, 3) == 0, "start adds the index");
    Check(J::SweepStart(1, kFarTsc, 3) == u32((1 + kFarTsc) % 3), "start is computed in 64 bits");
    Check(J::SweepStart(5, ~u64(0), 7) == u32((u64(5) + ~u64(0)) % 7), "the 64-bit sum wraps as the game's add does");
}

void TestSweep() {
    for (u32 n : {1u, 2u, 3u, 5u, 7u}) {
        for (u32 self = 0; self < n; ++self) {
            for (int stolen_at = -1; stolen_at < int(n); ++stolen_at) {
                Arena a(n);
                ScriptOps ops{&a};
                std::vector<bool> results(n, false);
                if (stolen_at >= 0) results[std::size_t(stolen_at)] = true;
                ops.steal_results = results;
                const bool took = J::SweepOthers(ops, a.At(self), kFarTsc);
                const auto want = GameSweepOrder(a, self, kFarTsc, results);
                Check(ops.log == want, "the sweep calls the steals in the game's order");
                Check(took == (stolen_at >= 0 && std::size_t(stolen_at) < want.size()), "the sweep says whether it took");
            }
        }
    }
    {
        Arena a(5);
        ScriptOps ops{&a};
        ops.steal_results.assign(8, false);
        ops.grow_count_after_steal = 0;
        J::SweepOthers(ops, a.At(0), 3);
        Arena b(5);
        const auto want = GameSweepOrder(b, 0, 3, std::vector<bool>(8, false), 0);
        Check(ops.log.size() == want.size(), "the sweep reloads the count every turn, as the game's loop does");
    }
    {
        Arena a(4);
        ScriptOps ops{&a};
        ops.steal_results.assign(4, false);
        Arena::Put64(a.At(0) + L::kDequeCount, 2);
        const bool took = J::SweepOthers(ops, a.At(0), 0);
        Check(took && ops.log.size() == 1, "jobs in its own deque end the sweep after the first steal");
    }
}

struct CountingOps {
    mutable int loads = 0;
    u64 Load64(u64) const { ++loads; return 0; }
    u32 Load32(u64) const { ++loads; return 0; }
};

void TestNullCaller() {
    CountingOps ops;
    const J::Shape s = J::ReadShape(ops, 0);
    Check(ops.loads == 0, "a null participant (the job system torn down) is not read");
    Check(J::CheckShape(s) == J::Check::NoSystem && J::Choose(true, J::CheckShape(s)) == J::Route::Game,
          "a null participant runs the game's step");
}

void TestCensus() {
    J::Census census;
    constexpr u64 kCalls = 400000;
    std::atomic<bool> done{false};
    J::CensusCounts taken;
    std::thread frame([&] {
        while (!done.load(std::memory_order_acquire)) {
            const J::CensusCounts c = census.Take();
            taken.total += c.total;
            taken.covered += c.covered;
            taken.fell_back += c.fell_back;
            taken.used += c.used;
        }
    });
    for (u64 i = 0; i < kCalls; ++i) census.Call(i % 7 != 0, i % 3 == 0);
    done.store(true, std::memory_order_release);
    frame.join();
    const J::CensusCounts rest = census.Take();
    const u64 want_covered = kCalls - (kCalls + 6) / 7, want_used = (kCalls + 2) / 3;
    Check(taken.total + rest.total == kCalls, "every step call counted while the frame thread takes the census");
    Check(taken.covered + rest.covered == want_covered && taken.fell_back + rest.fell_back == kCalls - want_covered,
          "covered and fell back add up across takes");
    Check(taken.used + rest.used == want_used, "every sweep that took jobs counted across takes");
    const J::CensusCounts empty = census.Take();
    Check(!empty.total && !empty.covered && !empty.fell_back && !empty.used, "a take leaves the census at zero");
}

void TestRunStep() {
    {
        Arena a(4);
        ScriptOps ops{&a};
        ops.own_count_after_step = 3;
        const J::Outcome o = J::RunStep(ops, a.At(0));
        Check(!o.swept && !o.took, "jobs left in its own deque: no sweep");
        Check(ops.log.size() == 1 && ops.log[0] == Call{'g', a.At(0), 0}, "the game's step runs once, first");
    }
    {
        Arena a(4);
        ScriptOps ops{&a};
        ops.steal_results = {false, true};
        const J::Outcome o = J::RunStep(ops, a.At(0));
        Check(o.swept && o.took, "its own deque empty: one sweep that took jobs");
        std::vector<Call> want{{'g', a.At(0), 0}, {'t', kFarTsc, 0}};
        const auto sweep = GameSweepOrder(a, 0, kFarTsc, ops.steal_results);
        want.insert(want.end(), sweep.begin(), sweep.end());
        Check(ops.log == want, "the game's step, one time stamp, then the game's sweep");
    }
    {
        Arena a(1);
        ScriptOps ops{&a};
        const J::Outcome o = J::RunStep(ops, a.At(0));
        Check(o.swept && !o.took && ops.log.size() == 2, "one participant: nothing to steal from");
    }
}

struct Job {
    std::atomic<int> runs{0};
    int parts = 1;
    std::atomic<int> runner{-1};
    int waits_for = -1;
    int work = 0;
    bool nested = false;
};

class Model {
public:
    Model(u32 n)
        : n_(n), arena_(n), deques_(n), locks_(std::make_unique<std::mutex[]>(n)), steal_locks_(std::make_unique<std::mutex[]>(n)) {}
    u32 Count() const { return n_; }
    u64 At(u32 i) const { return arena_.At(i); }
    u32 IndexOfDeque(u64 deque) const { return u32((deque - L::kDeque - arena_.base) / L::kParticipantBytes); }
    u32 IndexOf(u64 self) const { return u32((self - arena_.base) / L::kParticipantBytes); }
    u64 OwnCount(u32 i) const { return Arena::Get64(At(i) + L::kDequeCount); }
    void SetState(u32 i, u32 s) { __atomic_store_n(reinterpret_cast<u32*>(At(i) + kStateAt), s, __ATOMIC_RELEASE); }
    u32 State(u32 i) const { return __atomic_load_n(reinterpret_cast<const u32*>(At(i) + kStateAt), __ATOMIC_ACQUIRE); }
    void Push(u32 i, Job* j) {
        const std::scoped_lock lock(locks_[i]);
        deques_[i].push_back({j, j->parts});
        Sync(i);
    }
    bool Pop(u32 i, Job*& out) {
        std::unique_lock lock(locks_[i]);
        if (deques_[i].empty()) return false;
        Entry& front = deques_[i].front();
        out = front.job;
        if (front.parts > 1) {
            --front.parts;
            return true;
        }
        if (ModelIs(ModelMutant::PopRemovesAfterUnlock)) {
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
            if (deques_[i].empty()) return true;
        }
        deques_[i].pop_front();
        Sync(i);
        return true;
    }
    bool Steal(u32 victim, u32 thief) {
        steals_.fetch_add(1, std::memory_order_relaxed);
        const std::scoped_lock steal_lock(steal_locks_[thief]);
        if (OwnCount(thief) != 0) return false;
        if (victim == thief) return false;
        std::unique_lock victim_lock(locks_[victim]);
        if (deques_[victim].empty()) return false;
        std::unique_lock thief_lock(locks_[thief]);
        auto& from = deques_[victim];
        auto& to = deques_[thief];
        if (from.size() == 1 && from.front().parts > 1) {
            const int half = from.front().parts / 2;
            from.front().parts -= half;
            to.push_back({from.front().job, half});
        } else {
            const std::size_t take = (from.size() + 1) / 2;
            for (std::size_t k = 0; k < take; ++k) to.push_back(from[k]);
            if (ModelIs(ModelMutant::StealMovesAfterUnlock)) {
                Sync(thief);
                thief_lock.unlock();
                victim_lock.unlock();
                std::this_thread::yield();
                victim_lock.lock();
            }
            for (std::size_t k = 0; k < take && !from.empty(); ++k) from.pop_front();
        }
        Sync(thief);
        Sync(victim);
        return true;
    }
    bool AllEmpty() {
        for (u32 i = 0; i < n_; ++i) {
            const std::scoped_lock lock(locks_[i]);
            if (!deques_[i].empty() || OwnCount(i) != 0) return false;
        }
        return true;
    }
    std::atomic<u64> steals_{0};

private:
    struct Entry {
        Job* job;
        int parts;
    };
    void Sync(u32 i) { Arena::Put64(At(i) + L::kDequeCount, deques_[i].size()); }
    u32 n_;
    Arena arena_;
    std::vector<std::deque<Entry>> deques_;
    std::unique_ptr<std::mutex[]> locks_;
    std::unique_ptr<std::mutex[]> steal_locks_;
};

struct Rng {
    u64 s;
    u64 Next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

std::atomic<bool> g_hung{false};
std::vector<Job>* g_jobs = nullptr;

bool GameSweep(Model& m, u32 self, u64 tsc) {
    const u32 n = m.Count();
    const u64 start = (u64(self) + tsc) % n;
    for (u32 i = 0; i < n; ++i) {
        const u32 at = u32(start + i) % n;
        if (at != self && (m.Steal(at, self) || m.OwnCount(self) != 0)) return true;
    }
    return false;
}

void RunJob(Model& m, Job* job, u32 index, void (*nested_step)(Model&, Rng&), Rng& rng);

void GameStep(Model& m, u32 self, Rng& rng, void (*nested_step)(Model&, Rng&)) {
    Job* job = nullptr;
    if (!m.Pop(self, job) && GameSweep(m, self, rng.Next())) m.Pop(self, job);
    if (job) RunJob(m, job, self, nested_step, rng);
}

struct ModelOps {
    Model* m;
    Rng* rng;
    void (*nested_step)(Model&, Rng&);
    u64 Load64(u64 at) const { return Arena::Get64(at); }
    u32 Load32(u64 at) const { return Arena::Get32(at); }
    bool Steal(u64 victim, u64 thief) { return m->Steal(m->IndexOfDeque(victim), m->IndexOfDeque(thief)); }
    void RunGameStep(u64 self) { GameStep(*m, m->IndexOf(self), *rng, nested_step); }
    u64 Tsc() { return rng->Next(); }
};

std::atomic<u64> g_main_jobs{0};

void OriginalStep(Model& m, Rng& rng);
void OursStep(Model& m, Rng& rng) {
    ModelOps ops{&m, &rng, &OursStep};
    J::RunStep(ops, m.At(0));
}
void OriginalStep(Model& m, Rng& rng) {
    GameStep(m, 0, rng, &OriginalStep);
}

std::atomic<int> g_flip{0};
void FlipStep(Model& m, Rng& rng) {
    if (g_flip.load(std::memory_order_acquire) == 1) {
        ModelOps ops{&m, &rng, &FlipStep};
        J::RunStep(ops, m.At(0));
    } else {
        GameStep(m, 0, rng, &FlipStep);
    }
}

void RunJob(Model& m, Job* job, u32 index, void (*nested_step)(Model&, Rng&), Rng& rng) {
    job->runs.fetch_add(1, std::memory_order_acq_rel);
    job->runner.store(int(index), std::memory_order_relaxed);
    if (index == 0) g_main_jobs.fetch_add(1, std::memory_order_relaxed);
    if (job->waits_for >= 0) {
        const auto until = Clock::now() + kHangBound;
        while ((*g_jobs)[std::size_t(job->waits_for)].runs.load(std::memory_order_acquire) == 0) {
            if (Clock::now() > until) {
                g_hung.store(true);
                return;
            }
            std::this_thread::yield();
        }
    }
    volatile u64 sink = 0;
    for (int k = 0; k < job->work; ++k) sink = sink + u64(k);
    if (job->nested && index == 0) {
        nested_step(m, rng);
        while (m.OwnCount(0) != 0) nested_step(m, rng);
    }
}

class Workers {
public:
    explicit Workers(Model& m) : m_(m) {
        for (u32 i = 1; i < m.Count(); ++i) threads_.emplace_back([this, i] { Loop(i); });
    }
    ~Workers() {
        shutdown_.store(true, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        generation_.notify_all();
        for (auto& t : threads_) t.join();
    }
    void Kick() {
        idle_.store(0, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        generation_.notify_all();
    }
    bool WaitIdle() {
        const auto until = Clock::now() + kHangBound;
        while (idle_.load(std::memory_order_acquire) < m_.Count() - 1)
            if (Clock::now() > until) return false;
        for (u32 i = 1; i < m_.Count(); ++i)
            while (m_.State(i) != kIdle)
                if (Clock::now() > until) return false;
        return true;
    }

private:
    void Loop(u32 i) {
        Rng rng{0x9e3779b97f4a7c15ull * (i + 1)};
        u64 seen = 0;
        for (;;) {
            generation_.wait(seen, std::memory_order_acquire);
            seen = generation_.load(std::memory_order_acquire);
            if (shutdown_.load(std::memory_order_acquire)) return;
            m_.SetState(i, kRunning);
            for (;;) {
                Job* job = nullptr;
                while (m_.Pop(i, job)) RunJob(m_, job, i, nullptr, rng);
                if (!GameSweep(m_, i, rng.Next())) break;
            }
            m_.SetState(i, kIdle);
            idle_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    Model& m_;
    std::vector<std::thread> threads_;
    std::atomic<u64> generation_{0};
    std::atomic<u32> idle_{0};
    std::atomic<bool> shutdown_{false};
};

struct RoundStats {
    u64 spin_ns = 0;
    u64 main_jobs = 0;
    u64 jobs = 0;
};

bool Round(Model& m, Workers& w, std::vector<Job>& jobs, void (*step)(Model&, Rng&), Rng& rng, RoundStats& stats) {
    for (std::size_t j = 0; j < jobs.size(); ++j) m.Push(0, &jobs[j]);
    const u64 main_before = g_main_jobs.load();
    w.Kick();
    const auto until = Clock::now() + kHangBound;
    do step(m, rng);
    while (m.OwnCount(0) != 0 && Clock::now() < until);
    if (m.OwnCount(0) != 0) g_hung.store(true);
    const auto spin_start = Clock::now();
    const bool idle = w.WaitIdle();
    stats.spin_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - spin_start).count());
    stats.main_jobs += g_main_jobs.load() - main_before;
    stats.jobs += jobs.size();
    if (!idle || g_hung.load()) return false;
    bool once = m.AllEmpty();
    for (const Job& j : jobs) once = once && j.runs.load() == j.parts && j.runner.load() >= 0 && u32(j.runner.load()) < m.Count();
    return once;
}

std::vector<Job> MakeJobs(Rng& rng, std::size_t count, u32 n, int heavy) {
    std::vector<Job> jobs(count);
    for (std::size_t j = 0; j < count; ++j) {
        jobs[j].work = int(rng.Next() % 2000) + (heavy ? 20000 : 0);
        if (j >= n && rng.Next() % 6 == 0) jobs[j].waits_for = int(j - n);
        jobs[j].nested = rng.Next() % 17 == 0;
        jobs[j].parts = rng.Next() % 5 == 0 ? int(rng.Next() % 6) + 2 : 1;
    }
    return jobs;
}

bool Stress(const char* name, void (*step)(Model&, Rng&), int rounds, RoundStats& total) {
    bool ok = true;
    for (u32 n : {1u, 2u, 4u, 7u}) {
        Model m(n);
        Workers w(m);
        Rng rng{0x21b9980ull + n};
        for (int r = 0; r < rounds && ok; ++r) {
            const std::size_t count = r % 9 == 0 ? 0 : std::size_t(rng.Next() % 96);
            std::vector<Job> jobs = MakeJobs(rng, count, n, r % 11 == 5);
            g_jobs = &jobs;
            RoundStats s;
            if (!Round(m, w, jobs, step, rng, s)) {
                std::printf("FAILED: %s stress, %u participants, round %d: a job did not run exactly once, a deque kept "
                            "jobs, or the barrier did not end\n",
                            name, n, r);
                ok = false;
            }
            if (n > 1) {
                total.spin_ns += s.spin_ns;
                total.main_jobs += s.main_jobs;
                total.jobs += s.jobs;
            }
        }
    }
    return ok;
}

void TestStress(int rounds) {
    RoundStats game, ours;
    Check(Stress("game's step", &OriginalStep, rounds, game), "the game's step model keeps every job once");
    Check(Stress("our step", &OursStep, rounds, ours), "our step keeps every job once, with the same barrier");
    RoundStats flipped;
    std::atomic<bool> done{false};
    std::atomic<u64> flips{0};
    std::thread flipper([&] {
        while (!done.load(std::memory_order_acquire)) {
            g_flip.store(g_flip.load(std::memory_order_relaxed) ^ 1, std::memory_order_release);
            flips.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    const bool flip_ok = Stress("flipping step", &FlipStep, rounds, flipped);
    done.store(true, std::memory_order_release);
    flipper.join();
    Check(flip_ok, "a switch flipped at any time while main is inside the barrier keeps every job once and ends the barrier");
    Check(flips.load() > u64(rounds), "the switch flipped during the flipping stress");
    if (!g_quiet)
        std::printf("main-jobs-barrier-test: model stress (measured on the model, not the game): game's step: main ran "
                    "%llu of %llu jobs, spin %.1f us/round; ours: main ran %llu of %llu jobs, spin %.1f us/round\n",
                    (unsigned long long)game.main_jobs, (unsigned long long)game.jobs, game.spin_ns / 1e3 / (rounds * 3.0),
                    (unsigned long long)ours.main_jobs, (unsigned long long)ours.jobs, ours.spin_ns / 1e3 / (rounds * 3.0));
}

[[maybe_unused]] void Bench() {
    constexpr int kTurns = 2000000;
    constexpr int kSweeps = 200000;
    Arena a(7);
    ScriptOps unused{&a};
    u64 sink = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < kTurns; ++i) {
        const J::Shape s = J::ReadShape(unused, a.At(0));
        sink += u64(J::Choose(true, J::CheckShape(s)));
        __asm__ volatile("" : : "r"(sink) : "memory");
    }
    auto t1 = Clock::now();
    const double check_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kTurns;
    Model m(7);
    Rng rng{42};
    ModelOps ops{&m, &rng, &OursStep};
    t0 = Clock::now();
    for (int i = 0; i < kSweeps; ++i) sink += J::SweepOthers(ops, m.At(0), rng.Next());
    t1 = Clock::now();
    const double sweep_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kSweeps;
    std::printf("main-jobs-barrier-test: microbench (measured, this machine, -O2, not the game; not a frame-time result): read and check of the job system's shape "
                "%.1f ns a step; one empty sweep of 6 other deques (model steal: two uncontended std::mutex) %.1f ns; "
                "sink %llu\n",
                check_ns, sweep_ns, (unsigned long long)(sink & 1));
}

int RunAll(int rounds) {
    g_failed = 0;
    g_hung.store(false);
    TestShape();
    TestNullCaller();
    TestCensus();
    TestChooseAndLatch();
    TestStart();
    TestSweep();
    TestRunStep();
    if (rounds > 0) TestStress(rounds);
    return g_failed;
}
}

int main(int argc, char** argv) {
#ifdef LANTERN_MAIN_JOBS_BARRIER_MUTANTS
    if (argc > 1 && !std::strcmp(argv[1], "--mutants")) {
        g_quiet = true;
        kHangBound = std::chrono::seconds{3};
        if (RunAll(kMutantRounds)) {
            std::printf("main-jobs-barrier mutants: the unmutated run failed\n");
            return 1;
        }
        int survivors = 0;
        for (int m = 1; m < int(J::Mutant::kCount); ++m) {
            J::g_mutant = m;
            const int found = RunAll(kMutantRounds);
            if (!found) {
                std::printf("main-jobs-barrier mutants: mutant %d survived\n", m);
                ++survivors;
            } else {
                std::printf("main-jobs-barrier mutants: mutant %d caught by %d checks\n", m, found);
            }
        }
        J::g_mutant = 0;
        for (int m = 1; m < int(ModelMutant::kCount); ++m) {
            g_model_mutant = ModelMutant(m);
            const int found = RunAll(kMutantRounds);
            if (!found) {
                std::printf("main-jobs-barrier mutants: model concurrency mutant %d survived the threaded stress\n", m);
                ++survivors;
            } else {
                std::printf("main-jobs-barrier mutants: model concurrency mutant %d caught by %d checks\n", m, found);
            }
        }
        g_model_mutant = ModelMutant::None;
        if (survivors) return 1;
        std::printf("main-jobs-barrier mutants: every mutant caught (%d, %d of them concurrency mutants of the model, each under a %d-round threaded stress)\n",
                    int(J::Mutant::kCount) - 1 + int(ModelMutant::kCount) - 1, int(ModelMutant::kCount) - 1, kMutantRounds);
        return 0;
    }
    std::printf("main-jobs-barrier-test: the mutant build runs with --mutants\n");
    return 1;
#else
    const bool bench = argc > 1 && !std::strcmp(argv[1], "--bench");
    const int failed = RunAll(120);
    if (bench) Bench();
    std::printf("main-jobs-barrier-test: %s (%d failures)\n", failed ? "FAILED" : "passed", failed);
    return failed ? 1 : 0;
#endif
}
