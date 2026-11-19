// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/gx_whole_binds.h"
#include "gpu/shim/engine/gx_sealed.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace V = BbEngine::GxWholeBinds;
namespace W = BbEngine::GxWhole;
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

constexpr u64 kPipeline = 0x5000, kGeneration = 12;
constexpr u32 kStreams = 3;

std::unique_ptr<V::VertexKey> Key() {
    auto key = std::make_unique<V::VertexKey>();
    std::memset(key.get(), 0, sizeof *key);
    key->pipeline = kPipeline;
    key->bind_generation = kGeneration;
    key->count = kStreams;
    key->step_rate[0] = 1;
    key->step_rate[1] = 2;
    for (u32 i = 0; i < kStreams; ++i) {
        for (u32 w = 0; w < V::kSharpWords; ++w) key->sharps[i][w] = 0x100 * i + w;
        key->keys[i] = 0xabc0 + i;
    }
    return key;
}

void TestClassify() {
    using M = V::Miss;
    const auto memo = Key();
    const auto now = Key();
    Check(V::Classify(false, 1, *memo, *now, 1) == M::First, "an empty memo is a first use");
    Check(V::Classify(true, 1, *memo, *now, 1) == M::Hit, "the same streams right after are a hit");
    Check(V::Classify(true, 1, *memo, *now, 2) == M::Interleaved, "another resolve in between is a miss");
    auto other = Key();
    other->pipeline = kPipeline + 8;
    Check(V::Classify(true, 1, *memo, *other, 1) == M::Pipeline, "another pipeline is a miss");
    other = Key();
    other->bind_generation = kGeneration + 1;
    Check(V::Classify(true, 1, *memo, *other, 1) == M::Generation, "a changed buffer cache is a miss");
    other = Key();
    other->step_rate[1] = 3;
    Check(V::Classify(true, 1, *memo, *other, 1) == M::StepRate, "other step rates are a miss");
    other = Key();
    other->sharps[kStreams - 1][1] ^= 1;
    Check(V::Classify(true, 1, *memo, *other, 1) == M::Streams, "a change in the last stream's V# is a miss");
    other = Key();
    other->keys[1] ^= 1;
    Check(V::Classify(true, 1, *memo, *other, 1) == M::Objects, "another GX buffer version is a miss");
}

void TestKeyed() {
    const V::Stream keyed[2] = {{0x1000, 0x100, 7}, {0, 0, 0}};
    Check(V::AllKeyed(keyed), "streams with memory carry a GX object, empty streams need none");
    const V::Stream unkeyed[2] = {{0x1000, 0x100, 7}, {0x2000, 0x40, 0}};
    Check(!V::AllKeyed(unkeyed), "a stream with memory and no GX object is not bound by object");
}

struct Sealed {
    BbEngine::GxSealed::Registry registry{64};
    bool on = true;
    mutable u32 lookups = 0;
    std::vector<V::Extent> Extents(std::span<const V::Stream> streams) const {
        ++lookups;
        std::vector<u64> bases, sizes;
        for (const V::Stream& s : streams) {
            bases.push_back(s.base);
            sizes.push_back(s.base ? s.size : 0);
        }
        std::vector<BbEngine::GxSealed::Hit> hits(streams.size());
        std::unique_ptr<bool[]> found(new bool[streams.size() + 1]());
        registry.FindAll(bases.data(), sizes.data(), u32(streams.size()), hits.data(), found.get());
        std::vector<V::Extent> out;
        for (std::size_t i = 0; i < streams.size(); ++i) out.push_back({found[i], hits[i].id, hits[i].generation, hits[i].base, hits[i].size});
        return out;
    }
    V::Miss Verdict(std::span<const V::Stream> streams) const {
        const std::vector<V::Extent> extents = Extents(streams);
        return V::HeldMiss(on, streams, extents, BbEngine::GxSealed::Holds);
    }
    bool Held(std::span<const V::Stream> streams) const { return Verdict(streams) == V::Miss::Hit; }
    V::Miss Take(std::span<const V::Stream> streams, std::span<const V::Range> ranges, V::SealCache& cache, bool allowed,
                 const std::vector<u64>& written) const {
        const auto seal_once = [&](V::SealCache& c) {
            const std::vector<V::Extent> extents = Extents(streams);
            return V::SealOnce(on, streams, extents, BbEngine::GxSealed::Holds, c);
        };
        const auto valid = [this](u32 id, u32 generation) { return registry.Valid({id, generation, 0, 0}); };
        const auto clean = [&written](u64 base, u64 size, bool in_place) {
            if (in_place) return true;
            for (u64 page : written)
                if (page >= base && page < base + size) return false;
            return true;
        };
        return V::TakeMiss(allowed, cache, seal_once, valid, ranges, clean);
    }
};

