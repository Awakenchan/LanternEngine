// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gpu/shim/engine/stageb_counts.h"

namespace {
namespace Counts = BbEngine::StageBCounts;
using Counts::Seen;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

int failures = 0;
bool quiet = false;

void Expect(bool ok, const char* what) {
    if (!ok) {
        if (!quiet) std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<u64> Colliding(u32 count, u32 size) {
    std::vector<u64> keys;
    const u32 target = Counts::ShareSlotOf(1, size);
    keys.push_back(1);
    for (u64 h = 2; keys.size() < count && h < (u64(1) << 26); ++h) {
        if (Counts::ShareSlotOf(h, size) == target) keys.push_back(h);
    }
    return keys;
}

void TestLook() {
    Counts::ShareTable t;
    Counts::NewDraw(t);
    Expect(Counts::Look(t, 100, 64) == Seen::New, "first upload is new");
    Expect(Counts::Look(t, 100, 64) == Seen::SameDraw, "the same bytes twice in one draw are not an earlier draw's");
    Expect(Counts::Look(t, 100, 128) == Seen::New, "the same hash with another size is new");
    Counts::NewDraw(t);
    Expect(Counts::Look(t, 100, 64) == Seen::EarlierDraw, "the next draw's identical upload is shared");
    Expect(Counts::Look(t, 101, 64) == Seen::New, "other bytes are new");
    Counts::NewSubmission(t);
    Counts::NewDraw(t);
    Expect(Counts::Look(t, 100, 64) == Seen::New, "a new submission forgets the last one");
    Counts::NewDraw(t);
    Expect(Counts::Look(t, 100, 64) == Seen::EarlierDraw, "and shares within itself again");
}

void TestProbes() {
    const std::vector<u64> keys = Colliding(Counts::kShareProbes + 1, 32);
    Expect(keys.size() == Counts::kShareProbes + 1, "found colliding keys");
    if (keys.size() != Counts::kShareProbes + 1) return;
    Counts::ShareTable t;
    Counts::NewDraw(t);
    for (u32 i = 0; i < Counts::kShareProbes; ++i) {
        Expect(Counts::Look(t, keys[i], 32) == Seen::New, "colliding uploads take the next slots");
    }
    Expect(Counts::Look(t, keys[Counts::kShareProbes], 32) == Seen::Untracked, "a full probe run is untracked");
    Counts::NewDraw(t);
    Expect(Counts::Look(t, keys[1], 32) == Seen::EarlierDraw, "a colliding upload is found past the first slot");
    Expect(Counts::Look(t, keys[Counts::kShareProbes - 1], 32) == Seen::EarlierDraw, "and at the last probe");
    Counts::NewSubmission(t);
    Counts::NewDraw(t);
    Expect(Counts::Look(t, keys[Counts::kShareProbes], 32) == Seen::New, "stale slots are taken again");

    u32 other = 0;
    for (u32 size = 33; size < (1u << 24) && !other; ++size) {
        if (Counts::ShareSlotOf(77, size) == Counts::ShareSlotOf(77, 32)) other = size;
    }
    Expect(other != 0, "found a size sharing the slot");
    Counts::ShareTable s;
    Counts::NewDraw(s);
    Expect(Counts::Look(s, 77, 32) == Seen::New, "upload in its slot");
    Counts::NewDraw(s);
    Expect(Counts::Look(s, 77, other) == Seen::New, "the same hash with another size in the same slot is new");
}

void TestTally() {
    Counts::ShareTable t;
    Counts::NewDraw(t);
    Expect(Counts::NoteUpload(t, 7, 256, 100, 5) == Seen::New, "upload noted");
    Counts::NoteUnhashed(t, 64, 30);
    Counts::NewDraw(t);
    Expect(Counts::NoteUpload(t, 7, 256, 90, 5) == Seen::EarlierDraw, "shared upload noted");
    Counts::NewDraw(t);
    Counts::NoteUpload(t, 7, 256, 80, 5);
    Counts::NoteUpload(t, 9, 16, 10, 1);
    Counts::NoteUpload(t, 9, 16, 10, 1);
    Counts::NewDraw(t);
    const u64* v = t.tally;
    Expect(v[Counts::Uploads] == 6, "uploads counted");
    Expect(v[Counts::UploadBytes] == 256 * 3 + 64 + 32, "upload bytes counted");
    Expect(v[Counts::UploadCycles] == 100 + 30 + 90 + 80 + 20, "upload cycles counted");
    Expect(v[Counts::HashCycles] == 17, "hash cycles counted");
    Expect(v[Counts::Unhashed] == 1, "unhashed counted");
    Expect(v[Counts::Shared] == 2 && v[Counts::SharedBytes] == 512 && v[Counts::SharedCycles] == 170,
           "shared uploads, bytes and cycles counted");
    Expect(v[Counts::SameDraw] == 1, "same-draw repeat counted");
    Expect(v[Counts::Draws] == 3, "draws with uploads counted");
    Expect(v[Counts::DrawsSharing] == 2 && v[Counts::DrawsAllShared] == 1, "sharing draws counted");
    Counts::NewDrawIn(t, 5);
    Expect(t.tally[Counts::Submissions] == 1 && t.scope == 5, "a new scope is a new submission");
    Counts::NewDrawIn(t, 5);
    Expect(t.tally[Counts::Submissions] == 1, "the same scope is not");
}

void TestRepeats() {
    Counts::LastBind last;
    Expect(!Counts::Repeats(last, 0, 0, true), "the first bind never repeats");
    Expect(Counts::Repeats(last, 0, 0, true), "the same bind repeats");
    Expect(!Counts::Repeats(last, 0, 1, true), "a new scope does not repeat");
    Expect(!Counts::Repeats(last, 5, 1, true), "another key does not repeat");
    Expect(Counts::Repeats(last, 5, 1, true), "the new key is remembered");
    Expect(!Counts::Repeats(last, 5, 1, false), "an unknown bind does not repeat");
    Expect(!Counts::Repeats(last, 5, 1, true), "after an unknown bind nothing repeats");
    Expect(Counts::Repeats(last, 5, 1, true), "until a known bind is seen again");
}

void TestDraws() {
    Counts::BindState s;
    Counts::BindSample d;
    for (u32 b = 0; b < Counts::kBinds; ++b) {
        d.key[b] = 10 + b;
        d.known[b] = true;
        d.bound[b] = b != Counts::BindIndex;
        d.cycles[b] = 100;
    }
    Expect(Counts::NoteDraw(s, d, 1) == 0, "first draw repeats nothing");
    Expect(Counts::NoteDraw(s, d, 1) == Counts::kBinds - 1, "second draw repeats every bound kind");
    d.key[Counts::BindPipeline] = 99;
    Expect(Counts::NoteDraw(s, d, 1) == Counts::kBinds - 2, "a new pipeline does not repeat");
    Expect(Counts::NoteDraw(s, d, 2) == 0, "a scope cut repeats nothing");
    const u64* v = s.tally;
    Expect(v[Counts::BindDraws] == 4, "draws counted");
    Expect(v[Counts::DrawsAllRepeat] == 1, "draws whose binds all repeat counted");
    Expect(v[Counts::ScopeCuts] == 2, "scope cuts counted");
    Expect(v[Counts::At(Counts::Binds, Counts::BindIndex)] == 0, "unbound kinds not counted");
    Expect(v[Counts::At(Counts::Binds, Counts::BindVertex)] == 4 && v[Counts::At(Counts::Repeated, Counts::BindVertex)] == 2,
           "vertex binds and repeats counted");
    Expect(v[Counts::At(Counts::Repeated, Counts::BindPipeline)] == 1, "pipeline repeats counted");
    Expect(v[Counts::At(Counts::BindCycles, Counts::BindVertex)] == 400 && v[Counts::At(Counts::RepeatCycles, Counts::BindVertex)] == 200,
           "cycles of repeated binds counted apart");
    Expect(Counts::ScopeOf(1, 0, 0) != Counts::ScopeOf(1, 1, 0) && Counts::ScopeOf(1, 0, 0) != Counts::ScopeOf(1, 0, 1) &&
               Counts::ScopeOf(1, 0, 0) != Counts::ScopeOf(2, 0, 0),
           "scope changes with submission, segment and breaks");
    Counts::BindState broken;
    Counts::Break(broken);
    Expect(broken.breaks == 1, "a break is counted");
}

void TestPublishAndFormat() {
    Counts::Totals totals;
    Counts::ShareTable t;
    Counts::NewDraw(t);
    Counts::NoteUpload(t, 1, 1024, 3000, 100);
    Counts::NewDraw(t);
    Counts::NoteUpload(t, 1, 1024, 3000, 100);
    Counts::NewDraw(t);
    Counts::MaybePublish(t, totals.share_a);
    Expect(totals.share_a[Counts::Uploads].load() == 0, "nothing published before the interval");
    Counts::MaybePublish(t, totals.share_a, true);
    Expect(totals.share_a[Counts::Uploads].load() == 2 && t.tally[Counts::Uploads] == 0, "published and cleared");
    Counts::Window<Counts::kShareCounters> last;
    const auto w = Counts::Take(totals.share_a, last);
    Expect(w.v[Counts::Shared] == 1, "window takes the published counts");
    Expect(Counts::Take(totals.share_a, last).v[Counts::Shared] == 0, "a second window starts at zero");
    char line[1024];
    Counts::FormatShare(line, sizeof line, "stage A", w, 2.0, 1e9);
    Expect(std::strstr(line, "1 uploads/frame") != nullptr, "uploads per frame printed");
    Expect(std::strstr(line, "(50.0%)") != nullptr, "shared share printed");
    Expect(std::strstr(line, "3.0 us/frame copying") != nullptr, "copy time printed in us per frame");
    Counts::Window<Counts::kBindCounters> binds;
    binds.v[Counts::BindDraws] = 10;
    binds.v[Counts::At(Counts::Binds, Counts::BindVertex)] = 10;
    binds.v[Counts::At(Counts::Repeated, Counts::BindVertex)] = 4;
    binds.v[Counts::At(Counts::RepeatCycles, Counts::BindVertex)] = 2000;
    binds.v[Counts::At(Counts::BindCycles, Counts::BindVertex)] = 5000;
    Counts::FormatBinds(line, sizeof line, binds, 1.0, 1e9);
    Expect(std::strstr(line, "vertex 10/frame repeat 40.0% (2.0 of 5.0 us/frame)") != nullptr, "binds printed");
}

void TestMayHashGuest() {
    Expect(Counts::MayHashGuest(false, false), "relaxed readbacks, CPU-side bytes: hashed");
    Expect(!Counts::MayHashGuest(true, false), "precise readbacks: never hashed, the read may fault");
    Expect(!Counts::MayHashGuest(false, true), "GPU-modified bytes: never hashed");
    Expect(!Counts::MayHashGuest(true, true), "both: never hashed");
}

int RunAll() {
    failures = 0;
    TestMayHashGuest();
    TestLook();
    TestProbes();
    TestTally();
    TestRepeats();
    TestDraws();
    TestPublishAndFormat();
    return failures;
}
}

int main(int argc, char** argv) {
    const bool mutants = argc > 1 && std::strcmp(argv[1], "--mutants") == 0;
    if (!mutants) {
        const int failed = RunAll();
        std::printf("stageb-counts-test: %s (%d failures)\n", failed ? "FAILED" : "passed", failed);
        return failed ? 1 : 0;
    }
#ifdef LANTERN_STAGEB_MUTANTS
    quiet = true;
    int survivors = 0;
    for (int mutant = 1; mutant <= Counts::kMutants; ++mutant) {
        Counts::g_mutant = mutant;
        const int failed = RunAll();
        std::printf("mutant %d: %s (%d failures)\n", mutant, failed ? "caught" : "SURVIVED", failed);
        survivors += failed == 0;
    }
    Counts::g_mutant = 0;
    quiet = false;
    const int failed = RunAll();
    std::printf("stageb-counts-mutants: %d survivors, shipped lines %s\n", survivors, failed ? "FAILED" : "passed");
    return survivors || failed ? 1 : 0;
#else
    std::printf("stageb-counts-test: --mutants needs a build with LANTERN_STAGEB_MUTANTS\n");
    return 1;
#endif
}
