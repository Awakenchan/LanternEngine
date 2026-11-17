// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "gpu/shim/engine/vertex_census.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

using namespace BbEngine::VertexCensus;

double NsSince(std::chrono::steady_clock::time_point start, double count) {
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count;
}

void Bench() {
    Buffers buffers;
    constexpr u32 kBuffers = 40000;
    for (u32 i = 0; i < kBuffers; ++i) buffers.Add(0x100000000ull + u64(i) * 0x3000, 0x2000, (i & 7) == 0);
    std::vector<std::pair<u64, u64>> streams;
    for (u32 i = 0; i < 7500; ++i) {
        const u32 buffer = (i * 2654435761u) % kBuffers;
        streams.push_back({0x100000000ull + u64(buffer) * 0x3000 + (i % 3) * 0x100, 0x400});
    }
    auto memo = std::make_unique<Memo>();
    const Ring ring{0x900000000ull, 64ull << 20};
    u64 sink = 0;
    for (const auto& [a, s] : streams) sink += Classify(&buffers, *memo, ring, a, s);
    constexpr int kFrames = 200;
    auto start = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) {
        for (const auto& [a, s] : streams) sink += Classify(&buffers, *memo, ring, a, s);
    }
    const double steady = NsSince(start, double(kFrames) * streams.size());
    const double steady_miss = double(memo->counts.misses) / double(memo->counts.lookups);
    memo->counts = {};

    start = std::chrono::steady_clock::now();
    for (int f = 0; f < 20; ++f) {
        for (const auto& [a, s] : streams) sink += buffers.Find(a, s);
    }
    const double locked = NsSince(start, 20.0 * streams.size());

    std::atomic<bool> stop{false};
    std::atomic<u64> churn{0};
    std::thread writer([&] {
        u32 i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const u64 base = 0x100000000ull + u64((i * 7919u) % kBuffers) * 0x3000 + 0x2000;
            buffers.Add(base, 0x800, true);
            buffers.Remove(base);
            churn.fetch_add(2, std::memory_order_relaxed);
            ++i;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    start = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) {
        for (const auto& [a, s] : streams) sink += Classify(&buffers, *memo, ring, a, s);
    }
    const double churned = NsSince(start, double(kFrames) * streams.size());
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    stop = true;
    writer.join();
    const double churn_miss = double(memo->counts.misses) / double(memo->counts.lookups);

    start = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) {
        for (std::size_t i = 0; i + 3 <= streams.size(); i += 3) {
            Draw draw(*memo);
            for (std::size_t k = i; k < i + 3; ++k) {
                const auto& [a, s] = streams[k];
                draw.Stream(Classify(&buffers, *memo, ring, a, s), Arena, ring.Holds(a, s));
            }
            draw.Finish();
            Index(*memo, Classify(&buffers, *memo, ring, streams[i].first + 0x1000, 0x300), Arena);
            Flush(*memo);
        }
    }
    const double per_draw = NsSince(start, double(kFrames) * (streams.size() / 3));

    Buffers fresh;
    start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kBuffers; ++i) fresh.Add(0x200000000ull + u64(i) * 0x3000, 0x2000, false);
    for (u32 i = 0; i < kBuffers; ++i) fresh.Remove(0x200000000ull + u64(i) * 0x3000);
    const double bookkeeping = NsSince(start, 2.0 * kBuffers);

    BbEngine::GxSealed::Registry registry(1u << 17);
    registry.TrackRetired();
    for (u32 i = 0; i < kBuffers; ++i) {
        if (i & 7) registry.Seal(0x100000000ull + u64(i) * 0x3000, 0x2000);
    }
    for (u32 i = 0; i < kBuffers; i += 5) registry.RetireRange(0x100000000ull + u64(i) * 0x3000 + 0x10, 4, BbEngine::GxSealed::ByFault);
    const auto sorted_draws = [&] {
        for (std::size_t i = 0; i + 3 <= streams.size(); i += 3) {
            Draw draw(*memo);
            for (std::size_t k = i; k < i + 3; ++k) {
                const auto& [a, s] = streams[k];
                const Class kind = Classify(&buffers, *memo, ring, a, s);
                draw.Stream(kind, Arena, ring.Holds(a, s));
                draw.Sorted(Sort(&registry, *memo, kind, a, s, Arena, 1));
            }
            draw.Finish();
            const u64 index = streams[i].first + 0x1000;
            const Class kind = Classify(&buffers, *memo, ring, index, 0x300);
            Index(*memo, kind, Arena);
            IndexFate(*memo, Sort(&registry, *memo, kind, index, 0x300, Arena, 1));
            Flush(*memo);
        }
    };
    sorted_draws();
    start = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) sorted_draws();
    const double per_sorted_draw = NsSince(start, double(kFrames) * (streams.size() / 3));

    const auto recorded_draws = [&] {
        for (std::size_t i = 0; i + 3 <= streams.size(); i += 3) {
            Count(*memo, Gates + GateTried);
            Draw draw(*memo);
            for (std::size_t k = i; k < i + 3; ++k) {
                const auto& [a, s] = streams[k];
                u32 record = kRecordNone;
                const Class kind = Classify(&buffers, *memo, ring, a, s, &record);
                Record(*memo, StreamRecords, record, s);
                draw.Stream(kind, Arena, ring.Holds(a, s));
                draw.Sorted(Sort(&registry, *memo, kind, a, s, Arena, 1));
            }
            draw.Finish();
            Count(*memo, UnsealedDraws);
            const u64 index = streams[i].first + 0x1000;
            u32 record = kRecordNone;
            const Class kind = Classify(&buffers, *memo, ring, index, 0x300, &record);
            Record(*memo, IndexRecords, record, 0x300);
            Index(*memo, kind, Arena);
            const u32 cell = Sort(&registry, *memo, kind, index, 0x300, Arena, 1);
            IndexFate(*memo, cell);
            IndexCoverage(*memo, true, cell);
            Flush(*memo);
        }
    };
    recorded_draws();
    start = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) recorded_draws();
    const double per_recorded_draw = NsSince(start, double(kFrames) * (streams.size() / 3));

    std::printf("vertex census bench: %zu streams over %u buffers; memo lookup %.1f ns (%.2f%% misses), "
                "locked map lookup %.1f ns; with %.0f creates+releases/s beside it %.1f ns (%.2f%% misses); "
                "add or release %.1f ns; a draw of 3 streams and an index %.1f ns, %.1f ns sorted by GX object, "
                "%.1f ns with holder records, gate and coverage counts (sink %llu)\n",
                streams.size(), kBuffers, steady, 100.0 * steady_miss, locked, churn.load() / seconds, churned,
                100.0 * churn_miss, bookkeeping, per_draw, per_sorted_draw, per_recorded_draw,
                static_cast<unsigned long long>(sink & 1));
}
}

