// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace BbEngine::GxWholeCensus {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum Reason : u8 {
    Covered, NotToken, RecordsOff, RectList, HullOrGeometry, Predicated, NoRecord, RecordsNotUsed, NotApplied, NoPayload,
    NotChecked, Bindings, RenderState, kReasons
};
inline constexpr const char* kReasonNames[kReasons] = {
    "covered", "not a GX token", "records off", "rect list", "hull or geometry stage", "predicated", "no record",
    "records not used at =1", "record not applied", "record lost", "not checked (only =2 checks every stage)",
    "bindings", "render state"};
static_assert(sizeof(kReasonNames) / sizeof(kReasonNames[0]) == kReasons);

inline constexpr u8 kBindingsCovered = 0;
inline constexpr u8 kBindingsLost = 1;
inline constexpr u8 kBindingsNotApplied = 0xff;

struct Verdict {
    u8 bindings = kBindingsNotApplied;
    bool render = false;
    bool not_checked = false;
    bool pipeline_from_record = false;
};

struct DrawFacts {
    bool token = false;
    bool records_on = false;
    bool records_used = false;
    bool rect_list = false;
    bool hull_or_geometry = false;
    bool predicated = false;
    bool record = false;
    Verdict verdict;
};

enum class Mutant : u8 {
    None, PredicatedCovered, ShRegNotRegister, BetweenKept, SuspectNever, LostAsBindings, UncheckedCovered,
    DispatchCharged, UnusedAsNotApplied, kCount
};
#ifdef LANTERN_GX_CENSUS_MUTANTS
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#else
inline constexpr bool Mutated(Mutant) {
    return false;
}
#endif

inline Reason CoverageOf(const DrawFacts& draw) {
    if (!draw.token) return NotToken;
    if (!draw.records_on) return RecordsOff;
    if (draw.rect_list) return RectList;
    if (draw.hull_or_geometry) return HullOrGeometry;
    if (draw.predicated && !Mutated(Mutant::PredicatedCovered)) return Predicated;
    if (!draw.record) return NoRecord;
    if (!draw.records_used && !Mutated(Mutant::UnusedAsNotApplied)) return RecordsNotUsed;
    const Verdict& v = draw.verdict;
    if (v.bindings == kBindingsNotApplied) return NotApplied;
    if (v.bindings == kBindingsLost && !Mutated(Mutant::LostAsBindings)) return NoPayload;
    if (v.not_checked && !Mutated(Mutant::UncheckedCovered)) return NotChecked;
    if (v.bindings != kBindingsCovered) return Bindings;
    if (!v.render) return RenderState;
    return Covered;
}

inline constexpr u32 kOpNop = 0x10, kOpClearState = 0x12, kOpIndexBufferSize = 0x13, kOpDispatchDirect = 0x15;
inline constexpr u32 kOpDispatchIndirect = 0x16, kOpDrawIndirect = 0x24, kOpDrawIndexIndirect = 0x25, kOpIndexBase = 0x26;
inline constexpr u32 kOpDrawIndex2 = 0x27, kOpIndexType = 0x2a, kOpDrawIndirectMulti = 0x2c, kOpDrawIndexAuto = 0x2d;
inline constexpr u32 kOpNumInstances = 0x2f, kOpDrawIndexOffset2 = 0x35, kOpDrawIndexIndirectMulti = 0x38;
inline constexpr u32 kOpSetConfigReg = 0x68, kOpSetContextReg = 0x69, kOpSetShReg = 0x76, kOpSetUconfigReg = 0x79;
inline constexpr u32 kOpDrawIndexIndirectCountMulti = 0x9d;

inline bool IsDrawOpcode(u32 opcode) {
    switch (opcode) {
    case kOpDrawIndex2:
    case kOpDrawIndexOffset2:
    case kOpDrawIndexAuto:
    case kOpDrawIndirect:
    case kOpDrawIndirectMulti:
    case kOpDrawIndexIndirect:
    case kOpDrawIndexIndirectMulti:
    case kOpDrawIndexIndirectCountMulti:
        return true;
    default:
        return false;
    }
}

inline bool IsDispatchOpcode(u32 opcode) {
    return opcode == kOpDispatchDirect || opcode == kOpDispatchIndirect;
}

enum PacketKind : u8 { RegisterPacket, OtherPacket, kPacketKinds };

inline PacketKind KindOf(u32 opcode, bool bind_tag) {
    switch (opcode) {
    case kOpSetShReg:
        return Mutated(Mutant::ShRegNotRegister) ? OtherPacket : RegisterPacket;
    case kOpSetContextReg:
    case kOpSetUconfigReg:
    case kOpSetConfigReg:
    case kOpClearState:
    case kOpIndexType:
    case kOpIndexBase:
    case kOpIndexBufferSize:
    case kOpNumInstances:
        return RegisterPacket;
    case kOpNop:
        return bind_tag ? RegisterPacket : OtherPacket;
    default:
        return OtherPacket;
    }
}

struct Bucket {
    u64 packets[kPacketKinds]{};
    u64 dwords[kPacketKinds]{};
    u64 ticks = 0;
};

inline void Add(Bucket& to, const Bucket& from) {
    for (u32 k = 0; k < kPacketKinds; ++k) {
        to.packets[k] += from.packets[k];
        to.dwords[k] += from.dwords[k];
    }
    to.ticks += from.ticks;
}

