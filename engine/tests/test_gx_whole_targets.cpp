// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/gx_whole_targets.h"
#include "gpu/shim/engine/gx_whole_stageb.h"
#include <cstdio>
#include <cstring>
#include <memory>

namespace T = BbEngine::GxWholeTargets;
namespace B = BbEngine::GxWholeStageB;
namespace W = BbEngine::GxWhole;
namespace G = BbEngine::GxBindings;
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

constexpr u32 kGbufferMask = 0x1f, kTwoTargets = 0x3, kAllTargets = 0xff, kColorControl = 0x00cc0010;
constexpr u64 kGeneration = 77, kEpoch = 5;

T::Identity Gbuffer() {
    T::Identity id{};
    for (u32 i = 0; i < T::kSlots; ++i) id.views[i] = 0x7000000000ull + 0x100 * i;
    id.epoch = kEpoch;
    id.color_valid = kGbufferMask;
    id.depth_valid = 1;
    id.target_mask = 0xfffff;
    id.depth_control = 0x77;
    for (u32 i = 0; i < T::kColorSlots; ++i)
        for (u32 w = 0; w < W::kColorWords; ++w) id.color_words[i][w] = 0x1000 * (i + 1) + w;
    for (u32 w = 0; w < W::kDepthWords + 4; ++w) id.depth_words[w] = 0x9000 + w;
    return id;
}

std::array<u32, T::kSlots> Extents() {
    std::array<u32, T::kSlots> e{};
    for (u32 i = 0; i < T::kSlots; ++i) e[i] = (1080u << 16) | 1920u;
    return e;
}

T::Key KeyOf(const T::Identity& id, u32 mrt = kGbufferMask, u64 generation = kGeneration) {
    return T::MakeKey(id, mrt, kColorControl, Extents(), generation);
}

void TestCanonicalKey() {
    T::Identity a = Gbuffer(), b = Gbuffer();
    b.views[5] = 0x1234;
    b.views[7] = 0x5678;
    b.color_valid |= 0x80;
    const T::Key ka = KeyOf(a, kTwoTargets), kb = KeyOf(b, kTwoTargets);
    Check(std::memcmp(&ka, &kb, sizeof ka) == 0, "slots past the pipeline's targets do not enter the key");
    const T::Key full_a = KeyOf(a, kAllTargets), full_b = KeyOf(b, kAllTargets);
    Check(std::memcmp(&full_a, &full_b, sizeof full_a) != 0, "slots the pipeline uses do enter the key");
    Check(ka.views[T::kDepthSlot] == a.views[T::kDepthSlot] && ka.extents[T::kDepthSlot] == Extents()[T::kDepthSlot], "the depth slot is always keyed");
    Check(ka.color_valid == (kGbufferMask & kTwoTargets), "valid targets are masked to the pipeline's slots");
}

void TestClassify() {
    using M = T::Miss;
    const T::Key key = KeyOf(Gbuffer());
    T::Memo memo;
    Check(T::Classify(memo, key, 1) == M::First, "an empty memo is a first use");
    T::Remember(memo, key, 1);
    Check(T::Classify(memo, key, 1) == M::Hit, "the same targets right after are a hit");
    Check(T::Classify(memo, key, 2) == M::Interleaved, "another preparation in between is a miss");
    T::Identity id = Gbuffer();
    id.epoch = kEpoch + 1;
    Check(T::Classify(memo, KeyOf(id), 1) == M::Epoch, "a reused GX object is a miss");
    Check(T::Classify(memo, KeyOf(Gbuffer(), kGbufferMask, kGeneration + 1), 1) == M::Generation, "a changed image registry is a miss");
    id = Gbuffer();
    id.views[2] += 0x10;
    Check(T::Classify(memo, KeyOf(id), 1) == M::Views, "another color view is a miss");
    id = Gbuffer();
    id.views[T::kDepthSlot] += 0x10;
    Check(T::Classify(memo, KeyOf(id), 1) == M::Views, "another depth view is a miss");
    id = Gbuffer();
    id.color_valid = 0x0f;
    Check(T::Classify(memo, KeyOf(id), 1) == M::Valid, "another set of valid targets is a miss");
    id = Gbuffer();
    id.depth_control = 0x70;
    Check(T::Classify(memo, KeyOf(id), 1) == M::Control, "another depth control is a miss");
    T::Key other = key;
    other.extents[3] = 0x02000200;
    Check(T::Classify(memo, other, 1) == M::Extents, "other extents are a miss");
    other = key;
    other.color_control = 0;
    Check(T::Classify(memo, other, 1) == M::Control, "another color control is a miss");
    id = Gbuffer();
    id.color_words[1][3] ^= 0x40;
    Check(T::Classify(memo, KeyOf(id), 1) == M::Words, "a view changed in place (other CB words) is a miss");
    id = Gbuffer();
    id.depth_words[0] ^= 0x40;
    Check(T::Classify(memo, KeyOf(id), 1) == M::Words, "a depth view changed in place (other DB words) is a miss");
    id = Gbuffer();
    id.color_words[6][0] ^= 1;
    Check(T::Classify(memo, KeyOf(id), 1) == M::Hit, "words of a slot the pipeline doesn't use are not keyed");
    T::Identity a = Gbuffer(), b = Gbuffer();
    a.depth_valid = b.depth_valid = 0;
    b.depth_words[2] ^= 1;
    const T::Key ka = KeyOf(a), kb = KeyOf(b);
    Check(std::memcmp(&ka, &kb, sizeof ka) == 0, "depth words of an invalid depth target are not keyed");
}

