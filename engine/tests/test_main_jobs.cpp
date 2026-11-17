// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/job_lock.h"
#include "gpu/shim/engine/main_jobs.h"
#include "gpu/shim/engine/node_cache.h"

#include <array>
#include <cstddef>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {

using u64 = std::uint64_t;
using i32 = std::int32_t;
namespace MJ = BbEngine::MainJobs;
namespace JL = BbEngine::JobLock;
namespace NC = BbEngine::NodeCache;
#define SYSV __attribute__((sysv_abi))

int g_failures = 0;
void Fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
}
void Expect(bool ok, const char* what) {
    if (!ok) Fail(what);
}

void PureLock() {
    constexpr u64 S = 0x1234;
    constexpr u64 kOther = 0x777;
    constexpr u64 kTwo = 2 * JL::kOneSleeper;
    Expect(JL::Decide(0, S, true).next == JL::Next::Take && JL::Decide(0, S, true).want == S, "a free word is taken");
    Expect(JL::Decide(kTwo, S, false).want == (kTwo | S), "a take keeps the sleepers' count");
    Expect(JL::Decide(S, S, true).next == JL::Next::Take && JL::Decide(S | kTwo, S, false).want == (S | kTwo),
           "the owner takes its own word again");
    Expect(JL::Decide(kOther, S, true).next == JL::Next::Watch && JL::Decide(kOther, S, true).want == kOther,
           "a held word within the budget is only watched");
    Expect(JL::Decide(kOther, S, false).next == JL::Next::CountIn && JL::Decide(kOther, S, false).want == kOther + JL::kOneSleeper,
           "a held word after the budget counts one sleeper in");
    Expect(JL::Decide(JL::kHandedOwner, S, false).next == JL::Next::CountIn, "a handed word is held for everyone else");
    Expect(!JL::WithinBudget(110, 100, 10), "the budget ends at start plus budget");
    Expect(JL::WithinBudget(109, 100, 10), "inside the budget");
    Expect(!JL::WithinBudget(101, 100, 0), "a budget of 0 sleeps at once");
    Expect(JL::Deadline(100, -10) == 90, "the budget is sign-extended");
    Expect(JL::Taken(S | kTwo, S) && !JL::Taken(JL::kHandedOwner, S), "taken means the owner field is self");
    Expect(JL::HeldBy(S | kTwo, S) && !JL::HeldBy(kOther, S), "held by");
}

void PureCache() {
    constexpr u64 kSentinel = 0x5000, kNode = 0x6000, kTop = 0x7000;
    Expect(NC::OnFree(kSentinel, kSentinel, 3, true, true) == NC::Free::End, "the sentinel's free ends the pool");
    Expect(NC::OnFree(kSentinel, kSentinel, 3, false, false) == NC::Free::End, "the end comes whatever the state");
    Expect(NC::OnFree(kNode, kSentinel, 0, true, true) == NC::Free::Keep, "a node is kept");
    Expect(NC::OnFree(kNode, kSentinel, 0, true, false) == NC::Free::Pass, "not kept without the pool's lock");
    Expect(NC::OnFree(kNode, kSentinel, NC::kCap, true, true) == NC::Free::Pass, "not kept past the cap");
    Expect(NC::OnFree(kNode, kSentinel, NC::kCap - 1, true, true) == NC::Free::Keep, "kept up to the cap");
    Expect(NC::OnFree(kNode, kSentinel, 0, false, true) == NC::Free::Pass, "not kept when off");
    Expect(NC::OnFree(0, kSentinel, 0, true, true) == NC::Free::Pass, "a null free passes");
    Expect(NC::TakeOnAlloc(NC::kNodeSize, 8, 1, kTop, true), "a node alloc takes a kept node");
    Expect(!NC::TakeOnAlloc(0x20, 8, 1, kTop, true), "another size passes");
    Expect(!NC::TakeOnAlloc(NC::kNodeSize, 16, 1, kTop, true), "a wider alignment passes");
    Expect(!NC::TakeOnAlloc(NC::kNodeSize, 8, 0, kTop, true), "an empty count passes");
    Expect(!NC::TakeOnAlloc(NC::kNodeSize, 8, 1, 0, true), "an empty list passes");
    Expect(!NC::TakeOnAlloc(NC::kNodeSize, 8, 1, kTop, false), "no take without the pool's lock");
    const NC::Expected e{0x10, 0x20, 0x30, 0x40};
    const NC::PoolFacts good{0x100, 0x100, 0x10, 0x20, 0x30, 0x40, kSentinel};
    Expect(NC::Adopt(good, e), "a pool on the general allocator is adopted");
    NC::PoolFacts f = good;
    f.slot = 0x200;
    Expect(!NC::Adopt(f, e), "a pool on another allocator is not adopted");
    f = good;
    f.general_vtable = 0x11;
    Expect(!NC::Adopt(f, e), "another allocator class is not adopted");
    f = good;
    f.free = 0x31;
    Expect(!NC::Adopt(f, e), "another free is not adopted");
    f = good;
    f.lock_vtable = 0x41;
    Expect(!NC::Adopt(f, e), "another lock class is not adopted");
    f = good;
    f.sentinel = 0;
    Expect(!NC::Adopt(f, e), "no sentinel, no adoption");
    f = good;
    f.general = f.slot = 0;
    Expect(!NC::Adopt(f, e), "no general allocator, no adoption");
}

