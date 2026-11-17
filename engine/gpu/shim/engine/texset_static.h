// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

namespace BbEngine::TexsetStatic {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

#ifdef LANTERN_TEXSET_MUTANTS
enum class Mutant : std::uint8_t { None, ProxyIgnored, TargetIgnored, MovedIgnored, HeldIgnored, HeldAlwaysRequired, NullChecked,
                                   ProxyTransits, LastReasonWins, StaticChecksDropped, SuspectNever, kCount };
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#define LANTERN_TEXSET_PICK(mutant, wrong, right) \
    (::BbEngine::TexsetStatic::Mutated(::BbEngine::TexsetStatic::Mutant::mutant) ? (wrong) : (right))
#else
#define LANTERN_TEXSET_PICK(mutant, wrong, right) (right)
#endif

enum Reason : std::uint8_t { Static, Proxy, Target, Layout, NotHeld, kReasons };
inline constexpr const char* kReasonNames[kReasons] = {"static", "scene proxy", "target or storage", "layout change",
                                                       "not held by its GX texture"};
inline constexpr u32 kMaxImages = 16;

struct ImageFacts {
    bool null = false;
    bool proxy = false;
    bool target = false;
    bool moved = false;
    bool held = false;
};

inline Reason ClassifyImage(const ImageFacts& image, bool held_required) {
    if (image.null) return Static;
    if (LANTERN_TEXSET_PICK(ProxyIgnored, false, image.proxy)) return Proxy;
    if (LANTERN_TEXSET_PICK(TargetIgnored, false, image.target)) return Target;
    if (LANTERN_TEXSET_PICK(MovedIgnored, false, image.moved)) return Layout;
    const bool need_held = LANTERN_TEXSET_PICK(HeldAlwaysRequired, true, held_required);
    if (need_held && !LANTERN_TEXSET_PICK(HeldIgnored, true, image.held)) return NotHeld;
    return Static;
}

inline Reason Rank(Reason set, Reason image) {
    if (LANTERN_TEXSET_PICK(LastReasonWins, image != Static, false)) return image;
    if (image == Static) return set;
    return set == Static || image < set ? image : set;
}

struct SetCount {
    Reason reason = Static;
    u32 checks = 0;
    u32 transits = 0;
};

inline SetCount ClassifySet(std::span<const ImageFacts> images, bool held_required) {
    SetCount count;
    for (const ImageFacts& image : images) {
        if (!LANTERN_TEXSET_PICK(NullChecked, false, image.null)) {
            ++count.checks;
            if (LANTERN_TEXSET_PICK(ProxyTransits, true, !image.proxy)) ++count.transits;
        }
        count.reason = Rank(count.reason, ClassifyImage(image, held_required));
    }
    return count;
}

struct Tally {
    u64 hits = 0;
    u64 checks = 0;
    u64 transits = 0;
    u64 static_checks = 0;
    u64 static_transits = 0;
    u64 reasons[kReasons]{};
    u64 other_writers = 0;
    bool held_required = false;
};

inline void Add(Tally& tally, const SetCount& count) {
    ++tally.hits;
    tally.checks += count.checks;
    tally.transits += count.transits;
    ++tally.reasons[count.reason];
    if (count.reason == Static && LANTERN_TEXSET_PICK(StaticChecksDropped, false, true)) {
        tally.static_checks += count.checks;
        tally.static_transits += count.transits;
    }
}

inline bool Suspect(const Tally& tally) {
    return LANTERN_TEXSET_PICK(SuspectNever, false, tally.hits > 0 && tally.reasons[Static] == 0);
}

inline int Format(char* out, std::size_t room, const Tally& tally, u64 frames) {
    std::size_t used = 0;
    const auto put = [&](const char* format, auto... args) {
        if (used >= room) return;
        const int n = std::snprintf(out + used, room - used, format, args...);
        if (n > 0) used += static_cast<std::size_t>(n);
    };
    const double per = frames ? 1.0 / double(frames) : 0.0;
    put("  texture set hits static (%s): %.1f of %.1f a frame over %llu frames; static hits run %.1f image checks and %.1f "
        "transits a frame (all hits %.1f and %.1f); not static:",
        tally.held_required ? "held by GX textures" : "GX textures off, held not required", double(tally.reasons[Static]) * per,
        double(tally.hits) * per, static_cast<unsigned long long>(frames), double(tally.static_checks) * per,
        double(tally.static_transits) * per, double(tally.checks) * per, double(tally.transits) * per);
    for (u32 r = Proxy; r < kReasons; ++r) put(" %s %.1f;", kReasonNames[r], double(tally.reasons[r]) * per);
    put(" other writers %llu", static_cast<unsigned long long>(tally.other_writers));
    if (Suspect(tally)) put("%s", "; INSTRUMENT SUSPECT: hits but none static");
    return static_cast<int>(used < room ? used : room - 1);
}
}
