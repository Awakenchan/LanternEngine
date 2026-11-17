// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/gx_whole_stageb.h"
#include "gpu/shim/engine/gx_whole_owed_blocks.h"
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

u32 F(float value) {
    return std::bit_cast<u32>(value);
}

constexpr u64 kIndexBase = 0x0000001234567898ull;
constexpr u32 kTargetSlots = 2, kColorWords = 11, kDepthWords = 8;

std::unique_ptr<G::State> Recorded() {
    auto state = std::make_unique<G::State>();
    std::memset(static_cast<void*>(state.get()), 0, sizeof *state);
    G::Render& r = state->render;
    r.blend[0] = 0xf;
    for (u32 i = 0; i < 8; ++i) r.blend[1 + i] = 0x0000e000u | (0x10 + i);
    r.blend_factor[0] = 0.25f;
    r.blend_factor[3] = 1.0f;
    r.depth_stencil[0] = 0x00700077;
    r.depth_stencil[1] = 0x00123456;
    r.depth_stencil[2] = 0x00ee7700;
    r.stencil_ref = 0x15;
    r.raster[0] = 0x00010000;
    r.raster[1] = 0x00000242;
    r.raster[2] = F(2.5f);
    r.raster[3] = F(0.75f);
    r.raster[4] = F(1.5f);
    r.raster[5] = 0x0101;
    r.viewports = 1;
    const float viewport[6] = {0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f};
    std::memcpy(r.viewport, viewport, sizeof viewport);
    r.scissors = 1;
    const std::int32_t scissor[4] = {0, 0, 1920, 1080};
    std::memcpy(r.scissor, scissor, sizeof scissor);
    r.color_valid = 0b101;
    for (u32 w = 0; w < kColorWords; ++w) {
        r.targets[0][w] = 0x1000 + w;
        r.targets[2][w] = 0x3000 + w;
    }
    r.depth_valid = 1;
    for (u32 w = 0; w < 12; ++w) r.depth_target[w] = 0x5000 + w;
    r.index_known = 1;
    r.index_format = W::kIndexFormat32;
    r.index_base = kIndexBase;
    G::Whole& w = state->whole;
    w.captured = 1;
    w.topology = 4;
    w.depth_bounds[0] = 0.125f;
    w.depth_bounds[1] = 0.875f;
    w.index_bound = 1;
    w.vs_bound = 1;
    w.vs_registers[0] = 0x100;
    w.ps_bound = 1;
    for (u32 i = 0; i < 12; ++i) w.ps_registers[i] = 0x700 + i;
    w.depth_clear = 1.0f;
    w.stencil_clear = 0x80;
    return state;
}

struct Values {
    std::array<W::Value, B::kMaxValues> list{};
    u32 count = 0;
    std::span<const W::Value> Span() const { return {list.data(), count}; }
};

std::unique_ptr<Values> StageB(const G::State& state, bool indexed) {
    auto v = std::make_unique<Values>();
    v->count = B::StageBValues(B::Carry(state), indexed, v->list);
    return v;
}

std::vector<u32> Applied(const Values& v, u32 background) {
    std::vector<u32> regs(W::kRegisterWords, background);
    for (u32 i = 0; i < v.count; ++i) {
        const W::Value& value = v.list[i];
        regs[value.reg] = (regs[value.reg] & ~value.mask) | (value.value & value.mask);
    }
    return regs;
}

u32 InGroup(const Values& v, B::Group group) {
    u32 n = 0;
    for (u32 i = 0; i < v.count; ++i) n += B::GroupOf(v.list[i].field) == group;
    return n;
}

void TestOverlayLeavesUnread() {
    auto state = Recorded();
    const B::Carried carried = B::Carry(*state);
    std::array<W::Value, B::kMaxValues> values{};
    const u32 all = B::StageBValues(carried, true, values);
    const u32 read = B::StageBOverlayValues(carried, true, values);
    Check(all == read + kColorWords + 7 + 2, "the overlay leaves out slot 2's target words and slots 1-7's blend controls outside CB_TARGET_MASK 0xf, and both depth bounds words with bounds off");
    bool slot2 = false;
    for (u32 i = 0; i < read; ++i) slot2 |= values[i].reg >= W::kContext + 0x318 + 30 && values[i].reg < W::kContext + 0x318 + 41;
    Check(!slot2, "no overlaid value touches slot 2");
    std::vector<u32> regs(W::kRegisterWords, 0x5a5a5a5au);
    std::array<B::Saved, B::kMaxValues> saved{};
    B::Overlay(std::span<const W::Value>(values.data(), read), regs, saved);
    Check(regs[W::kContext + 0x318 + 30 + 4] == 0x5a5a5a5au, "an unread register keeps the value the registers gave");
    Check(regs[W::kContext + 0x318 + 4] != 0x5a5a5a5au, "slot 0's words are overlaid");
}

