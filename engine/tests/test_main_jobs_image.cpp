// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/job_lock.h"
#include "gpu/shim/engine/main_jobs.h"
#include "gpu/shim/engine/node_cache.h"
#include "tests/eboot_image.h"
#include "tools/engine/sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <immintrin.h>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
namespace MJ = BbEngine::MainJobs;
namespace JL = BbEngine::JobLock;
namespace NC = BbEngine::NodeCache;
#define SYSV __attribute__((sysv_abi))

constexpr const char* kTest = "main-jobs-image-test";
constexpr const char* kImageSwitch = "BB_BOOT_INSTALL_IMAGE";
constexpr std::size_t kJobRunSlot = 0x10 / 8;
constexpr u32 kJobKindNothing = 2;
constexpr i32 kGameBudget = 200000;
constexpr std::uint8_t kAbsoluteJump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};

int g_failures = 0;
void Fail(const std::string& what) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}
void Expect(bool ok, const std::string& what) {
    if (!ok) Fail(what);
}

void SetEnv(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

std::atomic<u64> g_next_self{0x2001};
thread_local u64 t_self = 0;
SYSV u64 TestSelf() {
    if (!t_self) t_self = g_next_self.fetch_add(1);
    return t_self;
}

struct Sema {
    std::mutex m;
    std::condition_variable cv;
    long count = 0;
};
constexpr int kSemas = 64;
Sema g_semas[kSemas];
std::atomic<int> g_next_sema{1};

SYSV i32 TestWait(u64 id, i32 need, u64) {
    Sema& s = g_semas[id % kSemas];
    std::unique_lock lock(s.m);
    s.cv.wait(lock, [&] { return s.count >= need; });
    s.count -= need;
    return 0;
}
SYSV i32 TestSignal(u64 id, i32 count) {
    Sema& s = g_semas[id % kSemas];
    {
        std::scoped_lock lock(s.m);
        s.count += count;
    }
    s.cv.notify_all();
    return 0;
}
SYSV void TestPanic(u64, i32 line, u64) {
    std::printf("FAIL: the game's panic was called (line %d)\n", line);
    std::fflush(stdout);
    std::abort();
}

u64 g_stack_guard = 0x5eed5eed5eed5eedull;

struct alignas(16) GeneralObject {
    u64 vtable = 0;
    u64 heap = 0x4ea9;
};
GeneralObject g_general;

std::mutex g_heap_lock;
std::set<u64> g_heap;
std::atomic<long> g_heap_faults{0};
std::atomic<long> g_wrong_this{0};
std::atomic<u64> g_heap_calls{0};

SYSV u64 TestAlloc(u64 self, u64 size, u64) {
    if (self != reinterpret_cast<u64>(&g_general)) g_wrong_this.fetch_add(1);
    void* p = std::calloc(1, size < NC::kNodeSize ? NC::kNodeSize : size);
    std::scoped_lock lock(g_heap_lock);
    g_heap_calls.fetch_add(1, std::memory_order_relaxed);
    g_heap.insert(reinterpret_cast<u64>(p));
    return reinterpret_cast<u64>(p);
}

SYSV u64 TestFree(u64 self, u64 p) {
    if (self != reinterpret_cast<u64>(&g_general)) g_wrong_this.fetch_add(1);
    if (!p) return 0;
    {
        std::scoped_lock lock(g_heap_lock);
        g_heap_calls.fetch_add(1, std::memory_order_relaxed);
        if (!g_heap.erase(p)) {
            g_heap_faults.fetch_add(1);
            return 0;
        }
    }
    std::free(reinterpret_cast<void*>(p));
    return 0;
}

std::size_t HeapBlocks() {
    std::scoped_lock lock(g_heap_lock);
    return g_heap.size();
}

struct Image {
    std::uint8_t* base = nullptr;
    std::size_t size = 0;
    u64 At(std::size_t offset) const { return reinterpret_cast<u64>(base + offset); }
};

template <typename T> void Poke(u64 at, T value) {
    std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value);
}
template <typename T> T Peek(u64 at) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof value);
    return value;
}

