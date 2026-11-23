// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/gx_whole_owed_blocks.h"
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace O = BbEngine::GxWholeOwedBlocks;
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

constexpr u32 kWords = 8 * O::kBlockWords;

struct Files {
    std::vector<u32> live, sent;
    Files() : live(kWords), sent(kWords) {
        for (u32 i = 0; i < kWords; ++i) live[i] = sent[i] = 0x1000u + i;
    }
};

W::Value V(u32 reg, u32 value, u32 mask = ~0u) {
    return {0, reg, value, mask};
}

u32 Sequential(u32 reg, u32 start, std::span<const O::PreWrite> pre, std::span<const W::Value> values) {
    u32 now = start;
    for (const O::PreWrite& p : pre) {
        if (p.reg == reg) now = p.value;
    }
    for (const W::Value& v : values) {
        if (v.reg == reg) now = (now & ~v.mask) | (v.value & v.mask);
    }
    return now;
}

bool SameDraw(const Files& f, u32 block, std::span<const O::PreWrite> pre, std::span<const W::Value> values) {
    for (u32 w = block * O::kBlockWords; w < (block + 1) * O::kBlockWords; ++w) {
        if (Sequential(w, f.sent[w], pre, values) != Sequential(w, f.live[w], pre, values)) return false;
    }
    return true;
}

O::Verdict Judge(const Files& f, u32 block, std::span<const O::PreWrite> pre, std::span<const W::Value> values) {
    O::Coverage coverage(kWords);
    coverage.Build(pre, values);
    return coverage.Judge(f.live, f.sent, block);
}

void TestJudge() {
    Files f;
    const std::vector<W::Value> none;
    Check(Judge(f, 1, {}, none) == O::Verdict::Withhold, "an unchanged block is withheld");
    f.live[33] = 7;
    const std::vector<W::Value> record = {V(33, 7)};
    Check(Judge(f, 1, {}, record) == O::Verdict::Withhold, "a changed word the record writes whole: withheld");
    Check(SameDraw(f, 1, {}, record), "withheld, the draw sees what the block would give it");
    const std::vector<W::Value> other = {V(33, 8)};
    Check(Judge(f, 1, {}, other) == O::Verdict::Withhold && SameDraw(f, 1, {}, other),
          "a whole-word record value hides the register either way: withheld, the same draw");
    f.live[63] = 9;
    Check(Judge(f, 1, {}, record) == O::Verdict::OutsideRecord, "the block's last word changed outside the record: sent");
    f.live[63] = f.sent[63];
    f.live[32] = 5;
    Check(Judge(f, 1, {}, record) == O::Verdict::OutsideRecord, "the block's first word changed outside the record: sent");
    f.live[32] = f.sent[32];
    Check(Judge(f, 0, {}, record) == O::Verdict::Withhold, "the record's word in another block does not matter here");
}

void TestMask() {
    Files f;
    f.live[40] = (f.sent[40] & ~0xffu) | 0x42u;
    const std::vector<W::Value> low = {V(40, 0x42, 0xff)};
    Check(Judge(f, 1, {}, low) == O::Verdict::Withhold, "a masked word whose other bits are unchanged: withheld");
    Check(SameDraw(f, 1, {}, low), "masked and withheld: the same draw");
    f.live[40] = 0xabcd0042u;
    Check(Judge(f, 1, {}, low) == O::Verdict::MaskedBits, "the bits outside the mask changed: sent");
    Check(!SameDraw(f, 1, {}, low), "and withholding it would change the draw");
}

void TestPreWrites() {
    Files f;
    f.live[70] = 11;
    const std::vector<O::PreWrite> swap = {{70, 11}};
    const std::vector<W::Value> none;
    Check(Judge(f, 2, swap, none) == O::Verdict::Withhold, "a word the swap list writes is covered");
    Check(SameDraw(f, 2, swap, none), "swap-covered and withheld: the same draw");
    f.live[71] = 12;
    const std::vector<W::Value> masked = {V(71, 0, 0xf0)};
    Check(Judge(f, 2, swap, masked) == O::Verdict::MaskedBits, "a masked value over a changed word: sent");
    O::Coverage coverage(kWords);
    const std::vector<O::PreWrite> pre = {{75, 0xaaaa5555u}};
    const std::vector<W::Value> low = {V(75, 0x12, 0xff)};
    coverage.Build(pre, low);
    Check(coverage.Apply(75, 0) == 0xaaaa5512u, "the swap list first, then the overlay");
    Check(coverage.Apply(76, 0x77) == 0x77 && !coverage.Covers(76), "an uncovered word keeps its value");
}