void TestFollowsStageA() {
    Check(B::TakeOffFollowing(false, true), "stage A's record route turned off: stage B stops taking too");
    Check(!B::TakeOffFollowing(false, false), "both on: stage B keeps taking");
    Check(B::TakeOffFollowing(true, false), "a take-off of its own stays");
}

void TestUnreadWordIsSent() {
    namespace O = BbEngine::GxWholeOwedBlocks;
    const auto state = Recorded();
    const B::Carried carried = B::Carry(*state);
    std::array<W::Value, B::kMaxValues> values{};
    const u32 read = B::StageBOverlayValues(carried, true, values);
    O::Coverage coverage(W::kRegisterWords);
    coverage.Build({}, std::span<const W::Value>(values.data(), read));
    std::vector<u32> sent(W::kRegisterWords, 0), live(W::kRegisterWords, 0);
    const u32 unread_word = W::kContext + 0x318 + 30 + 4;
    live[unread_word] = 0x60718;
    Check(coverage.Judge(live, sent, unread_word / O::kBlockWords) == O::Verdict::OutsideRecord,
          "only an unread word changed: it is not covered, so its block is sent");
    live[unread_word] = 0;
    const u32 read_word = W::kContext + 0x318 + 4;
    live[read_word] = 0x1004;
    Check(coverage.Judge(live, sent, read_word / O::kBlockWords) != O::Verdict::OutsideRecord,
          "a read word the record writes is covered");
}

void TestUnboundWrittenSlot() {
    auto state = Recorded();
    state->render.color_valid = 0b001;
    state->render.blend[0] = 0xff;
    state->whole.ps_bound = 1;
    state->whole.ps_registers[11] = 0xff;
    const B::Carried carried = B::Carry(*state);
    std::array<W::Value, B::kMaxValues> values{};
    const u32 count = B::StageBValues(carried, true, values);
    u32 unbound = 0;
    for (u32 i = 0; i < count; ++i) unbound += values[i].field == W::CbColorUnbound && values[i].reg == W::kContext + 0x32b;
    Check(unbound == 1, "stage B carries the pixel shader's mask: an unbound slot the draw writes gets its info word 0");
    state->whole.ps_registers[11] = 0x0f;
    const B::Carried unwritten = B::Carry(*state);
    const u32 none = B::StageBValues(unwritten, true, values);
    unbound = 0;
    for (u32 i = 0; i < none; ++i) unbound += values[i].field == W::CbColorUnbound;
    Check(unbound == 0, "an unbound slot the pixel shader does not write gets no info word");
}

void TestGroups() {
    const auto state = Recorded();
    const auto v = StageB(*state, true);
    Check(InGroup(*v, B::Group::None) == 0, "stage B values hold no field outside stage B's groups");
    const u32 targets = kTargetSlots * kColorWords + kDepthWords + 2 + 2;
    Check(InGroup(*v, B::Group::Targets) == targets,
          "two bound color targets, no info word for the six unbound slots the draw does not write, depth target, view, htile and both clears");
    Check(InGroup(*v, B::Group::Render) == 9, "target mask and the eight blend controls");
    Check(InGroup(*v, B::Group::Binds) == 3, "index type and both index base words for an indexed draw");
    Check(InGroup(*StageB(*state, false), B::Group::Binds) == 0, "no index words for a draw without indices");
    Check(B::GroupOf(W::VsProgram) == B::Group::None && B::GroupOf(W::PsProgram) == B::Group::None, "shader programs are not stage B's");
    Check(B::GroupOf(W::VgtShaderStages) == B::Group::None, "shader stages are stage A's key");
    for (u32 f = 0; f < W::kFields; ++f) {
        if (W::kFieldInfo[f].source != W::Source::Record) Check(B::GroupOf(f) == B::Group::None, "kept and token fields stay with the registers");
    }
}