std::atomic<u64> g_next_self{0x1001};
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
void TestSignal(u64 id) {
    Sema& s = g_semas[id % kSemas];
    {
        std::scoped_lock lock(s.m);
        ++s.count;
    }
    s.cv.notify_all();
}
SYSV void TestPanic(u64, i32 line, u64) {
    std::printf("FAIL: the game's panic was called (line %d)\n", line);
    std::fflush(stdout);
    std::abort();
}

std::array<u64, 4> g_mutex_vtables[3]{};

struct Mutex {
    u64 vtable = 0;
    i32 budget = 0;
    i32 pad = 0;
    u64 sema = 0;
    u64 word = 0;
};
static_assert(offsetof(Mutex, budget) == MJ::kMutexBudget && offsetof(Mutex, sema) == MJ::kMutexSema &&
              offsetof(Mutex, word) == MJ::kMutexWord);

std::atomic_ref<u64> Word(Mutex& m) {
    return std::atomic_ref<u64>(m.word);
}

void GameUnlock(Mutex& m) {
    u64 old = Word(m).load(std::memory_order_relaxed), want = 0;
    do {
        want = (old >> 40) == 0 ? old & JL::kSleepersMask : (old - JL::kOneSleeper) | JL::kOwnerMask;
    } while (!Word(m).compare_exchange_weak(old, want, std::memory_order_release, std::memory_order_relaxed));
    if (want & JL::kOwnerMask) TestSignal(m.sema);
}

u64 Ticks() {
    return u64(std::chrono::steady_clock::now().time_since_epoch().count());
}

void GameLock(Mutex& m) {
    const u64 self = TestSelf();
    const u64 start = Ticks();
    for (;;) {
        const u64 now = Ticks();
        const u64 until = start + u64(std::int64_t(m.budget));
        u64 old = Word(m).load(std::memory_order_relaxed), want = 0;
        do {
            const u64 owner = old & JL::kOwnerMask;
            if (owner == 0 || owner == self) want = (old & JL::kSleepersMask) | (self & JL::kOwnerMask);
            else want = now < until ? old : old + JL::kOneSleeper;
        } while (!Word(m).compare_exchange_weak(old, want, std::memory_order_acquire, std::memory_order_relaxed));
        if ((want & JL::kOwnerMask) == self) return;
        if (now >= until) {
            TestWait(m.sema, 1, 0);
            return;
        }
    }
}

bool OursLock(Mutex& m) {
    std::array<u64, 16> frame{};
    u64* saved = frame.data() + 4;
    saved[BbEngine::Saved::kRdi] = reinterpret_cast<u64>(&m);
    saved[BbEngine::Saved::kResult] = 0xdead;
    const bool ours = MJ::LockCallback()(0, saved) != 0;
    if (ours && saved[BbEngine::Saved::kResult] != 0) Fail("our Lock did not return 0");
    return ours;
}

void Take(Mutex& m, bool ours) {
    if (!(ours && OursLock(m))) GameLock(m);
}

Mutex NewMutex(i32 budget, int vtable = 0) {
    Mutex m;
    m.vtable = reinterpret_cast<u64>(g_mutex_vtables[vtable].data());
    m.budget = budget;
    m.sema = u64(g_next_sema.fetch_add(1));
    return m;
}

