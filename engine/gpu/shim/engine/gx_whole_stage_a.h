// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <type_traits>

namespace BbEngine::GxWhole::StageA {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum class Mode : u8 { Off, Use, Compare };

inline constexpr u32 kSampleEvery = 64;
inline constexpr u32 kMaxGroups = 16;
inline constexpr u32 kMaxOverlay = 256;

enum class Left : u8 { Covered, NoRecord, NoPayload, NotCaptured, NoVertexShader, kCount };
inline constexpr std::array<const char*, static_cast<std::size_t>(Left::kCount)> kLeftNames = {
    "covered", "no record", "record lost", "whole part not captured", "no vertex shader"};

enum class Route : u8 { Registers, Record };

enum class Outcome : u8 { Same, KeyDiffers, PipelineDiffers, RecordKeyMissing, RegisterKeyMissing, kCount };
inline constexpr std::array<const char*, static_cast<std::size_t>(Outcome::kCount)> kOutcomeNames = {
    "same", "key", "pipeline", "record key missing", "register key missing"};

struct Facts {
    bool record;
    bool payload;
    bool captured;
    bool vertex_shader;
    bool capture_on;
};

struct ByteRange {
    u32 offset;
    u32 size;
};

#ifdef LANTERN_GX_STAGE_A_MUTANTS
enum class Mutant : u8 {
    None, CoverageIgnoresVertexShader, CountOnlyNeedsCapture, RouteAtCompare, RouteWhenDisabled, SampleEveryDraw,
    OverlayIgnoresMask, OverlaySkipsLast, RestoreForward, GroupSkipsLastByte, JudgeIgnoresPipeline, MissingRecordIsSame,
    MissingRegisterIsDifference, SuspectMissesUnchecked, CompareAfterDisable, FeedWithWhole, MarksAfterDisable,
    DisabledDropsPrepared, kCount
};
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

inline Left CoverageOf(const Facts& facts) {
    if (!facts.record) return Left::NoRecord;
    bool capture_on = facts.capture_on;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    if (Mutated(Mutant::CountOnlyNeedsCapture)) capture_on = true;
#endif
    if (!capture_on) return Left::Covered;
    if (!facts.payload) return Left::NoPayload;
    if (!facts.captured) return Left::NotCaptured;
    bool vertex_shader = facts.vertex_shader;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    if (Mutated(Mutant::CoverageIgnoresVertexShader)) vertex_shader = true;
#endif
    if (!vertex_shader) return Left::NoVertexShader;
    return Left::Covered;
}

inline Route RouteOf(Mode mode, Left left, bool disabled) {
    if (left != Left::Covered) return Route::Registers;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    if (Mutated(Mutant::RouteAtCompare) && mode == Mode::Compare) return Route::Record;
    if (Mutated(Mutant::RouteWhenDisabled)) disabled = false;
#endif
    if (mode != Mode::Use || disabled) return Route::Registers;
    return Route::Record;
}

inline bool ShouldCompare(Mode mode, Left left, u64 covered_index, bool disabled) {
    if (left != Left::Covered || mode == Mode::Off) return false;
    if (mode == Mode::Compare) return true;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    if (Mutated(Mutant::CompareAfterDisable)) disabled = false;
#endif
    if (disabled) return false;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    if (Mutated(Mutant::SampleEveryDraw)) return true;
#endif
    return covered_index % kSampleEvery == 0;
}

template <typename Value>
u32 Overlay(std::span<u32> registers, std::span<const Value> values, std::span<u32> saved) {
    const std::size_t count = values.size() < saved.size() ? values.size() : saved.size();
    for (std::size_t i = 0; i < count; ++i) {
        const Value& value = values[i];
        bool inside = value.reg < registers.size();
#ifdef LANTERN_GX_STAGE_A_MUTANTS
        if (Mutated(Mutant::OverlaySkipsLast)) inside = value.reg + 1 < registers.size();
#endif
        if (!inside) continue;
        u32 mask = value.mask;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
        if (Mutated(Mutant::OverlayIgnoresMask)) mask = ~0u;
#endif
        const u32 old = registers[value.reg];
        saved[i] = old;
        registers[value.reg] = (old & ~mask) | (value.value & mask);
    }
    return static_cast<u32>(count);
}

template <typename Value>
void Restore(std::span<u32> registers, std::span<const Value> values, std::span<const u32> saved, u32 count) {
    if (count > values.size()) count = static_cast<u32>(values.size());
    if (count > saved.size()) count = static_cast<u32>(saved.size());
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    if (Mutated(Mutant::RestoreForward)) {
        for (u32 i = 0; i < count; ++i) {
            if (values[i].reg < registers.size()) registers[values[i].reg] = saved[i];
        }
        return;
    }
#endif
    for (u32 i = count; i-- > 0;) {
        if (values[i].reg < registers.size()) registers[values[i].reg] = saved[i];
    }
}

inline u32 DifferingGroups(std::span<const u8> ours, std::span<const u8> theirs, std::span<const ByteRange> groups) {
    u32 differ = 0;
    const std::size_t count = groups.size() < kMaxGroups ? groups.size() : kMaxGroups;
    for (std::size_t g = 0; g < count; ++g) {
        const ByteRange range = groups[g];
        u32 size = range.size;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
        if (Mutated(Mutant::GroupSkipsLastByte) && size) --size;
#endif
        if (range.offset + size > ours.size() || range.offset + size > theirs.size()) {
            differ |= 1u << g;
            continue;
        }
        if (std::memcmp(ours.data() + range.offset, theirs.data() + range.offset, size) != 0) differ |= 1u << g;
    }
    return differ;
}

inline constexpr bool Partitions(std::span<const ByteRange> groups, u32 size) {
    u32 at = 0;
    for (const ByteRange& range : groups) {
        if (range.offset != at || range.size == 0) return false;
        at += range.size;
    }
    return at == size && groups.size() <= kMaxGroups;
}

inline Outcome Judge(bool record_key, bool register_key, u32 groups, bool same_pipeline) {
    if (!register_key) {
#ifdef LANTERN_GX_STAGE_A_MUTANTS
        if (Mutated(Mutant::MissingRegisterIsDifference)) return Outcome::RecordKeyMissing;
#endif
        return Outcome::RegisterKeyMissing;
    }
    if (!record_key) {
#ifdef LANTERN_GX_STAGE_A_MUTANTS
        if (Mutated(Mutant::MissingRecordIsSame)) return Outcome::Same;
#endif
        return Outcome::RecordKeyMissing;
    }
    if (groups) return Outcome::KeyDiffers;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    if (Mutated(Mutant::JudgeIgnoresPipeline)) return Outcome::Same;
#endif
    return same_pipeline ? Outcome::Same : Outcome::PipelineDiffers;
}

inline bool IsDifference(Outcome outcome) {
    return outcome == Outcome::KeyDiffers || outcome == Outcome::PipelineDiffers || outcome == Outcome::RecordKeyMissing;
}

inline bool IsChecked(Outcome outcome) {
    return outcome != Outcome::RegisterKeyMissing;
}

struct Counts {
    u64 total = 0, covered = 0, routed = 0, prepared_dropped = 0, marked = 0;
    u64 checked = 0, skipped = 0, differ = 0, unchecked = 0, other_writers = 0, disabled = 0, after_disable = 0;
    u64 left[static_cast<std::size_t>(Left::kCount)]{};
    u64 outcomes[static_cast<std::size_t>(Outcome::kCount)]{};
    u64 groups[kMaxGroups]{};
};
static_assert(std::is_trivially_copyable_v<Counts> && sizeof(Counts) % sizeof(u64) == 0);
inline constexpr std::size_t kCountWords = sizeof(Counts) / sizeof(u64);

inline void Count(Counts& counts, Left left) {
    ++counts.total;
    ++counts.left[static_cast<std::size_t>(left)];
    if (left == Left::Covered) ++counts.covered;
}

inline void CountOutcome(Counts& counts, Outcome outcome, u32 groups) {
    ++counts.outcomes[static_cast<std::size_t>(outcome)];
    if (!IsChecked(outcome)) {
        ++counts.unchecked;
        return;
    }
    ++counts.checked;
    if (!IsDifference(outcome)) return;
    ++counts.differ;
    for (u32 g = 0; g < kMaxGroups; ++g) counts.groups[g] += (groups >> g) & 1;
}

inline bool InstrumentSuspect(Mode mode, const Counts& counts) {
    if (counts.total > 0 && counts.covered == 0) return true;
    if (mode == Mode::Off) return false;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    if (Mutated(Mutant::SuspectMissesUnchecked)) return counts.differ > 0 && counts.differ == counts.checked;
#endif
    if (counts.covered > 0 && counts.checked == 0) return true;
    return counts.differ > 0 && counts.differ == counts.checked;
}

inline int ModeNumber(Mode mode) {
    return mode == Mode::Compare ? 2 : mode == Mode::Use ? 1 : 0;
}

class Line {
public:
    Line(char* out, std::size_t size) : out_(out), size_(size) {
        if (size_) out_[0] = '\0';
    }
    __attribute__((format(printf, 2, 3))) void Add(const char* format, ...) {
        if (used_ + 1 >= size_) return;
        std::va_list args;
        va_start(args, format);
        const int wrote = std::vsnprintf(out_ + used_, size_ - used_, format, args);
        va_end(args);
        if (wrote <= 0) return;
        const std::size_t room = size_ - used_ - 1;
        used_ += static_cast<std::size_t>(wrote) < room ? static_cast<std::size_t>(wrote) : room;
    }
    std::size_t Used() const {
        return used_;
    }

private:
    char* out_;
    std::size_t size_;
    std::size_t used_ = 0;
};

inline std::size_t FormatLine(char* out, std::size_t size, Mode mode, const Counts& c, std::span<const char* const> group_names) {
    using ull = unsigned long long;
    Line line(out, size);
    line.Add("GX whole stage A (LANTERN_GX_WHOLE=%d): %llu draws, %llu covered", ModeNumber(mode), ull(c.total), ull(c.covered));
    bool first = true;
    for (std::size_t l = 1; l < kLeftNames.size(); ++l) {
        if (!c.left[l]) continue;
        line.Add("%s%s %llu", first ? " (left out: " : ", ", kLeftNames[l], ull(c.left[l]));
        first = false;
    }
    if (!first) line.Add(")");
    if (mode == Mode::Off) {
        line.Add("; count only");
        return line.Used();
    }
    line.Add("; %llu by the record, %llu prepared draws not used, %llu not prepared; %llu checked, %llu skipped by the sample, %llu not checked (no register key), %llu differ",
             ull(c.routed), ull(c.prepared_dropped), ull(c.marked), ull(c.checked), ull(c.skipped), ull(c.unchecked), ull(c.differ));
    first = true;
    for (std::size_t o = 1; o < kOutcomeNames.size(); ++o) {
        if (!c.outcomes[o] || !IsDifference(static_cast<Outcome>(o))) continue;
        line.Add("%s%s %llu", first ? " (" : ", ", kOutcomeNames[o], ull(c.outcomes[o]));
        first = false;
    }
    if (!first) line.Add(")");
    first = true;
    for (std::size_t g = 0; g < group_names.size() && g < kMaxGroups; ++g) {
        if (!c.groups[g]) continue;
        line.Add("%s%s %llu", first ? "; key differs in: " : ", ", group_names[g], ull(c.groups[g]));
        first = false;
    }
    if (c.disabled) {
        line.Add("; the record route turned itself off after its first difference, %llu covered draws on the register path since, not compared",
                 ull(c.after_disable));
    }
    if (c.other_writers) line.Add("; %llu calls from other writers", ull(c.other_writers));
    return line.Used();
}

inline bool RecordsFeed(const char* records, const char* native, bool whole_on) {
    if (!records || (records[0] != '1' && records[0] != '2' && records[0] != '3')) return false;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    if (Mutated(Mutant::FeedWithWhole) && whole_on && records[0] == '1') return true;
#else
    (void)whole_on;
#endif
    if (records[0] == '3') return true;
    return records[0] == '1' && native && native[0] == '1';
}

struct Switches {
    bool on = false;
    Mode mode = Mode::Off;
    bool disabled = false;
    bool MarksPrepared() const {
        bool off = disabled;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
        if (Mutated(Mutant::MarksAfterDisable)) off = false;
#endif
        return on && mode == Mode::Use && !off;
    }
};

struct Chooser {
    Counts counts;
    u64 covered_seen = 0;
};

template <typename Port, typename Prepared>
auto Choose(Port& port, Chooser& chooser, Mode mode, Prepared prepared, u64 record, bool indexed) {
    Counts& counts = chooser.counts;
    const auto* state = record && mode != Mode::Off ? port.Taken(record) : nullptr;
    const Left left = CoverageOf(port.FactsOf(record != 0, state, mode != Mode::Off));
    Count(counts, left);
    if (mode == Mode::Off) return port.Registers(prepared, record);
    const bool disabled = port.Disabled();
    const bool compare = ShouldCompare(mode, left, chooser.covered_seen, disabled);
    const Route route = RouteOf(mode, left, disabled);
    if (left == Left::Covered) {
        ++chooser.covered_seen;
        if (mode == Mode::Use && disabled) ++counts.after_disable;
        else counts.skipped += !compare;
    }
    std::array<typename Port::Value, kMaxOverlay> values;
    const u32 count = state && (compare || route == Route::Record) ? port.Values(*state, indexed, std::span(values)) : 0;
    const std::span<const typename Port::Value> used(values.data(), count);
    if (route == Route::Record) {
        ++counts.routed;
        counts.prepared_dropped += prepared != nullptr;
    }
    Prepared handed = route == Route::Record ? nullptr : prepared;
#ifdef LANTERN_GX_STAGE_A_MUTANTS
    if (Mutated(Mutant::DisabledDropsPrepared) && disabled && left == Left::Covered) handed = nullptr;
#endif
    const auto pipeline = route == Route::Record ? port.ByRecord(used, record) : port.Registers(handed, record);
    if (compare && pipeline) port.Compare(used, record, pipeline, route);
    return pipeline;
}
}