void TestTakenHits() {
    using M = V::Miss;
    Sealed s;
    Check(s.registry.Seal(0x10000, 0x1000) != 0 && s.registry.Seal(0x20000, 0x1000) != 0, "two sealed buffers for the taken hits");
    const int arena = 1;
    const V::Stream streams[3] = {{0x10000, 0x100, 1}, {0, 0, 0}, {0x20000, 0x80, 2}};
    const V::Range ranges[2] = {{&arena, 0, 0x10000, 0x100, true, false}, {&arena, 0x100, 0x20000, 0x80, true, false}};
    V::SealCache cache;
    const std::vector<u64> none, written_vb = {0x10040}, written_ib = {0x20010};
    Check(s.Take(streams, ranges, cache, true, none) == M::Hit && cache.checked && cache.count == 2, "the first hit finds its seals once");
    Check(s.lookups == 1, "the first hit takes one registry lookup for all its streams");
    Check(s.Take(streams, ranges, cache, true, none) == M::Hit && s.lookups == 1, "later hits check their seals without a lookup");
    Check(s.Take(streams, ranges, cache, true, written_vb) == M::CpuModified, "a taken hit over a range the CPU wrote falls back to the register path");
    const V::Range in_place[2] = {{&arena, 0, 0x10000, 0x100, true, true}, {&arena, 0x100, 0x20000, 0x80, true, false}};
    Check(s.Take(streams, in_place, cache, true, written_vb) == M::Hit, "a range bound in place needs no clean check");
    Check(s.Take(streams, in_place, cache, true, written_ib) == M::CpuModified, "a written range not bound in place is still refused");
    Check(s.Take(streams, ranges, cache, false, none) == M::Gated, "with buffer cache memos refused nothing is taken");
    s.registry.RetireRange(0x20000, 0x10, BbEngine::GxSealed::ByCpuWrite);
    Check(s.Take(streams, ranges, cache, true, none) == M::Retired, "a seal retired after the first hit refuses the take");
    V::SealCache fresh;
    Check(s.Take(streams, ranges, fresh, true, none) == M::Unsealed, "a fresh lookup names the retired buffer as unsealed");
    Sealed off;
    off.on = false;
    V::SealCache none_cache;
    Check(off.Take(streams, ranges, none_cache, true, none) == M::NoSeals && !none_cache.checked, "with sealing off nothing is taken");
    const V::Stream index[1] = {{0x10000, 0x40, 7}};
    const V::Range index_range[1] = {{&arena, 0, 0x10000, 0x40, true, false}};
    V::SealCache index_cache;
    Check(s.Take(index, index_range, index_cache, true, none) == M::Hit, "an index range in a sealed buffer is taken");
    const std::vector<u64> written_index = {0x10020};
    Check(s.Take(index, index_range, index_cache, true, written_index) == M::CpuModified,
          "a kept index range the CPU wrote since is refused on every later take");
    V::VertexChain chain;
    V::BeginResolve(chain);
    V::Note(chain, ranges[0]);
    V::Keep(chain);
    Check(s.Take(streams, std::span<const V::Range>(chain.ranges.data(), chain.num_ranges), chain.seals, true, none) == M::Unsealed,
          "a kept chain looks up its seals at its first hit");
    chain.seals.checked = true;
    chain.seals.count = 0;
    V::Keep(chain);
    Check(!chain.seals.checked && chain.seals.count == 0, "a new memo forgets the seals of the old one");
}

