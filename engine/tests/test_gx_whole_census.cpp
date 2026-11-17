// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/gx_whole_census.h"
#include "video_core/amdgpu/pm4_opcodes.h"

#include <cstdio>
#include <cstring>

namespace {
namespace C = BbEngine::GxWholeCensus;
using Op = AmdGpu::PM4ItOpcode;
int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

constexpr bool Same(Op op, std::uint32_t value) {
    return static_cast<std::uint32_t>(op) == value;
}
static_assert(Same(Op::Nop, C::kOpNop));
static_assert(Same(Op::ClearState, C::kOpClearState));
static_assert(Same(Op::IndexBufferSize, C::kOpIndexBufferSize));
static_assert(Same(Op::DispatchDirect, C::kOpDispatchDirect));
static_assert(Same(Op::DispatchIndirect, C::kOpDispatchIndirect));
static_assert(Same(Op::DrawIndirect, C::kOpDrawIndirect));
static_assert(Same(Op::DrawIndexIndirect, C::kOpDrawIndexIndirect));
static_assert(Same(Op::IndexBase, C::kOpIndexBase));
static_assert(Same(Op::DrawIndex2, C::kOpDrawIndex2));
static_assert(Same(Op::IndexType, C::kOpIndexType));
static_assert(Same(Op::DrawIndirectMulti, C::kOpDrawIndirectMulti));
static_assert(Same(Op::DrawIndexAuto, C::kOpDrawIndexAuto));
static_assert(Same(Op::NumInstances, C::kOpNumInstances));
static_assert(Same(Op::DrawIndexOffset2, C::kOpDrawIndexOffset2));
static_assert(Same(Op::DrawIndexIndirectMulti, C::kOpDrawIndexIndirectMulti));
static_assert(Same(Op::DrawIndexIndirectCountMulti, C::kOpDrawIndexIndirectCountMulti));
static_assert(Same(Op::SetConfigReg, C::kOpSetConfigReg));
static_assert(Same(Op::SetContextReg, C::kOpSetContextReg));
static_assert(Same(Op::SetShReg, C::kOpSetShReg));
static_assert(Same(Op::SetUconfigReg, C::kOpSetUconfigReg));

std::uint32_t Code(Op op) {
    return static_cast<std::uint32_t>(op);
}

C::DrawFacts Whole() {
    C::DrawFacts facts;
    facts.token = true;
    facts.records_on = true;
    facts.records_used = true;
    facts.record = true;
    facts.verdict.bindings = C::kBindingsCovered;
    facts.verdict.render = true;
    return facts;
}

void Coverage() {
    Check(C::CoverageOf(Whole()) == C::Covered, "a token draw with its record, bindings and render state is covered");
    Check(C::CoverageOf(C::DrawFacts{}) == C::NotToken, "a draw packet without a token is not a GX token");
    C::DrawFacts f = Whole();
    f.records_on = false;
    Check(C::CoverageOf(f) == C::RecordsOff, "records off");
    f = Whole();
    f.rect_list = true;
    Check(C::CoverageOf(f) == C::RectList, "rect list");
    f = Whole();
    f.hull_or_geometry = true;
    f.record = false;
    Check(C::CoverageOf(f) == C::HullOrGeometry, "hull or geometry before the missing record it causes");
    f = Whole();
    f.predicated = true;
    Check(C::CoverageOf(f) == C::Predicated, "a predicated draw stays on the old path");
    f = Whole();
    f.record = false;
    Check(C::CoverageOf(f) == C::NoRecord, "no record");
    f = Whole();
    f.records_used = false;
    f.verdict = {};
    Check(C::CoverageOf(f) == C::RecordsNotUsed, "records taken but not used at =1");
    f = Whole();
    f.verdict.bindings = C::kBindingsNotApplied;
    Check(C::CoverageOf(f) == C::NotApplied, "a record that never reached the bindings");
    f = Whole();
    f.verdict.bindings = C::kBindingsLost;
    Check(C::CoverageOf(f) == C::NoPayload, "a record whose payload was lost");
    f = Whole();
    f.verdict.not_checked = true;
    Check(C::CoverageOf(f) == C::NotChecked, "a skipped stage is not covered");
    f = Whole();
    f.verdict.bindings = 5;
    Check(C::CoverageOf(f) == C::Bindings, "a bindings reason");
    f = Whole();
    f.verdict.render = false;
    Check(C::CoverageOf(f) == C::RenderState, "render state not from the record");
}

void Kinds() {
    Check(C::KindOf(C::kOpSetShReg, false) == C::RegisterPacket, "SET_SH_REG is a register packet");
    Check(C::KindOf(C::kOpSetContextReg, false) == C::RegisterPacket, "SET_CONTEXT_REG is a register packet");
    Check(C::KindOf(C::kOpSetUconfigReg, false) == C::RegisterPacket, "SET_UCONFIG_REG is a register packet");
    Check(C::KindOf(C::kOpSetConfigReg, false) == C::RegisterPacket, "SET_CONFIG_REG is a register packet");
    Check(C::KindOf(C::kOpIndexType, false) == C::RegisterPacket, "INDEX_TYPE is a register packet");
    Check(C::KindOf(C::kOpNumInstances, false) == C::RegisterPacket, "NUM_INSTANCES is a register packet");
    Check(C::KindOf(C::kOpNop, true) == C::RegisterPacket, "the bind NOP is a register packet");
    Check(C::KindOf(C::kOpNop, false) == C::OtherPacket, "another NOP is not");
    Check(C::KindOf(Code(Op::EventWrite), false) == C::OtherPacket, "an event is not");
    Check(C::IsDrawOpcode(Code(Op::DrawIndexAuto)) && C::IsDrawOpcode(Code(Op::DrawIndexIndirectCountMulti)),
          "draw opcodes are draws");
    Check(!C::IsDrawOpcode(Code(Op::DispatchDirect)) && C::IsDispatchOpcode(Code(Op::DispatchDirect)),
          "a dispatch is not a draw");
}

void Windows() {
    C::Window w;
    C::NotePacket(w, C::RegisterPacket, 4);
    C::NotePacket(w, C::RegisterPacket, 6);
    C::NotePacket(w, C::OtherPacket, 3);
    C::NoteDraw(w, C::Covered);
    Check(w.draws[C::Covered] == 1, "one covered draw");
    Check(w.walked[C::Covered].dwords[C::RegisterPacket] == 10 && w.walked[C::Covered].packets[C::RegisterPacket] == 2,
          "the register packets before a draw go to its reason");
    Check(w.walked[C::Covered].dwords[C::OtherPacket] == 3, "the other packets too");
    C::NotePacket(w, C::RegisterPacket, 2);
    C::NoteDraw(w, C::NoRecord);
    Check(w.walked[C::NoRecord].dwords[C::RegisterPacket] == 2, "the next draw gets only the packets after the last draw");
    Check(C::Total(w) == 2, "total counts every reason");
    C::NotePacket(w, C::OtherPacket, 7);
    C::Restart(w);
    Check(C::Total(w) == 0 && w.frames == 0, "restart clears the window");
    Check(w.between.dwords[C::OtherPacket] == 7, "restart keeps the packets owed to the next draw");
}

void Outside() {
    C::Window w;
    C::NotePm4(w, C::kOpSetShReg, 5, false, false);
    C::NotePm4(w, C::kOpDispatchDirect, 4, false, false);
    C::NotePm4(w, C::kOpSetContextReg, 3, false, false);
    C::NotePm4(w, C::kOpNop, 6, false, true);
    C::NoteToken(w, Whole());
    Check(w.walked[C::Covered].dwords[C::RegisterPacket] == 3, "a dispatch takes the packets before it out of the next draw");
    Check(w.outside.dwords[C::RegisterPacket] == 5 && w.outside.dwords[C::OtherPacket] == 4,
          "the dispatch and its setup are outside draws");
    Check(w.walked[C::Covered].dwords[C::OtherPacket] == 0, "the token itself is not walked");
    C::NotePm4(w, C::kOpSetContextReg, 9, false, false);
    C::NoteOutside(w);
    Check(w.outside.dwords[C::RegisterPacket] == 14, "the end of a submission or a flip moves what is owed outside");
    C::NotePm4(w, C::kOpDrawIndex2, 7, false, false);
    Check(w.draws[C::NotToken] == 1, "a draw packet without a token counts as not a GX token");
    C::NoteTicks(w, 40, true);
    Check(w.draw_ticks[C::NotToken] == 40, "a draw packet's time goes to the last draw");
    C::NoteTicks(w, 9, false);
    Check(w.between.ticks == 9, "other packets' time waits for the next draw");
}

void Tokens() {
    C::Window w;
    C::DrawFacts f = Whole();
    f.verdict.pipeline_from_record = true;
    Check(C::NoteToken(w, f) == C::Covered, "note token returns the reason");
    f.verdict.render = false;
    C::NoteToken(w, f);
    f = Whole();
    f.record = false;
    C::NoteToken(w, f);
    Check(w.taken == 2, "taken counts draws with a record");
    Check(w.covered_pipeline_from_record == 1, "only covered draws count as covered with the pipeline from the record");
}

void Suspect() {
    C::Window w;
    Check(!C::InstrumentSuspect(w), "an empty window is not suspect");
    C::NoteDraw(w, C::RecordsOff);
    Check(C::InstrumentSuspect(w), "draws with none covered are suspect");
    char line[1024];
    C::Format(w, 0.0, line, sizeof(line));
    Check(std::strstr(line, "INSTRUMENT SUSPECT") != nullptr, "the suspect line is printed");
    Check(std::strstr(line, "records off 1") != nullptr, "the reason is printed with its count");
}

void Formats() {
    C::Window w;
    w.frames = 2;
    w.stamped = 3;
    C::NotePacket(w, C::RegisterPacket, 8);
    C::NoteToken(w, Whole());
    C::NoteDraw(w, C::NotToken);
    char line[1024];
    const std::size_t used = C::Format(w, 0.0, line, sizeof(line));
    Check(used == std::strlen(line), "the length returned is the line's");
    Check(std::strstr(line, "2 draws, 2 frames, 1 draws/frame, 50.0% covered") != nullptr, "share and draws per frame");
    Check(std::strstr(line, "records 3 stamped, 1 taken") != nullptr, "stamped and taken");
    Check(std::strstr(line, "8.0 register dwords in 1.0 packets") != nullptr, "register dwords per covered draw");
    Check(std::strstr(line, "not a GX token 1 (50.0%") != nullptr, "the uncovered reason");
    Check(std::strstr(line, "walk") == nullptr, "no time without the timers");
    Check(std::strstr(line, "INSTRUMENT SUSPECT") == nullptr, "no suspect line with a covered draw");
    w.walked[C::Covered].ticks = 3000;
    C::Format(w, 1000.0, line, sizeof(line));
    Check(std::strstr(line, "walk 3.000 us") != nullptr, "walk time per covered draw with the timers");
    char small[8];
    const std::size_t cut = C::Format(w, 0.0, small, sizeof(small));
    Check(cut < sizeof(small) && std::strlen(small) == cut, "a short buffer is cut, not overrun");
}

int RunSuite() {
    failures = 0;
    Coverage();
    Kinds();
    Windows();
    Outside();
    Tokens();
    Suspect();
    Formats();
    return failures;
}
}

int main() {
#ifdef LANTERN_GX_CENSUS_MUTANTS
    using C::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        C::selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("gx whole census mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("gx whole census mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("gx whole census mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("gx whole census mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("gx whole census: %d failures\n", failures);
        return 1;
    }
    std::printf("gx whole census: all tests passed\n");
    return 0;
#endif
}
