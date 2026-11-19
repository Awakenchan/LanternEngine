// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_jobs.h"

#include "hook_entries.h"
#include "job_lock.h"
#include "node_cache.h"
#include "../lantern_steps.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <span>
#include <x86intrin.h>

namespace BbEngine::MainJobs {
namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

constexpr i32 kPanicLine = 0xa9;
constexpr u32 kPublishEvery = 4096;
constexpr auto kReportEvery = std::chrono::seconds(60);
constexpr u64 kFnvBasis = 0xcbf29ce484222325ull;
constexpr u64 kFnvPrime = 0x100000001b3ull;

inline constexpr std::size_t kTlsReadBytes = 9;

struct BodyRange {
    std::size_t start;
    std::size_t end;
    u64 hash;
    const char* name;
    std::array<std::size_t, 2> tls_reads{};
};
constexpr BodyRange kBodies[] = {
    {kLock, 0x20369a8, 0xd42072c375950c3dull, "Lock"},
    {kTryLock, 0x2036a37, 0x29a3d110e9f8030eull, "TryLock"},
    {kUnlock, 0x2036ad4, 0xe9182e976eb081f7ull, "Unlock"},
    {kProcessOne, 0x2032531, 0x0089e985d5e53c89ull, "ProcessOne"},
    {kAppend, 0x2032869, 0xf71b95942c0aef94ull, "Append", {0x20327cc, 0x203283d}},
    {kGeneralAlloc, 0x1fc5f95, 0x9ab1a342656f33a9ull, "the general allocator's alloc"},
    {kGeneralFree, 0x1fc600f, 0x9e4edb1beacc7fefull, "the general allocator's free"},
};
static_assert(std::size(kBodies) == std::size_t(Body::GeneralFree) + 1, "one range per body");

enum class CacheState : u32 { Free, Claimed, InUse };

struct alignas(64) Cache {
    u64 vtable = 0;
    u64 heap = 0;
    u64 general = 0;
    u64 pool = 0;
    u64 sentinel = 0;
    u64 top = 0;
    std::atomic<u32> kept{0};
    std::atomic<u32> state{u32(CacheState::Free)};
    std::array<u64, kAllocatorSlots> slots{};
};
static_assert(offsetof(Cache, vtable) == 0 && offsetof(Cache, heap) == 8, "the game reads the stand-in's vtable and heap");

struct Tally {
    std::array<u64, kCounters> n;
    u32 since;
};

struct State {
    Game game{};
    bool bound = false;
    bool lock_installed = false;
    bool cache_installed = false;
    std::atomic<int> lock_switch{0};
    std::atomic<int> cache_switch{0};
    bool lock_step_was = false;
    bool cache_step_was = false;
    u64 alloc_stub = 0;
    u64 free_stub = 0;
    std::array<std::atomic<u64>, kCounters> totals{};
    bool lock_was_on = false;
    bool cache_was_on = false;
    std::chrono::steady_clock::time_point last_report{};
    u64 broken_shown = 0;
};
State g;
Cache g_caches[NodeCache::kCaches];
constinit thread_local Tally t_tally{};

void Publish(Tally& t) {
    for (std::size_t i = 0; i < kCounters; ++i) {
        if (t.n[i]) g.totals[i].fetch_add(t.n[i], std::memory_order_relaxed);
        t.n[i] = 0;
    }
    t.since = 0;
}

void Count(Counter c) {
    Tally& t = t_tally;
    ++t.n[c];
    if (++t.since >= kPublishEvery) Publish(t);
}

void CountNow(Counter c, u64 n = 1) {
    g.totals[c].fetch_add(n, std::memory_order_relaxed);
}

template <typename T> T Peek(u64 at) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof value);
    return value;
}

template <typename T> void Poke(u64 at, T value) {
    std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value);
}

std::atomic_ref<u64> WordAt(u64 at) {
    return std::atomic_ref<u64>(*reinterpret_cast<u64*>(at));
}

u64 Self() {
    return u64(Call(g.game.self));
}