struct alignas(16) GeneralObject {
    u64 vtable = 0;
    u64 heap = 0x4ea9;
};
GeneralObject g_general;
GeneralObject g_other_allocator;
std::array<u64, MJ::kAllocatorSlots> g_general_vtable{};
u64 g_general_slot = 0;

std::mutex g_heap_lock;
std::set<u64> g_heap;
std::atomic<long> g_heap_faults{0};
std::atomic<long> g_wrong_this{0};

SYSV u64 TestAlloc(u64 self, u64 size, u64) {
    if (self != reinterpret_cast<u64>(&g_general) && self != reinterpret_cast<u64>(&g_other_allocator)) g_wrong_this.fetch_add(1);
    void* p = std::calloc(1, size < NC::kNodeSize ? NC::kNodeSize : size);
    std::scoped_lock lock(g_heap_lock);
    g_heap.insert(reinterpret_cast<u64>(p));
    return reinterpret_cast<u64>(p);
}

SYSV u64 TestFree(u64 self, u64 p) {
    if (self != reinterpret_cast<u64>(&g_general) && self != reinterpret_cast<u64>(&g_other_allocator)) g_wrong_this.fetch_add(1);
    if (!p) return 0;
    {
        std::scoped_lock lock(g_heap_lock);
        if (!g_heap.erase(p)) {
            g_heap_faults.fetch_add(1);
            return 0;
        }
    }
    std::free(reinterpret_cast<void*>(p));
    return 0;
}

SYSV u64 TestOtherSlot(u64) {
    std::printf("FAIL: an allocator slot other than alloc and free was called\n");
    std::abort();
}

std::size_t HeapBlocks() {
    std::scoped_lock lock(g_heap_lock);
    return g_heap.size();
}

using AllocFn = u64(SYSV*)(u64, u64, u64);
using FreeFn = u64(SYSV*)(u64, u64);

u64 Slot(u64 object, std::size_t slot) {
    u64 vtable = 0, fn = 0;
    std::memcpy(&vtable, reinterpret_cast<void*>(object), 8);
    std::memcpy(&fn, reinterpret_cast<void*>(vtable + 8 * slot), 8);
    return fn;
}
u64 Alloc(u64 allocator, u64 size, u64 align) {
    return reinterpret_cast<AllocFn>(Slot(allocator, MJ::kAllocSlot))(allocator, size, align);
}
void FreeNode(u64 allocator, u64 p) {
    reinterpret_cast<FreeFn>(Slot(allocator, MJ::kFreeSlot))(allocator, p);
}

struct Pool {
    std::array<u64, 0xe8 / 8> words{};
    u64 At(std::size_t offset) const { return words[offset / 8]; }
    void Set(std::size_t offset, u64 v) { words[offset / 8] = v; }
    u64 Base() { return reinterpret_cast<u64>(words.data()); }
    Mutex& Lock() { return *reinterpret_cast<Mutex*>(words.data() + MJ::kPoolQueueLock / 8); }
    u64 Allocator() { return std::atomic_ref<u64>(words[MJ::kPoolAllocator / 8]).load(); }
};

void MakePool(Pool& p, i32 budget, u64 allocator = reinterpret_cast<u64>(&g_general)) {
    p.words.fill(0);
    const u64 sentinel = TestAlloc(allocator, NC::kNodeSize, 8);
    std::memcpy(reinterpret_cast<void*>(sentinel), &sentinel, 8);
    std::memcpy(reinterpret_cast<void*>(sentinel + 8), &sentinel, 8);
    p.Set(MJ::kPoolSentinel, sentinel);
    p.Set(MJ::kPoolAllocator, allocator);
    Mutex m = NewMutex(budget);
    std::memcpy(&p.Lock(), &m, sizeof m);
}

u64 Next(u64 node) {
    u64 v;
    std::memcpy(&v, reinterpret_cast<void*>(node), 8);
    return v;
}
u64 Prev(u64 node) {
    u64 v;
    std::memcpy(&v, reinterpret_cast<void*>(node + 8), 8);
    return v;
}
void SetNext(u64 node, u64 v) { std::memcpy(reinterpret_cast<void*>(node), &v, 8); }
void SetPrev(u64 node, u64 v) { std::memcpy(reinterpret_cast<void*>(node + 8), &v, 8); }