void TestMatchesRecordPath() {
    const auto state = Recorded();
    std::array<W::Value, W::kMaxValues> whole{};
    const u32 count = W::RegistersFromRecord(*state, true, whole);
    const auto v = StageB(*state, true);
    u32 expected = 0;
    bool same = true;
    for (u32 i = 0; i < count; ++i) {
        if (B::GroupOf(whole[i].field) == B::Group::None) continue;
        const W::Value& ours = v->list[expected++];
        same &= ours.field == whole[i].field && ours.reg == whole[i].reg && ours.value == whole[i].value && ours.mask == whole[i].mask;
    }
    Check(expected == v->count && same, "the carried record gives the same stage B values as W1's record path, in order");
}

void TestCompare() {
    const auto state = Recorded();
    const auto v = StageB(*state, true);
    auto regs = Applied(*v, 0);
    B::Counts counts;
    std::array<B::Difference, B::kEvidencePerDraw> evidence{};
    Check(B::CompareValues(v->Span(), regs, counts, evidence) == 0 && counts.values == v->count, "registers written from the record compare equal");
    regs[W::kUconfig + 0x242] |= 0x40;
    regs[W::kContext + 0x00a] |= 0x100;
    B::Counts masked;
    Check(B::CompareValues(v->Span(), regs, masked, evidence) == 0, "bits outside a field's mask are not differences");
    regs[W::kContext + 0x318 + 30 + 4] ^= 1;
    regs[W::kContext + 0x10f] ^= 1;
    B::Counts differ;
    const u32 found = B::CompareValues(v->Span(), regs, differ, evidence);
    Check(found == 2 && differ.values_differ == 2, "a target word and a viewport word differ");
    Check(differ.group_differ[u32(B::Group::Targets)] == 1 && differ.group_differ[u32(B::Group::Dynamic)] == 1, "differences are counted by group");
    Check(differ.field_differ[W::CbColorTarget] == 1 && differ.field_differ[W::PaClVport0] == 1, "differences are counted by field");
    Check(evidence[0].reg == W::kContext + 0x10f || evidence[1].reg == W::kContext + 0x10f, "evidence names the register");
    B::Counts read;
    const u32 read_found = B::CompareValues(v->Span(), regs, read, evidence, &state->render);
    Check(read_found == 1 && read.values_differ == 1 && read.unused_differ == 1 &&
              read.unused_by[u32(W::Unused::SlotNotWritten)] == 1,
          "with the record's render state the slot 2 word outside CB_TARGET_MASK is unread: no take-off, counted by class");
    char line[2048];
    B::FormatLine(line, sizeof line, W::Mode::Feed, false, read);
    Check(std::string(line).find("no take-off") != std::string::npos, "the stage B line lists unread differences");
}

void TestOverlay() {
    const auto state = Recorded();
    const auto v = StageB(*state, true);
    std::vector<u32> regs(W::kRegisterWords, 0xa5a5a5a5u);
    const std::vector<u32> before = regs;
    std::array<B::Saved, B::kMaxValues> saved{};
    const u32 n = B::Overlay(v->Span(), regs, saved);
    Check(n > 0 && n <= v->count, "the overlay saves each word it changes");
    B::Counts counts;
    std::array<B::Difference, B::kEvidencePerDraw> evidence{};
    Check(B::CompareValues(v->Span(), regs, counts, evidence) == 0, "after the overlay the registers read the record's values");
    Check((regs[W::kUconfig + 0x242] & ~0x3fu) == (0xa5a5a5a5u & ~0x3fu), "bits outside the mask keep the register's value");
    B::Restore(std::span<const B::Saved>(saved.data(), n), regs);
    Check(regs == before, "restore puts every overlaid word back");
    std::array<W::Value, 2> twice = {{{W::CbTargetMask, W::kContext + 0x08e, 1, ~0u}, {W::CbTargetMask, W::kContext + 0x08e, 2, ~0u}}};
    const u32 m = B::Overlay(twice, regs, saved);
    Check(m == 2 && regs[W::kContext + 0x08e] == 2, "a word written twice is saved twice");
    B::Restore(std::span<const B::Saved>(saved.data(), m), regs);
    Check(regs == before, "restoring in reverse leaves the first value, not the middle one");
    auto same = Applied(*v, 0);
    Check(B::Overlay(v->Span(), same, saved) == 0, "words already equal are not saved or written");
}