bool IsAdaptiveMutex(u64 m) {
    if (!m) return false;
    const u64 vtable = Peek<u64>(m);
    for (const u64 v : g.game.mutex_vtables)
        if (vtable == v) return true;
    return false;
}

void Sleep(u64 m) {
    const i32 result = i32(Call(g.game.wait_sema, Peek<u64>(m + kMutexSema), 1, 0));
    if (result != 0) Call(g.game.panic, g.game.panic_file, kPanicLine, g.game.panic_text);
}

Counter TakeLock(u64 m) {
    using namespace JobLock;
    const u64 self = Self();
    std::atomic_ref<u64> word = WordAt(m + kMutexWord);
    u64 old = word.load(std::memory_order_relaxed);
    u64 start = 0;
    bool timed = false;
    for (;;) {
        const bool within = !timed || WithinBudget(__rdtsc(), start, Peek<i32>(m + kMutexBudget));
        for (;;) {
            const Turn turn = Decide(old, self, within);
            if (turn.next == Next::Watch) break;
            const u64 seen = old;
            if (!word.compare_exchange_weak(old, turn.want, std::memory_order_acquire, std::memory_order_relaxed)) continue;
            if (turn.next == Next::CountIn) {
                Sleep(m);
                return LockSlept;
            }
            if (Taken(turn.want, self)) return timed ? LockSpun : (seen & kOwnerMask) == self ? LockAgain : LockFirst;
            old = word.load(std::memory_order_relaxed);
            break;
        }
        if (!timed) {
            start = __rdtsc();
            timed = true;
            continue;
        }
        _mm_pause();
        old = word.load(std::memory_order_relaxed);
    }
}

std::int64_t __attribute__((sysv_abi)) OnLock(std::uint64_t, std::uint64_t* saved) noexcept {
    if (g.lock_switch.load(std::memory_order_relaxed) == 0) return 0;
    const u64 m = saved[Saved::kRdi];
    if (!IsAdaptiveMutex(m)) {
        Count(LockPassed);
        return 0;
    }
    Count(TakeLock(m));
    saved[Saved::kResult] = 0;
    return 1;
}

Cache* CacheOf(u64 object) {
    const auto first = reinterpret_cast<u64>(&g_caches[0]);
    const auto end = reinterpret_cast<u64>(&g_caches[NodeCache::kCaches]);
    if (object < first || object >= end || (object - first) % sizeof(Cache)) return nullptr;
    return reinterpret_cast<Cache*>(object);
}

bool PoolLockHeld(const Cache& c) {
    return c.pool != 0 && JobLock::HeldBy(WordAt(c.pool + kPoolQueueLock + kMutexWord).load(std::memory_order_relaxed), Self());
}

std::int64_t __attribute__((sysv_abi)) OnNodeAlloc(std::uint64_t, std::uint64_t* saved) noexcept {
    Cache* c = CacheOf(saved[Saved::kRdi]);
    if (!c) return 0;
    const bool held = PoolLockHeld(*c);
    const u64 top = held ? c->top : 0;
    if (NodeCache::TakeOnAlloc(saved[Saved::kRsi], saved[Saved::kRdx], c->kept.load(std::memory_order_relaxed), top, held)) {
        const u64 node = c->top;
        c->top = Peek<u64>(node);
        c->kept.fetch_sub(1, std::memory_order_relaxed);
        Count(AllocHit);
        saved[Saved::kResult] = node;
        return 1;
    }
    Count(held ? AllocPassed : NotHeld);
    saved[Saved::kRdi] = c->general;
    return 0;
}

void GiveBack(Cache& c) {
    u64 given = 0;
    const u32 kept = c.kept.load(std::memory_order_relaxed);
    for (u32 n = kept; n != 0 && c.top; --n) {
        const u64 node = c.top;
        c.top = Peek<u64>(node);
        Call(g.game.general_free, c.general, node);
        ++given;
    }
    if (given != kept) CountNow(BrokenLists);
    CountNow(NodesGivenBack, given);
    c.top = 0;
    c.kept.store(0, std::memory_order_relaxed);
}

