// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "gpu/shim/lantern_steps.h"

extern "C" int lantern_mutant = 0;

namespace {
std::atomic<long> g_news{0};
std::atomic<bool> g_count_news{false};
}

void* operator new(std::size_t size) {
    if (g_count_news.load(std::memory_order_relaxed)) g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {
using namespace Lantern;

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct FakeEnv {
    std::vector<std::pair<std::string, std::string>> vars;
};

const char* FakeLookup(const char* name, void* context) {
    for (const auto& [n, v] : static_cast<FakeEnv*>(context)->vars) {
        if (n == name) return v.c_str();
    }
    return nullptr;
}

void Begin(std::vector<std::pair<std::string, std::string>> vars = {}) {
    static FakeEnv env;
    env.vars = std::move(vars);
    SwitchInput input;
    input.env = FakeLookup;
    input.context = &env;
    ReloadSwitches(input);
    Steps::ResetForTest();
    Steps::Start(Switches::Get());
    Steps::SetTicksPerSecond(1e9);
    Steps::OnFlip({1'000'000'000ull, false, 16'666'667ull});
}

std::vector<std::string> g_logged;
void Sink(std::string_view line, void*) {
    g_logged.emplace_back(line);
}

u32 Tid() {
#ifdef _WIN32
    return u32(GetCurrentThreadId());
#else
    return u32(syscall(SYS_gettid));
#endif
}

void NameThread(const char* name) {
#ifdef _WIN32
    wchar_t wide[32] = {};
    for (int i = 0; i < 31 && name[i]; ++i) wide[i] = wchar_t(name[i]);
    SetThreadDescription(GetCurrentThread(), wide);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

int FreeSlots() {
    int n = 0;
    for (const ThreadSlot& s : g_slots) n += u32(s.owner.load()) == kFreeTid;
    return n;
}

void Decisions() {
    Expect(Decide::MixHash(0) != Decide::kNoHash, "the draw hash is never the empty marker");
    int hits = 0;
    for (u64 d = 0; d < 1'024'000; ++d) hits += Decide::MaskHit(Decide::MixHash(d), 1023);
    Expect(hits > 900 && hits < 1100, "1 in 1024 draws hit the steady mask, within 10%");
    Decide::MinShare m{5, 4, 3, 60, 3};
    Expect(Decide::MinShareAllowed(m), "a step takes its minimum in its own frame of a new second");
    m.last_second = 5;
    Expect(!Decide::MinShareAllowed(m), "only once a second");
    m = {5, 4, 4, 60, 3};
    Expect(!Decide::MinShareAllowed(m), "not in another step's frame");
    Expect(Decide::SampleVerdict(true, false, false) == Decide::Verdict::Starved, "no tokens: starved, not checked");
    Expect(Decide::SampleVerdict(true, true, false) == Decide::Verdict::Check, "tokens and a hit: checked");
    Expect(Decide::SampleVerdict(false, false, true) == Decide::Verdict::Check, "the minimum share checks");
    const Decide::Buckets b = Decide::Refill({90, 90}, {20, 20, 100, true});
    Expect(b.first == 100 && b.rate == 100, "both buckets stop at the cap, the spill too");
    const Decide::Buckets w = Decide::Refill({0, 95}, {10, 10, 100, true});
    Expect(w.first == 100 && w.rate == 15, "a full first-hit bucket spills into the rate bucket");
    const Decide::Buckets nw = Decide::Refill({0, 95}, {10, 10, 100, false});
    Expect(nw.rate == 10, "no spill in the warm window");
    Expect(Decide::Spend(10, 1ull << 40) < 0, "a check larger than the bucket leaves it negative");
    Expect(Decide::Spend(INT64_MIN / 2 - 1, 1ull << 62) < 0, "a negative bucket never wraps to positive");
    for (int count = 1; count <= 20; ++count) {
        for (int n = 1; n <= 5; ++n) {
            for (int index = 0; index < count; ++index) {
                int owners = 0;
                for (int k = 1; k <= n; ++k) owners += Decide::InPart(index, count, k, n);
                if (owners != 1) {
                    Expect(false, "@k/n covers every on step exactly once");
                    return;
                }
            }
        }
    }
    Expect(Decide::InstrumentSuspect(false, 0, 5, 0, 0) == Decide::Suspect::NothingCovered, "nothing covered is suspect");
    Expect(Decide::InstrumentSuspect(true, 5, 5, 0, 0) == Decide::Suspect::CoveredButUnchecked,
           "covered and never checked is suspect in mode 1");
    Expect(Decide::InstrumentSuspect(true, 5, 5, 3, 3) == Decide::Suspect::AllDiffer, "every check differing is suspect");
    Expect(Decide::InstrumentSuspect(false, 5, 5, 0, 0) == Decide::Suspect::None, "count-only state checks nothing");
}

void Claims() {
    Begin();
    constexpr int kThreads = 64;
    constexpr int kUses = 10000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([] {
            ThreadSlot& slot = Steps::Slot();
            for (int i = 0; i < kUses; ++i) Steps::Used(slot, StepId::vertex_memo);
        });
    }
    for (auto& t : threads) t.join();
    Steps::Totals totals;
    Steps::Sum(totals);
    Expect(totals.used[std::size_t(StepId::vertex_memo)] == u64(kThreads) * kUses, "sums are exact after 64 threads");
    Expect(FreeSlots() == kMaxThreads, "every slot is free again after the threads end");
}

void Overflow() {
    Begin();
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    constexpr int kThreads = kMaxThreads + 2;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            ThreadSlot& slot = Steps::Slot();
            Steps::Used(slot, StepId::pipeline_memo);
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
        });
    }
    while (ready.load() < kThreads) std::this_thread::yield();
    const u64 without = g_threads_without_slot.load();
    go = true;
    for (auto& t : threads) t.join();
    Expect(without == 2, "the 129th and 130th threads go to the overflow slot and are counted");
    Steps::Totals totals;
    Steps::Sum(totals);
    Expect(totals.used[std::size_t(StepId::pipeline_memo)] == u64(kMaxThreads), "the overflow slot counts nothing");
}

void Churn() {
    Begin();
    std::atomic<bool> stop{false};
    std::atomic<int> bad_sum{0}, bad_name{0}, reads{0};
    std::thread reader([&] {
        u64 last = 0;
        while (!stop.load()) {
            Steps::Totals t;
            Steps::Sum(t);
            const u64 now = t.used[std::size_t(StepId::gx_own_buffers)];
            if (now < last) bad_sum.fetch_add(1);
            last = now;
            for (int i = 0; i < kMaxThreads; ++i) {
                Steps::SlotView v;
                if (Steps::ReadSlot(i, v) != Steps::SlotRead::Ok) continue;
                reads.fetch_add(1);
                if (std::strtoul(v.name, nullptr, 10) != v.tid) bad_name.fetch_add(1);
            }
        }
    });
    for (int round = 0; round < 40; ++round) {
        std::vector<std::thread> threads;
        for (int t = 0; t < 16; ++t) {
            threads.emplace_back([] {
                char name[16];
                std::snprintf(name, sizeof name, "%u", Tid());
                NameThread(name);
                ThreadSlot& slot = Steps::Slot();
                for (int i = 0; i < 1000; ++i) {
                    Steps::Used(slot, StepId::gx_own_buffers);
                    Crumb::Draw(slot, u64(i), 1, 2, 3, 4);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    stop = true;
    reader.join();
    Expect(bad_sum.load() == 0, "C-B1: the total never decreases while threads come and go");
    Expect(bad_name.load() == 0, "C-B2: a name and a tid always come from the same owner");
    Expect(reads.load() > 0, "C-B2: the reader saw claimed slots");
}

struct LateUser {
    int index = -1;
    ~LateUser() {
        ThreadSlot& slot = Steps::Slot();
        Steps::Used(slot, StepId::vertex_memo);
        Crumb::Draw(slot, 99, 99, 99, 99, 99);
    }
};
thread_local LateUser t_late;

void LateDestructor() {
    Begin();
    std::thread([] {
        t_late.index = 0;
        ThreadSlot& slot = Steps::Slot();
        Steps::Used(slot, StepId::vertex_memo);
    }).join();
    Steps::Totals t;
    Steps::Sum(t);
    Expect(t.used[std::size_t(StepId::vertex_memo)] == 1, "C-B3: a later thread_local destructor writes nothing");
    Expect(g_threads_without_slot.load() == 0 && FreeSlots() == kMaxThreads, "C-B3: no claim runs after the guard");
    Expect(g_dead_slot.recent_head.load() == 0 && g_dead_slot.used[0].load() == 0, "C-B3: the dead slot stays empty");
}

void DisableRace() {
    for (int round = 0; round < 200; ++round) {
        Begin({{"LANTERN_FRESH_FLATS", "1"}});
        std::atomic<bool> go{false};
        std::thread menu([&] {
            while (!go.load()) {
            }
            for (int i = 0; i < 100; ++i) Steps::SetFromMenu(StepId::fresh_flats, i % 2 ? Mode::On : Mode::Off);
        });
        std::thread check([&] {
            ThreadSlot& slot = Steps::Slot();
            go = true;
            Steps::Checked(slot, StepId::fresh_flats, false, Evidence{"vertex buffer"}, 10);
        });
        menu.join();
        check.join();
        if (Steps::State(StepId::fresh_flats) != StepState::Disabled) {
            Expect(false, "C-B4: Disabled always wins over the menu");
            return;
        }
    }
    Expect(!Steps::SetFromMenu(StepId::fresh_flats, Mode::On), "C-B4: Disabled stays");
}

void DiagnosisFolder() {
    if (std::getenv("BB_LOG_FILE")) return;
#ifdef _WIN32
    _putenv_s("BB_LOG_DIR", "C:/Lantern Engine/logs");
#else
    setenv("BB_LOG_DIR", "C:/Lantern Engine/logs", 1);
#endif
    char path[256];
    Lantern::Diagnosis::Path(path, sizeof path, "diagnosis.txt");
    Expect(std::string(path) == "C:/Lantern Engine/logs/diagnosis.txt", "the diagnosis file goes to BB_LOG_DIR without a log file");
#ifdef _WIN32
    _putenv_s("BB_LOG_DIR", "");
#else
    unsetenv("BB_LOG_DIR");
#endif
    Lantern::Diagnosis::Path(path, sizeof path, "diagnosis.txt");
    Expect(std::string(path) == "logs/diagnosis.txt", "the diagnosis file falls back to logs/ without either");
}

void Tearing() {
    Begin();
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0}, changing{0}, whole{0};
    std::atomic<ThreadSlot*> slot_seen{nullptr};
    std::thread writer([&] {
        ThreadSlot& slot = Steps::Slot();
        slot_seen = &slot;
        u64 n = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            Crumb::Draw(slot, n, n, n, n, n);
            Crumb::Scope scope(slot, StepId::fresh_flats, "test");
            ++n;
        }
    });
    while (!slot_seen.load()) std::this_thread::yield();
    for (int i = 0; i < 200000; ++i) {
        Steps::SlotView v;
        if (Steps::ReadSlot(*slot_seen.load(), v) != Steps::SlotRead::Ok) continue;
        for (u32 k = 0; k < v.recent_count; ++k) {
            const auto& r = v.recent[k];
            if (r.changing) {
                changing.fetch_add(1);
                continue;
            }
            whole.fetch_add(1);
            if (r.draw != r.frame || r.draw != r.vs || r.draw != r.ps || r.draw != r.key) {
                if (torn.fetch_add(1) == 0) {
                    std::printf("steps-test: torn record k %u head-relative: draw %llu frame %llu vs %llu ps %llu key %llu seq %llu\n", k,
                                (unsigned long long)r.draw, (unsigned long long)r.frame, (unsigned long long)r.vs,
                                (unsigned long long)r.ps, (unsigned long long)r.key, (unsigned long long)r.seq);
                }
            }
        }
    }
    stop = true;
    writer.join();
    Expect(torn.load() == 0, "C-B5: a reader never sees a torn recent draw");
    Expect(whole.load() > 0, "C-B5: the reader saw whole records");
    std::printf("steps-test: tearing reads whole %d, changing %d\n", whole.load(), changing.load());
}

void SharedSlots() {
    Begin();
    for (ThreadSlot* s : {&g_dead_slot, &g_overflow_slot}) {
        Crumb::Draw(*s, 1, 1, 1, 1, 1);
        {
            Crumb::Scope scope(*s, StepId::vertex_memo, "x");
        }
        Expect(!Steps::Sample(*s, StepId::vertex_memo), "R2: a shared slot never samples");
        Expect(!Steps::SampleNew(*s, StepId::vertex_memo, true), "R2: a shared slot never takes a first hit");
        Steps::Checked(*s, StepId::pipeline_memo, true, Evidence{}, 5);
        Expect(s->checked[std::size_t(StepId::pipeline_memo)].load() == 0, "R2: Checked writes nothing into it");
        Expect(s->rate_tokens.load() == 0 && s->first_tokens.load() == 0 && s->refill_tsc.load() == 0,
               "rev 4: a shared slot's Draw and Scope leave the buckets alone");
    }
}

void OddSequence() {
    Begin();
    g_slots[0].crumb_seq.store(7);
    g_slots[0].recent[1].seq.store(3);
    std::thread([] {
        ThreadSlot& slot = Steps::Slot();
        Expect(&slot == &g_slots[0], "R3: the first free slot is claimed");
        Crumb::Draw(slot, 5, 5, 5, 5, 5);
        Steps::SlotView v;
        Expect(Steps::ReadSlot(slot, v) == Steps::SlotRead::Ok && !v.crumbs_changing && v.recent_count >= 1 &&
                   !v.recent[0].changing && v.recent[0].draw == 5,
               "R3: a slot left odd is readable after the next claim");
    }).join();
}

void Reclaim() {
    Begin();
    std::atomic<bool> hold{true};
    std::atomic<u32> tid{0};
    std::thread t([&] {
        Steps::Slot();
        tid = t_tid;
        while (hold.load()) std::this_thread::yield();
    });
    while (!tid.load()) std::this_thread::yield();
    const u32 held = tid.load();
    const int before = FreeSlots();
    Expect(Steps::ReclaimDead([](u32) { return true; }) == 0 && FreeSlots() == before,
           "a thread the system calls alive keeps its slot");
    static u32 dead_tid = 0;
    dead_tid = held;
    Expect(Steps::ReclaimDead([](u32 id) { return id != dead_tid; }) == 1 && FreeSlots() == before + 1,
           "a thread the system calls gone gives its slot back");
    Expect(Steps::ReclaimDead([](u32) { return false; }) == 0, "a slot is reclaimed once");
    hold = false;
    t.join();
}

void StaleRecent() {
    Begin();
    std::thread([] {
        ThreadSlot& slot = Steps::Slot();
        for (u64 d = 1; d <= 5; ++d) Crumb::Draw(slot, d, 7, 8, 9, 10);
    }).join();
    std::thread([] {
        ThreadSlot& slot = Steps::Slot();
        Steps::SlotView v;
        Expect(Steps::ReadSlot(slot, v) == Steps::SlotRead::Ok && v.recent_count == 0,
               "a new owner shows none of the previous owner's recent draws");
        Crumb::Draw(slot, 42, 42, 42, 42, 42);
        Expect(Steps::ReadSlot(slot, v) == Steps::SlotRead::Ok && v.recent_count == 1 && v.recent[0].draw == 42,
               "a new owner shows only its own draws");
    }).join();
}

void Unavailable() {
    Begin({{"LANTERN_FRESH_FLATS", "1"}});
    Expect(Steps::SetUnavailable(StepId::fresh_flats, "test device"), "SetUnavailable takes the start value");
    Expect(!Steps::SetFromMenu(StepId::fresh_flats, Mode::On), "R5: the menu cannot enable an unavailable step");
    Expect(Steps::Get(StepId::fresh_flats) == Mode::Off, "an unavailable step reads as off");
    Expect(!Steps::SetFromMenu(StepId::native_shaders, Mode::Off), "a start-only step is not switched from the menu");
}

void DrawHash() {
    Begin();
    std::thread([] {
        ThreadSlot& slot = Steps::Slot();
        Expect(slot.draw_hash.load() == Decide::kNoHash, "N4: draw_hash starts empty");
        for (int i = 0; i < 5000; ++i) {
            if (Steps::Sample(slot, StepId::vertex_memo)) {
                Expect(false, "N4: an empty draw hash never samples");
                break;
            }
        }
        const u64 reads_before = Steps::TicksReads();
        int timed = 0, sampled = 0;
        for (u64 use = 1; use <= 20000; ++use) {
            timed += ((Decide::MixHash(use) >> 10) & 1023u) == 1u;
            Crumb::Scope scope(slot, StepId::key_hash, "prep");
            sampled += Steps::Sample(slot, StepId::key_hash);
        }
        Expect(sampled > 0, "N4: a thread without draws samples after Crumb::Scope");
        Expect(Steps::TicksReads() - reads_before == u64(2 * timed), "O1: scopes read the clock only to time a step");
    }).join();
}

void BucketBudget() {
    Begin({{"LANTERN_VERTEX_MEMO", "1"}, {"LANTERN_PIPELINE_MEMO", "1"}});
    Expect(g_budget.earn_first.load() == g_budget.earn_rate.load() ||
               g_budget.earn_first.load() + 1 == g_budget.earn_rate.load(),
           "N3: first hits and rate samples each get half in the warm window");
    std::thread([] {
        ThreadSlot& slot = Steps::Slot();
        Crumb::Draw(slot, 1, 1, 0, 0, 0);
        Expect(slot.refill_tsc.load() == 1 && slot.rate_tokens.load() > 0, "N1: Crumb::Draw refills");
        Steps::Checked(slot, StepId::vertex_memo, true, Evidence{}, 1'000'000);
        Expect(slot.rate_tokens.load() < 0, "N2: a large check leaves the bucket negative");
        int checks = 0, starved_before = int(slot.skipped_budget[std::size_t(StepId::vertex_memo)].load());
        u64 d = 2;
        for (; d < 4000; ++d) {
            Crumb::Draw(slot, d, 1, 0, 0, 0);
            if (Decide::MaskHit(slot.draw_hash.load(), g_sample_mask[std::size_t(StepId::vertex_memo)].load()) &&
                Steps::Sample(slot, StepId::vertex_memo)) {
                ++checks;
            }
        }
        Expect(checks == 0, "an exhausted budget stops checks");
        Expect(int(slot.skipped_budget[std::size_t(StepId::vertex_memo)].load()) > starved_before,
               "refused samples are counted as skipped");
        for (int i = 0; i < 200000 && slot.rate_tokens.load() <= 0; ++i) Crumb::Draw(slot, ++d, 1, 0, 0, 0);
        Expect(slot.rate_tokens.load() > 0, "N1: an exhausted bucket recovers on later draws");
        Expect(!Steps::SampleNew(slot, StepId::pipeline_memo, false), "SampleNew needs a first hit");
        slot.first_tokens.store(-1);
        Expect(!Steps::SampleNew(slot, StepId::pipeline_memo, true), "N3: a first hit without tokens is skipped");
        for (int i = 0; i < 100000 && slot.first_tokens.load() <= 0; ++i) Crumb::Draw(slot, ++d, 1, 0, 0, 0);
        Expect(Steps::SampleNew(slot, StepId::pipeline_memo, true), "N3: the skipped first hit is taken on a later hit");
        for (int i = 0; i < 100000; ++i) {
            Crumb::Draw(slot, ++d, 1, 0, 0, 0);
            if (Decide::MaskHit(slot.draw_hash.load(), 15) && slot.draw_may_check.load()) break;
        }
        Expect(Steps::Sample(slot, StepId::vertex_memo), "the first step of a sampled draw checks");
        Steps::Checked(slot, StepId::vertex_memo, true, Evidence{}, 1'000'000);
        Expect(Steps::Sample(slot, StepId::pipeline_memo), "O2: a later step of the same draw still checks");
    }).join();
}

void NoChecksBeforeCalibration() {
    Begin({{"LANTERN_VERTEX_MEMO", "1"}});
    Steps::ResetForTest();
    Steps::Start(Switches::Get());
    std::thread([] {
        ThreadSlot& slot = Steps::Slot();
        int checks = 0;
        for (u64 d = 0; d < 100000; ++d) {
            Crumb::Draw(slot, d, 1, 0, 0, 0);
            checks += Steps::Sample(slot, StepId::vertex_memo);
        }
        Expect(checks == 0, "O3: no checks before the clock is calibrated");
    }).join();
}

void Fairness() {
    std::vector<std::pair<std::string, std::string>> vars;
    for (const StepInfo& s : kSteps) {
        const SwitchRow& row = Row(s.sw);
        if (row.kind == SwitchKind::Mode) vars.emplace_back(std::string(row.name), "1");
    }
    Begin(vars);
    std::thread([] {
        ThreadSlot& slot = Steps::Slot();
        u64 now = 1'000'000'000ull, draw = 0;
        for (int frame = 0; frame < 180; ++frame) {
            now += 16'666'667ull;
            Steps::OnFlip({now, false, 16'666'667ull});
            for (int d = 0; d < 2600; ++d) {
                Crumb::Draw(slot, ++draw, u64(frame), 0, 0, 0);
                for (int i = 0; i < kStepCount; ++i) {
                    if (Steps::Get(StepId(i)) == Mode::Off) continue;
                    Steps::Used(slot, StepId(i));
                    if (Steps::Sample(slot, StepId(i))) {
                        const u64 cost = kSteps[i].weight == Weight::Heavy ? 5000 : 1000;
                        Steps::Checked(slot, StepId(i), true, Evidence{}, cost);
                    }
                }
            }
        }
        Steps::Totals t;
        Steps::Sum(t);
        bool all = true;
        for (int i = 0; i < kStepCount; ++i) {
            if (Steps::Get(StepId(i)) != Mode::Off && t.checked[i] == 0) all = false;
        }
        Expect(all, "N3: under a tight cap every on step gets at least one check");
    }).join();
    Steps::Totals t;
    Steps::Sum(t);
    int distinct = 0;
    for (int i = 0; i < kStepCount; ++i) {
        if (t.checked[i]) ++distinct;
    }
    Expect(distinct > 1, "O4: the steps' minimum checks are spread");
    Decide::MinShare a{2, 1, 3, 60, 3}, b{2, 1, 3, 60, 4};
    Expect(Decide::MinShareAllowed(a) != Decide::MinShareAllowed(b), "O4: two steps take their minimum in different frames");
}

void SteadyMasks() {
    Begin();
    Expect(g_sample_mask[std::size_t(StepId::vertex_memo)].load() == 15 &&
               g_sample_mask[std::size_t(StepId::ring_constants)].load() == 63,
           "warm window: 1 in 16, heavy 1 in 64");
    Steps::OnFlip({1'000'000'000ull + 6'000'000'000ull, false, 16'666'667ull});
    Expect(g_sample_mask[std::size_t(StepId::vertex_memo)].load() == 1023 &&
               g_sample_mask[std::size_t(StepId::ring_constants)].load() == 4095,
           "steady state: 1 in 1024, heavy 1 in 4096");
    Expect(g_budget.spill.load() == 1, "O5: the steady state spills");
    Steps::OnFlip({7'100'000'000ull, true, 16'666'667ull});
    Steps::OnFlip({7'200'000'000ull, false, 16'666'667ull});
    Expect(g_sample_mask[std::size_t(StepId::vertex_memo)].load() == 15 && Steps::Area() == 1,
           "a loading screen opens a new warm window and a new area");
}

void TwoDiffer() {
    Begin({{"LANTERN_FRESH_FLATS", "1"}});
    std::atomic<bool> go{false};
    auto differ = [&] {
        ThreadSlot& slot = Steps::Slot();
        while (!go.load()) {
        }
        Steps::Checked(slot, StepId::fresh_flats, false, Evidence{"vertex buffer", 0x1000, 0x80}, 10);
    };
    std::thread a(differ), b(differ);
    go = true;
    a.join();
    b.join();
    Steps::Totals t;
    Steps::Sum(t);
    Expect(t.differ[std::size_t(StepId::fresh_flats)] == 2, "both differences are counted");
    Expect(Steps::State(StepId::fresh_flats) == StepState::Disabled, "a live step is Disabled");
    g_logged.clear();
    SetLogSinkForTest(Sink, nullptr);
    Steps::PrintNewFirstDifferences();
    Steps::PrintNewFirstDifferences();
    SetLogSinkForTest(nullptr, nullptr);
    Expect(g_logged.size() == 1 && g_logged[0].rfind("Step off: fresh-flats", 0) == 0,
           "one record, printed once, with the Step off prefix");
}

std::string Attention() {
    static char text[65536];
    Steps::Out o{text, sizeof text};
    Diagnosis::Inputs in;
    Diagnosis::Write(o, in);
    const std::string all(text, o.used);
    const std::size_t a = all.find("[attention]\n"), b = all.find("[build]");
    return all.substr(a, b - a);
}

std::string FirstLog() {
    g_logged.clear();
    SetLogSinkForTest(Sink, nullptr);
    Steps::PrintNewFirstDifferences();
    SetLogSinkForTest(nullptr, nullptr);
    std::string all;
    for (const auto& l : g_logged) all += l + "\n";
    return all;
}

void DifferStartOnly(StepId id) {
    std::thread([id] {
        ThreadSlot& slot = Steps::Slot();
        Crumb::Draw(slot, 77, 3, 0xaa, 0xbb, 0xcc);
        Steps::Checked(slot, id, false, Evidence{"packet re-encode"}, 10);
    }).join();
}

void CopyReadyLine() {
    Begin({{"LANTERN_STEPS_OFF", "gx-own-buffers"}});
    DifferStartOnly(StepId::draw_tokens);
    Expect(Steps::State(StepId::draw_tokens) == StepState::On, "a start-only step stays on");
    const std::string want = "LANTERN_STEPS_OFF=gx-own-buffers,draw-tokens";
    const std::string log = FirstLog();
    Expect(log.find("Step differs: draw-tokens") != std::string::npos, "a start-only step is flagged");
    Expect(log.find("Next start, copy this into Extra variables: " + want + "\n") != std::string::npos,
           "D4: the log carries the merged line");
    const std::string att = Attention();
    Expect(att.find(want + "\n") != std::string::npos && att.find("STEP DIFFERED") != std::string::npos,
           "D4: the diagnosis file carries the merged line");

    Begin();
    DifferStartOnly(StepId::draw_tokens);
    Expect(FirstLog().find("copy this into Extra variables: LANTERN_STEPS_OFF=draw-tokens\n") != std::string::npos,
           "D4: with nothing set the line names only the step");

    Begin();
    DifferStartOnly(StepId::draw_tokens);
    DifferStartOnly(StepId::gx_records);
    char line[512];
    Steps::FormatStepsOffLine(line, sizeof line);
    Expect(std::string(line) == "LANTERN_STEPS_OFF=draw-tokens,gx-records", "D4: two start-only differences, one line");
    Expect(Attention().find("variables: LANTERN_STEPS_OFF=draw-tokens,gx-records\n") != std::string::npos,
           "D4: the attention section merges both");

    Begin({{"LANTERN_STEPS_OFF", "draw-tokens"}});
    DifferStartOnly(StepId::draw_tokens);
    Steps::FormatStepsOffLine(line, sizeof line);
    Expect(std::string(line) == "LANTERN_STEPS_OFF=draw-tokens", "D4: a step already in the value is not repeated");

    Begin({{"LANTERN_STEPS_OFF", "group:shaders"}});
    DifferStartOnly(StepId::draw_tokens);
    Steps::FormatStepsOffLine(line, sizeof line);
    Expect(std::string(line) == "LANTERN_STEPS_OFF=native-shaders,native-shader-compiler,native-tables,draw-tokens",
           "D4: a group in the start value is expanded to names");
}

void Grammar() {
    const std::vector<std::pair<std::string, std::string>> on = {{"BB_NATIVE_CALLS", "1"},
                                                                 {"LANTERN_VERTEX_MEMO", "1"},
                                                                 {"LANTERN_PIPELINE_MEMO", "1"},
                                                                 {"LANTERN_GX_OWN_BUFFERS", "1"}};
    auto with = [&](const char* off) {
        auto v = on;
        v.emplace_back("LANTERN_STEPS_OFF", off);
        Begin(v);
        u64 mask = 0;
        for (int i = 0; i < kStepCount; ++i) {
            if (Steps::StartInfo().off_why[i] == OffWhy::StepsOff) mask |= u64(1) << i;
        }
        return mask;
    };
    const u64 first = with("@1/2"), second = with("@2/2");
    Begin(on);
    u64 wired_on = 0;
    for (int i = 0; i < kStepCount; ++i) {
        if (kSteps[i].wiring == Wiring::Wired && Steps::Get(StepId(i)) != Mode::Off) wired_on |= u64(1) << i;
    }
    Expect((first & second) == 0 && (first | second) == wired_on && first && second,
           "@1/2 and @2/2 cover every on step exactly once");
    Expect(with("vertex-memo") == (u64(1) << u8(StepId::vertex_memo)), "a step name");
    const u64 memo = with(" group:memo ");
    u64 wired_memo = 0, wired_new = 0;
    for (int i = 0; i < kStepCount; ++i) {
        if (kSteps[i].wiring != Wiring::Wired) continue;
        if (kSteps[i].group == "memo") wired_memo |= u64(1) << i;
        if (kSteps[i].since == Since::New) wired_new |= u64(1) << i;
    }
    Expect(memo == wired_memo && (memo & (u64(1) << u8(StepId::vertex_memo))), "group:memo turns off the wired memo steps");
    Expect(Steps::StartInfo().steps_off_not_switched & (u64(1) << u8(StepId::ring_constants)),
           "a registry-only step named is reported, not claimed off");
    with("toggle:ConstantRing,nonsense");
    Expect(std::string(Steps::StartInfo().toggles) == "toggle:ConstantRing", "toggle entries are kept");
    Expect(std::string(Steps::StartInfo().problems) == "nonsense", "unknown entries are reported");
    Expect(with("all-new") == wired_new && (wired_new & (u64(1) << u8(StepId::pipeline_memo))),
           "all-new names the steps added since 0.9.42");
    Expect(Steps::Get(StepId::vertex_memo) == Mode::On, "all-new leaves older steps on");
}

void Warnings() {
    Begin();
    static WarnSite sites[16] = {WarnSite("a"), WarnSite("b"), WarnSite("c"), WarnSite("d"), WarnSite("e"), WarnSite("f"),
                                 WarnSite("g"), WarnSite("h"), WarnSite("i"), WarnSite("j"), WarnSite("k"), WarnSite("l"),
                                 WarnSite("m"), WarnSite("n"), WarnSite("o"), WarnSite("p")};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([] {
            for (int i = 0; i < 1000; ++i) {
                for (WarnSite& s : sites) Diag::Warn(s, Evidence{"x"});
            }
        });
    }
    for (auto& t : threads) t.join();
    int listed = 0;
    bool counts = true;
    for (WarnSite* s = Diag::Warnings(); s; s = s->next) {
        ++listed;
        counts = counts && s->count.load() == 8000;
    }
    Expect(listed == 16 && counts, "WarnSite: each site listed once, every hit counted, under 8 threads");
}

void VulkanTable() {
    Expect(VkMessages::Note(0, "zero"), "id 0 is stored");
    Expect(!VkMessages::Note(0, "zero"), "id 0 is found again");
    Expect(VkMessages::Note(-1, "minus one"), "id -1 is not taken for id 0");
    Expect(VkMessages::Count(0) == 2 && VkMessages::Count(-1) == 1, "counts per id");
}

void BeingWritten() {
    Begin();
    Expect(Steps::HoldFirstDifferenceForTest(StepId::gx_textures), "the record is claimed");
    char text[256];
    Expect(Steps::FirstDifferenceText(StepId::gx_textures, text, sizeof text) &&
               std::string(text) == "gx-textures: being written",
           "claimed but not ready prints being written");
}

void Unverified() {
    constexpr StepInfo fake[] = {{"x", "g", SwitchId::native_shaders, CheckKind::None, Stage::A, Weight::Light,
                                  Wiring::Registry, Since::Old, "", {}, {}}};
    static_assert(!EvidenceGiven(fake), "a step without a comparison and without evidence fails the check");
    static_assert(EvidenceGiven(kSteps));
}

const char* kGolden =
#include "test_steps_golden.inc"
    ;

void Golden() {
    Begin({{"BB_FRAME_STATS", "1"}, {"LANTERN_VERTEX_MEMO", "1"}, {"LANTERN_STEPS_OFF", "gx-own-buffers"},
           {"BB_ROOM_PASSWORD", "hunter2"}});
    std::thread([] {
        NameThread("bb:DrawRec#1");
        ThreadSlot& slot = Steps::Slot();
        Crumb::Draw(slot, 1843221, 50211, 0x6c62a79f, 0xa0e88f56, 0x9f13c0a2e4b1d377);
        Steps::Used(slot, StepId::vertex_memo);
        Steps::Covered(slot, StepId::vertex_memo, true);
        Steps::FellBack(slot, StepId::vertex_memo, 2);
        Steps::Checked(slot, StepId::vertex_memo, false,
                       Evidence{"vertex buffer", 0x10dbb62000, 0x8000, {0x2b1c0, 0}, {0x2b1c0, 0x1000}, 0}, 0);
        Steps::Checked(slot, StepId::draw_tokens, false, Evidence{"packet re-encode"}, 0);
    }).join();
    static char text[65536];
    Steps::Out o{text, sizeof text};
    Diagnosis::Inputs in;
    in.pid = 12345;
    in.started = "2026-11-13T21:04:11";
    in.engine_id = "8adc288f";
    in.os = "Windows";
    in.minutes_tenths = 473;
    in.frames = 302112;
    in.areas = 6;
    in.fps_tenths = 1412;
    g_news = 0;
    g_count_news = true;
    Diagnosis::Write(o, in);
    g_count_news = false;
    Expect(g_news.load() == 0, "the diagnosis writer allocates nothing");
    const std::string got(text, o.used);
    if (got != kGolden) {
        Expect(false, "diagnosis golden text");
        std::FILE* f = std::fopen("out/steps-golden-got.txt", "wb");
        if (f) {
            std::fwrite(got.data(), 1, got.size(), f);
            std::fclose(f);
        }
    }
    Expect(got.find("hunter2") == std::string::npos, "no secret in the diagnosis file");
    Expect(got.find("[unverified]\nnative-shaders | on | offline:") != std::string::npos,
           "the unverified section names each step's evidence");
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--mutants") == 0) {
        for (int m = Decide::kFirstStepMutant; m <= Decide::kLastStepMutant; ++m) std::printf("%d\n", m);
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--mutant") == 0) lantern_mutant = std::atoi(argv[2]);
    Decisions();
    Claims();
    Overflow();
    Churn();
    LateDestructor();
    DisableRace();
    Tearing();
    DiagnosisFolder();
    SharedSlots();
    OddSequence();
    StaleRecent();
    Reclaim();
    Unavailable();
    DrawHash();
    BucketBudget();
    NoChecksBeforeCalibration();
    SteadyMasks();
    Fairness();
    TwoDiffer();
    CopyReadyLine();
    Grammar();
    Warnings();
    VulkanTable();
    BeingWritten();
    Unverified();
    Golden();
    if (failures) {
        std::printf("steps-test: %d failed\n", failures);
        return 1;
    }
    std::printf("steps-test: ok\n");
    return 0;
}