void JumpAt(const Image& image, std::size_t offset, const void* to) {
    std::memcpy(image.base + offset, kAbsoluteJump, sizeof kAbsoluteJump);
    const u64 target = reinterpret_cast<u64>(to);
    std::memcpy(image.base + offset + sizeof kAbsoluteJump, &target, sizeof target);
}

bool MapOne(const std::vector<std::uint8_t>& elf, Image& image) {
    image.base = EbootImage::MapImage(elf, image.size, true);
    if (!image.base || !EbootImage::Relocate(elf, image.base, image.size)) return false;
    Poke<u64>(image.At(MJ::kSelfGot), reinterpret_cast<u64>(&TestSelf));
    Poke<u64>(image.At(MJ::kWaitSemaGot), reinterpret_cast<u64>(&TestWait));
    Poke<u64>(image.At(MJ::kSignalSemaGot), reinterpret_cast<u64>(&TestSignal));
    Poke<u64>(image.At(MJ::kStackGuardGot), reinterpret_cast<u64>(&g_stack_guard));
    JumpAt(image, MJ::kPanic, reinterpret_cast<const void*>(&TestPanic));
    return true;
}

Image g_game;
Image g_ours;

void BodiesAcceptLoaderTls() {
    const std::size_t end = 0x2032869;
    std::vector<std::uint8_t> copy(g_game.base, g_game.base + end);
    const auto ok = [&] { return MJ::BodyIs109(copy.data(), copy.size(), MJ::Body::Append); };
    Expect(ok(), "the raw 1.09 Append passes");
    for (const std::size_t site : {std::size_t(0x20327cc), std::size_t(0x203283d)}) {
        copy[site] = 0x65;
        Expect(ok(), "Append passes with the loader's gs prefix at " + std::to_string(site));
        const std::uint8_t jump[9] = {0xe9, 0x11, 0x22, 0x33, 0x44, 0x90, 0x90, 0x90, 0x90};
        std::memcpy(copy.data() + site, jump, sizeof jump);
        Expect(ok(), "Append passes with the loader's TLS jump at " + std::to_string(site));
        copy[site] = 0xcc;
        Expect(!ok(), "Append passes with a foreign byte where a TLS read was");
        std::memcpy(copy.data() + site, g_game.base + site, 9);
    }
    copy[0x2032800] ^= 0xff;
    Expect(!ok(), "Append passes with a changed byte outside the TLS reads");
}

using LockFn = u64(SYSV*)(u64, u64);
using UnlockFn = u64(SYSV*)(u64);
using PopFn = u64(SYSV*)(u64);
using EnqueueFn = void(SYSV*)(u64, u64);
using FreeFn = u64(SYSV*)(u64, u64);

struct Mutex {
    u64 vtable = 0;
    i32 budget = 0;
    i32 pad = 0;
    u64 sema = 0;
    u64 word = 0;
};
static_assert(offsetof(Mutex, budget) == MJ::kMutexBudget && offsetof(Mutex, sema) == MJ::kMutexSema &&
              offsetof(Mutex, word) == MJ::kMutexWord);

Mutex NewMutex(i32 budget) {
    Mutex m;
    m.vtable = g_ours.At(MJ::kQueueLockVtable);
    m.budget = budget;
    m.sema = u64(g_next_sema.fetch_add(1));
    return m;
}

u64 Word(Mutex& m) {
    return std::atomic_ref<u64>(m.word).load();
}

void OursLock(u64 m) {
    reinterpret_cast<LockFn>(Peek<u64>(Peek<u64>(m) + MJ::kMutexLockSlot))(m, 0xffffffffu);
}
void GameLock(u64 m) {
    reinterpret_cast<LockFn>(g_game.At(MJ::kLock))(m, 0xffffffffu);
}
void GameUnlock(u64 m) {
    reinterpret_cast<UnlockFn>(g_game.At(MJ::kUnlock))(m);
}