std::int64_t __attribute__((sysv_abi)) OnNodeFree(std::uint64_t, std::uint64_t* saved) noexcept {
    Cache* c = CacheOf(saved[Saved::kRdi]);
    if (!c) return 0;
    const u64 p = saved[Saved::kRsi];
    const u64 general = c->general;
    const bool sentinel = p != 0 && p == c->sentinel;
    const bool held = !sentinel && PoolLockHeld(*c);
    switch (NodeCache::OnFree(p, c->sentinel, c->kept.load(std::memory_order_relaxed), g.cache_switch.load(std::memory_order_relaxed) != 0, held)) {
    case NodeCache::Free::Keep:
        Poke<u64>(p, c->top);
        c->top = p;
        c->kept.fetch_add(1, std::memory_order_relaxed);
        Count(FreeKept);
        saved[Saved::kResult] = 0;
        return 1;
    case NodeCache::Free::End:
        GiveBack(*c);
        c->sentinel = 0;
        c->pool = 0;
        CountNow(PoolsEnded);
        c->state.store(u32(CacheState::Free), std::memory_order_release);
        break;
    case NodeCache::Free::Pass:
        Count(held ? FreePassed : NotHeld);
        break;
    }
    saved[Saved::kRdi] = general;
    return 0;
}

NodeCache::PoolFacts FactsOf(u64 pool) {
    NodeCache::PoolFacts f{};
    f.slot = WordAt(pool + kPoolAllocator).load(std::memory_order_relaxed);
    f.general = WordAt(g.game.general_slot).load(std::memory_order_relaxed);
    if (f.general) {
        f.general_vtable = Peek<u64>(f.general);
        if (f.general_vtable == g.game.general_vtable) {
            f.alloc = Peek<u64>(f.general_vtable + 8 * kAllocSlot);
            f.free = Peek<u64>(f.general_vtable + 8 * kFreeSlot);
        }
    }
    f.lock_vtable = Peek<u64>(pool + kPoolQueueLock);
    f.sentinel = Peek<u64>(pool + kPoolSentinel);
    return f;
}

void Adopt(u64 pool, const NodeCache::PoolFacts& f) {
    for (Cache& c : g_caches) {
        u32 free = u32(CacheState::Free);
        if (!c.state.compare_exchange_strong(free, u32(CacheState::Claimed), std::memory_order_acquire, std::memory_order_relaxed))
            continue;
        for (std::size_t i = 0; i < kAllocatorSlots; ++i) c.slots[i] = Peek<u64>(f.general_vtable + 8 * i);
        c.slots[kAllocSlot] = g.alloc_stub;
        c.slots[kFreeSlot] = g.free_stub;
        c.vtable = reinterpret_cast<u64>(c.slots.data());
        c.heap = Peek<u64>(f.general + 8);
        c.general = f.general;
        c.pool = pool;
        c.sentinel = f.sentinel;
        c.top = 0;
        c.kept.store(0, std::memory_order_relaxed);
        c.state.store(u32(CacheState::InUse), std::memory_order_release);
        u64 expected = f.general;
        if (WordAt(pool + kPoolAllocator)
                .compare_exchange_strong(expected, reinterpret_cast<u64>(&c), std::memory_order_acq_rel, std::memory_order_relaxed)) {
            CountNow(PoolsAdopted);
            return;
        }
        c.pool = 0;
        c.sentinel = 0;
        c.state.store(u32(CacheState::Free), std::memory_order_release);
        return;
    }
    CountNow(PoolsRefused);
}

