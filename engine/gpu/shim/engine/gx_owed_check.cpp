// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_owed_check.h"
#include "gx_bindings.h"
#include "gx_records.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>

namespace BbEngine::GxWhole::OwedCheck {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Owed::Reader;

constexpr std::size_t kReaders = std::size_t(Reader::kCount);
constexpr u32 kLoggedDifferences = 8;
constexpr u32 kTopRegisters = 8;
constexpr u32 kConfigBase = 0x2000, kShBase = GxWhole::kSh, kShEnd = 0x3000, kUconfigBase = GxWhole::kUconfig;
constexpr u32 kPacketWordsRead = 2;

struct Counts {
    std::atomic<u64> draws{0}, covered{0}, lost{0}, overwritten{0}, no_whole{0};
    std::array<std::atomic<u64>, kReaders> transitions{};
    std::atomic<u64> checked_transitions{0}, differ_transitions{0};
    std::atomic<u64> materialized{0}, checked_words{0}, differ_words{0}, refused{0};
};

struct Window {
    u64 draws = 0, covered = 0, lost = 0, overwritten = 0, no_whole = 0;
    std::array<u64, kReaders> transitions{};
    u64 checked_transitions = 0, differ_transitions = 0;
    u64 materialized = 0, checked_words = 0, differ_words = 0, refused = 0;
};

struct Owner {
    std::atomic<bool> on{false};
    bool attached = false;
    std::unique_ptr<Owed::OwedCompare<kRegisterWords>> compare;
    std::unique_ptr<std::array<Owed::OwedDifference, kRegisterWords>> differences;
    std::unique_ptr<std::atomic<u32>[]> by_register;
    Counts counts;
    std::atomic<u32> logged{0};
    Window reported;
};
Owner g_owed;

void Bump(std::atomic<u64>& counter, u64 by = 1) {
    counter.fetch_add(by, std::memory_order_relaxed);
}

u64 Load(const std::atomic<u64>& counter) {
    return counter.load(std::memory_order_relaxed);
}

Window Snapshot() {
    const Counts& c = g_owed.counts;
    Window w;
    w.draws = Load(c.draws);
    w.covered = Load(c.covered);
    w.lost = Load(c.lost);
    w.overwritten = Load(c.overwritten);
    w.no_whole = Load(c.no_whole);
    for (std::size_t r = 0; r < kReaders; ++r) w.transitions[r] = Load(c.transitions[r]);
    w.checked_transitions = Load(c.checked_transitions);
    w.differ_transitions = Load(c.differ_transitions);
    w.materialized = Load(c.materialized);
    w.checked_words = Load(c.checked_words);
    w.differ_words = Load(c.differ_words);
    w.refused = Load(c.refused);
    return w;
}

struct Bank {
    const char* name;
    u32 index;
};

Bank BankOf(u32 word) {
    if (word >= kUconfigBase) return {"uconfig", word - kUconfigBase};
    if (word >= kContextBase) return {"context", word - kContextBase};
    if (word >= kShBase && word < kShEnd) return {"sh", word - kShBase};
    if (word >= kConfigBase && word < kShBase) return {"config", word - kConfigBase};
    return {"other", word};
}

const char* NameOf(u32 word) {
    const Bank bank = BankOf(word);
    if (word >= kContextBase && word < kUconfigBase) return GxBindings::RegisterName(static_cast<std::uint16_t>(bank.index));
    for (const FieldInfo& field : kFieldInfo) {
        if (word >= field.first && word < field.first + field.count) return field.name;
    }
    return "?";
}

void LogDifference(Reader reader, const Owed::OwedDifference& difference, u64 guest_packet) {
    if (g_owed.logged.fetch_add(1, std::memory_order_relaxed) >= kLoggedDifferences) return;
    const Bank bank = BankOf(difference.word);
    std::printf("GX whole: owed register %s %#x (%s) differs before a %s at guest %#llx: materialized %08x, decoded %08x\n",
                bank.name, bank.index, NameOf(difference.word), Owed::kReaderNames[std::size_t(reader)],
                static_cast<unsigned long long>(guest_packet), difference.materialized, difference.decoded);
}

std::span<const Owed::OwedWord> RecordWords(const GxBindings::State& state, bool indexed, std::array<Owed::OwedWord, kMaxValues>& out) {
    std::array<Value, kMaxValues> values;
    const u32 count = std::min<u32>(RegistersFromRecord(state, indexed, values), kMaxValues);
    for (u32 i = 0; i < count; ++i) out[i] = {values[i].reg, values[i].value, values[i].mask};
    return {out.data(), count};
}

void NoteTransition(Reader reader, std::span<const u32> file, u64 guest_packet) {
    Counts& c = g_owed.counts;
    Bump(c.transitions[std::size_t(reader)]);
    std::array<Owed::OwedDifference, kRegisterWords>& differences = *g_owed.differences;
    const Owed::TransitionResult result = Owed::Transition(*g_owed.compare, file, std::span<Owed::OwedDifference>(differences));
    Bump(c.materialized, result.materialized);
    if (result.checked == 0) return;
    Bump(c.checked_transitions);
    Bump(c.checked_words, result.checked);
    if (result.differ == 0) return;
    Bump(c.differ_transitions);
    Bump(c.differ_words, result.differ);
    const u32 listed = std::min<u32>(result.differ, kRegisterWords);
    for (u32 i = 0; i < listed; ++i) {
        g_owed.by_register[differences[i].word].fetch_add(1, std::memory_order_relaxed);
        LogDifference(reader, differences[i], guest_packet);
    }
}

bool Covered(u64 record, bool indexed, std::span<const u32> file) {
    Counts& c = g_owed.counts;
    if (!record) return false;
    const GxBindings::State* state = GxRecords::Taken(record);
    if (!state) {
        Bump(c.lost);
        return false;
    }
    if (!state->whole.captured) {
        Bump(c.no_whole);
        return false;
    }
    std::array<Owed::OwedWord, kMaxValues> words;
    const std::span<const Owed::OwedWord> owed = RecordWords(*state, indexed, words);
    if (!GxRecords::Intact()) {
        Bump(c.overwritten);
        return false;
    }
    Bump(c.covered);
    const u64 refused_before = g_owed.compare->owed.refused;
    Owed::CoveredDraw(*g_owed.compare, owed, file);
    Bump(c.refused, g_owed.compare->owed.refused - refused_before);
    return true;
}
}