void LockSingle() {
    Mutex m = NewMutex(kGameBudget);
    const u64 at = reinterpret_cast<u64>(&m);
    const u64 self = TestSelf();
    OursLock(at);
    Expect(Word(m) == self, "the hooked Lock takes a free word for the caller");
    OursLock(at);
    Expect(Word(m) == self, "the hooked Lock lets the owner take it again");
    GameUnlock(at);
    Expect(Word(m) == 0, "the game's Unlock frees it");
    std::atomic_ref<u64>(m.word).store(2 * JL::kOneSleeper);
    OursLock(at);
    Expect(Word(m) == (2 * JL::kOneSleeper | self), "the hooked Lock keeps the sleepers' count");
    std::atomic_ref<u64>(m.word).store(0);
}

void LockStress(i32 budget, int iterations) {
    constexpr int kThreads = 8;
    Mutex m = NewMutex(budget);
    const u64 at = reinterpret_cast<u64>(&m);
    long counter = 0;
    std::atomic<u64> holder{0};
    std::atomic<long> overlaps{0};
    std::atomic<bool> stop{false};
    std::thread flipper([&] {
        bool on = true;
        while (!stop.load()) {
            on = !on;
            MJ::SetActive(on, true);
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        MJ::SetActive(true, true);
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            std::mt19937 random(unsigned(t + 7));
            for (int i = 0; i < iterations; ++i) {
                if (random() & 1) OursLock(at);
                else GameLock(at);
                u64 nobody = 0;
                if (!holder.compare_exchange_strong(nobody, TestSelf())) overlaps.fetch_add(1);
                ++counter;
                holder.store(0);
                GameUnlock(at);
            }
            MJ::FlushThread();
        });
    for (auto& th : threads) th.join();
    stop.store(true);
    flipper.join();
    const std::string at_budget = "budget " + std::to_string(budget) + ": ";
    Expect(counter == long(kThreads) * iterations, at_budget + "the counter is exact (" + std::to_string(counter) + ")");
    Expect(overlaps.load() == 0, at_budget + "no two holders (" + std::to_string(overlaps.load()) + ")");
    Expect(Word(m) == 0, at_budget + "the word is free at the end");
}

struct Job {
    u64 vtable = 0;
    u64 group = 0;
    u32 kind = kJobKindNothing;
    u32 id = 0;
};
std::vector<std::atomic<int>>* g_runs = nullptr;
SYSV u64 TestRun(u64 job) {
    (*g_runs)[Peek<u32>(job + offsetof(Job, id))].fetch_add(1);
    return 0;
}
SYSV u64 TestJobOther(u64) {
    std::printf("FAIL: a job method other than run was called\n");
    std::abort();
}
std::array<u64, 4> g_job_vtable{};

struct Pool {
    alignas(16) std::array<u64, 0xe8 / 8> words{};
    u64 Base() { return reinterpret_cast<u64>(words.data()); }
    u64 Allocator() { return std::atomic_ref<u64>(words[MJ::kPoolAllocator / 8]).load(); }
    u64 Sentinel() const { return words[MJ::kPoolSentinel / 8]; }
};

void MakePool(Pool& p, i32 budget) {
    p.words.fill(0);
    const u64 sentinel = TestAlloc(reinterpret_cast<u64>(&g_general), NC::kNodeSize, 8);
    Poke<u64>(sentinel, sentinel);
    Poke<u64>(sentinel + 8, sentinel);
    p.words[MJ::kPoolSentinel / 8] = sentinel;
    p.words[MJ::kPoolAllocator / 8] = reinterpret_cast<u64>(&g_general);
    Mutex m = NewMutex(budget);
    std::memcpy(p.words.data() + MJ::kPoolQueueLock / 8, &m, sizeof m);
}

void EndPool(Pool& p) {
    const u64 allocator = p.Allocator();
    reinterpret_cast<FreeFn>(Peek<u64>(Peek<u64>(allocator) + 8 * MJ::kFreeSlot))(allocator, p.Sentinel());
}

void Enqueue(Pool& p, Job* job) {
    reinterpret_cast<EnqueueFn>(g_ours.At(MJ::kEnqueueUnwoken))(p.Base(), reinterpret_cast<u64>(job));
}
u64 PopWith(const Image& image, Pool& p) {
    return reinterpret_cast<PopFn>(image.At(MJ::kProcessOne))(p.Base());
}

struct PoolRun {
    double ms = 0;
    u64 heap_calls = 0;
    long wrong = 0;
};

PoolRun RunPool(int producers, int workers, u32 jobs_each, bool flip, bool mixed) {
    const std::size_t before = HeapBlocks();
    Pool p;
    MakePool(p, kGameBudget);
    const u32 total = u32(producers) * jobs_each;
    std::vector<Job> jobs(total + 1);
    for (u32 j = 1; j <= total; ++j) {
        jobs[j].vtable = reinterpret_cast<u64>(g_job_vtable.data());
        jobs[j].id = j;
    }
    std::vector<std::atomic<int>> runs(total + 1);
    g_runs = &runs;
    std::atomic<u32> done{0};
    std::atomic<bool> stop{false};
    const u64 heap0 = g_heap_calls.load();
    const auto start = std::chrono::steady_clock::now();
    std::thread flipper([&] {
        bool on = true;
        while (flip && !stop.load()) {
            on = !on;
            MJ::SetActive(true, on);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < producers; ++t)
        threads.emplace_back([&, t] {
            for (u32 j = 1; j <= jobs_each; ++j) Enqueue(p, &jobs[u32(t) * jobs_each + j]);
            MJ::FlushThread();
        });
    for (int t = 0; t < workers; ++t)
        threads.emplace_back([&, t] {
            std::mt19937 random(unsigned(300 + t));
            while (done.load(std::memory_order_relaxed) < total) {
                const Image& image = mixed && (random() & 1) ? g_game : g_ours;
                if (PopWith(image, p)) done.fetch_add(1, std::memory_order_relaxed);
                else std::this_thread::yield();
            }
            MJ::FlushThread();
        });
    for (auto& th : threads) th.join();
    stop.store(true);
    flipper.join();
    PoolRun r;
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    r.heap_calls = g_heap_calls.load() - heap0;
    for (u32 j = 1; j <= total; ++j) r.wrong += runs[j].load() != 1;
    EndPool(p);
    g_runs = nullptr;
    if (HeapBlocks() != before) Fail("the pool's blocks are not all back in the heap (" + std::to_string(HeapBlocks() - before) + " left)");
    return r;
}

void CacheStress() {
    const u64 adopted = MJ::Read().n[MJ::PoolsAdopted];
    const u64 hits = MJ::Read().n[MJ::AllocHit];
    MJ::SetActive(true, true);
    const PoolRun r = RunPool(3, 6, 20000, true, true);
    MJ::SetActive(true, true);
    MJ::FlushThread();
    const MJ::Totals t = MJ::Read();
    Expect(r.wrong == 0, "every job ran exactly once (" + std::to_string(r.wrong) + " wrong)");
    Expect(t.n[MJ::PoolsAdopted] == adopted + 1, "the game's ProcessOne gave the pool a cache");
    Expect(t.n[MJ::AllocHit] > hits, "the game's Append took nodes from the cache");
    Expect(t.live == 0, "the pool's end freed its cache");
    Expect(g_heap_faults.load() == 0, "no heap block freed twice or unknown");
    Expect(g_wrong_this.load() == 0, "the heap only ever met the general allocator");
}

bool SetUp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> elf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string sha = sha256_hex(elf.data(), elf.size());
    if (std::none_of(std::begin(EbootImage::kEbootSha), std::end(EbootImage::kEbootSha), [&](const char* s) { return sha == s; })) {
        std::printf("%s: SKIPPED: %s is not the 1.09 image (sha256 %s)\n", kTest, path, sha.c_str());
        return false;
    }
    if (!MapOne(elf, g_game) || !MapOne(elf, g_ours)) {
        Fail("the image could not be mapped twice");
        return false;
    }
    if (!MJ::Install(g_ours.base, g_ours.size)) {
        Fail("MainJobs::Install wrote nothing on the 1.09 image");
        return false;
    }
    Expect(g_ours.base[MJ::kLock] == 0xff && g_ours.base[MJ::kProcessOne] == 0xff, "both entries are hooked");
    for (const Image* image : {&g_game, &g_ours}) {
        JumpAt(*image, MJ::kGeneralAlloc, reinterpret_cast<const void*>(&TestAlloc));
        JumpAt(*image, MJ::kGeneralFree, reinterpret_cast<const void*>(&TestFree));
    }
    g_general.vtable = g_ours.At(MJ::kGeneralVtable);
    Poke<u64>(g_ours.At(MJ::kGeneralAllocatorSlot), reinterpret_cast<u64>(&g_general));
    Poke<u64>(g_game.At(MJ::kGeneralAllocatorSlot), reinterpret_cast<u64>(&g_general));
    for (u64& slot : g_job_vtable) slot = reinterpret_cast<u64>(&TestJobOther);
    g_job_vtable[kJobRunSlot] = reinterpret_cast<u64>(&TestRun);
    return true;
}

template <typename F> double NsPer(int n, F&& body) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) body();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / n;
}

