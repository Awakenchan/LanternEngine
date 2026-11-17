// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

#ifdef LANTERN_STAGEB_MUTANTS
namespace BbEngine::StageBCounts {
inline int g_mutant = 0;
}
#define LANTERN_STAGEB_PICK(n, wrong, right) (::BbEngine::StageBCounts::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_STAGEB_PICK(n, wrong, right) (right)
#endif

namespace BbEngine::StageBCounts {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr int kMutants = 11;
inline constexpr u32 kShareSlotBits = 14;
inline constexpr u32 kShareProbes = 8;
inline constexpr u32 kPublishEvery = 256;

enum class Seen : std::uint8_t { New, EarlierDraw, SameDraw, Untracked };

struct ShareSlot {
    u64 hash = 0;
    u32 size = 0, submission = 0, draw = 0;
};

enum ShareCounter : u32 {
    Uploads,
    UploadBytes,
    UploadCycles,
    HashCycles,
    Unhashed,
    Shared,
    SharedBytes,
    SharedCycles,
    SameDraw,
    Untracked,
    Draws,
    DrawsSharing,
    DrawsAllShared,
    Submissions,
    kShareCounters
};

struct ShareTable {
    std::unique_ptr<ShareSlot[]> slots{new ShareSlot[std::size_t(1) << kShareSlotBits]};
    u32 submission = 1, draw = 0;
    u64 scope = 0;
    u32 draw_uploads = 0, draw_shared = 0, unpublished = 0;
    u64 tally[kShareCounters]{};
};

inline u32 ShareSlotOf(u64 hash, u32 size) {
    return u32(((hash ^ (u64(size) << 32)) * 0x9E3779B97F4A7C15ull) >> (64 - kShareSlotBits));
}

inline Seen Look(ShareTable& t, u64 hash, u32 size) {
    constexpr u32 mask = (1u << kShareSlotBits) - 1;
    u32 at = ShareSlotOf(hash, size);
    for (u32 probe = 0; probe < LANTERN_STAGEB_PICK(1, 1u, kShareProbes); ++probe, at = (at + 1) & mask) {
        ShareSlot& s = t.slots[at];
        if (LANTERN_STAGEB_PICK(2, s.submission == 0, s.submission != t.submission)) {
            s = {hash, size, t.submission, t.draw};
            return Seen::New;
        }
        if (s.hash == hash && LANTERN_STAGEB_PICK(3, true, s.size == size)) {
            return LANTERN_STAGEB_PICK(4, s.draw <= t.draw, s.draw < t.draw) ? Seen::EarlierDraw : Seen::SameDraw;
        }
    }
    return LANTERN_STAGEB_PICK(5, Seen::New, Seen::Untracked);
}

inline void FinishDraw(ShareTable& t) {
    if (!t.draw_uploads) return;
    ++t.tally[Draws];
    t.tally[DrawsSharing] += t.draw_shared != 0;
    t.tally[DrawsAllShared] += t.draw_shared == t.draw_uploads;
    t.draw_uploads = 0;
    t.draw_shared = 0;
}

inline void NewDraw(ShareTable& t) {
    FinishDraw(t);
    ++t.draw;
    ++t.unpublished;
}

inline void NewSubmission(ShareTable& t) {
    FinishDraw(t);
    ++t.submission;
    t.draw = 0;
    ++t.tally[Submissions];
}

inline void NewDrawIn(ShareTable& t, u64 scope) {
    if (scope != t.scope) {
        NewSubmission(t);
        t.scope = scope;
    }
    NewDraw(t);
}

inline void NoteUnhashed(ShareTable& t, u64 size, u64 copy_cycles) {
    ++t.draw_uploads;
    ++t.tally[Uploads];
    ++t.tally[Unhashed];
    t.tally[UploadBytes] += size;
    t.tally[UploadCycles] += copy_cycles;
}

inline Seen NoteUpload(ShareTable& t, u64 hash, u64 size, u64 copy_cycles, u64 hash_cycles) {
    const Seen seen = Look(t, hash, u32(size));
    ++t.draw_uploads;
    ++t.tally[Uploads];
    t.tally[UploadBytes] += size;
    t.tally[UploadCycles] += copy_cycles;
    t.tally[HashCycles] += hash_cycles;
    if (seen == Seen::EarlierDraw) {
        ++t.draw_shared;
        ++t.tally[Shared];
        t.tally[SharedBytes] += size;
        t.tally[SharedCycles] += copy_cycles;
    } else if (seen == Seen::SameDraw) {
        ++t.tally[SameDraw];
    } else if (seen == Seen::Untracked) {
        ++t.tally[Untracked];
    }
    return seen;
}

enum Bind : std::uint8_t { BindVertex, BindIndex, BindDescriptors, BindPush, BindPipeline, kBinds };
inline constexpr const char* kBindNames[kBinds] = {"vertex", "index", "descriptors", "push constants", "pipeline"};

struct LastBind {
    u64 key = 0, scope = 0;
    bool valid = false;
};

inline bool Repeats(LastBind& last, u64 key, u64 scope, bool known) {
    const bool same = LANTERN_STAGEB_PICK(6, true, last.valid) && LANTERN_STAGEB_PICK(9, true, known) &&
                      LANTERN_STAGEB_PICK(7, true, last.scope == scope) && last.key == key;
    if (LANTERN_STAGEB_PICK(8, !last.valid, true)) last = {key, scope, known};
    return same;
}

enum BindCounter : u32 {
    BindDraws,
    DrawsAllRepeat,
    ScopeCuts,
    Binds,
    Repeated = Binds + kBinds,
    BindCycles = Repeated + kBinds,
    RepeatCycles = BindCycles + kBinds,
    kBindCounters = RepeatCycles + kBinds
};

inline constexpr u32 At(BindCounter base, Bind bind) {
    return u32(base) + u32(bind);
}

struct BindState {
    LastBind last[kBinds];
    u64 scope = 0, breaks = 0;
    u32 unpublished = 0;
    u64 tally[kBindCounters]{};
};

inline void Break(BindState& s) {
    ++s.breaks;
}

inline u64 ScopeOf(u64 submission, u64 segment, u64 breaks) {
    return ((submission * 0x9E3779B97F4A7C15ull) ^ (segment << 40)) + breaks * 0xC2B2AE3D27D4EB4Full;
}

struct BindSample {
    u64 key[kBinds]{};
    u64 cycles[kBinds]{};
    bool known[kBinds]{};
    bool bound[kBinds]{};
};

inline u32 NoteDraw(BindState& s, const BindSample& d, u64 scope) {
    ++s.tally[BindDraws];
    if (scope != s.scope) {
        ++s.tally[ScopeCuts];
        s.scope = scope;
    }
    u32 repeats = 0, bound = 0;
    for (u32 b = 0; b < kBinds; ++b) {
        if (!d.bound[b]) continue;
        ++bound;
        ++s.tally[Binds + b];
        s.tally[BindCycles + b] += d.cycles[b];
        if (Repeats(s.last[b], d.key[b], scope, d.known[b])) {
            ++repeats;
            ++s.tally[Repeated + b];
            s.tally[RepeatCycles + b] += d.cycles[b];
        }
    }
    s.tally[DrawsAllRepeat] += bound && repeats == bound;
    ++s.unpublished;
    return repeats;
}

struct Totals {
    std::atomic<u64> share_a[kShareCounters]{};
    std::atomic<u64> share_b[kShareCounters]{};
    std::atomic<u64> binds[kBindCounters]{};
};
inline Totals g_totals;

template <std::size_t N>
inline void PublishInto(std::atomic<u64> (&to)[N], u64 (&from)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (!from[i]) continue;
        to[i].fetch_add(from[i], std::memory_order_relaxed);
        from[i] = 0;
    }
}