int main(int argc, char** argv) {
    Buffers b;
    auto owned = std::make_unique<Memo>();
    Memo& memo = *owned;
    const Ring ring{0x800000, 0x100000};

    Expect(b.Find(0x10000, 0x10) == NoBuffer, "nothing made yet");
    b.Add(0x10000, 0x1000, false);
    b.Add(0x20000, 0x800, true);
    b.Add(0x30000, 0, false);
    Expect(b.Find(0x10000, 0x1000) == InSealed, "the whole static buffer");
    Expect(b.Find(0x10800, 0x100) == InSealed, "a range inside it");
    Expect(b.Find(0x10f00, 0x200) == RunsPast, "a range running past its end");
    Expect(b.Find(0x0ff00, 0x200) == NoBuffer, "a range starting before it");
    Expect(b.Find(0x11000, 0x10) == NoBuffer, "the byte after it");
    Expect(b.Find(0x20100, 0x10) == InDynamic, "a dynamic buffer");
    Expect(b.Find(0x30000, 0x10) == NoBuffer, "an empty buffer is not recorded");

    Expect(Classify(&b, memo, ring, 0x10000, 0x10) == SealedGx, "classified sealed");
    Expect(Classify(&b, memo, ring, 0x20000, 0x10) == DynamicGx, "classified dynamic");
    Expect(Classify(&b, memo, ring, 0x10ff0, 0x20) == PastEnd, "classified past the end");
    Expect(Classify(&b, memo, ring, 0x800100, 0x100) == UploadRing, "classified upload ring");
    Expect(Classify(&b, memo, ring, 0x8fff00, 0x200) == Other, "a range leaving the ring is other");
    Expect(Classify(&b, memo, ring, 0x900000, 0x10) == Other, "classified other");
    Expect(Classify(nullptr, memo, ring, 0x10000, 0x10) == Other, "no buffers watched");

    memo.counts = {};
    Expect(Classify(&b, memo, ring, 0x10000, 0x10) == SealedGx && memo.counts.misses == 0, "a repeat is a memo hit");
    b.Remove(0x10000);
    Expect(Classify(&b, memo, ring, 0x10000, 0x10) == Other && memo.counts.misses == 1, "a release on the page voids the memo");
    b.Add(0x10000, 0x1000, true);
    Expect(Classify(&b, memo, ring, 0x10000, 0x10) == DynamicGx, "a new buffer at the same address");
    b.Add(0x10000, 0x1000, false);
    Expect(Classify(&b, memo, ring, 0x10000, 0x10) == SealedGx, "a create at a live base replaces it");
    b.Add(0x7f0000, 0x20000, false);
    Expect(Classify(&b, memo, ring, 0x80f000, 0x100) == SealedGx, "a GX buffer inside the ring is classed by the buffer");

    const u64 big = 0x100000000ull;
    Expect(Classify(&b, memo, ring, big + 0x100, 0x10) == Other, "before a large buffer");
    b.Add(big, u64(Buffers::kMaxBump + 2) << Buffers::kPageShift, false);
    Expect(Classify(&b, memo, ring, big + 0x100, 0x10) == SealedGx, "a large buffer bumps the global generation");

    b.Add(0x50000, 0x10000, false);
    b.Add(0x54000, 0x100, true);
    Expect(b.Find(0x54010, 0x10) == InDynamic, "the inner buffer of a nested pair");
    Expect(b.Find(0x58000, 0x100) == InSealed, "the outer buffer past the inner one");
    Expect(b.Find(0x54080, 0x100) == InSealed, "a range leaving the inner buffer but inside the outer one");
    b.Remove(0x12345);
    const Buffers::Stats stats = b.Snapshot();
    Expect(stats.made == 8 && stats.made_dynamic == 3, "made counts");
    Expect(stats.released == 1 && stats.unknown_releases == 1, "release counts");
    Expect(stats.overlapped == 2 && stats.live == 6, "overlap and live counts");

    Window last;
    memo.counts = {};
    Draw d1(memo);
    d1.Stream(SealedGx, Arena, false);
    d1.Stream(SealedGx, StreamCopy, false);
    d1.Finish();
    Draw d2(memo);
    d2.Stream(SealedGx, Arena, false);
    d2.Stream(UploadRing, StreamCopy, true);
    d2.Finish();
    Draw d3(memo);
    d3.Stream(DynamicGx, StreamCopy, true);
    d3.Finish();
    Draw(memo).Finish();
    Index(memo, SealedGx, Arena);
    Index(memo, Other, StreamCopy);
    memo.counts.lookups = 10;
    memo.counts.misses = 1;
    for (int i = 0; i < 5; ++i) Flush(memo);
    Expect(Read().draws == 0 && memo.counts.draws == 4, "counts wait on the stage B thread");
    Publish(memo);
    Expect(memo.counts.lookups == 0 && memo.unflushed == 0, "publishing clears the thread's counts");
    Window w = Take(last);
    Expect(w.draws == 4 && w.empty_draws == 1 && w.sealed_draws == 1, "draw counts");
    Expect(w.streams[SealedGx] == 3 && w.streams[UploadRing] == 1 && w.streams[DynamicGx] == 1, "stream counts");
    Expect(w.stream_paths[StreamCopy] == 3 && w.stream_paths[Arena] == 2, "path counts");
    Expect(w.ring_gx == 1, "a GX buffer inside the ring");
    Expect(w.indices[SealedGx] == 1 && w.index_paths[StreamCopy] == 1, "index counts");
    char line[1536];
    Format(line, sizeof line, "5 s", w, &stats);
    std::printf("%s\n", line);
    Expect(std::strstr(line, "Vertex data (5 s): 4 draws (1 without streams); streams 5: 60.0% sealed GX, 20.0% dynamic GX") != nullptr,
           "line start");
    Expect(std::strstr(line, "draws fully in sealed GX: 33.3%") != nullptr, "sealed draw share");
    Expect(std::strstr(line, "index 2: 50.0% sealed GX") != nullptr, "index share");
    Expect(std::strstr(line, "memo misses 10.00% of 10 lookups") != nullptr, "memo share");
    Expect(Take(last).draws == 0, "a second window starts empty");
    for (u32 i = 0; i < Memo::kFlushEvery; ++i) {
        Draw(memo).Finish();
        Flush(memo);
    }
    Expect(Take(last).draws == Memo::kFlushEvery && Read().draws == 4 + Memo::kFlushEvery, "published every 256 draws");
    Format(line, 64, "x", w, nullptr);
    Expect(std::strlen(line) == 63, "a short buffer is cut, not overrun");
    Format(line, sizeof line, "in all", Read(), nullptr);
    Expect(std::strstr(line, "GX buffers not watched") != nullptr, "unwatched");

    {
        namespace Sealed = BbEngine::GxSealed;
        Sealed::Registry reg(64);
        reg.TrackRetired();
        Buffers gx;
        for (u64 base : {0x100000ull, 0x110000ull, 0x120000ull, 0x130000ull}) {
            gx.Add(base, 0x1000, false);
            reg.Seal(base, 0x1000);
        }
        gx.Add(0x140000, 0x1000, true);
        gx.Add(0x150000, 0x1000, false);
        auto owned_memo = std::make_unique<Memo>();
        Memo& m = *owned_memo;
        const Ring r{0x800000, 0x100000};
        const auto sort = [&](u64 address, u64 size, Path path, u64 layout, const Sealed::Registry* registry) {
            return Sort(registry, m, Classify(&gx, m, r, address, size), address, size, path, layout);
        };
        Expect(sort(0x100000, 0x100, Arena, 5, &reg) == FateRefill, "a sealed stream is first a refill");
        Expect(sort(0x100000, 0x100, Arena, 5, &reg) == FateHit, "then a hit");
        Expect(sort(0x100000, 0x100, Arena, 6, &reg) == FateRefill, "a layout change refills");
        Expect(sort(0x100000, 0x100, Arena, 6, &reg) == FateHit, "and hits again");
        reg.RetireRange(0x100010, 4, Sealed::ByCpuWrite);
        Expect(sort(0x100000, 0x100, Arena, 6, &reg) == CellOf(FateRetired, Sealed::ByCpuWrite), "a retired hit names its reason");
        reg.RetireRange(0x110010, 4, Sealed::ByFault);
        Expect(sort(0x110000, 0x100, Arena, 6, &reg) == CellOf(FateRetired, Sealed::ByFault), "retired by a write fault");
        reg.RetireRange(0x120000, 0x1000, Sealed::ByEviction);
        Expect(sort(0x120800, 0x100, Arena, 6, &reg) == CellOf(FateRetired, Sealed::ByEviction), "retired by an eviction");
        Expect(Sealed::LayoutEvent(Sealed::ByEviction) && !Sealed::LayoutEvent(Sealed::ByFault), "eviction is a layout event, a fault is not");
        Expect(sort(0x130000, 0x100, StreamCopy, 6, &reg) == FateStreamCopied, "a sealed stream bound by stream copy");
        Expect(sort(0x130000, 0x100, Arena, 6, &reg) == FateRefill, "a stream copy leaves no hit behind");
        Expect(sort(0x130ff0, 0x20, Arena, 6, &reg) == FateOtherGx, "a stream running past a sealed end is not covered: the lookup refuses it");
        Expect(sort(0x140000, 0x100, Arena, 6, &reg) == FateDynamic, "dynamic");
        Expect(sort(0x800100, 0x100, StreamCopy, 6, &reg) == FateRing, "upload ring");
        Expect(sort(0x900000, 0x100, StreamCopy, 6, &reg) == FateNotGx, "not GX");
        Expect(sort(0x150000, 0x100, Arena, 6, &reg) == FateOtherGx, "a static buffer never sealed");
        Expect(sort(0x150000, 0x100, Arena, 6, nullptr) == FateOtherGx, "no registry");
        reg.Seal(0x110000, 0x1000);
        Expect(sort(0x110000, 0x100, Arena, 6, &reg) == FateRefill, "sealed again, the reason is gone");
        reg.Seal(0x160000, 0x1000);
        reg.RetireRange(0x160000, 0x1000, Sealed::ByRelease);
        Sealed::Retired why = Sealed::ByOverlap;
        Expect(!reg.RetiredAt(0x160000, &why), "a released buffer keeps no reason");
        Expect(reg.RetiredAt(0x120000, &why) && why == Sealed::ByEviction, "a retired buffer keeps its last reason");

        m.counts = {};
        Draw d1(m);
        d1.Sorted(FateHit);
        d1.Sorted(FateRefill);
        d1.Finish();
        IndexFate(m, FateHit);
        Draw d2(m);
        d2.Sorted(FateHit);
        d2.Sorted(CellOf(FateRetired, Sealed::ByFault));
        d2.Finish();
        Draw d3(m);
        d3.Sorted(FateDynamic);
        d3.Sorted(FateNotGx);
        d3.Finish();
        Draw d4(m);
        d4.Sorted(FateHit);
        d4.Finish();
        IndexFate(m, FateRing);
        Draw(m).Finish();
        IndexFate(m, FateNotGx);
        Publish(m);
        const Window c = Take(last);
        Expect(c.fated_draws == 4 && c.covered_draws == 2 && c.mixed_draws == 1, "covered and partly covered draws");
        const u32 fault = CellOf(FateRetired, Sealed::ByFault);
        Expect(c.blocked_any[fault] == 1 && c.blocked_only[fault] == 1, "a draw blocked only by a write fault");
        Expect(c.blocked_any[FateDynamic] == 1 && c.blocked_only[FateDynamic] == 0 && c.blocked_any[FateNotGx] == 1,
               "a draw with two blockers is in both, alone in neither");
        Expect(c.index_uncovered == 1 && c.index_fates[FateRing] == 1 && c.index_fates[FateHit] == 1 && c.index_fates[FateNotGx] == 1,
               "an index outside GX uncovers a covered draw once");
        Expect(c.fates[FateHit] == 3 && c.fates[fault] == 1, "stream fates");

        const RetiredCounts counts = Retirements(&reg);
        char objects[2048];
        FormatObjects(objects, sizeof objects, "5 s", c, &counts);
        std::printf("%s\n", objects);
        Expect(std::strstr(objects, "Vertex data by GX object (5 s): 4 draws, 50.0% fully covered, 25.0% partly") != nullptr, "objects line start");
        Expect(std::strstr(objects, "retired by write fault 25.0% 25.0%;") != nullptr, "blocker shares");
        Expect(std::strstr(objects, "retired 0 by overlapping seals, 2 at our own sites, 2 by the guest: release 1; CPU write 1; "
                                    "write fault 1; eviction 1;") != nullptr,
               "retirements split by overlapping seals, our own sites and the guest");
        reg.Seal(0x110800, 0x100);
        const RetiredCounts overlapped = Retirements(&reg);
        FormatObjects(objects, sizeof objects, "5 s", c, &overlapped);
        Expect(std::strstr(objects, "retired 1 by overlapping seals, 2 at our own sites, 2 by the guest:") != nullptr &&
                   std::strstr(objects, "overlapping seal 1;") != nullptr,
               "a seal over a sealed buffer is an overlapping seal");
        FormatObjects(objects, sizeof objects, "in all", c, nullptr);
        Expect(std::strstr(objects, "no sealed registry") != nullptr, "no registry");
        FormatObjects(objects, 40, "x", c, &counts);
        Expect(std::strlen(objects) == 39, "a short buffer is cut, not overrun");
    }

    {
        namespace Sealed = BbEngine::GxSealed;
        Buffers gx;
        gx.Add(0x200000, 0x1000, false, Sealed::HolderImmutable);
        gx.Add(0x210000, 0x1000, false, Sealed::HolderDefaultData);
        gx.Add(0x220000, 0x1000, false, Sealed::HolderDefaultEmpty);
        gx.Add(0x230000, 0x1000, true, Sealed::HolderDynamic);
        gx.Add(0x240000, 0x1000, false, Sealed::HolderOtherGx);
        auto owned = std::make_unique<Memo>();
        Memo& m = *owned;
        const Ring none{};
        const auto record_of = [&](u64 address, u64 size) {
            u32 record = kRecords;
            Classify(&gx, m, none, address, size, &record);
            return record;
        };
        Expect(record_of(0x200100, 0x100) == Sealed::HolderImmutable, "an immutable holder");
        Expect(record_of(0x200100, 0x100) == Sealed::HolderImmutable && m.counts.misses == 1, "the memo keeps the holder");
        Expect(record_of(0x210000, 0x1000) == Sealed::HolderDefaultData, "default with data");
        Expect(record_of(0x220000, 0x10) == Sealed::HolderDefaultEmpty, "default without data");
        Expect(record_of(0x230000, 0x10) == Sealed::HolderDynamic, "dynamic");
        Expect(record_of(0x240000, 0x10) == Sealed::HolderOtherGx, "another GX holder");
        Expect(record_of(0x200f00, 0x200) == kRecordPastEnd, "a record running past its holder's end");
        Expect(record_of(0x300000, 0x10) == kRecordNone, "no GX holder");
        gx.Remove(0x200000);
        Expect(record_of(0x200100, 0x100) == kRecordNone, "a released holder is gone from the memo too");

        Window last;
        Take(last);
        m.counts = {};
        Record(m, StreamRecords, Sealed::HolderImmutable, 0x300);
        Record(m, StreamRecords, Sealed::HolderImmutable, 0x100);
        Record(m, StreamRecords, Sealed::HolderDynamic, 0x400);
        Record(m, StreamRecords, kRecordNone, 0x800);
        Record(m, IndexRecords, Sealed::HolderDefaultData, 0x60);
        Record(m, IndexRecords, kRecordPastEnd, 0x20);
        Record(m, IndexRecords, 99, 0x20);
        Count(m, Gates + GateTried);
        Count(m, Gates + GateTried);
        Count(m, Gates + GateTried);
        Count(m, Gates + GateShadows);
        Count(m, SealedDraws);
        Count(m, UnsealedDraws);
        Count(m, UnsealedDraws);
        Count(m, SealedHits);
        Count(m, SealedMisses);
        IndexCoverage(m, true, FateHit);
        IndexCoverage(m, true, FateStreamCopied);
        IndexCoverage(m, true, FateNotGx);
        IndexCoverage(m, false, FateHit);
        Publish(m);
        const Window o = Take(last);
        Expect(o.extra[StreamRecords + Sealed::HolderImmutable] == 2 && o.extra[StreamBytes + Sealed::HolderImmutable] == 0x400,
               "records and bytes per holder");
        Expect(o.extra[IndexRecords + kRecordNone] == 1 && o.extra[IndexRecords + kRecordPastEnd] == 1,
               "an unknown record class counts as no holder");
        Expect(o.extra[IndexChecked] == 3 && o.extra[IndexSealed] == 2, "index coverage counts only draws own buffers tried");
        char own[2048];
        FormatOwn(own, sizeof own, "5 s", o);
        std::printf("%s\n", own);
        Expect(std::strstr(own, "Own buffers (5 s): by GX holder, vertex records 4: immutable 50.0% (25.0% of bytes); dynamic 25.0% "
                                "(25.0% of bytes); no GX holder 25.0% (50.0% of bytes);") != nullptr,
               "vertex records weighted per record and per byte");
        Expect(std::strstr(own, "index ranges 3: default with data 33.3% (60.0% of bytes); past a GX buffer's end 33.3% (20.0% of "
                                "bytes); no GX holder 33.3% (20.0% of bytes);") != nullptr,
               "index ranges per holder");
        Expect(std::strstr(own, "own-buffers gate per draw 4: tried 75.0%; BB_SHADOWS 25.0%;") != nullptr, "gate causes");
        Expect(std::strstr(own, "draws all sealed 1 of 3 tried (33.3%); sealed lookups 1 hits, 1 misses;") != nullptr, "sealed draws");
        Expect(std::strstr(own, "index ranges in a live sealed buffer 2 of 3 checked (66.7%)") != nullptr, "index coverage");
        FormatOwn(own, 50, "x", o);
        Expect(std::strlen(own) == 49, "a short buffer is cut, not overrun");
    }

    if (argc > 1 && !std::strcmp(argv[1], "--bench")) Bench();
    if (failures) {
        std::printf("vertex census: %d failures\n", failures);
        return 1;
    }
    std::printf("vertex census: all tests passed\n");
    return 0;
}