void BenchUncontended() {
    constexpr int kPairs = 5000000;
    Mutex m = NewMutex(kGameBudget);
    const u64 at = reinterpret_cast<u64>(&m);
    const char* names[] = {"game's Lock", "Lantern's Lock", "hook, part off"};
    double best[3] = {1e30, 1e30, 1e30};
    const int order[] = {0, 1, 2, 2, 1, 0, 0, 1, 2};
    for (int which : order) {
        MJ::SetActive(which != 2, true);
        const double ns = NsPer(kPairs, [&] {
            if (which == 0) GameLock(at);
            else OursLock(at);
            GameUnlock(at);
        });
        std::printf("bench lock uncontended: %-16s %6.2f ns a Lock+Unlock pair (measured, %d pairs, one thread)\n", names[which],
                    ns, kPairs);
        best[which] = std::min(best[which], ns);
    }
    MJ::SetActive(true, true);
    std::printf("bench lock uncontended, best of 3: game %.2f, Lantern %.2f, hook with the part off %.2f ns (measured)\n", best[0],
                best[1], best[2]);
}

void SpinWork(int n) {
    for (int i = 0; i < n; ++i) _mm_pause();
}

void BenchContended() {
    constexpr int kOps = 100000;
    constexpr int kInside = 20, kOutside = 40;
    for (const int threads : {2, 4, 8}) {
        for (const int which : {0, 1, 1, 0}) {
            MJ::SetActive(true, true);
            Mutex m = NewMutex(kGameBudget);
            const u64 at = reinterpret_cast<u64>(&m);
            long counter = 0;
            const auto start = std::chrono::steady_clock::now();
            std::vector<std::thread> pool;
            for (int t = 0; t < threads; ++t)
                pool.emplace_back([&] {
                    for (int i = 0; i < kOps; ++i) {
                        if (which == 0) GameLock(at);
                        else OursLock(at);
                        ++counter;
                        SpinWork(kInside);
                        GameUnlock(at);
                        SpinWork(kOutside);
                    }
                    MJ::FlushThread();
                });
            for (auto& th : pool) th.join();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::printf("bench lock contended: %d threads, %s: %.1f ms for %d ops each (%.0f ns an op; %d pauses inside, %d "
                        "outside; measured)%s\n",
                        threads, which == 0 ? "game's Lock   " : "Lantern's Lock", ms, kOps, ms * 1e6 / (double(threads) * kOps),
                        kInside, kOutside, counter == long(threads) * kOps ? "" : " COUNT WRONG");
        }
    }
}