std::int64_t __attribute__((sysv_abi)) OnPop(std::uint64_t, std::uint64_t* saved) noexcept {
    if (g.cache_switch.load(std::memory_order_relaxed) == 0) return 0;
    const u64 pool = saved[Saved::kRdi];
    if (!pool) return 0;
    const u64 slot = WordAt(pool + kPoolAllocator).load(std::memory_order_relaxed);
    if (slot == 0 || slot != WordAt(g.game.general_slot).load(std::memory_order_relaxed)) return 0;
    const NodeCache::PoolFacts f = FactsOf(pool);
    const NodeCache::Expected e{g.game.general_vtable, g.game.general_alloc, g.game.general_free, g.game.queue_lock_vtable};
    if (NodeCache::Adopt(f, e)) Adopt(pool, f);
    return 0;
}

bool TlsReadAt(const std::uint8_t* site) {
    return site[0] == 0x64 || site[0] == 0x65 || site[0] == 0xe9;
}

u64 BodyHash(const std::uint8_t* image, const BodyRange& r) {
    u64 h = kFnvBasis;
    for (std::size_t at = r.start; at < r.end; ++at) {
        bool masked = false;
        for (const std::size_t tls : r.tls_reads) masked = masked || (tls && at >= tls && at < tls + kTlsReadBytes);
        if (!masked) h = (h ^ image[at]) * kFnvPrime;
    }
    return h;
}

const char* FirstChanged(const std::uint8_t* image, std::size_t size, std::initializer_list<Body> bodies) {
    for (const Body b : bodies)
        if (!BodyIs109(image, size, b)) return kBodies[std::size_t(b)].name;
    return nullptr;
}

bool HookEntry(std::uint8_t* image, std::size_t size, std::size_t offset, std::span<const std::uint8_t> entry, Callback callback,
               const char* what) {
    BbEngine::Hook hook;
    if (!Prepare(hook, image, size, offset, SiteKind::Entry, entry, callback)) {
        Lantern::Log(Lantern::LogChannel::Always, "Engine: %s off: its entry at %#zx is not 1.09's (or no memory for the hook)", what,
                     offset);
        return false;
    }
    if (const WriteResult result = Commit(std::span<BbEngine::Hook>(&hook, 1)); result != WriteResult::Applied) {
        Lantern::Log(Lantern::LogChannel::Always, "Engine: %s off: its hook at %#zx was %s", what, offset, Describe(result));
        return false;
    }
    return true;
}

bool WantedAtStart(Lantern::StepId id) {
    return Lantern::Steps::Get(id) != Lantern::Mode::Off;
}

bool InstallLock(std::uint8_t* image, std::size_t size, const char* changed) {
    if (!WantedAtStart(Lantern::StepId::job_lock)) {
        Lantern::Log(Lantern::LogChannel::Always,
                     "Engine: Job lock: the game's own (LANTERN_JOB_LOCK=0); it can be turned on only at start");
        return false;
    }
    if (changed) {
        Lantern::Steps::SetUnavailable(Lantern::StepId::job_lock, "the game's lock code is not 1.09's");
        Lantern::Log(Lantern::LogChannel::Always, "Engine: Job lock unavailable: the game's %s is not 1.09's", changed);
        return false;
    }
    if (!HookEntry(image, size, kLock, HookEntries::kMainJobsLockEntry, OnLock, "Job lock")) {
        Lantern::Steps::SetUnavailable(Lantern::StepId::job_lock, "the hook could not be written");
        return false;
    }
    Lantern::Log(Lantern::LogChannel::Always,
                 "Engine: Job lock: on (LANTERN_JOB_LOCK=1): the adaptive mutex spins with pause on a plain load and swaps only "
                 "to take the word or to count in");
    return true;
}

bool InstallCache(std::uint8_t* image, std::size_t size, const char* changed) {
    if (!WantedAtStart(Lantern::StepId::job_node_cache)) {
        Lantern::Log(Lantern::LogChannel::Always,
                     "Engine: Job node cache: the game's own (LANTERN_JOB_NODE_CACHE=0); it can be turned on only at start");
        return false;
    }
    if (changed) {
        Lantern::Steps::SetUnavailable(Lantern::StepId::job_node_cache, "the game's job pool code is not 1.09's");
        Lantern::Log(Lantern::LogChannel::Always, "Engine: Job node cache unavailable: the game's %s is not 1.09's", changed);
        return false;
    }
    if (!HookEntry(image, size, kProcessOne, HookEntries::kMainJobsProcessOneEntry, OnPop, "Job node cache")) {
        Lantern::Steps::SetUnavailable(Lantern::StepId::job_node_cache, "the hook could not be written");
        return false;
    }
    Lantern::Log(Lantern::LogChannel::Always,
                 "Engine: Job node cache: on (LANTERN_JOB_NODE_CACHE=1): each work pool keeps up to %u of its job list nodes "
                 "(%d pools)",
                 NodeCache::kCap, NodeCache::kCaches);
    return true;
}