void TestHeld() {
    Sealed s;
    Check(s.registry.Seal(0x10000, 0x1000) != 0 && s.registry.Seal(0x11000, 0x1000) != 0, "two adjacent sealed buffers");
    const V::Stream inside[2] = {{0x10000, 0x1000, 1}, {0x11100, 0x80, 2}};
    Check(s.Held(inside), "streams inside their sealed buffers are held");
    const V::Stream past[1] = {{0x10f00, 0x200, 1}};
    Check(!s.Held(past), "a stream running past its sealed buffer's end is refused");
    const V::Stream outside[1] = {{0x20000, 0x10, 1}};
    Check(!s.Held(outside), "a stream in no sealed buffer is refused");
    const V::Stream empty[1] = {{0, 0, 0}};
    Check(s.Held(empty), "an empty stream needs no sealed buffer");
    Check(s.Verdict(past) == V::Miss::NotHeld, "a stream past its sealed buffer's end is named as such");
    Check(s.Verdict(outside) == V::Miss::Unsealed, "a stream in no sealed buffer is named as such");
    const V::Stream mixed[2] = {{0x10000, 0x100, 1}, {0x20000, 0x10, 2}};
    Check(s.Verdict(mixed) == V::Miss::Unsealed, "one unsealed stream refuses the whole draw");
    Sealed off;
    off.on = false;
    Check(off.registry.Seal(0x10000, 0x1000) != 0, "a seal in a registry the engine does not use");
    Check(off.Verdict(inside) == V::Miss::NoSeals && off.Verdict(empty) == V::Miss::NoSeals, "with sealing off nothing is held");
    std::vector<V::Stream> sweep;
    u32 refused = 0;
    for (u64 stride : {u64(4), u64(12), u64(16), u64(32)}) {
        for (u64 first = 0; first + stride <= 0x1000; first += stride * 5) {
            const u64 bytes = (0x1000 - first) / stride * stride;
            const V::Stream one[1] = {{0x10000 + first, bytes, 1}};
            refused += !s.Held(one);
        }
    }
    Check(refused == 0, "every stream range a bind inside one sealed buffer can ask for is held");
}

void TestIndex() {
    const V::IndexKey memo{9, 0x4000, 0x600, kGeneration, 1, 0};
    Check(V::IndexHit(true, memo, memo), "the same index range is a hit");
    Check(!V::IndexHit(false, memo, memo), "no memo is no hit");
    V::IndexKey other = memo;
    other.key = 10;
    Check(!V::IndexHit(true, memo, other), "another GX index buffer version is a miss");
    other = memo;
    other.bytes = 0x300;
    Check(!V::IndexHit(true, memo, other), "another index count is a miss");
    other = memo;
    other.key = 0;
    Check(!V::IndexHit(true, other, other), "an index without a GX object is never taken");
}