void TestChain() {
    using M = T::Miss;
    const T::Key key = KeyOf(Gbuffer());
    T::Chain chain;
    Check(T::Look(chain, key, false) == M::MemoOff, "the texture binding memo switch turns the memo off");
    T::NotePrepare(chain);
    T::Found(chain, key);
    T::BeginConfirmed(chain);
    Check(T::Look(chain, key, true) == M::Hit, "a register preparation leaves a memo the next covered draw hits");
    T::Took(chain, key);
    Check(T::BeginFastNow(chain, true, true, true), "a take right after a confirmed begin memo skips the signature");
    T::BeginConfirmed(chain);
    Check(T::Look(chain, key, true) == M::Hit, "a take keeps the chain for the next draw");
    T::NotePrepare(chain);
    Check(T::Look(chain, key, true) == M::Interleaved, "a preparation of another draw in between breaks the chain");
    T::NotePrepare(chain);
    Check(!T::BeginFastNow(chain, true, true, true), "the begin memo of two preparations back is not trusted");
    T::Found(chain, key);
    T::Rebound(chain);
    Check(T::Look(chain, key, true) == M::First, "a target rebound in begin rendering drops the memo");
}

void TestIdentity() {
    auto state = std::make_unique<G::State>();
    std::memset(static_cast<void*>(state.get()), 0, sizeof *state);
    state->render.color_valid = 0x3;
    state->render.depth_valid = 1;
    state->render.blend[0] = 0xff;
    state->render.depth_stencil[0] = 0xffffffff;
    for (u32 i = 0; i < T::kSlots; ++i) state->target_views[i] = 0x9000 + i;
    state->objects_epoch = 9;
    state->whole.captured = 1;
    const B::Carried carried = B::Carry(*state);
    const T::Identity id = T::IdentityOf(carried.render, carried.target_views, carried.objects_epoch);
    Check(id.views[0] == 0x9000 && id.views[T::kDepthSlot] == 0x9000 + T::kDepthSlot && id.epoch == 9, "views and epoch travel with the record");
    Check(id.target_mask == 0xff && id.depth_control == W::kDepthControlMask, "the target mask and the masked depth control key the targets");
}

void TestBeginFast() {
    Check(T::BeginFast(true, 4, 4, true, true), "a target hit right after a begin memo hit skips the signature");
    Check(!T::BeginFast(false, 4, 4, true, true), "without a target hit the signature is built");
    Check(!T::BeginFast(true, 3, 4, true, true), "a begin memo from an older preparation needs the signature");
    Check(!T::BeginFast(true, 4, 4, false, true), "no begin memo needs the signature");
    Check(!T::BeginFast(true, 4, 4, true, false), "other scalars need the signature");
}

void TestSuspectAndCost() {
    T::Counts c;
    c.prepares = 10;
    Check(T::InstrumentSuspect(W::Mode::Off, c), "preparations with none covered are suspect");
    c.covered = T::kSuspectCovered;
    Check(T::InstrumentSuspect(W::Mode::Feed, c), "many covered preparations and no memo hit are suspect");
    c.hits = 10;
    Check(!T::InstrumentSuspect(W::Mode::Feed, c), "hits are fine");
    c.compared = 3;
    c.differ = 3;
    Check(T::InstrumentSuspect(W::Mode::Compare, c), "every compare differing is suspect");
    Check(T::SkippedMicroseconds(10, 3000, 3, 1000.0) == 10.0, "skipped time is skips times the mean timed cost");
    Check(T::SkippedMicroseconds(10, 3000, 0, 1000.0) == 0.0, "no timed lookup gives no estimate");
}

void TestFormat() {
    T::Counts c;
    c.prepares = 100;
    c.covered = 90;
    c.hits = 80;
    c.taken = 79;
    c.misses[u32(T::Miss::Views)] = 10;
    c.slot_id_differ[T::kDepthSlot] = 1;
    c.find_cycles = 2000;
    c.finds_timed = 1;
    char line[2048];
    const std::size_t used = T::FormatLine(line, sizeof line, W::Mode::Feed, c, 1000.0);
    Check(used < sizeof line && std::strstr(line, "100 target preparations, 90 covered") != nullptr, "the line gives total and covered");
    Check(std::strstr(line, "other target views 10") != nullptr && std::strstr(line, "depth image 1") != nullptr, "misses and slot differences are named");
    Check(std::strstr(line, "158.0 us targets") != nullptr, "skipped microseconds are printed");
    char tiny[16];
    std::memset(tiny, 'x', sizeof tiny);
    const std::size_t small = T::FormatLine(tiny, 8, W::Mode::Feed, c, 1000.0);
    Check(small < 8 && tiny[8] == 'x', "a short buffer is never overrun");
}

int RunSuite() {
    g_failed = 0;
    TestCanonicalKey();
    TestClassify();
    TestIdentity();
    TestBeginFast();
    TestChain();
    TestSuspectAndCost();
    TestFormat();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_GX_TARGETS_MUTANTS
    using T::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        T::selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("gx whole targets mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("gx whole targets mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("gx whole targets mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("gx whole targets mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("gx whole targets: %d failures\n", g_failed);
        return 1;
    }
    std::printf("gx whole targets: all tests passed\n");
    return 0;
#endif
}