struct Window {
    u64 frames = 0, submissions = 0, ce_buffers = 0, ce_dwords = 0;
    u64 taken = 0, stamped = 0, covered_pipeline_from_record = 0;
    u64 draws[kReasons]{};
    u64 draw_ticks[kReasons]{};
    Bucket walked[kReasons];
    Bucket outside;
    Bucket between;
    Reason last = NotToken;
};

inline void NotePacket(Window& window, PacketKind kind, u32 dwords) {
    ++window.between.packets[kind];
    window.between.dwords[kind] += dwords;
}

inline void NoteDraw(Window& window, Reason reason) {
    ++window.draws[reason];
    Add(window.walked[reason], window.between);
    if (!Mutated(Mutant::BetweenKept)) window.between = {};
    window.last = reason;
}

inline void NoteOutside(Window& window) {
    if (Mutated(Mutant::DispatchCharged)) return;
    Add(window.outside, window.between);
    window.between = {};
}

inline void NoteTicks(Window& window, u64 ticks, bool draw_packet) {
    if (draw_packet) window.draw_ticks[window.last] += ticks;
    else window.between.ticks += ticks;
}

inline void NotePm4(Window& window, u32 opcode, u32 dwords, bool bind_tag, bool token_draw) {
    if (token_draw) return;
    if (IsDrawOpcode(opcode)) {
        NoteDraw(window, NotToken);
        return;
    }
    NotePacket(window, KindOf(opcode, bind_tag), dwords);
    if (IsDispatchOpcode(opcode)) NoteOutside(window);
}

inline Reason NoteToken(Window& window, const DrawFacts& facts) {
    const Reason reason = CoverageOf(facts);
    window.taken += facts.record;
    window.covered_pipeline_from_record += reason == Covered && facts.verdict.pipeline_from_record;
    NoteDraw(window, reason);
    return reason;
}

inline u64 Total(const Window& window) {
    u64 total = 0;
    for (u32 r = 0; r < kReasons; ++r) total += window.draws[r];
    return total;
}

inline bool InstrumentSuspect(const Window& window) {
    if (Mutated(Mutant::SuspectNever)) return false;
    return Total(window) > 0 && window.draws[Covered] == 0;
}

inline void Restart(Window& window) {
    const Bucket between = window.between;
    const Reason last = window.last;
    window = {};
    window.between = between;
    window.last = last;
}

inline std::size_t Format(const Window& window, double ticks_per_us, char* out, std::size_t size) {
    if (!size) return 0;
    out[0] = 0;
    std::size_t used = 0;
    const auto put = [&](int written) {
        if (written > 0) used += static_cast<std::size_t>(written);
        if (used >= size) used = size - 1;
    };
    const auto per = [](u64 value, u64 count) { return count ? double(value) / double(count) : 0.0; };
    const auto us = [&](u64 ticks, u64 count) {
        return ticks_per_us > 0.0 && count ? double(ticks) / ticks_per_us / double(count) : 0.0;
    };
    const u64 total = Total(window);
    const double frames = window.frames ? double(window.frames) : 1.0;
    const u64 covered = window.draws[Covered];
    const Bucket& c = window.walked[Covered];
    put(std::snprintf(out + used, size - used,
                      "Whole GX census: %llu draws, %llu frames, %.0f draws/frame, %.1f%% covered (%.0f/frame, "
                      "%llu with the pipeline restored from the record, =3 only); records %llu stamped, %llu taken; "
                      "stage A per covered draw %.1f register dwords in %.1f packets, %.1f other dwords in %.1f packets",
                      static_cast<unsigned long long>(total), static_cast<unsigned long long>(window.frames),
                      double(total) / frames, total ? 100.0 * double(covered) / double(total) : 0.0,
                      double(covered) / frames, static_cast<unsigned long long>(window.covered_pipeline_from_record),
                      static_cast<unsigned long long>(window.stamped), static_cast<unsigned long long>(window.taken),
                      per(c.dwords[RegisterPacket], covered), per(c.packets[RegisterPacket], covered),
                      per(c.dwords[OtherPacket], covered), per(c.packets[OtherPacket], covered)));
    if (ticks_per_us > 0.0) {
        put(std::snprintf(out + used, size - used, ", walk %.3f us + draw packet %.3f us (sampled)", us(c.ticks, covered),
                          us(window.draw_ticks[Covered], covered)));
    }
    for (u32 r = 1; r < kReasons; ++r) {
        if (!window.draws[r]) continue;
        put(std::snprintf(out + used, size - used, "; %s %llu (%.1f%%, %.1f register dwords/draw)", kReasonNames[r],
                          static_cast<unsigned long long>(window.draws[r]), 100.0 * double(window.draws[r]) / double(total),
                          per(window.walked[r].dwords[RegisterPacket], window.draws[r])));
    }
    const Bucket& o = window.outside;
    put(std::snprintf(out + used, size - used,
                      "; outside draws %.0f register and %.0f other dwords/frame; constant engine %.0f dwords/frame in %llu buffers",
                      double(o.dwords[RegisterPacket]) / frames, double(o.dwords[OtherPacket]) / frames,
                      double(window.ce_dwords) / frames, static_cast<unsigned long long>(window.ce_buffers)));
    if (InstrumentSuspect(window)) {
        put(std::snprintf(out + used, size - used, "\nINSTRUMENT SUSPECT: Whole GX census counted %llu draws and none covered",
                          static_cast<unsigned long long>(total)));
    }
    return used;
}
}
