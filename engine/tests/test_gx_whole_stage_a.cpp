// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/gx_whole_stage_a.h"
#include "gpu/shim/engine/gx_whole.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace A = BbEngine::GxWhole::StageA;
namespace W = BbEngine::GxWhole;
namespace G = BbEngine::GxBindings;
using u8 = std::uint8_t;
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

struct Word {
    u32 reg;
    u32 value;
    u32 mask;
};

A::Facts Covered() {
    return {true, true, true, true, true};
}

void TestCoverage() {
    Check(A::CoverageOf(Covered()) == A::Left::Covered, "a captured record with a vertex shader is covered");
    A::Facts f = Covered();
    f.record = false;
    Check(A::CoverageOf(f) == A::Left::NoRecord, "no record");
    f = Covered();
    f.payload = false;
    Check(A::CoverageOf(f) == A::Left::NoPayload, "record lost");
    f = Covered();
    f.captured = false;
    Check(A::CoverageOf(f) == A::Left::NotCaptured, "whole part not captured");
    f = Covered();
    f.vertex_shader = false;
    Check(A::CoverageOf(f) == A::Left::NoVertexShader, "no vertex shader");
    const A::Facts count_only{true, false, false, false, false};
    Check(A::CoverageOf(count_only) == A::Left::Covered, "count only: a taken record counts as covered");
    const A::Facts count_only_none{false, false, false, false, false};
    Check(A::CoverageOf(count_only_none) == A::Left::NoRecord, "count only: no record");
}

void TestRoute() {
    Check(A::RouteOf(A::Mode::Use, A::Left::Covered, false) == A::Route::Record, "=1 takes covered draws");
    Check(A::RouteOf(A::Mode::Use, A::Left::Covered, true) == A::Route::Registers, "=1 after a difference: registers");
    Check(A::RouteOf(A::Mode::Use, A::Left::NoPayload, false) == A::Route::Registers, "=1 uncovered: registers");
    Check(A::RouteOf(A::Mode::Compare, A::Left::Covered, false) == A::Route::Registers, "=2 uses the register path");
    Check(A::RouteOf(A::Mode::Off, A::Left::Covered, false) == A::Route::Registers, "=0 uses the register path");
}

void TestSample() {
    Check(A::ShouldCompare(A::Mode::Compare, A::Left::Covered, 7, false), "=2 compares every covered draw");
    Check(!A::ShouldCompare(A::Mode::Compare, A::Left::NoRecord, 0, false), "=2 does not compare uncovered draws");
    Check(A::ShouldCompare(A::Mode::Use, A::Left::Covered, 0, false), "=1 compares the first covered draw");
    Check(A::ShouldCompare(A::Mode::Use, A::Left::Covered, A::kSampleEvery, false), "=1 compares one in kSampleEvery");
    Check(!A::ShouldCompare(A::Mode::Use, A::Left::Covered, 1, false), "=1 skips the others");
    Check(!A::ShouldCompare(A::Mode::Off, A::Left::Covered, 0, false), "=0 never compares");
    Check(!A::ShouldCompare(A::Mode::Use, A::Left::Covered, 0, true), "=1 after the turn-off compares nothing");
    Check(A::ShouldCompare(A::Mode::Compare, A::Left::Covered, 1, true), "=2 keeps comparing");
}

struct FakeState {
    bool captured = true;
    bool vertex_shader = true;
    u32 value = 0;
};

struct FakePipeline {
    int id;
};

struct FakePrepared {
    int id;
};

struct FakePort {
    using Value = Word;
    FakeState state;
    bool record_taken = true;
    bool disabled = false;
    bool disable_on_compare = false;
    const FakePipeline register_pipeline{1};
    const FakePipeline record_pipeline{2};
    u32 taken = 0, values = 0, by_record = 0, registers = 0, compares = 0;
    const FakePrepared* handed = nullptr;
    const FakeState* Taken(u64) {
        ++taken;
        return record_taken ? &state : nullptr;
    }
    A::Facts FactsOf(bool record, const FakeState* s, bool capture_on) {
        return {record, s != nullptr, s && s->captured, s && s->vertex_shader, capture_on};
    }
    bool Disabled() {
        return disabled;
    }
    u32 Values(const FakeState& s, bool, std::span<Value, A::kMaxOverlay> out) {
        ++values;
        out[0] = {3, s.value, ~0u};
        return 1;
    }
    const FakePipeline* Registers(const FakePrepared* prepared, u64) {
        ++registers;
        handed = prepared;
        return &register_pipeline;
    }
    const FakePipeline* ByRecord(std::span<const Value> v, u64) {
        ++by_record;
        Check(v.size() == 1, "the record route gets the record's values");
        return &record_pipeline;
    }
    void Compare(std::span<const Value>, u64, const FakePipeline*, A::Route) {
        ++compares;
        if (disable_on_compare) disabled = true;
    }
};