void TestSuspectAndLine() {
    V::Counts c;
    c.resolves = 10;
    Check(V::InstrumentSuspect(W::Mode::Off, c), "resolves with none covered are suspect");
    c.covered = V::kSuspectCovered;
    Check(V::InstrumentSuspect(W::Mode::Feed, c), "many covered resolves and no hit are suspect");
    c.hits = 5;
    Check(!V::InstrumentSuspect(W::Mode::Feed, c), "hits are fine");
    c.taken = 4;
    c.vertex_cycles = 3000;
    c.vertex_timed = 1;
    c.misses[u32(V::Miss::NotHeld)] = 2;
    c.misses[u32(V::Miss::Unsealed)] = 3;
    c.kept = 7;
    c.index_misses[u32(V::Miss::NotPersistent)] = 5;
    char line[2048];
    const std::size_t used = V::FormatLine(line, sizeof line, W::Mode::Feed, c, 1000.0);
    Check(used < sizeof line && std::strstr(line, "10 vertex resolves, 64 covered, 7 memos kept") != nullptr, "the line gives total, covered and kept");
    Check(std::strstr(line, "in no sealed GX buffer 3") != nullptr, "the line names unsealed streams apart");
    Check(std::strstr(line, "(a range in the stream ring or a shadow 5)") != nullptr, "the line names why index ranges were not memoized");
    Check(std::strstr(line, "not inside its sealed GX buffer 2") != nullptr && std::strstr(line, "12.0 us vertex") != nullptr,
          "the line names misses and skipped time");
    V::Counts sealed_off;
    sealed_off.resolves = sealed_off.covered = sealed_off.hits = 1;
    sealed_off.misses[u32(V::Miss::NoSeals)] = 1;
    Check(V::InstrumentSuspect(W::Mode::Feed, sealed_off), "covered draws with no seal registry are suspect");
    Check(!V::InstrumentSuspect(W::Mode::Off, sealed_off), "with GX whole off a missing registry is not suspect");
    sealed_off.misses[u32(V::Miss::NoSeals)] = 0;
    sealed_off.index_misses[u32(V::Miss::NoSeals)] = 1;
    Check(V::InstrumentSuspect(W::Mode::Compare, sealed_off), "an index range with no seal registry is suspect");
    char seals[256];
    V::FormatSeals(seals, sizeof seals, false, 0, 0, 0);
    Check(std::strstr(seals, "no seal registry") != nullptr, "the seal line says when there is no registry");
    V::FormatSeals(seals, sizeof seals, true, 3, 9, 4);
    Check(std::strstr(seals, "3 GX buffers sealed now, 9 sealed in all, 4 write and release notes") != nullptr, "the seal line gives the registry's levels");
    BbEngine::GxSealed::Registry registry(8);
    registry.Seal(0x1000, 0x100);
    registry.RetireRange(0x5000, 0x10, BbEngine::GxSealed::ByGpuWrite);
    registry.RetireRange(0x1000, 0x10, BbEngine::GxSealed::ByCpuWrite);
    registry.RetireRange(0x1000, 0, BbEngine::GxSealed::ByCpuWrite);
    Check(registry.Notes() == 2 && registry.Live() == 0 && registry.Sealed() == 1, "the registry counts every write note, retiring or not");
    char tiny[16];
    std::memset(tiny, 'x', sizeof tiny);
    const std::size_t small = V::FormatLine(tiny, 8, W::Mode::Feed, c, 1000.0);
    Check(small < 8 && tiny[8] == 'x', "a short buffer is never overrun");
}

void TestChainAndPersistence() {
    using M = V::Miss;
    const auto key = Key();
    V::VertexChain chain;
    V::BeginResolve(chain);
    const int ring = 1, arena = 2;
    V::Note(chain, {&arena, 0x40, 0x10000, 0x100, true, false});
    const V::Range noted_persistent[1] = {{&arena, 0x40, 0x10000, 0x100, true, false}};
    const V::Range noted_ring[2] = {{&arena, 0x40, 0x10000, 0x100, true, false}, {&ring, 0x2000, 0x11000, 0x80, false, false}};
    Check(V::MemoVerdict(true, noted_persistent) == M::Hit, "keyed, persistent ranges are memoized");
    Check(V::MemoVerdict(true, noted_ring) == M::NotPersistent, "a range in the stream ring or a shadow is never memoized");
    Check(V::MemoVerdict(false, noted_persistent) == M::Unkeyed, "a stream without a GX object is never memoized");
    V::Keep(chain);
    Check(chain.num_ranges == 1 && chain.ranges[0].base == 0x10000, "the kept memo holds the noted ranges");
    Check(V::Classify(chain.valid, chain.memo_serial, *key, *key, chain.serial) == M::Hit, "the next covered draw hits");
    const u64 before = chain.serial;
    V::Took(chain);
    Check(chain.serial == before + 1 && chain.memo_serial == chain.serial, "a take moves the chain on");
    Check(V::Classify(chain.valid, chain.memo_serial, *key, *key, chain.serial) == M::Hit, "a take keeps the memo for the next draw");
    V::BeginResolve(chain);
    Check(chain.num_noted == 0, "a register resolve starts with no noted ranges");
    Check(V::Classify(chain.valid, chain.memo_serial, *key, *key, chain.serial) == M::Interleaved, "another resolve in between breaks the chain");
    V::Drop(chain);
    Check(V::Classify(chain.valid, chain.memo_serial, *key, *key, chain.serial) == M::First, "a dropped memo is a first use");
    Check(V::IndexVerdict(false) == M::NotPersistent, "an index range in the stream ring is never memoized");
    Check(V::IndexVerdict(true) == M::Hit, "a persistent index range is memoized");
}