void TestRebuild() {
    O::Coverage coverage(kWords);
    const std::vector<W::Value> first = {V(33, 1)};
    coverage.Build({}, first);
    Check(coverage.Covers(33), "the first build covers its word");
    const std::vector<W::Value> second = {V(100, 1)};
    coverage.Build({}, second);
    const std::vector<W::Value> third = {V(130, 1)};
    coverage.Build({}, third);
    Check(!coverage.Covers(33) && !coverage.Covers(100), "a rebuild forgets the last packets' words");
    Check(coverage.Covers(130), "a rebuild covers its own words");
    Files f;
    f.live[100] = 99;
    Check(coverage.Judge(f.live, f.sent, 3) == O::Verdict::OutsideRecord, "the last packet's word does not hide a change");
}

void TestWrap() {
    O::Coverage coverage(kWords);
    coverage.PresetGeneration(0xfffffffeu);
    const std::vector<W::Value> before = {V(33, 1)};
    coverage.Build({}, before);
    Check(coverage.Covers(33), "the last generation before the wrap covers its word");
    const std::vector<W::Value> after = {V(100, 1)};
    coverage.Build({}, after);
    Check(coverage.Covers(100), "the first generation after the wrap covers its word");
    Check(!coverage.Covers(33), "after the wrap the last packet's word is forgotten");
    Check(!coverage.Covers(50), "after the wrap a word never covered is not covered");
    Files f;
    f.live[50] = 1;
    Check(coverage.Judge(f.live, f.sent, 1) == O::Verdict::OutsideRecord, "after the wrap a changed word is sent");
}

void TestRandom() {
    std::mt19937 rng(12345);
    u32 withheld = 0, sent = 0, wrong = 0, apply_wrong = 0;
    O::Coverage coverage(kWords);
    for (u32 round = 0; round < 3000; ++round) {
        Files f;
        const u32 block = rng() % (kWords / O::kBlockWords);
        const u32 base = block * O::kBlockWords;
        std::vector<O::PreWrite> pre;
        std::vector<W::Value> values;
        for (u32 i = 0, n = rng() % 4; i < n; ++i) pre.push_back({u32(base + rng() % O::kBlockWords), u32(rng())});
        for (u32 i = 0, n = rng() % 12; i < n; ++i) {
            const u32 masks[] = {~0u, 0xffu, 0xff00u, 0x80000000u};
            values.push_back(V(base + rng() % O::kBlockWords, u32(rng()), masks[rng() % 4]));
        }
        for (u32 i = 0, n = rng() % 4; i < n; ++i) {
            const u32 w = base + rng() % O::kBlockWords;
            f.live[w] ^= (rng() % 2) ? 0xffu : (1u << (rng() % 32));
        }
        coverage.Build(pre, values);
        const bool judged = coverage.Judge(f.live, f.sent, block) == O::Verdict::Withhold;
        const bool same = SameDraw(f, block, pre, values);
        if (judged != same) ++wrong;
        judged ? ++withheld : ++sent;
        for (u32 w = base; w < base + O::kBlockWords; ++w) {
            if (coverage.Apply(w, f.live[w]) != Sequential(w, f.live[w], pre, values)) ++apply_wrong;
        }
    }
    Check(wrong == 0, "random blocks: withheld exactly when the draw is the same");
    Check(apply_wrong == 0, "random blocks: the built coverage gives the overlay's words");
    Check(withheld > 100 && sent > 100, "random blocks: both verdicts are exercised");
}

void TestEligible() {
    Check(O::Eligible(true, true, false, false), "=1, carried, no reset, take on: eligible");
    Check(!O::Eligible(false, true, false, false), "=2 or =0 never withholds");
    Check(!O::Eligible(true, false, false, false), "a draw without its record never withholds");
    Check(!O::Eligible(true, true, true, false), "a ClearState packet sends every block");
    Check(!O::Eligible(true, true, false, true), "after the take-off every block is sent");
    Check(O::SampleDraw(0) && !O::SampleDraw(1) && O::SampleDraw(O::kSampleEvery), "one judged draw in kSampleEvery is sent whole");
}

void TestAfterPacket() {
    std::bitset<8> dirty;
    dirty.set(1);
    dirty.set(3);
    dirty.set(5);
    const std::vector<u32> withheld = {3};
    O::AfterPacket(dirty, withheld);
    Check(!dirty.test(1) && !dirty.test(5), "sent blocks are clean");
    Check(dirty.test(3), "a withheld block stays owed for the next packet");
    O::AfterPacket(dirty, {});
    Check(dirty.none(), "a packet that sends every block leaves nothing owed");
}