void TestWiring() {
    const FakePrepared prepared{7};
    {
        FakePort port;
        A::Chooser chooser;
        const FakePipeline* p = A::Choose(port, chooser, A::Mode::Compare, &prepared, 0x1000, true);
        Check(p == &port.register_pipeline && port.by_record == 0, "=2: the register path's pipeline is used");
        Check(port.handed == &prepared, "=2: the prepared draw is handed on");
        Check(port.compares == 1, "=2: every covered draw compared");
        Check(chooser.counts.routed == 0 && chooser.counts.prepared_dropped == 0, "=2: nothing routed by the record");
    }
    {
        FakePort port;
        A::Chooser chooser;
        port.disable_on_compare = true;
        const FakePipeline* first = A::Choose(port, chooser, A::Mode::Use, &prepared, 0x1000, false);
        Check(first == &port.record_pipeline && port.registers == 0, "=1: the first covered draw takes the record route");
        Check(port.compares == 1 && port.disabled, "=1: its compare turned the route off");
        Check(chooser.counts.prepared_dropped == 1, "=1: the prepared draw counted as not used");
        for (int i = 0; i < 3; ++i) {
            const FakePipeline* after = A::Choose(port, chooser, A::Mode::Use, &prepared, 0x1000, false);
            Check(after == &port.register_pipeline, "=1 after the turn-off: the register key's pipeline");
            Check(port.handed == &prepared, "=1 after the turn-off: the prepared draw is used again");
        }
        Check(port.by_record == 1 && port.registers == 3, "=1 after the turn-off: no more record routes");
        Check(port.compares == 1, "=1 after the turn-off: no more compares");
        Check(port.values == 1, "=1 after the turn-off: the record's values are not built");
        Check(chooser.counts.after_disable == 3 && chooser.counts.routed == 1, "draws after the turn-off are counted apart");
        Check(chooser.counts.skipped == 0, "draws after the turn-off are not counted as sample skips");
    }
    {
        FakePort port;
        A::Chooser chooser;
        A::Choose(port, chooser, A::Mode::Use, &prepared, 0x1000, false);
        A::Choose(port, chooser, A::Mode::Use, &prepared, 0x1000, false);
        Check(port.by_record == 2 && port.compares == 1 && chooser.counts.skipped == 1, "=1: one in kSampleEvery compared");
        port.state.vertex_shader = false;
        A::Choose(port, chooser, A::Mode::Use, &prepared, 0x1000, false);
        Check(port.registers == 1 && port.handed == &prepared, "=1: an uncovered draw keeps its prepared draw");
        A::Choose(port, chooser, A::Mode::Use, &prepared, 0, false);
        Check(port.taken == 3, "no record: nothing taken");
    }
    {
        FakePort port;
        A::Chooser chooser;
        A::Choose(port, chooser, A::Mode::Off, &prepared, 0x1000, false);
        Check(port.taken == 0 && port.registers == 1 && port.handed == &prepared && port.compares == 0, "=0: the old call only");
    }
    A::Switches s{true, A::Mode::Use, false};
    Check(s.MarksPrepared(), "=1 marks prepared draws");
    s.disabled = true;
    Check(!s.MarksPrepared(), "=1 after the turn-off: the workers prepare every draw again");
    s = {true, A::Mode::Compare, false};
    Check(!s.MarksPrepared(), "=2 never marks");
    s = {false, A::Mode::Use, false};
    Check(!s.MarksPrepared(), "off never marks");
    Check(!A::RecordsFeed("1", nullptr, true), "WHOLE=1 or 2 with records=1: no records feed");
    Check(!A::RecordsFeed("1", "0", true), "records=1, native 0: no feed");
    Check(!A::RecordsFeed("2", "1", true), "records=2: never a feed");
    Check(A::RecordsFeed("3", nullptr, false) && A::RecordsFeed("3", nullptr, true), "records=3 feeds");
    Check(A::RecordsFeed("1", "1", false), "records=1 with LANTERN_GX_NATIVE=1 feeds");
    Check(!A::RecordsFeed("0", "1", true) && !A::RecordsFeed(nullptr, "1", true), "records off: no feed");
}

