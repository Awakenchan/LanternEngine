// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>

#ifdef LANTERN_GX_OWED_MUTANTS
namespace BbEngine::GxWhole::Owed {
enum class Mutant : std::uint8_t {
    None,
    OweFirstWins,
    OweSkipsLastWord,
    MaterializeKeepsBit,
    MaterializeWritesEqual,
    MaterializeSkipsMark,
    ForgetMissesLastWord,
    TouchesOffByOne,
    TokenKeepsPreRun,
    KeptNotSuperseding,
    TransitionDropsRun,
    ResyncSkipped,
    CoveredTokenReads,
    WaitOnMemoryReads,
    MaterializeIgnoresMask,
    OweDropsEarlierMask,
    StreamoutAnyEvent,
    kCount
};
inline Mutant mutant = Mutant::None;
}
#define GX_OWED_PICK(which, wrong, right)                                                                    \
    (::BbEngine::GxWhole::Owed::mutant == ::BbEngine::GxWhole::Owed::Mutant::which ? (wrong) : (right))
#else
#define GX_OWED_PICK(which, wrong, right) (right)
#endif

namespace BbEngine::GxWhole::Owed {

inline constexpr std::uint32_t kMaxWords = 0x10000;

struct OwedWord {
    std::uint32_t word;
    std::uint32_t value;
    std::uint32_t mask = ~0u;
};

struct MarkSink {
    void (*fn)(void* context, std::uint32_t word, std::uint32_t count) = nullptr;
    void* context = nullptr;
};

template <std::uint32_t Words>
using WordBits = std::array<std::uint64_t, (Words + 63) / 64>;

template <std::uint32_t Words>
constexpr bool TestBit(const WordBits<Words>& bits, std::uint32_t word) {
    return (bits[word / 64] >> (word % 64)) & 1;
}

template <std::uint32_t Words>
constexpr void SetBit(WordBits<Words>& bits, std::uint32_t word) {
    bits[word / 64] |= std::uint64_t(1) << (word % 64);
}

template <std::uint32_t Words>
constexpr void ClearBit(WordBits<Words>& bits, std::uint32_t word) {
    bits[word / 64] &= ~(std::uint64_t(1) << (word % 64));
}

struct WordSpan {
    std::uint32_t first = 0;
    std::uint32_t last = 0;
};

constexpr WordSpan ClampSpan(std::uint32_t words, std::uint32_t first, std::uint32_t count) {
    if (first >= words || count == 0) return {0, 0};
    const std::uint32_t room = words - first;
    return {first, first + (count < room ? count : room)};
}

constexpr std::uint32_t Merge(std::uint32_t under, std::uint32_t value, std::uint32_t mask) {
    return (under & ~mask) | (value & mask);
}

template <std::uint32_t Words>
struct OwedImage {
    static_assert(Words > 0 && Words <= kMaxWords);
    WordBits<Words> owed{};
    WordBits<Words> listed{};
    std::array<std::uint32_t, Words> values{};
    std::array<std::uint32_t, Words> masks{};
    std::array<std::uint16_t, Words> order{};
    std::uint32_t count = 0;
    std::uint64_t refused = 0;
};

template <std::uint32_t Words>
constexpr bool IsOwed(const OwedImage<Words>& image, std::uint32_t word) {
    return word < Words && TestBit<Words>(image.owed, word);
}

template <std::uint32_t Words>
constexpr std::uint32_t Owe(OwedImage<Words>& image, std::span<const OwedWord> words) {
    std::uint32_t refused = 0;
    const std::size_t end = GX_OWED_PICK(OweSkipsLastWord, words.empty() ? 0 : words.size() - 1, words.size());
    for (std::size_t i = 0; i < end; ++i) {
        const OwedWord& owed = words[i];
        if (owed.word >= Words) {
            ++refused;
            continue;
        }
        if (owed.mask == 0) continue;
        const bool before = TestBit<Words>(image.owed, owed.word);
        if (GX_OWED_PICK(OweFirstWins, before, false)) continue;
        if (!TestBit<Words>(image.listed, owed.word)) {
            SetBit<Words>(image.listed, owed.word);
            image.order[image.count++] = static_cast<std::uint16_t>(owed.word);
        }
        const std::uint32_t earlier = before ? image.masks[owed.word] : 0;
        image.values[owed.word] = Merge(image.values[owed.word], owed.value, owed.mask);
        image.masks[owed.word] = GX_OWED_PICK(OweDropsEarlierMask, owed.mask, earlier | owed.mask);
        SetBit<Words>(image.owed, owed.word);
    }
    image.refused += refused;
    return refused;
}

template <std::uint32_t Words>
constexpr bool Touches(const OwedImage<Words>& image, std::uint32_t first, std::uint32_t count) {
    const WordSpan span = ClampSpan(Words, first, GX_OWED_PICK(TouchesOffByOne, count ? count - 1 : 0, count));
    for (std::uint32_t word = span.first; word < span.last; ++word) {
        if (TestBit<Words>(image.owed, word)) return true;
    }
    return false;
}

template <std::uint32_t Words>
constexpr void Forget(OwedImage<Words>& image, std::uint32_t first, std::uint32_t count) {
    const WordSpan span = ClampSpan(Words, first, GX_OWED_PICK(ForgetMissesLastWord, count ? count - 1 : 0, count));
    for (std::uint32_t word = span.first; word < span.last; ++word) {
        ClearBit<Words>(image.owed, word);
    }
}

template <std::uint32_t Words>
constexpr std::uint32_t Materialize(OwedImage<Words>& image, std::span<std::uint32_t> file, MarkSink mark) {
    std::uint32_t written = 0;
    for (std::uint32_t i = 0; i < image.count; ++i) {
        const std::uint32_t word = image.order[i];
        ClearBit<Words>(image.listed, word);
        if (!TestBit<Words>(image.owed, word)) continue;
        if (GX_OWED_PICK(MaterializeKeepsBit, false, true)) ClearBit<Words>(image.owed, word);
        if (word >= file.size()) {
            ++image.refused;
            continue;
        }
        const std::uint32_t mask = GX_OWED_PICK(MaterializeIgnoresMask, ~0u, image.masks[word]);
        const std::uint32_t value = Merge(file[word], image.values[word], mask);
        if (file[word] == value && GX_OWED_PICK(MaterializeWritesEqual, false, true)) continue;
        file[word] = value;
        ++written;
        if (mark.fn && GX_OWED_PICK(MaterializeSkipsMark, false, true)) mark.fn(mark.context, word, 1);
    }
    image.count = 0;
    return written;
}

template <std::uint32_t Words>
constexpr void Drop(OwedImage<Words>& image) {
    for (std::uint32_t i = 0; i < image.count; ++i) {
        ClearBit<Words>(image.listed, image.order[i]);
        ClearBit<Words>(image.owed, image.order[i]);
    }
    image.count = 0;
}

template <std::uint32_t Words>
struct WordRun {
    static_assert(Words > 0 && Words <= kMaxWords);
    WordBits<Words> in{};
    std::array<std::uint16_t, Words> order{};
    std::uint32_t count = 0;
};

template <std::uint32_t Words>
constexpr void Add(WordRun<Words>& run, std::uint32_t first, std::uint32_t count) {
    const WordSpan span = ClampSpan(Words, first, count);
    for (std::uint32_t word = span.first; word < span.last; ++word) {
        if (TestBit<Words>(run.in, word)) continue;
        SetBit<Words>(run.in, word);
        run.order[run.count++] = static_cast<std::uint16_t>(word);
    }
}

template <std::uint32_t Words>
constexpr void Empty(WordRun<Words>& run) {
    for (std::uint32_t i = 0; i < run.count; ++i) {
        ClearBit<Words>(run.in, run.order[i]);
    }
    run.count = 0;
}

enum class Reader : std::uint8_t {
    None,
    CoveredDraw,
    UncoveredDraw,
    PacketDraw,
    IndirectDraw,
    Dispatch,
    RegisterWait,
    StreamoutFlush,
    ComputeShWrite,
    ComputeDispatch,
    SubmissionEnd,
    ClearState,
    kCount
};

inline constexpr std::array<const char*, std::size_t(Reader::kCount)> kReaderNames = {
    "none",          "covered draw",    "uncovered draw",   "draw packet",      "indirect draw",  "dispatch",
    "register wait", "streamout flush", "compute SH write", "compute dispatch", "submission end", "clear state"};

struct PacketFacts {
    bool token = false;
    bool covered = false;
    bool packet_draw = false;
    bool indirect_draw = false;
    bool dispatch = false;
    bool wait = false;
    bool wait_on_register = false;
    bool streamout_flush = false;
    bool clear_state = false;
};

namespace Op {
inline constexpr std::uint32_t ClearState = 0x12, DispatchDirect = 0x15, DispatchIndirect = 0x16;
inline constexpr std::uint32_t DrawIndirect = 0x24, DrawIndexIndirect = 0x25, DrawIndex2 = 0x27;
inline constexpr std::uint32_t DrawIndirectMulti = 0x2c, DrawIndexAuto = 0x2d, DrawIndexOffset2 = 0x35;
inline constexpr std::uint32_t DrawIndexIndirectMulti = 0x38, WaitRegMem = 0x3c, EventWrite = 0x46;
inline constexpr std::uint32_t DrawIndexIndirectCountMulti = 0x9d;
inline constexpr std::uint32_t kWaitMemSpaceShift = 4, kEventTypeMask = 0x3f, kStreamoutFlushEvent = 31;
}

constexpr PacketFacts FactsOf(std::uint32_t opcode, std::span<const std::uint32_t> packet, bool token) {
    PacketFacts facts;
    const bool body = packet.size() > 1;
    const std::uint32_t first = body ? packet[1] : 0;
    facts.token = token;
    facts.packet_draw = opcode == Op::DrawIndex2 || opcode == Op::DrawIndexOffset2 || opcode == Op::DrawIndexAuto;
    facts.indirect_draw = opcode == Op::DrawIndirect || opcode == Op::DrawIndirectMulti || opcode == Op::DrawIndexIndirect ||
                          opcode == Op::DrawIndexIndirectMulti || opcode == Op::DrawIndexIndirectCountMulti;
    facts.dispatch = opcode == Op::DispatchDirect || opcode == Op::DispatchIndirect;
    facts.wait = opcode == Op::WaitRegMem;
    facts.wait_on_register = facts.wait && body && ((first >> Op::kWaitMemSpaceShift) & 1) == 0;
    facts.streamout_flush =
        opcode == Op::EventWrite && body && GX_OWED_PICK(StreamoutAnyEvent, true, (first & Op::kEventTypeMask) == Op::kStreamoutFlushEvent);
    facts.clear_state = opcode == Op::ClearState;
    return facts;
}

constexpr Reader ReaderOf(const PacketFacts& facts) {
    if (facts.clear_state) return Reader::ClearState;
    if (facts.token) return facts.covered && GX_OWED_PICK(CoveredTokenReads, false, true) ? Reader::CoveredDraw : Reader::UncoveredDraw;
    if (facts.packet_draw) return Reader::PacketDraw;
    if (facts.indirect_draw) return Reader::IndirectDraw;
    if (facts.dispatch) return Reader::Dispatch;
    if (facts.wait && GX_OWED_PICK(WaitOnMemoryReads, true, facts.wait_on_register)) return Reader::RegisterWait;
    if (facts.streamout_flush) return Reader::StreamoutFlush;
    return Reader::None;
}

struct OwedDifference {
    std::uint32_t word;
    std::uint32_t materialized;
    std::uint32_t decoded;
};

template <std::uint32_t Words>
struct OwedCompare {
    std::array<std::uint32_t, Words> image{};
    OwedImage<Words> owed;
    WordRun<Words> run;
    WordRun<Words> pre_token;
    WordRun<Words> suspects;
};

template <std::uint32_t Words>
void NoteSuspect(void* context, std::uint32_t word, std::uint32_t count) {
    Add(*static_cast<WordRun<Words>*>(context), word, count);
}

template <std::uint32_t Words>
void Sync(OwedCompare<Words>& compare, std::span<const std::uint32_t> file) {
    for (std::uint32_t word = 0; word < Words && word < file.size(); ++word) {
        compare.image[word] = file[word];
    }
    Drop(compare.owed);
    Empty(compare.run);
    Empty(compare.pre_token);
    Empty(compare.suspects);
}

template <std::uint32_t Words>
void Written(OwedCompare<Words>& compare, std::uint32_t first, std::uint32_t count) {
    Add(compare.run, first, count);
}

template <std::uint32_t Words>
void TokenStart(OwedCompare<Words>& compare) {
    for (std::uint32_t i = 0; i < compare.run.count; ++i) {
        Add(compare.pre_token, compare.run.order[i], 1);
    }
    Empty(compare.run);
}

template <std::uint32_t Words>
void Keep(OwedCompare<Words>& compare, const WordRun<Words>& kept, std::span<const std::uint32_t> file) {
    for (std::uint32_t i = 0; i < kept.count; ++i) {
        const std::uint32_t word = kept.order[i];
        if (GX_OWED_PICK(KeptNotSuperseding, false, true)) Forget(compare.owed, word, 1);
        if (word < file.size()) compare.image[word] = file[word];
    }
}

template <std::uint32_t Words>
void CoveredDraw(OwedCompare<Words>& compare, std::span<const OwedWord> record, std::span<const std::uint32_t> file) {
    WordRun<Words>& pre = compare.pre_token;
    if (GX_OWED_PICK(TokenKeepsPreRun, true, false)) {
        Keep(compare, pre, file);
    } else {
        for (std::uint32_t i = 0; i < pre.count; ++i) {
            Add(compare.suspects, pre.order[i], 1);
        }
    }
    Owe(compare.owed, record);
    Keep(compare, compare.run, file);
    Empty(pre);
    Empty(compare.run);
}

struct TransitionResult {
    std::uint32_t materialized = 0;
    std::uint32_t checked = 0;
    std::uint32_t differ = 0;
};

template <std::uint32_t Words>
TransitionResult Transition(OwedCompare<Words>& compare, std::span<const std::uint32_t> file, std::span<OwedDifference> out) {
    if (GX_OWED_PICK(TransitionDropsRun, false, true)) {
        Keep(compare, compare.pre_token, file);
        Keep(compare, compare.run, file);
    }
    Empty(compare.pre_token);
    Empty(compare.run);
    TransitionResult result;
    const MarkSink mark{&NoteSuspect<Words>, &compare.suspects};
    result.materialized = Materialize(compare.owed, std::span<std::uint32_t>(compare.image), mark);
    WordRun<Words>& suspects = compare.suspects;
    result.checked = suspects.count;
    for (std::uint32_t i = 0; i < suspects.count; ++i) {
        const std::uint32_t word = suspects.order[i];
        const std::uint32_t decoded = word < file.size() ? file[word] : compare.image[word];
        if (compare.image[word] == decoded) continue;
        if (result.differ < out.size()) out[result.differ] = {word, compare.image[word], decoded};
        ++result.differ;
        compare.image[word] = GX_OWED_PICK(ResyncSkipped, compare.image[word], decoded);
    }
    Empty(suspects);
    return result;
}

}