void BenchPool() {
    constexpr int kProducers = 1, kWorkers = 6;
    constexpr u32 kJobs = 100000;
    struct Config {
        const char* name;
        bool lock;
        bool cache;
    };
    const Config configs[] = {{"game's lock, no cache    ", false, false},
                              {"Lantern's lock, no cache ", true, false},
                              {"Lantern's lock and cache ", true, true},
                              {"Lantern's lock and cache ", true, true},
                              {"Lantern's lock, no cache ", true, false},
                              {"game's lock, no cache    ", false, false}};
    for (const Config& c : configs) {
        MJ::SetActive(c.lock, c.cache);
        const PoolRun r = RunPool(kProducers, kWorkers, kJobs, false, false);
        std::printf("bench pool: %s %.1f ms for %u jobs (%.0f ns a job), %llu heap calls (measured, %d producer, %d workers, "
                    "the test heap takes one mutex)%s\n",
                    c.name, r.ms, kJobs, r.ms * 1e6 / kJobs, static_cast<unsigned long long>(r.heap_calls), kProducers, kWorkers,
                    r.wrong ? " JOBS WRONG" : "");
    }
    MJ::SetActive(true, true);
}

}

int main(int argc, char** argv) {
    SetEnv("LANTERN_JOB_LOCK", "1");
    SetEnv("LANTERN_JOB_NODE_CACHE", "1");
    const bool bench = argc == 2 && !std::strcmp(argv[1], "--bench");
    const char* path = std::getenv(kImageSwitch);
    if (!path || !*path) {
        std::printf("%s: SKIPPED: %s is not set (the path of the 1.09 eboot.elf, read only)\n", kTest, kImageSwitch);
        return 0;
    }
    if (!SetUp(path)) return g_failures ? 1 : 0;
    if (bench) {
        std::printf("%s: bench on %u hardware threads; report only (rule T3)\n", kTest, std::thread::hardware_concurrency());
        BenchUncontended();
        BenchContended();
        BenchPool();
        return g_failures ? 1 : 0;
    }
    BodiesAcceptLoaderTls();
    LockSingle();
    LockStress(0, 3000);
    LockStress(2000, 20000);
    LockStress(kGameBudget, 20000);
    CacheStress();
    const MJ::Totals t = MJ::Read();
    std::printf("%s: lock %llu at once, %llu again, %llu spun, %llu slept, %llu passed; cache %llu hits, %llu kept, %llu passed, "
                "%llu without the lock, %llu given back\n",
                kTest, static_cast<unsigned long long>(t.n[MJ::LockFirst]), static_cast<unsigned long long>(t.n[MJ::LockAgain]),
                static_cast<unsigned long long>(t.n[MJ::LockSpun]), static_cast<unsigned long long>(t.n[MJ::LockSlept]),
                static_cast<unsigned long long>(t.n[MJ::LockPassed]), static_cast<unsigned long long>(t.n[MJ::AllocHit]),
                static_cast<unsigned long long>(t.n[MJ::FreeKept]), static_cast<unsigned long long>(t.n[MJ::FreePassed]),
                static_cast<unsigned long long>(t.n[MJ::NotHeld]), static_cast<unsigned long long>(t.n[MJ::NodesGivenBack]));
    if (g_failures) {
        std::printf("%s: %d failures\n", kTest, g_failures);
        return 1;
    }
    std::printf("%s: ok\n", kTest);
    return 0;
}