void Push(Pool& p, u64 job, bool ours) {
    Take(p.Lock(), ours);
    const u64 sentinel = p.At(MJ::kPoolSentinel);
    const u64 node = Alloc(p.Allocator(), NC::kNodeSize, 8);
    const u64 last = Prev(sentinel);
    SetNext(node, sentinel);
    SetPrev(node, last);
    std::memcpy(reinterpret_cast<void*>(node + 16), &job, 8);
    SetNext(last, node);
    SetPrev(sentinel, node);
    p.Set(0x20, p.At(0x20) + 1);
    GameUnlock(p.Lock());
}

u64 Pop(Pool& p, bool ours) {
    std::array<u64, 16> frame{};
    u64* saved = frame.data() + 4;
    saved[BbEngine::Saved::kRdi] = p.Base();
    MJ::PopCallback()(0, saved);
    Take(p.Lock(), ours);
    u64 job = 0;
    if (p.At(0x20)) {
        const u64 sentinel = p.At(MJ::kPoolSentinel);
        const u64 first = Next(sentinel);
        if (first != sentinel) {
            std::memcpy(&job, reinterpret_cast<void*>(first + 16), 8);
            SetNext(Prev(first), Next(first));
            SetPrev(Next(first), Prev(first));
            FreeNode(p.Allocator(), first);
            p.Set(0x20, p.At(0x20) - 1);
        }
    }
    GameUnlock(p.Lock());
    return job;
}

void EndPool(Pool& p) {
    FreeNode(p.Allocator(), p.At(MJ::kPoolSentinel));
}

u64 Count(MJ::Counter c) {
    MJ::FlushThread();
    return MJ::Read().n[c];
}

void BindGame() {
    for (std::size_t i = 0; i < MJ::kAllocatorSlots; ++i) g_general_vtable[i] = reinterpret_cast<u64>(&TestOtherSlot);
    g_general_vtable[MJ::kAllocSlot] = reinterpret_cast<u64>(&TestAlloc);
    g_general_vtable[MJ::kFreeSlot] = reinterpret_cast<u64>(&TestFree);
    g_general.vtable = reinterpret_cast<u64>(g_general_vtable.data());
    g_other_allocator.vtable = g_general.vtable;
    g_general_slot = reinterpret_cast<u64>(&g_general);
    MJ::Game game;
    game.self = reinterpret_cast<u64>(&TestSelf);
    game.wait_sema = reinterpret_cast<u64>(&TestWait);
    game.panic = reinterpret_cast<u64>(&TestPanic);
    for (std::size_t i = 0; i < game.mutex_vtables.size(); ++i) game.mutex_vtables[i] = reinterpret_cast<u64>(g_mutex_vtables[i].data());
    game.general_slot = reinterpret_cast<u64>(&g_general_slot);
    game.general_vtable = reinterpret_cast<u64>(g_general_vtable.data());
    game.general_alloc = reinterpret_cast<u64>(&TestAlloc);
    game.general_free = reinterpret_cast<u64>(&TestFree);
    game.queue_lock_vtable = reinterpret_cast<u64>(g_mutex_vtables[0].data());
    if (!MJ::Bind(game)) Fail("Bind refused the test game");
    MJ::SetActive(true, true);
}

void LockSingle() {
    Mutex m = NewMutex(200000);
    const u64 self = TestSelf();
    Expect(OursLock(m), "our Lock takes a free word");
    Expect(Word(m).load() == self, "the word holds the owner");
    Expect(OursLock(m), "our Lock lets the owner take it again");
    Expect(Word(m).load() == self, "the re-take changes nothing");
    GameUnlock(m);
    Expect(Word(m).load() == 0, "one unlock frees a re-taken lock");
    Word(m).store(3 * JL::kOneSleeper);
    Expect(OursLock(m) && Word(m).load() == (3 * JL::kOneSleeper | self), "the sleepers' count is kept");
    Word(m).store(0);
    Mutex foreign = NewMutex(0);
    foreign.vtable = 0x1234;
    const u64 passed = Count(MJ::LockPassed);
    Expect(!OursLock(foreign), "a lock that is not an adaptive mutex goes to the game's Lock");
    Expect(Count(MJ::LockPassed) == passed + 1, "it is counted as passed");
    MJ::SetActive(false, true);
    Expect(!OursLock(m), "with the part off the game's Lock runs");
    MJ::SetActive(true, true);
}

