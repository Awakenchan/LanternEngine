// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/lantern_gpu_stamps.h"
#include <cstdio>
#include <memory>
#include <vector>

namespace G = Lantern::GpuStamps;
namespace R = BbEngine::BottleneckMeter;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

namespace {
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++g_failed;
    }
}

constexpr u64 kSecond = 1'000'000'000ull;

struct Ring {
    std::unique_ptr<G::Slots> slots = std::make_unique<G::Slots>();
    std::vector<R::Span> spans;
    u64 clock = 0;

    u32 Collect(u64 now_ns, u64 value_base, const std::vector<bool>& available) {
        const G::Pending p = slots->Outstanding();
        std::vector<G::Result> results(p.count);
        for (u32 i = 0; i < p.count; ++i) {
            const bool on = i < available.size() ? bool(available[i]) : true;
            results[i] = {on, value_base + i * 10, value_base + i * 10 + 5};
        }
        const u32 ready = slots->Ready(p, results, now_ns, spans);
        slots->Release(ready);
        return ready;
    }

    bool Cycle(u64 handle, u64 value_base) {
        if (!slots->Begin(handle, clock)) return false;
        if (!slots->End(handle, 0)) return false;
        return Collect(clock, value_base, {}) >= 1;
    }
};

void TestWrap() {
    Ring r;
    u32 done = 0;
    for (u64 i = 0; i < 5000; ++i) done += r.Cycle(i + 1, 1000 + i * 100) ? 1 : 0;
    const G::Counts c = r.slots->Read();
    Check(done == 5000 && c.collected == 5000 && c.skipped == 0, "the ring wraps past 4096 without losing a slot");
}

void TestLost() {
    Ring r;
    const auto stuck = r.slots->Begin(1, 0);
    Check(stuck.has_value(), "the stuck slot is begun");
    Check(r.slots->Begin(2, 0) && r.slots->End(2, 0), "the next slot is begun and ended");
    const u32 early = r.Collect(kSecond, 100, {false, true});
    Check(early == 0, "a slot not yet lost holds the slots after it");
    const u32 late = r.Collect(3 * kSecond, 100, {false, true});
    Check(late == 2, "after 2 s the stuck slot is lost and the ring moves on");
    Check(r.slots->Read().lost == 1 && r.slots->StateOf(*stuck) == G::State::Lost, "the lost slot is counted and never reused");
    u32 done = 0;
    r.clock = 3 * kSecond;
    for (u64 i = 0; i < 5000; ++i) done += r.Cycle(100 + i, 10'000 + i * 100) ? 1 : 0;
    Check(done == 5000 && r.slots->Read().skipped == 0, "the ring keeps working around a lost slot");
    Check(r.slots->StateOf(*stuck) == G::State::Lost, "the lost slot is still not reused after wrapping");
}

void TestFull() {
    Ring r;
    for (u64 i = 0; i < G::kSlots; ++i) {
        if (!r.slots->Begin(i + 1, 0) || !r.slots->End(i + 1, 0)) {
            Check(false, "the ring takes 4096 command buffers");
            return;
        }
    }
    Check(!r.slots->Begin(9'999, 0).has_value(), "a full ring stamps nothing");
    Check(r.slots->Read().skipped == 1, "the skipped command buffer is counted");
}

void TestReusedHandle() {
    Ring r;
    const auto first = r.slots->Begin(7, 0);
    const auto first_end = r.slots->End(7, 0);
    const auto second = r.slots->Begin(7, 0);
    const auto second_end = r.slots->End(7, 0);
    Check(first && first_end && *first == *first_end, "the first use ends its own slot");
    Check(second && second_end && *second == *second_end && *second != *first, "a reused handle ends its new slot");
    Check(!r.slots->End(7, 0).has_value(), "a handle ends once");
}

bool TornReadWaits() {
    Ring r;
    if (!r.Cycle(1, 500)) return false;
    for (u64 i = 1; i < G::kSlots; ++i) r.Cycle(i + 1, 100'000 + i * 100);
    if (!r.slots->Begin(9'001, 0) || !r.slots->End(9'001, 0)) return false;
    const G::Pending p = r.slots->Outstanding();
    std::vector<G::Result> torn = {{true, 500, 77'777}};
    if (r.slots->Ready(p, torn, 0, r.spans) != 0) return false;
    std::vector<G::Result> whole = {{true, 66'666, 77'777}};
    return r.slots->Ready(p, whole, 0, r.spans) == 1;
}

void TestReclaim() {
    Ring r;
    const auto stuck = r.slots->Begin(1, 0);
    Check(stuck && r.slots->End(1, 40), "a slot ends in submission 40");
    Check(r.Collect(3 * kSecond, 0, {false}) == 1, "unfinished after 2 s, the slot is lost");
    Check(r.slots->LostNow() == 1, "one slot is lost");
    Check(r.slots->Reclaim(39) == 0 && r.slots->StateOf(*stuck) == G::State::Lost, "not freed before its submission completed");
    Check(r.slots->Reclaim(40) == 1 && r.slots->StateOf(*stuck) == G::State::Free, "freed once tick 40 completed");
    const auto never = r.slots->Begin(2, 0);
    Check(never && r.Collect(6 * kSecond, 0, {false}) == 1, "a slot never ended is lost");
    Check(r.slots->Reclaim(1'000'000) == 0, "a slot that never ended is not freed");
    Check(!r.slots->End(2, 50).has_value(), "its late end writes nothing");
    Check(r.slots->Reclaim(49) == 0 && r.slots->Reclaim(50) == 1, "its late end lets it free after its submission");
    u32 done = 0;
    for (u64 i = 0; i < 2 * G::kSlots; ++i) done += r.Cycle(100 + i, 10'000'000 + i * 100) ? 1 : 0;
    Check(done == 2 * G::kSlots && r.slots->LostNow() == 0, "reclaimed slots are used again");
}

void TestStale() {
    Ring r;
    Check(r.Cycle(1, 500), "slot 0 is used once");
    for (u64 i = 1; i < G::kSlots; ++i) r.Cycle(i + 1, 100'000 + i * 100);
    const auto again = r.slots->Begin(9'000, 0);
    r.slots->End(9'000, 0);
    Check(again && *again == 0, "the ring comes back to slot 0");
    const u64 before = r.slots->Read().collected;
    Check(r.Collect(0, 500, {}) == 0 && r.slots->Read().collected == before, "slot 0's old values are not taken again");
    Check(TornReadWaits(), "a torn read with only one new value is not taken");
    Check(r.Collect(0, 900'000, {}) == 1, "slot 0's new values are taken");
}

int RunSuite() {
    g_failed = 0;
    TestWrap();
    TestLost();
    TestFull();
    TestReusedHandle();
    TestStale();
    TestReclaim();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_GPU_STAMPS_MUTANTS
    int survivors = 0;
    for (int m = 0; m <= G::kMutants; ++m) {
        G::g_mutant = m;
        const int found = RunSuite();
        if (m == 0 && found) {
            std::printf("gpu stamps mutants: the unmutated run failed %d checks\n", found);
            return 1;
        }
        if (m && !found) {
            std::printf("gpu stamps mutants: mutant %d survived\n", m);
            ++survivors;
        }
    }
    if (survivors) return 1;
    std::printf("gpu stamps mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("gpu stamps: %d checks failed\n", g_failed);
        return 1;
    }
    std::printf("gpu stamps: all tests passed\n");
    return 0;
#endif
}