void TestDecide() {
    using A = B::Action;
    Check(B::Decide(W::Mode::Off, true, false, 0) == A::Register, "mode 0 takes the registers");
    Check(B::Decide(W::Mode::Feed, false, false, 0) == A::Register, "no carried record takes the registers");
    Check(B::Decide(W::Mode::Compare, true, true, 5) == A::Compare, "mode 2 compares every covered draw and keeps the registers");
    Check(B::Decide(W::Mode::Feed, true, true, 0) == A::Register, "a take turned off by a difference stays with the registers");
    Check(B::Decide(W::Mode::Feed, true, false, 0) == A::TakeAndCompare, "the first covered draw of each sample is compared");
    Check(B::Decide(W::Mode::Feed, true, false, 1) == A::Take, "other covered draws are taken without a check");
    Check(B::Decide(W::Mode::Feed, true, false, B::kSampleEvery) == A::TakeAndCompare, "the sample repeats");
}

void TestCover() {
    using C = B::Cover;
    Check(B::CoverOf(false, true, true, true) == C::NotToken, "a draw without a record");
    Check(B::CoverOf(true, false, true, true) == C::NoPayload, "a record whose payload is gone");
    Check(B::CoverOf(true, true, false, true) == C::NoWhole, "a payload without its whole part");
    Check(B::CoverOf(true, true, true, false) == C::Overwritten, "a payload overwritten during the copy is not carried");
    Check(B::CoverOf(true, true, true, true) == C::Carried, "an intact record is carried");
}

void TestCarry() {
    const auto state = Recorded();
    const B::Carried carried = B::Carry(*state);
    const G::Whole whole = B::WholeOf(carried);
    Check(whole.depth_bounds[0] == 0.125f && whole.depth_bounds[1] == 0.875f, "depth bounds travel with the record");
    Check(whole.topology == 4 && whole.index_bound == 1 && whole.depth_clear == 1.0f && whole.stencil_clear == 0x80, "the whole tail travels with the record");
    Check(std::memcmp(&carried.render, &state->render, sizeof carried.render) == 0, "render state travels unchanged");
}

void TestSuspect() {
    B::Counts c;
    c.draws = 1;
    Check(!B::InstrumentSuspect(W::Mode::Feed, c, false), "a one-packet window at the start is not suspect");
    c.draws = 1000;
    Check(B::InstrumentSuspect(W::Mode::Off, c, false), "draws with none covered are suspect");
    c.covered = 100;
    Check(!B::InstrumentSuspect(W::Mode::Off, c, false), "the count-only state with covered draws is fine");
    Check(B::InstrumentSuspect(W::Mode::Feed, c, false), "covered draws with no check in mode 1 are suspect");
    Check(!B::InstrumentSuspect(W::Mode::Feed, c, true), "after the take-off no checks are expected: not suspect");
    c.checked = 2;
    Check(!B::InstrumentSuspect(W::Mode::Feed, c, false), "checks running and passing are fine");
    c.differ = 2;
    Check(B::InstrumentSuspect(W::Mode::Compare, c, false), "every check differing is suspect");
}

void TestFormat() {
    B::Counts c;
    c.draws = 10;
    c.covered = 8;
    c.by_cover[u32(B::Cover::NotToken)] = 2;
    c.checked = 8;
    c.differ = 1;
    c.group_differ[u32(B::Group::Targets)] = 1;
    c.field_differ[W::CbColorTarget] = 1;
    char line[2048];
    const std::size_t used = B::FormatLine(line, sizeof line, W::Mode::Compare, false, c);
    Check(used > 0 && used < sizeof line && std::strstr(line, "10 draw packets, 8 covered") != nullptr, "the line gives covered and total");
    Check(std::strstr(line, "no GX record 2") != nullptr && std::strstr(line, "targets 1") != nullptr && std::strstr(line, "CB_COLOR 1") != nullptr,
          "the line names reasons, groups and fields");
    char tiny[16];
    std::memset(tiny, 'x', sizeof tiny);
    const std::size_t small = B::FormatLine(tiny, 8, W::Mode::Compare, true, c);
    Check(small < 8 && tiny[small] == '\0' && tiny[8] == 'x', "a short buffer is never overrun");
}

int RunSuite() {
    g_failed = 0;
    TestGroups();
    TestOverlayLeavesUnread();
    TestUnboundWrittenSlot();
    TestFollowsStageA();
    TestUnreadWordIsSent();
    TestMatchesRecordPath();
    TestCompare();
    TestOverlay();
    TestDecide();
    TestCover();
    TestCarry();
    TestSuspect();
    TestFormat();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_GX_STAGEB_MUTANTS
    using B::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        B::selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("gx whole stage B mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("gx whole stage B mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("gx whole stage B mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("gx whole stage B mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("gx whole stage B: %d failures\n", g_failed);
        return 1;
    }
    std::printf("gx whole stage B: all tests passed\n");
    return 0;
#endif
}