void LockHanded() {
    Mutex m = NewMutex(0);
    Word(m).store(0x999);
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        Expect(OursLock(m), "our Lock sleeps on a held word");
        done.store(true);
    });
    while (Word(m).load() != (0x999 | JL::kOneSleeper)) std::this_thread::yield();
    Expect(!done.load(), "the waiter sleeps until the unlock");
    GameUnlock(m);
    waiter.join();
    Expect(done.load(), "the unlock hands the lock over");
    Expect(Word(m).load() == JL::kHandedOwner, "the woken thread owns a handed word");
    GameUnlock(m);
    Expect(Word(m).load() == 0, "the handed lock is freed by its owner's unlock");
}

void LockStress(i32 budget, int iterations) {
    constexpr int kThreads = 8;
    Mutex m = NewMutex(budget);
    long counter = 0;
    std::atomic<u64> holder{0};
    std::atomic<long> overlaps{0};
    std::atomic<bool> stop{false};
    std::thread flipper([&] {
        bool on = true;
        while (!stop.load()) {
            on = !on;
            MJ::SetActive(on, true);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        MJ::SetActive(true, true);
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            std::mt19937 random(unsigned(t + 1));
            for (int i = 0; i < iterations; ++i) {
                Take(m, (random() & 3) != 0);
                u64 nobody = 0;
                if (!holder.compare_exchange_strong(nobody, TestSelf())) overlaps.fetch_add(1);
                ++counter;
                holder.store(0);
                GameUnlock(m);
            }
            MJ::FlushThread();
        });
    for (auto& th : threads) th.join();
    stop.store(true);
    flipper.join();
    char what[128];
    std::snprintf(what, sizeof what, "budget %d: the counter is exact (%ld)", budget, counter);
    Expect(counter == long(kThreads) * iterations, what);
    std::snprintf(what, sizeof what, "budget %d: no two holders (%ld overlaps)", budget, overlaps.load());
    Expect(overlaps.load() == 0, what);
    std::snprintf(what, sizeof what, "budget %d: the word is free at the end (%llx)", budget,
                  static_cast<unsigned long long>(Word(m).load()));
    Expect(Word(m).load() == 0, what);
}

void CacheSingle() {
    const std::size_t before = HeapBlocks();
    Pool p;
    MakePool(p, 200000);
    const u64 adopted = Count(MJ::PoolsAdopted);
    Expect(Pop(p, true) == 0, "an empty pool pops nothing");
    Expect(p.Allocator() != reinterpret_cast<u64>(&g_general), "the pool's list allocator is a stand-in after a pop");
    Expect(Count(MJ::PoolsAdopted) == adopted + 1, "one pool adopted");
    Expect(Slot(p.Allocator(), 0) == Slot(reinterpret_cast<u64>(&g_general), 0), "the stand-in keeps the allocator's other slots");
    const u64 kept0 = Count(MJ::FreeKept), hit0 = Count(MJ::AllocHit);
    for (u64 j = 1; j <= 10; ++j) Push(p, j, true);
    for (u64 j = 1; j <= 10; ++j) Expect(Pop(p, true) == j, "jobs come out in order");
    Expect(Count(MJ::FreeKept) == kept0 + 10, "ten nodes kept");
    for (u64 j = 1; j <= 10; ++j) Push(p, j, false);
    Expect(Count(MJ::AllocHit) == hit0 + 10, "ten pushes from the cache, under the game's Lock too");
    for (u64 j = 1; j <= 10; ++j) Pop(p, true);
    const u64 passed0 = Count(MJ::FreePassed);
    for (u64 j = 1; j <= 1500; ++j) Push(p, j, true);
    for (u64 j = 1; j <= 1500; ++j) Pop(p, true);
    Expect(MJ::Read().kept_now == NC::kCap, "the cache stops at its cap");
    Expect(Count(MJ::FreePassed) == passed0 + (1500 - NC::kCap), "frees past the cap pass on");
    const u64 unheld = Count(MJ::NotHeld);
    const u64 loose = Alloc(p.Allocator(), NC::kNodeSize, 8);
    Expect(Count(MJ::NotHeld) == unheld + 1, "an alloc without the pool's lock passes on");
    FreeNode(p.Allocator(), loose);
    Expect(Count(MJ::NotHeld) == unheld + 2, "a free without the pool's lock passes on");
    Take(p.Lock(), true);
    const u64 wide = Alloc(p.Allocator(), 0x40, 8);
    FreeNode(p.Allocator(), wide);
    GameUnlock(p.Lock());
    MJ::SetActive(true, false);
    const u64 drained0 = Count(MJ::AllocHit);
    for (u64 j = 1; j <= 100; ++j) Push(p, j, true);
    for (u64 j = 1; j <= 100; ++j) Pop(p, true);
    Expect(Count(MJ::AllocHit) == drained0 + 100, "switched off, pushes still take kept nodes");
    Expect(MJ::Read().kept_now == NC::kCap - 100, "switched off, frees are not kept");
    MJ::SetActive(true, true);
    const u64 given0 = Count(MJ::NodesGivenBack);
    EndPool(p);
    Expect(Count(MJ::NodesGivenBack) == given0 + (NC::kCap - 100), "the pool's end gives every kept node back");
    Expect(MJ::Read().live == 0, "the pool's end frees its cache");
    Expect(HeapBlocks() == before, "every block back in the heap");
    Expect(g_heap_faults.load() == 0, "no block freed twice or unknown");
    Expect(g_wrong_this.load() == 0, "the heap is always called with the general allocator");
}

void CacheAdoption() {
    const std::size_t before = HeapBlocks();
    Pool other;
    MakePool(other, 200000, reinterpret_cast<u64>(&g_other_allocator));
    Pop(other, true);
    Expect(other.Allocator() == reinterpret_cast<u64>(&g_other_allocator), "a list on another allocator is not adopted");
    EndPool(other);
    Pool odd;
    MakePool(odd, 200000);
    odd.Lock().vtable = reinterpret_cast<u64>(g_mutex_vtables[1].data());
    Pop(odd, true);
    Expect(odd.Allocator() == reinterpret_cast<u64>(&g_general), "a pool whose lock is another class is not adopted");
    EndPool(odd);
    std::vector<Pool> pools(NC::kCaches + 1);
    const u64 refused = Count(MJ::PoolsRefused);
    for (Pool& p : pools) {
        MakePool(p, 2000);
        Pop(p, true);
    }
    Expect(Count(MJ::PoolsRefused) == refused + 1, "the pool past the last cache is refused");
    Expect(MJ::Read().live == NC::kCaches, "every cache in use");
    for (Pool& p : pools) EndPool(p);
    Expect(MJ::Read().live == 0, "every cache free after the pools end");
    Expect(HeapBlocks() == before, "every block back after the pools end");
}

void CacheStress() {
    constexpr int kProducers = 3, kWorkers = 6;
    constexpr u64 kJobs = 20000;
    const std::size_t before = HeapBlocks();
    Pool p;
    MakePool(p, 2000);
    std::vector<std::atomic<int>> runs(kProducers * kJobs + 1);
    std::atomic<u64> done{0};
    std::atomic<bool> stop{false};
    std::thread flipper([&] {
        bool on = true;
        while (!stop.load()) {
            on = !on;
            MJ::SetActive(true, on);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        MJ::SetActive(true, true);
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < kProducers; ++t)
        threads.emplace_back([&, t] {
            std::mt19937 random(unsigned(100 + t));
            for (u64 j = 1; j <= kJobs; ++j) Push(p, u64(t) * kJobs + j, (random() & 1) != 0);
            MJ::FlushThread();
        });
    for (int t = 0; t < kWorkers; ++t)
        threads.emplace_back([&, t] {
            std::mt19937 random(unsigned(200 + t));
            while (done.load() < kProducers * kJobs) {
                const u64 job = Pop(p, (random() & 3) != 0);
                if (!job) {
                    std::this_thread::yield();
                    continue;
                }
                runs[job].fetch_add(1);
                done.fetch_add(1);
            }
            MJ::FlushThread();
        });
    for (auto& th : threads) th.join();
    stop.store(true);
    flipper.join();
    long wrong = 0;
    for (std::size_t j = 1; j < runs.size(); ++j) wrong += runs[j].load() != 1;
    Expect(wrong == 0, "every job popped exactly once");
    Expect(p.At(0x20) == 0, "the list is empty");
    EndPool(p);
    Expect(HeapBlocks() == before, "every block back in the heap after the stress");
    Expect(g_heap_faults.load() == 0, "no block freed twice during the stress");
    Expect(g_wrong_this.load() == 0, "the heap always met the general allocator during the stress");
    Expect(MJ::Read().live == 0, "no cache left in use");
}

}

void PureSwitch() {
    struct Stores {
        int value = 0;
        int count = 0;
        void store(int v, std::memory_order) {
            value = v;
            ++count;
        }
    };
    const auto follow = [](int value, bool step, bool was) {
        Stores s{value, 0};
        MJ::FollowStep(s, step, was);
        return std::pair{s, was};
    };
    auto [on, on_was] = follow(0, true, false);
    Expect(on.value == 1 && on.count == 1 && on_was, "a step turned on from the menu turns the switch on");
    auto [off, off_was] = follow(1, false, true);
    Expect(off.value == 0 && off.count == 1 && !off_was, "a step turned off from the menu turns the switch off");
    auto [kept_off, kept_off_was] = follow(0, true, true);
    Expect(kept_off.value == 0 && kept_off.count == 0 && kept_off_was, "an unchanged step does not store over a flip to off");
    auto [kept_on, kept_on_was] = follow(1, false, false);
    Expect(kept_on.value == 1 && kept_on.count == 0 && !kept_on_was, "an unchanged step does not store over a flip to on");
    std::atomic<int> value{1};
    bool was = true;
    std::atomic<bool> flipped{false};
    std::thread meter([&] {
        value.store(0);
        flipped.store(true);
    });
    while (!flipped.load()) MJ::FollowStep(value, true, was);
    for (int frame = 0; frame < 100000; ++frame) MJ::FollowStep(value, true, was);
    meter.join();
    Expect(value.load() == 0, "frames with an unchanged step never undo the meter thread's flip");
    Expect(MJ::LockSwitch() != MJ::CacheSwitch() && MJ::LockSwitch() && MJ::CacheSwitch(), "each part has its own switch");
    MJ::SetActive(true, false);
    Expect(MJ::LockSwitch()->load() == 1 && MJ::CacheSwitch()->load() == 0, "the switches hold what SetActive wrote");
    MJ::SetActive(false, false);
}

int main(int argc, char** argv) {
    int wrong = 0;
#ifdef LANTERN_JOBS_WRONG
    if (argc == 2 && !std::strcmp(argv[1], "--count")) {
        std::printf("%d\n", JL::kLockWrongCount + NC::kCacheWrongCount + MJ::kSwitchWrongCount);
        return 0;
    }
    if (argc == 3 && !std::strcmp(argv[1], "--wrong")) wrong = std::atoi(argv[2]);
    JL::g_wrong = wrong;
#endif
    (void)argc;
    (void)argv;
    PureLock();
    PureCache();
    PureSwitch();
    if (wrong) {
        std::printf("main-jobs-test wrong %d: %d failures\n", wrong, g_failures);
        return g_failures ? 1 : 0;
    }
    BindGame();
    LockSingle();
    LockHanded();
    LockStress(0, 3000);
    LockStress(2000, 20000);
    LockStress(200000, 20000);
    CacheSingle();
    CacheAdoption();
    CacheStress();
    const MJ::Totals t = MJ::Read();
    std::printf("main-jobs-test: lock %llu at once, %llu again, %llu spun, %llu slept, %llu passed; cache %llu hits, %llu kept, "
                "%llu passed, %llu without the lock\n",
                static_cast<unsigned long long>(t.n[MJ::LockFirst]), static_cast<unsigned long long>(t.n[MJ::LockAgain]),
                static_cast<unsigned long long>(t.n[MJ::LockSpun]), static_cast<unsigned long long>(t.n[MJ::LockSlept]),
                static_cast<unsigned long long>(t.n[MJ::LockPassed]), static_cast<unsigned long long>(t.n[MJ::AllocHit]),
                static_cast<unsigned long long>(t.n[MJ::FreeKept]), static_cast<unsigned long long>(t.n[MJ::FreePassed]),
                static_cast<unsigned long long>(t.n[MJ::NotHeld]));
    if (g_failures) {
        std::printf("main-jobs-test: %d failures\n", g_failures);
        return 1;
    }
    std::printf("main-jobs-test: ok\n");
    return 0;
}