inline void MaybePublish(ShareTable& t, std::atomic<u64> (&to)[kShareCounters], bool now = false) {
    if (!now && t.unpublished < kPublishEvery) return;
    t.unpublished = 0;
    PublishInto(to, t.tally);
}

inline void MaybePublish(BindState& s, bool now = false) {
    if (!now && s.unpublished < kPublishEvery) return;
    s.unpublished = 0;
    PublishInto(g_totals.binds, s.tally);
}

template <std::size_t N>
struct Window {
    u64 v[N]{};
};

template <std::size_t N>
inline Window<N> Take(const std::atomic<u64> (&totals)[N], Window<N>& last) {
    Window<N> d;
    for (std::size_t i = 0; i < N; ++i) {
        const u64 now = totals[i].load(std::memory_order_relaxed);
        d.v[i] = now - last.v[i];
        last.v[i] = now;
    }
    return d;
}

struct Report {
    u32 last_frame = 0;
    Window<kShareCounters> share_a, share_b;
    Window<kBindCounters> binds;
};

inline bool MayHashGuest(bool precise_readbacks, bool gpu_modified) {
    return LANTERN_STAGEB_PICK(10, !gpu_modified, LANTERN_STAGEB_PICK(11, !precise_readbacks, !precise_readbacks && !gpu_modified));
}