void TestSealsForWhole() {
    using W::Mode;
    Check(V::SealsForWhole(true, Mode::Feed, false), "GX whole with records seals GX buffers");
    Check(V::SealsForWhole(true, Mode::Compare, false), "GX whole's compare mode seals GX buffers too");
    Check(!V::SealsForWhole(false, Mode::Feed, false) && !V::SealsForWhole(false, Mode::Compare, false),
          "without GX records nothing is sealed for GX whole");
    Check(!V::SealsForWhole(true, Mode::Off, false) && !V::SealsForWhole(false, Mode::Off, false), "with GX whole off nothing is sealed for it");
    Check(!V::SealsForWhole(true, Mode::Feed, true) && !V::SealsForWhole(true, Mode::Compare, true),
          "LANTERN_GX_BINDS=0 seals nothing for GX whole");
    Check(V::VersionsWanted(false, false, Mode::Feed, false) && V::VersionsWanted(false, false, Mode::Compare, false),
          "GX whole versions GX buffers by their updates");
    Check(V::VersionsWanted(true, false, Mode::Off, false) && V::VersionsWanted(false, true, Mode::Off, false),
          "records compared or used still version GX buffers");
    Check(!V::VersionsWanted(false, false, Mode::Off, false), "records alone do not version GX buffers");
    Check(!V::VersionsWanted(false, false, Mode::Feed, true) && !V::VersionsWanted(false, false, Mode::Compare, true),
          "LANTERN_GX_BINDS=0 versions nothing for GX whole");
    Check(V::VersionsWanted(true, false, Mode::Feed, true) && V::VersionsWanted(false, true, Mode::Off, true),
          "LANTERN_GX_BINDS=0 leaves the versions records compared or used install");
    Check(!V::BindsByObjectWanted(false, false, Mode::Feed) && !V::BindsByObjectWanted(false, false, Mode::Compare),
          "GX whole alone keeps the register path binding by address");
    Check(V::BindsByObjectWanted(true, false, Mode::Off) && V::BindsByObjectWanted(false, true, Mode::Feed),
          "records compared or used bind by object as before");
    Check(!V::ObjectBind(0x55, false), "a GX object key the whole draw carries is not a bind by object");
    Check(V::ObjectBind(0x55, true) && !V::ObjectBind(0, true), "with binds by object on, a keyed range binds by object");
}

int RunSuite() {
    g_failed = 0;
    TestClassify();
    TestKeyed();
    TestHeld();
    TestTakenHits();
    TestIndex();
    TestChainAndPersistence();
    TestSealsForWhole();
    TestSuspectAndLine();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_GX_BINDS_MUTANTS
    using V::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        V::selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("gx whole binds mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("gx whole binds mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("gx whole binds mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("gx whole binds mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("gx whole binds: %d failures\n", g_failed);
        return 1;
    }
    std::printf("gx whole binds: all tests passed\n");
    return 0;
#endif
}