void TestOverlay() {
    std::array<u32, 8> regs{};
    for (u32 i = 0; i < regs.size(); ++i) regs[i] = 0x11110000u + i;
    const std::array<u32, 8> original = regs;
    const std::array<Word, 5> values = {{
        {1, 0xaaaaaaaa, ~0u},
        {2, 0x0000beef, 0x0000ffff},
        {3, 0x5, ~0u},
        {3, 0x6, ~0u},
        {7, 0x77, ~0u},
    }};
    const std::array<Word, 1> outside = {{{8, 0x99, ~0u}}};
    std::array<u32, A::kMaxOverlay> saved{};
    const u32 count = A::Overlay<Word>(regs, values, saved);
    Check(count == values.size(), "every value is applied");
    Check(regs[1] == 0xaaaaaaaa, "a whole word is written");
    Check(regs[2] == 0x1111beef, "a masked word keeps the bits outside its mask");
    Check(regs[3] == 0x6, "the later of two values for one register wins");
    Check(regs[7] == 0x77, "the last register is written");
    Check(regs[0] == original[0] && regs[4] == original[4], "other registers are untouched");
    A::Restore<Word>(regs, values, saved, count);
    Check(regs == original, "restore gives back every register, duplicates included");
    const u32 none = A::Overlay<Word>(regs, outside, saved);
    A::Restore<Word>(regs, outside, saved, none);
    Check(regs == original, "a value past the register file is skipped");
    std::array<u32, 2> small{};
    const u32 limited = A::Overlay<Word>(regs, values, small);
    Check(limited == small.size() && regs[3] == original[3], "the saved space bounds the overlay");
    A::Restore<Word>(regs, values, small, limited);
    Check(regs == original, "a bounded overlay restores");
}

void TestOverlayRecord() {
    auto state = std::make_unique<G::State>();
    std::memset(static_cast<void*>(state.get()), 0, sizeof *state);
    state->whole.captured = 1;
    state->whole.vs_bound = 1;
    state->whole.ps_bound = 1;
    state->whole.topology = 4;
    state->render.viewports = 1;
    state->render.viewport[2] = 640.0f;
    state->render.viewport[3] = 360.0f;
    state->render.blend[0] = 0xf;
    state->render.depth_stencil[2] = 0xffffffff;
    state->whole.vs_registers[0] = 0x12345;
    std::array<W::Value, W::kMaxValues> values;
    const u32 count = W::RegistersFromRecord(*state, false, values);
    Check(count > 0 && count <= A::kMaxOverlay, "the record gives register values within the overlay bound");
    std::vector<u32> regs(W::kRegisterWords);
    for (u32 i = 0; i < regs.size(); ++i) regs[i] = i * 2654435761u;
    const std::vector<u32> original = regs;
    std::array<u32, A::kMaxOverlay> saved{};
    const std::span<const W::Value> used(values.data(), count);
    const u32 applied = A::Overlay<W::Value>(regs, used, saved);
    bool all = applied == count;
    for (u32 i = 0; i < count; ++i) {
        u32 last = values[i].value;
        for (u32 j = i + 1; j < count; ++j) {
            if (values[j].reg == values[i].reg) last = values[j].value;
        }
        if (values[i].mask == ~0u && regs[values[i].reg] != last) all = false;
    }
    Check(all, "every record register reaches the register file");
    Check(regs[W::kSh + 0x048] == 0x12345, "the vertex program address comes from the record");
    A::Restore<W::Value>(regs, used, saved, applied);
    Check(regs == original, "the register file is whole again after the key build");
}

void TestGroups() {
    constexpr std::array<A::ByteRange, 3> groups = {{{0, 4}, {4, 4}, {8, 2}}};
    static_assert(A::Partitions(groups, 10));
    constexpr std::array<A::ByteRange, 2> gap = {{{0, 4}, {5, 5}}};
    static_assert(!A::Partitions(gap, 10));
    constexpr std::array<A::ByteRange, 2> short_cover = {{{0, 4}, {4, 4}}};
    static_assert(!A::Partitions(short_cover, 10));
    std::array<u8, 10> a{}, b{};
    for (u8 i = 0; i < a.size(); ++i) a[i] = b[i] = i;
    Check(A::DifferingGroups(a, b, groups) == 0, "equal keys differ in no group");
    b[7] ^= 1;
    Check(A::DifferingGroups(a, b, groups) == 2, "the last byte of a group is compared");
    b[7] ^= 1;
    b[0] ^= 1;
    b[9] ^= 1;
    Check(A::DifferingGroups(a, b, groups) == 5, "each differing group sets its bit");
    std::array<u8, 6> shorter{};
    Check(A::DifferingGroups(shorter, b, groups) & 4, "a group past the end counts as different");
}