bool Start(Mode mode) {
    if (mode != Mode::Compare) return false;
    g_owed.compare = std::make_unique<Owed::OwedCompare<kRegisterWords>>();
    g_owed.differences = std::make_unique<std::array<Owed::OwedDifference, kRegisterWords>>();
    g_owed.by_register = std::make_unique<std::atomic<u32>[]>(kRegisterWords);
    g_owed.on.store(true, std::memory_order_release);
    std::printf("GX whole: each covered draw's record owes its registers, compared with the decoded register file "
                "before every reader (LANTERN_GX_WHOLE=2)\n");
    return true;
}

bool Attach(MarkHook& on_mark, std::span<const std::uint32_t> file) {
    if (!g_owed.on.load(std::memory_order_acquire)) return false;
    if (!g_owed.attached) {
        g_owed.attached = true;
        on_mark = &OnMark;
        Owed::Sync(*g_owed.compare, file);
    }
    return true;
}

void OnMark(void*, std::uint32_t word, std::uint32_t count) {
    Owed::Written(*g_owed.compare, word, count);
}

void TokenStart() {
    Owed::TokenStart(*g_owed.compare);
}

void Read(Reader reader, std::span<const std::uint32_t> file, std::uint64_t guest_packet) {
    if (reader == Reader::None || reader == Reader::CoveredDraw) return;
    if (reader == Reader::ClearState) {
        Bump(g_owed.counts.transitions[std::size_t(reader)]);
        Owed::Sync(*g_owed.compare, file);
        return;
    }
    NoteTransition(reader, file, guest_packet);
}