inline int FormatShare(char* out, std::size_t room, const char* where, const Window<kShareCounters>& w, double frames,
                       double tsc_hz) {
    const double f = frames > 0 ? frames : 1.0;
    const double us = tsc_hz > 0 ? 1e6 / tsc_hz : 0.0;
    const auto pct = [](u64 part, u64 all) { return all ? 100.0 * double(part) / double(all) : 0.0; };
    const int n = std::snprintf(
        out, room,
        "Constant sharing (%s, 5 s): %.0f uploads/frame, %.1f KiB/frame, %.1f us/frame copying; identical to an earlier "
        "draw's in the same submission %.0f/frame (%.1f%%), %.1f KiB/frame, %.1f us/frame of copies; draws uploading "
        "%.0f/frame, sharing one or more %.1f%%, all shared %.1f%%; hashing %.1f us/frame; same-draw repeats %.0f/frame, "
        "not hashed %.0f/frame, table full %.0f/frame; %.1f submissions/frame",
        where, double(w.v[Uploads]) / f, double(w.v[UploadBytes]) / (1024.0 * f), double(w.v[UploadCycles]) * us / f,
        double(w.v[Shared]) / f, pct(w.v[Shared], w.v[Uploads]), double(w.v[SharedBytes]) / (1024.0 * f),
        double(w.v[SharedCycles]) * us / f, double(w.v[Draws]) / f, pct(w.v[DrawsSharing], w.v[Draws]),
        pct(w.v[DrawsAllShared], w.v[Draws]), double(w.v[HashCycles]) * us / f, double(w.v[SameDraw]) / f,
        double(w.v[Unhashed]) / f, double(w.v[Untracked]) / f, double(w.v[Submissions]) / f);
    return n < 0 ? 0 : (std::size_t(n) < room ? n : int(room) - 1);
}

inline int FormatBinds(char* out, std::size_t room, const Window<kBindCounters>& w, double frames, double tsc_hz) {
    const double f = frames > 0 ? frames : 1.0;
    const double us = tsc_hz > 0 ? 1e6 / tsc_hz : 0.0;
    const auto pct = [](u64 part, u64 all) { return all ? 100.0 * double(part) / double(all) : 0.0; };
    std::size_t used = 0;
    const auto put = [&](const char* format, auto... args) {
        if (used >= room) return;
        const int n = std::snprintf(out + used, room - used, format, args...);
        if (n > 0) used += std::size_t(n);
    };
    put("Repeated binds (stage B, 5 s; repeat = same as the previous DrawRecord in one command buffer segment, "
        "late pipeline binds of compiling pipelines count as not repeating): %.0f draws/frame, all binds repeat "
        "%.1f%%, scope cuts %.0f/frame;",
        double(w.v[BindDraws]) / f, pct(w.v[DrawsAllRepeat], w.v[BindDraws]), double(w.v[ScopeCuts]) / f);
    for (u32 b = 0; b < kBinds; ++b) {
        put(" %s %.0f/frame repeat %.1f%% (%.1f of %.1f us/frame);", kBindNames[b], double(w.v[Binds + b]) / f,
            pct(w.v[Repeated + b], w.v[Binds + b]), double(w.v[RepeatCycles + b]) * us / f,
            double(w.v[BindCycles + b]) * us / f);
    }
    return int(used < room ? used : room - 1);
}
}