void TestJudge() {
    Check(A::Judge(true, true, 0, true) == A::Outcome::Same, "same key and pipeline");
    Check(A::Judge(true, true, 4, true) == A::Outcome::KeyDiffers, "a group differs");
    Check(A::Judge(true, true, 0, false) == A::Outcome::PipelineDiffers, "same key, another pipeline");
    Check(A::Judge(false, true, 0, true) == A::Outcome::RecordKeyMissing, "the record names no existing program");
    Check(A::Judge(false, false, 0, true) == A::Outcome::RegisterKeyMissing, "no register key: not checked");
    Check(A::Judge(true, false, 1, false) == A::Outcome::RegisterKeyMissing, "no register key wins over a difference");
    Check(A::IsDifference(A::Outcome::RecordKeyMissing) && !A::IsDifference(A::Outcome::RegisterKeyMissing), "missing keys class");
    Check(!A::IsChecked(A::Outcome::RegisterKeyMissing) && A::IsChecked(A::Outcome::Same), "checked class");
}

void TestCounts() {
    A::Counts c;
    A::Count(c, A::Left::Covered);
    A::Count(c, A::Left::NoRecord);
    A::Count(c, A::Left::Covered);
    Check(c.total == 3 && c.covered == 2 && c.left[static_cast<int>(A::Left::NoRecord)] == 1, "coverage counts");
    A::CountOutcome(c, A::Outcome::Same, 0);
    A::CountOutcome(c, A::Outcome::KeyDiffers, 5);
    A::CountOutcome(c, A::Outcome::RegisterKeyMissing, 1);
    Check(c.checked == 2 && c.differ == 1 && c.unchecked == 1, "checked, differ and unchecked are apart");
    Check(c.groups[0] == 1 && c.groups[1] == 0 && c.groups[2] == 1, "differing groups are counted");
}

void TestSuspect() {
    A::Counts c;
    Check(!A::InstrumentSuspect(A::Mode::Off, c), "no draws: not suspect");
    c.total = 10;
    Check(A::InstrumentSuspect(A::Mode::Off, c), "draws but none covered: suspect even count-only");
    c.covered = 5;
    Check(!A::InstrumentSuspect(A::Mode::Off, c), "count only with covered draws: fine");
    Check(A::InstrumentSuspect(A::Mode::Compare, c), "=2 covered but nothing checked: suspect");
    c.checked = 5;
    Check(!A::InstrumentSuspect(A::Mode::Compare, c), "=2 checked: fine");
    c.differ = 5;
    Check(A::InstrumentSuspect(A::Mode::Use, c), "every check differs: suspect");
    c.differ = 1;
    Check(!A::InstrumentSuspect(A::Mode::Use, c), "some differ: a finding, not suspect");
}

void TestLine() {
    A::Counts c;
    A::Count(c, A::Left::Covered);
    A::Count(c, A::Left::NoPayload);
    A::CountOutcome(c, A::Outcome::KeyDiffers, 2);
    c.disabled = 1;
    const std::array<const char*, 2> names = {"stages", "vertex formats"};
    char line[512];
    A::FormatLine(line, sizeof line, A::Mode::Use, c, names);
    const std::string text = line;
    Check(text.find("LANTERN_GX_WHOLE=1") != std::string::npos, "the line names the switch");
    Check(text.find("record lost 1") != std::string::npos, "the line names reasons left out");
    Check(text.find("key 1") != std::string::npos, "the line names the outcome");
    Check(text.find("vertex formats 1") != std::string::npos, "the line names the group");
    Check(text.find("turned itself off") != std::string::npos, "the line says the route is off");
    Check(text.find("0 covered draws on the register path since") != std::string::npos, "the line counts draws after the turn-off");
    A::FormatLine(line, sizeof line, A::Mode::Off, c, names);
    Check(std::string(line).find("count only") != std::string::npos, "=0 prints the count-only line");
    char tiny[16];
    const std::size_t used = A::FormatLine(tiny, sizeof tiny, A::Mode::Compare, c, names);
    Check(used < sizeof tiny && tiny[used] == '\0', "the line stays inside its buffer");
}

int RunSuite() {
    g_failed = 0;
    TestCoverage();
    TestRoute();
    TestSample();
    TestWiring();
    TestOverlay();
    TestOverlayRecord();
    TestGroups();
    TestJudge();
    TestCounts();
    TestSuspect();
    TestLine();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    using A::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        A::selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("gx whole stage A mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("gx whole stage A mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("gx whole stage A mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("gx whole stage A mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("gx whole stage A: %d failures\n", g_failed);
        return 1;
    }
    std::printf("gx whole stage A: all tests passed\n");
    return 0;
#endif
}
