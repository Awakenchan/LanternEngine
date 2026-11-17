// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <map>
#include <set>
#include <vector>

#include "gpu/shim/engine/gx_owed_registers.h"

namespace {
namespace Gw = BbEngine::GxWhole::Owed;
using u32 = std::uint32_t;

constexpr u32 kWords = 64;
constexpr u32 kTokenFirst = 60, kTokenWords = 3;
constexpr u32 kFlushWords = 48;
constexpr int kSequences = 200;
constexpr int kEventsPerSequence = 120;

int failures = 0;
bool quiet = false;

void Expect(bool ok, const char* what) {
    if (!ok) {
        if (!quiet) std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct MarkLog {
    std::vector<u32> words;
};

void LogMark(void* context, u32 word, u32 count) {
    auto& log = *static_cast<MarkLog*>(context);
    for (u32 i = 0; i < count; ++i) log.words.push_back(word + i);
}

void TestMask() {
    auto image = std::make_unique<Gw::OwedImage<kWords>>();
    std::array<u32, kWords> file{};
    file[4] = 0x1234;
    file[5] = 0x1111;
    file[6] = 0x5555;
    const std::array<Gw::OwedWord, 4> words{{{4, 0xabcd, 0xff00}, {5, 0x99cd, 0x00ff}, {5, 0xab77, 0xff00}, {6, 0x7777, 0}}};
    Gw::Owe(*image, words);
    Expect(!Gw::IsOwed(*image, 6) && image->refused == 0, "a word with an empty mask owes nothing and is not refused");
    MarkLog log;
    Expect(Gw::Materialize(*image, std::span<u32>(file), Gw::MarkSink{&LogMark, &log}) == 2, "masked words are written");
    Expect(file[4] == 0xab34, "only the masked bits of an owed word reach the file");
    Expect(file[5] == 0xabcd, "two masks owed on one word both reach the file");
    Expect(file[6] == 0x5555, "an empty mask leaves the word");
    const std::array<Gw::OwedWord, 1> same{{{4, 0x00ab, 0xff00}}};
    file[4] = 0x0034;
    Gw::Owe(*image, same);
    Expect(Gw::Materialize(*image, std::span<u32>(file), {}) == 0 && file[4] == 0x0034,
           "a word whose masked bits already hold the value is not written");
}

void TestFacts() {
    namespace Op = Gw::Op;
    const auto facts = [](u32 opcode, u32 body, bool token = false) {
        const std::array<u32, 2> packet{0, body};
        return Gw::FactsOf(opcode, packet, token);
    };
    Expect(Gw::ReaderOf(facts(Op::DrawIndex2, 0)) == Gw::Reader::PacketDraw, "DRAW_INDEX_2 is a draw packet");
    Expect(Gw::ReaderOf(facts(Op::DrawIndexOffset2, 0)) == Gw::Reader::PacketDraw, "DRAW_INDEX_OFFSET_2 is a draw packet");
    Expect(Gw::ReaderOf(facts(Op::DrawIndexAuto, 0)) == Gw::Reader::PacketDraw, "DRAW_INDEX_AUTO is a draw packet");
    for (const u32 op : {Op::DrawIndirect, Op::DrawIndirectMulti, Op::DrawIndexIndirect, Op::DrawIndexIndirectMulti,
                         Op::DrawIndexIndirectCountMulti}) {
        Expect(Gw::ReaderOf(facts(op, 0)) == Gw::Reader::IndirectDraw, "every indirect draw opcode is an indirect draw");
    }
    Expect(Gw::ReaderOf(facts(Op::DispatchDirect, 0)) == Gw::Reader::Dispatch, "DISPATCH_DIRECT dispatches");
    Expect(Gw::ReaderOf(facts(Op::DispatchIndirect, 0)) == Gw::Reader::Dispatch, "DISPATCH_INDIRECT dispatches");
    Expect(Gw::ReaderOf(facts(Op::WaitRegMem, 0x3)) == Gw::Reader::RegisterWait, "WAIT_REG_MEM with mem space 0 reads a register");
    Expect(Gw::ReaderOf(facts(Op::WaitRegMem, 0x13)) == Gw::Reader::None, "WAIT_REG_MEM with mem space 1 reads memory");
    Expect(Gw::ReaderOf(facts(Op::EventWrite, 0x41f)) == Gw::Reader::StreamoutFlush, "event 31 is the streamout flush");
    Expect(Gw::ReaderOf(facts(Op::EventWrite, 0x416)) == Gw::Reader::None, "another event reads no register");
    Expect(Gw::ReaderOf(facts(Op::ClearState, 0)) == Gw::Reader::ClearState, "CLEAR_STATE resets");
    Expect(Gw::ReaderOf(facts(0x10, 0)) == Gw::Reader::None, "a NOP that is no token reads nothing");
    Expect(Gw::ReaderOf(facts(0x10, 0, true)) == Gw::Reader::UncoveredDraw, "a token is a draw");
    Expect(Gw::ReaderOf(Gw::FactsOf(Op::WaitRegMem, std::span<const u32>(), false)) == Gw::Reader::None,
           "a packet without a body reads nothing");
}

void TestClamp() {
    Expect(Gw::ClampSpan(kWords, 0, 4).first == 0 && Gw::ClampSpan(kWords, 0, 4).last == 4, "a span inside the window");
    Expect(Gw::ClampSpan(kWords, 62, 8).last == kWords, "a span past the window is cut at its end");
    Expect(Gw::ClampSpan(kWords, kWords, 2).last == 0, "a span after the window is empty");
    Expect(Gw::ClampSpan(kWords, 5, 0).last == 0, "an empty span stays empty");
}

void TestOwe() {
    auto image = std::make_unique<Gw::OwedImage<kWords>>();
    const std::array<Gw::OwedWord, 4> words{{{5, 1}, {9, 7}, {5, 2}, {12, 3}}};
    Expect(Gw::Owe(*image, words) == 0, "words inside the window are taken");
    Expect(Gw::IsOwed(*image, 5) && Gw::IsOwed(*image, 9) && Gw::IsOwed(*image, 12), "every word is owed, the last too");
    Expect(image->count == 3, "a word owed twice is listed once");
    Expect(image->values[5] == 2, "the later write of a word wins");
    const std::array<Gw::OwedWord, 2> outside{{{kWords, 1}, {kWords + 9, 1}}};
    Expect(Gw::Owe(*image, outside) == 2 && image->refused == 2, "words outside the window are refused and counted");
    Expect(Gw::Touches(*image, 5, 1), "a one-word read of an owed word touches it");
    Expect(Gw::Touches(*image, 3, 3), "a range ending on an owed word touches it");
    Expect(!Gw::Touches(*image, 6, 3), "a range between owed words does not");
    Gw::Forget(*image, 9, 1);
    Expect(!Gw::IsOwed(*image, 9), "a one-word write forgets the owed word");
    Gw::Forget(*image, 10, 3);
    Expect(!Gw::IsOwed(*image, 12), "a range write forgets its last word");
    Expect(Gw::IsOwed(*image, 5), "words outside the forgotten range stay owed");
}

void TestMaterialize() {
    auto image = std::make_unique<Gw::OwedImage<kWords>>();
    std::array<u32, kWords> file{};
    file[3] = 0x33;
    const std::array<Gw::OwedWord, 3> words{{{3, 0x33}, {4, 0x44}, {7, 0x77}}};
    Gw::Owe(*image, words);
    Gw::Forget(*image, 7, 1);
    MarkLog log;
    const u32 written = Gw::Materialize(*image, std::span<u32>(file), Gw::MarkSink{&LogMark, &log});
    Expect(written == 1, "only owed words that differ are written");
    Expect(file[4] == 0x44, "the owed value reaches the file");
    Expect(file[7] == 0, "a forgotten word is not written");
    Expect(log.words.size() == 1 && log.words[0] == 4, "each written word is marked, nothing else");
    Expect(!Gw::IsOwed(*image, 3) && !Gw::IsOwed(*image, 4) && image->count == 0, "materializing settles every owed word");
    Expect(!Gw::Touches(*image, 0, kWords), "nothing is owed after materializing");
    file[4] = 0;
    Expect(Gw::Materialize(*image, std::span<u32>(file), {}) == 0 && file[4] == 0, "a second materialize writes nothing");
    const std::array<Gw::OwedWord, 1> again{{{4, 0x45}}};
    Gw::Owe(*image, again);
    Expect(image->count == 1 && Gw::Materialize(*image, std::span<u32>(file), {}) == 1 && file[4] == 0x45,
           "a word owed again after materializing is listed and written again");
    std::array<u32, 2> small{};
    const std::array<Gw::OwedWord, 1> beyond{{{10, 1}}};
    Gw::Owe(*image, beyond);
    const std::uint64_t refused = image->refused;
    Expect(Gw::Materialize(*image, std::span<u32>(small), {}) == 0 && image->refused == refused + 1,
           "a word past the file is refused, not written");
}

void TestReaders() {
    Gw::PacketFacts facts;
    Expect(Gw::ReaderOf(facts) == Gw::Reader::None, "a packet that reads nothing is no transition");
    facts.token = true;
    Expect(Gw::ReaderOf(facts) == Gw::Reader::UncoveredDraw, "a token without a record is an uncovered draw");
    facts.covered = true;
    Expect(Gw::ReaderOf(facts) == Gw::Reader::CoveredDraw, "a covered token owes, it does not read");
    facts = {};
    facts.packet_draw = true;
    Expect(Gw::ReaderOf(facts) == Gw::Reader::PacketDraw, "a draw packet reads");
    facts = {};
    facts.indirect_draw = true;
    Expect(Gw::ReaderOf(facts) == Gw::Reader::IndirectDraw, "an indirect draw reads");
    facts = {};
    facts.dispatch = true;
    Expect(Gw::ReaderOf(facts) == Gw::Reader::Dispatch, "a dispatch reads");
    facts = {};
    facts.wait = true;
    Expect(Gw::ReaderOf(facts) == Gw::Reader::None, "a wait on memory reads no register");
    facts.wait_on_register = true;
    Expect(Gw::ReaderOf(facts) == Gw::Reader::RegisterWait, "a wait on a register reads it");
    facts = {};
    facts.streamout_flush = true;
    Expect(Gw::ReaderOf(facts) == Gw::Reader::StreamoutFlush, "the streamout flush changes one bit of a register");
    facts.clear_state = true;
    Expect(Gw::ReaderOf(facts) == Gw::Reader::ClearState, "clear state comes first");
    Expect(Gw::kReaderNames.size() == std::size_t(Gw::Reader::kCount), "every reader has a name");
}

struct Stream {
    std::array<u32, kWords> file{};
    std::unique_ptr<Gw::OwedCompare<kWords>> compare = std::make_unique<Gw::OwedCompare<kWords>>();
    std::array<Gw::OwedDifference, kWords> out{};

    void Write(u32 word, u32 value) {
        file[word] = value;
        Gw::Written(*compare, word, 1);
    }
    Gw::TransitionResult Read() {
        return Gw::Transition(*compare, std::span<const u32>(file), std::span<Gw::OwedDifference>(out));
    }
};

void TestCompareScenarios() {
    {
        Stream s;
        Gw::Sync(*s.compare, s.file);
        s.Write(4, 0x40);
        Gw::TokenStart(*s.compare);
        const std::array<Gw::OwedWord, 1> record{{{5, 0}}};
        Gw::CoveredDraw(*s.compare, record, s.file);
        const auto result = s.Read();
        Expect(result.differ == 1 && s.out[0].word == 4 && s.out[0].decoded == 0x40 && s.out[0].materialized == 0,
               "a word the flush wrote and the record lacks differs at the transition");
        Expect(s.compare->image[4] == 0x40, "a differing word is resynced to the decoded file");
        Expect(s.Read().differ == 0, "a difference is counted at one transition only");
    }
    {
        Stream s;
        Gw::Sync(*s.compare, s.file);
        s.Write(4, 0x40);
        Gw::TokenStart(*s.compare);
        s.Write(kTokenFirst, 0x99);
        const std::array<Gw::OwedWord, 2> record{{{4, 0x40}, {kTokenFirst, 0x11}}};
        Gw::CoveredDraw(*s.compare, record, s.file);
        const auto result = s.Read();
        Expect(result.differ == 0, "a complete record and the token's own write leave no difference");
        Expect(s.compare->image[kTokenFirst] == 0x99, "the token's write is later than the record's value");
        Expect(result.materialized == 1 && result.checked == 1, "the owed word is materialized and checked");
    }
    {
        Stream s;
        Gw::Sync(*s.compare, s.file);
        s.Write(4, 0x40);
        Gw::TokenStart(*s.compare);
        const std::array<Gw::OwedWord, 1> record{{{6, 0}}};
        Gw::CoveredDraw(*s.compare, record, s.file);
        s.Write(4, 0x41);
        const auto result = s.Read();
        Expect(result.differ == 0, "a kept write before the reader puts the missing word back");
        Expect(s.compare->image[4] == 0x41, "the kept write reaches the materialized image");
    }
    {
        Stream s;
        Gw::Sync(*s.compare, s.file);
        s.Write(8, 0x80);
        const auto result = s.Read();
        Expect(result.differ == 0 && result.checked == 0, "with no covered draw a transition checks nothing");
        Expect(s.compare->image[8] == 0x80, "an uncovered draw's own writes are kept");
    }
    {
        Stream s;
        Gw::Sync(*s.compare, s.file);
        s.Write(4, 0x40);
        Gw::TokenStart(*s.compare);
        const std::array<Gw::OwedWord, 1> record{{{4, 0x42}}};
        Gw::CoveredDraw(*s.compare, record, s.file);
        const auto result = s.Read();
        Expect(result.differ == 1 && s.out[0].materialized == 0x42 && s.out[0].decoded == 0x40,
               "a record value that disagrees with the flush differs");
    }
    {
        Stream s;
        s.file[4] = 0x0011;
        Gw::Sync(*s.compare, s.file);
        s.Write(4, 0x1211);
        Gw::TokenStart(*s.compare);
        const std::array<Gw::OwedWord, 1> record{{{4, 0x12ee, 0xff00}}};
        Gw::CoveredDraw(*s.compare, record, s.file);
        Expect(s.Read().differ == 0, "a masked record carrying every bit the flush changed leaves no difference");
    }
    {
        Stream s;
        s.file[4] = 0x0011;
        Gw::Sync(*s.compare, s.file);
        s.Write(4, 0x1234);
        Gw::TokenStart(*s.compare);
        const std::array<Gw::OwedWord, 1> record{{{4, 0x12ee, 0xff00}}};
        Gw::CoveredDraw(*s.compare, record, s.file);
        const auto result = s.Read();
        Expect(result.differ == 1 && s.out[0].materialized == 0x1211 && s.out[0].decoded == 0x1234,
               "bits the flush changed outside the record's mask differ");
    }
}

struct Model {
    std::array<u32, kWords> old_file{};
    std::array<u32, kWords> whole_file{};
    std::unique_ptr<Gw::OwedImage<kWords>> owed = std::make_unique<Gw::OwedImage<kWords>>();
    Stream compare;
};

void KeptWrite(Model& m, u32 word, u32 value) {
    m.old_file[word] = value;
    Gw::Forget(*m.owed, word, 1);
    m.whole_file[word] = value;
    m.compare.Write(word, value);
}

void RunSequence(std::mt19937& random, bool complete) {
    Model m;
    Gw::Sync(*m.compare.compare, m.compare.file);
    const auto pick = [&](u32 n) { return u32(random() % n); };
    bool equal_everywhere = true, matched = true;
    for (int event = 0; event < kEventsPerSequence; ++event) {
        const u32 kind = pick(4);
        if (kind <= 1) {
            std::vector<Gw::OwedWord> record;
            std::map<u32, u32> flushed;
            for (u32 n = pick(5); n > 0; --n) {
                const u32 word = pick(kFlushWords);
                const u32 mask = flushed.count(word) ? flushed[word] : pick(2) ? ~0u : u32(random()) | 1u;
                const u32 value = complete ? Gw::Merge(m.old_file[word], u32(random()), mask) : u32(random());
                m.old_file[word] = value;
                m.compare.Write(word, value);
                flushed[word] = mask;
            }
            for (const auto& [word, mask] : flushed) {
                record.push_back({word, Gw::Merge(u32(random()), m.old_file[word], mask), mask});
            }
            if (pick(2)) record.push_back({pick(kFlushWords), 0});
            if (!record.empty() && record.back().value == 0) record.back().value = m.old_file[record.back().word];
            if (!complete && !record.empty() && pick(3) == 0) {
                const std::size_t at = pick(u32(record.size()));
                if (pick(2)) record.erase(record.begin() + std::ptrdiff_t(at));
                else record[at].value ^= 1;
            }
            Gw::TokenStart(*m.compare.compare);
            Gw::Owe(*m.owed, record);
            for (u32 t = 0; t < kTokenWords; ++t) {
                if (pick(2)) KeptWrite(m, kTokenFirst + t, random());
            }
            Gw::CoveredDraw(*m.compare.compare, record, m.compare.file);
        } else {
            if (kind == 2) {
                for (u32 n = pick(4); n > 0; --n) KeptWrite(m, pick(kWords), random());
            }
            Gw::Materialize(*m.owed, std::span<u32>(m.whole_file), {});
            std::set<u32> expected;
            for (u32 w = 0; w < kWords; ++w) {
                if (m.whole_file[w] != m.old_file[w]) expected.insert(w);
            }
            equal_everywhere = equal_everywhere && expected.empty();
            const auto result = m.compare.Read();
            std::set<u32> reported;
            for (u32 i = 0; i < result.differ && i < kWords; ++i) reported.insert(m.compare.out[i].word);
            matched = matched && reported == expected;
            m.whole_file = m.old_file;
        }
    }
    if (complete) {
        Expect(equal_everywhere, "complete records, owed words materialized before readers, give the old path's file");
    }
    Expect(matched, "the compare reports exactly the words where the owed file and the old file differ");
}

void TestSequences() {
    std::mt19937 random(0x0ed0ed);
    for (int i = 0; i < kSequences; ++i) RunSequence(random, true);
    for (int i = 0; i < kSequences; ++i) RunSequence(random, false);
}

int RunAll() {
    failures = 0;
    TestMask();
    TestFacts();
    TestClamp();
    TestOwe();
    TestMaterialize();
    TestReaders();
    TestCompareScenarios();
    TestSequences();
    return failures;
}
}

int main(int argc, char** argv) {
    const bool mutants = argc > 1 && std::strcmp(argv[1], "--mutants") == 0;
    if (!mutants) {
        const int failed = RunAll();
        std::printf("gx-owed-registers-test: %s (%d failures)\n", failed ? "FAILED" : "passed", failed);
        return failed ? 1 : 0;
    }
#ifdef LANTERN_GX_OWED_MUTANTS
    quiet = true;
    int survivors = 0;
    for (int m = 1; m < int(Gw::Mutant::kCount); ++m) {
        Gw::mutant = Gw::Mutant(m);
        const int failed = RunAll();
        std::printf("mutant %d: %s (%d failures)\n", m, failed ? "caught" : "SURVIVED", failed);
        survivors += failed == 0;
    }
    Gw::mutant = Gw::Mutant::None;
    quiet = false;
    const int failed = RunAll();
    std::printf("gx-owed-registers-mutants: %d survivors, shipped lines %s\n", survivors, failed ? "FAILED" : "passed");
    return survivors || failed ? 1 : 0;
#else
    std::printf("gx-owed-registers-test: --mutants needs a build with LANTERN_GX_OWED_MUTANTS\n");
    return 1;
#endif
}