void SayChange(const char* what, bool on) {
    Lantern::Log(Lantern::LogChannel::Always, "Engine: %s: turned %s at frame %llu", what, on ? "on" : "off",
                 static_cast<unsigned long long>(Lantern::g_frame.load(std::memory_order_relaxed)));
}

void ReportLines() {
    const Totals t = Read();
    if (g.lock_installed)
        Lantern::Log(Lantern::LogChannel::Always,
                     "Engine: Job lock: %llu taken at once, %llu re-taken by their owner, %llu after spinning, %llu after sleeping, "
                     "%llu left to the game's lock (not an adaptive mutex)",
                     static_cast<unsigned long long>(t.n[LockFirst]), static_cast<unsigned long long>(t.n[LockAgain]),
                     static_cast<unsigned long long>(t.n[LockSpun]), static_cast<unsigned long long>(t.n[LockSlept]),
                     static_cast<unsigned long long>(t.n[LockPassed]));
    if (g.cache_installed)
        Lantern::Log(Lantern::LogChannel::Always,
                     "Engine: Job node cache: %llu pools (%u live, holding %llu nodes; %llu ended; %llu found every cache taken); "
                     "allocs %llu from a cache, %llu passed on; frees %llu kept, %llu passed on; %llu calls without the pool's "
                     "lock; %llu nodes given back, %llu lists broken",
                     static_cast<unsigned long long>(t.n[PoolsAdopted]), t.live, static_cast<unsigned long long>(t.kept_now),
                     static_cast<unsigned long long>(t.n[PoolsEnded]), static_cast<unsigned long long>(t.n[PoolsRefused]),
                     static_cast<unsigned long long>(t.n[AllocHit]), static_cast<unsigned long long>(t.n[AllocPassed]),
                     static_cast<unsigned long long>(t.n[FreeKept]), static_cast<unsigned long long>(t.n[FreePassed]),
                     static_cast<unsigned long long>(t.n[NotHeld]), static_cast<unsigned long long>(t.n[NodesGivenBack]),
                     static_cast<unsigned long long>(t.n[BrokenLists]));
}
}

Game GameAt(const std::uint8_t* image) {
    const auto at = [image](std::size_t offset) { return reinterpret_cast<u64>(image + offset); };
    Game game;
    game.self = at(kSelfStub);
    game.wait_sema = at(kWaitSemaStub);
    game.panic = at(kPanic);
    game.panic_file = at(kPanicFile);
    game.panic_text = at(kPanicText);
    for (std::size_t i = 0; i < std::size(kMutexVtables); ++i) game.mutex_vtables[i] = at(kMutexVtables[i]);
    game.general_slot = at(kGeneralAllocatorSlot);
    game.general_vtable = at(kGeneralVtable);
    game.general_alloc = at(kGeneralAlloc);
    game.general_free = at(kGeneralFree);
    game.queue_lock_vtable = at(kQueueLockVtable);
    return game;
}

bool BodyIs109(const std::uint8_t* image, std::size_t size, Body body) {
    const BodyRange& r = kBodies[std::size_t(body)];
    if (!image || r.end > size) return false;
    for (const std::size_t tls : r.tls_reads)
        if (tls && !TlsReadAt(image + tls)) return false;
    return BodyHash(image, r) == r.hash;
}