void SubmissionEnd(std::span<const std::uint32_t> file) {
    NoteTransition(Reader::SubmissionEnd, file, 0);
}

void Note(std::uint32_t opcode, const std::uint32_t* packet, bool token, bool indexed, std::uint64_t record,
          std::span<const std::uint32_t> file, std::uint64_t guest_packet) {
    Owed::PacketFacts facts = Owed::FactsOf(opcode, std::span<const u32>(packet, kPacketWordsRead), token);
    if (facts.token || facts.packet_draw || facts.indirect_draw) Bump(g_owed.counts.draws);
    if (facts.token) facts.covered = Covered(record, indexed, file);
    Read(Owed::ReaderOf(facts), file, guest_packet);
}

void Print(double frames) {
    if (!g_owed.on.load(std::memory_order_acquire)) return;
    const Window now = Snapshot();
    const Window& last = g_owed.reported;
    const auto d = [](u64 a, u64 b) { return a >= b ? a - b : a; };
    const double f = frames > 0.0 ? frames : 1.0;
    u64 transitions = 0;
    for (std::size_t r = 0; r < kReaders; ++r) transitions += d(now.transitions[r], last.transitions[r]);
    const u64 draws = d(now.draws, last.draws), covered = d(now.covered, last.covered);
    const u64 checked = d(now.checked_words, last.checked_words), differ = d(now.differ_words, last.differ_words);
    std::printf("GX whole owed registers (compare): %.1f readers/frame (", transitions / f);
    bool first = true;
    for (std::size_t r = 0; r < kReaders; ++r) {
        const u64 n = d(now.transitions[r], last.transitions[r]);
        if (!n) continue;
        std::printf("%s%s %.1f", first ? "" : ", ", Owed::kReaderNames[r], n / f);
        first = false;
    }
    std::printf("); covered %llu of %llu draws, records lost %llu, overwritten %llu, without whole state %llu; "
                "%.1f owed words materialized a reader; checked %llu words at %llu readers, %llu differ at %llu readers; "
                "%llu owed words refused\n",
                static_cast<unsigned long long>(covered), static_cast<unsigned long long>(draws),
                static_cast<unsigned long long>(d(now.lost, last.lost)),
                static_cast<unsigned long long>(d(now.overwritten, last.overwritten)),
                static_cast<unsigned long long>(d(now.no_whole, last.no_whole)),
                transitions ? double(d(now.materialized, last.materialized)) / double(transitions) : 0.0,
                static_cast<unsigned long long>(checked),
                static_cast<unsigned long long>(d(now.checked_transitions, last.checked_transitions)),
                static_cast<unsigned long long>(differ),
                static_cast<unsigned long long>(d(now.differ_transitions, last.differ_transitions)),
                static_cast<unsigned long long>(d(now.refused, last.refused)));
    if ((draws > 0 && covered == 0) || (covered > 0 && checked == 0) || (checked > 0 && differ == checked)) {
        std::printf("GX whole owed registers: INSTRUMENT SUSPECT (covered %llu of %llu draws, %llu of %llu checked words differ)\n",
                    static_cast<unsigned long long>(covered), static_cast<unsigned long long>(draws),
                    static_cast<unsigned long long>(differ), static_cast<unsigned long long>(checked));
    }
    std::array<std::pair<u32, u32>, kTopRegisters> top{};
    for (u32 word = 0; word < kRegisterWords; ++word) {
        const u32 n = g_owed.by_register[word].load(std::memory_order_relaxed);
        if (n <= top.back().second) continue;
        top.back() = {word, n};
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    }
    if (top.front().second) {
        std::printf("GX whole owed registers differing so far:");
        for (const auto& [word, n] : top) {
            if (!n) break;
            const Bank bank = BankOf(word);
            std::printf(" %s %#x %s %u;", bank.name, bank.index, NameOf(word), n);
        }
        std::printf("\n");
    }
    g_owed.reported = now;
}
}