void TestLine() {
    O::Counts c;
    c.packets = 10;
    c.carried = 6;
    c.eligible = 5;
    c.withheld_draws = 4;
    c.dirty_blocks = 20;
    c.withheld_blocks = 5;
    c.sent[1] = 7;
    c.sent[2] = 3;
    char line[640];
    O::FormatLine(line, sizeof line, true, c);
    std::string text = line;
    Check(text.find("LANTERN_GX_WHOLE=1") != std::string::npos, "the line names the switch");
    Check(text.find("5 of 20 dirty blocks withheld (25.0%)") != std::string::npos, "the line gives the withheld share");
    Check(text.find("outside the record changed 7") != std::string::npos, "the line gives why blocks were sent");
    Check(text.find("outside a value's mask changed 3") != std::string::npos, "the line names masked bits");
    Check(text.find("REACHED A TASK") == std::string::npos, "no task warning without one");
    c.owed_at_task = 1;
    O::FormatLine(line, sizeof line, false, c);
    text = line;
    Check(text.find("could be withheld") != std::string::npos && text.find("REACHED A TASK") != std::string::npos,
          "=2 wording and the task warning");
    char tiny[12];
    const std::size_t used = O::FormatLine(tiny, sizeof tiny, false, c);
    Check(used < sizeof tiny && tiny[used] == '\0', "the line stays inside its buffer");
}

int RunSuite() {
    g_failed = 0;
    TestJudge();
    TestMask();
    TestPreWrites();
    TestRebuild();
    TestWrap();
    TestRandom();
    TestEligible();
    TestAfterPacket();
    TestLine();
    return g_failed;
}

#ifndef LANTERN_GX_OWED_BLOCKS_MUTANTS
void Bench(u32 pre_count, u32 value_count, u32 dirty_blocks, const char* label) {
    constexpr u32 kRegs = 0xD000;
    std::mt19937 rng(7);
    std::vector<u32> live(kRegs), sent(kRegs);
    for (u32 i = 0; i < kRegs; ++i) live[i] = sent[i] = u32(rng());
    const u32 context = 0xA000;
    std::vector<O::PreWrite> pre;
    std::vector<W::Value> values;
    for (u32 i = 0; i < pre_count; ++i) pre.push_back({u32(context + (rng() % 0x400)), u32(rng())});
    for (u32 i = 0; i < value_count; ++i) values.push_back(V(context + (rng() % 0x400), u32(rng()), i % 5 ? ~0u : 0xffu));
    std::vector<u32> blocks;
    for (u32 i = 0; i < dirty_blocks; ++i) {
        const u32 block = (context / O::kBlockWords) + rng() % (0x400 / O::kBlockWords);
        blocks.push_back(block);
        live[block * O::kBlockWords + rng() % O::kBlockWords] ^= 1;
    }
    O::Coverage coverage(kRegs);
    constexpr u32 kDraws = 200000;
    u64 verdicts = 0;
    const auto start = std::chrono::steady_clock::now();
    for (u32 d = 0; d < kDraws; ++d) {
        coverage.Build(pre, values);
        for (u32 b : blocks) verdicts += u32(coverage.Judge(live, sent, b));
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kDraws;
    std::printf("gx whole owed blocks microbench (%s): %u swap words, %u values, %u dirty blocks: %.0f ns per carried draw "
                "(build + judge, one thread, %llu)\n",
                label, pre_count, value_count, dirty_blocks, ns, static_cast<unsigned long long>(verdicts % 7));
}
#endif
}

int main(int argc, char** argv) {
#ifdef LANTERN_GX_OWED_BLOCKS_MUTANTS
    (void)argc;
    (void)argv;
    using O::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        O::selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("gx whole owed blocks mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("gx whole owed blocks mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("gx whole owed blocks mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("gx whole owed blocks mutants: every mutant caught\n");
    return 0;
#else
    if (argc == 2 && std::strcmp(argv[1], "--bench") == 0) {
        Bench(20, 60, 6, "guessed typical draw");
        Bench(160, 256, 24, "worst case");
        return 0;
    }
    if (RunSuite()) {
        std::printf("gx whole owed blocks: %d failures\n", g_failed);
        return 1;
    }
    std::printf("gx whole owed blocks: all tests passed\n");
    return 0;
#endif
}