bool Bind(const Game& game) {
    if (g.bound) return g.game.self == game.self;
    g.game = game;
    const auto alloc = reinterpret_cast<u64>(TailStub(OnNodeAlloc, game.general_alloc));
    const auto free = reinterpret_cast<u64>(TailStub(OnNodeFree, game.general_free));
    if (!alloc || !free) {
        FreeStub(reinterpret_cast<std::uint8_t*>(alloc));
        FreeStub(reinterpret_cast<std::uint8_t*>(free));
        return false;
    }
    g.alloc_stub = alloc;
    g.free_stub = free;
    g.bound = true;
    return true;
}

void SetActive(bool lock, bool cache) {
    g.lock_switch.store(lock, std::memory_order_relaxed);
    g.cache_switch.store(cache, std::memory_order_relaxed);
}

std::atomic<int>* LockSwitch() {
    return &g.lock_switch;
}

std::atomic<int>* CacheSwitch() {
    return &g.cache_switch;
}

Callback LockCallback() {
    return OnLock;
}

Callback PopCallback() {
    return OnPop;
}

void FlushThread() {
    Publish(t_tally);
}

Totals Read() {
    Totals t;
    for (std::size_t i = 0; i < kCounters; ++i) t.n[i] = g.totals[i].load(std::memory_order_relaxed);
    for (const Cache& c : g_caches) {
        if (c.state.load(std::memory_order_acquire) != u32(CacheState::InUse)) continue;
        ++t.live;
        t.kept_now += c.kept.load(std::memory_order_relaxed);
    }
    return t;
}

bool Install(std::uint8_t* image, std::size_t size) {
    Lantern::Steps::StartOnce();
    const bool lock_wanted = WantedAtStart(Lantern::StepId::job_lock);
    const bool cache_wanted = WantedAtStart(Lantern::StepId::job_node_cache);
    if ((lock_wanted || cache_wanted) && !Bind(GameAt(image))) {
        Lantern::Log(Lantern::LogChannel::Always, "Engine: Job lock and node cache off: no memory for the allocator stubs");
        return false;
    }
    const char* lock_changed = FirstChanged(image, size, {Body::Lock, Body::TryLock, Body::Unlock});
    const char* cache_changed = FirstChanged(
        image, size, {Body::ProcessOne, Body::Append, Body::GeneralAlloc, Body::GeneralFree, Body::Lock, Body::TryLock, Body::Unlock});
    g.lock_installed = InstallLock(image, size, lock_changed);
    g.cache_installed = InstallCache(image, size, cache_changed);
    SetActive(g.lock_installed, g.cache_installed);
    g.lock_was_on = g.lock_installed;
    g.cache_was_on = g.cache_installed;
    g.lock_step_was = g.lock_installed;
    g.cache_step_was = g.cache_installed;
    g.last_report = std::chrono::steady_clock::now();
    return g.lock_installed || g.cache_installed;
}

void Tick() {
    if (!g.lock_installed && !g.cache_installed) return;
    const bool lock_step = g.lock_installed && Lantern::Steps::Get(Lantern::StepId::job_lock) != Lantern::Mode::Off;
    const bool cache_step = g.cache_installed && Lantern::Steps::Get(Lantern::StepId::job_node_cache) != Lantern::Mode::Off;
    FollowStep(g.lock_switch, lock_step, g.lock_step_was);
    FollowStep(g.cache_switch, cache_step, g.cache_step_was);
    const bool lock = g.lock_installed && g.lock_switch.load(std::memory_order_relaxed) != 0;
    const bool cache = g.cache_installed && g.cache_switch.load(std::memory_order_relaxed) != 0;
    if (lock != g.lock_was_on) SayChange("Job lock", lock);
    if (cache != g.cache_was_on) SayChange("Job node cache", cache);
    g.lock_was_on = lock;
    g.cache_was_on = cache;
    FlushThread();
    const u64 broken = g.totals[BrokenLists].load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (now - g.last_report < kReportEvery && broken == g.broken_shown) return;
    g.last_report = now;
    g.broken_shown = broken;
    ReportLines();
}
}
