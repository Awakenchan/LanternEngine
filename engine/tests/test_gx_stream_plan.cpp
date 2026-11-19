// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/shim/engine/gx_stream_plan.h"

namespace {
namespace P = BbEngine::StreamPlan;
using u64 = std::uint64_t;
using u32 = std::uint32_t;

constexpr int kSealedBuffer = 1;
constexpr int kCacheBuffer = 2;
constexpr u64 kSealedBase = 0x100000;

struct Stream {
    u64 base = 0, size = 0;
    bool sealed = false;
};

struct Bind {
    int buffer = 0;
    u64 offset = 0;
    bool operator==(const Bind&) const = default;
};

struct Range {
    u64 base = 0, end = 0;
};

struct Ready {
    std::vector<Range> ranges;
    std::vector<u32> range_index;
};

struct Result {
    std::vector<Bind> binds;
    std::vector<Range> ranges;
    bool split = false;
};

Ready ReadyFor(const std::vector<Stream>& streams) {
    Ready r;
    for (const Stream& s : streams) {
        if (!s.size) continue;
        r.range_index.push_back(u32(r.ranges.size()));
        r.ranges.push_back({s.base, s.base + s.size});
    }
    return r;
}

Result Resolve(const std::vector<Stream>& streams, P::Mode mode, bool carried, const Ready* ready) {
    Result out;
    u32 sealed_streams = 0, memory_streams = 0;
    for (const Stream& s : streams) {
        if (!s.size) continue;
        ++memory_streams;
        sealed_streams += s.sealed ? 1u : 0u;
    }
    out.split = P::Split(mode, sealed_streams, memory_streams, carried);
    const bool use_ready = P::UseReady(ready != nullptr, out.split);
    if (use_ready) {
        out.ranges = ready->ranges;
    } else {
        std::vector<Range> ranges;
        for (const Stream& s : streams) {
            if (s.size && !P::BoundSealed(out.split, s.sealed)) ranges.push_back({s.base, s.base + s.size});
        }
        std::ranges::sort(ranges, [](const Range& a, const Range& b) { return a.base < b.base; });
        for (const Range& r : ranges) {
            if (out.ranges.empty() || out.ranges.back().end < r.base) out.ranges.push_back(r);
            else out.ranges.back().end = std::max(out.ranges.back().end, r.end);
        }
    }
    u32 with_memory = 0;
    for (const Stream& s : streams) {
        if (s.size && P::BoundSealed(out.split, s.sealed)) {
            out.binds.push_back(s.sealed ? Bind{kSealedBuffer, s.base - kSealedBase} : Bind{0, 0});
        } else if (s.size) {
            const Range* r = nullptr;
            if (use_ready) {
                const u32 k = ready->range_index[with_memory++];
                r = k < out.ranges.size() ? &out.ranges[k] : nullptr;
            } else {
                for (const Range& c : out.ranges) {
                    if (s.base >= c.base && s.base < c.end) { r = &c; break; }
                }
            }
            out.binds.push_back(r && s.base >= r->base && s.base < r->end ? Bind{kCacheBuffer, s.base} : Bind{-1, 0});
        } else {
            out.binds.push_back({0, 0});
        }
    }
    return out;
}

int g_failures = 0;
bool g_quiet = false;

void Expect(bool ok, const std::string& what) {
    if (ok) return;
    ++g_failures;
    if (!g_quiet) std::printf("FAILED: %s\n", what.c_str());
}

Bind Want(const Stream& s, bool split) {
    if (!s.size) return {0, 0};
    if (split && s.sealed) return {kSealedBuffer, s.base - kSealedBase};
    return {kCacheBuffer, s.base};
}

void CheckDraw(const char* name, const std::vector<Stream>& streams, P::Mode mode, bool carried, bool with_ready,
               bool want_split) {
    const Ready ready = ReadyFor(streams);
    const Result r = Resolve(streams, mode, carried, with_ready ? &ready : nullptr);
    const std::string what = std::string(name) + (with_ready ? " (prepared ranges)" : "");
    Expect(r.split == want_split, what + ": split " + (r.split ? "on" : "off") + ", wanted " + (want_split ? "on" : "off"));
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const Bind want = Want(streams[i], want_split);
        Expect(r.binds[i] == want, what + ": stream " + std::to_string(i) + " bound to buffer " + std::to_string(r.binds[i].buffer) +
                                       " offset " + std::to_string(r.binds[i].offset) + ", wanted buffer " + std::to_string(want.buffer) +
                                       " offset " + std::to_string(want.offset));
    }
    if (!want_split) return;
    for (const Stream& s : streams) {
        if (!s.size || !s.sealed) continue;
        for (const Range& c : r.ranges) {
            Expect(!(s.base >= c.base && s.base < c.end) || [&] {
                for (const Stream& o : streams)
                    if (o.size && !o.sealed && o.base < s.base + s.size && s.base < o.base + o.size) return true;
                return false;
            }(), what + ": a sealed stream's range was built for the cache path");
        }
    }
}

int RunAll() {
    g_failures = 0;
    const std::vector<Stream> mixed = {{kSealedBase + 0x40, 0x100, true}, {0x900000, 0x80, false}, {0, 0, false},
                                       {kSealedBase + 0x2000, 0x200, true}, {0x900400, 0x40, false}};
    const std::vector<Stream> all_sealed = {{kSealedBase, 0x100, true}, {kSealedBase + 0x1000, 0x80, true}};
    const std::vector<Stream> none_sealed = {{0x900000, 0x100, false}, {0x900100, 0x80, false}};
    for (const bool with_ready : {false, true}) {
        CheckDraw("mixed draw, on", mixed, P::Mode::On, false, with_ready, true);
        CheckDraw("mixed draw, compare", mixed, P::Mode::Compare, false, with_ready, false);
        CheckDraw("mixed draw, off", mixed, P::Mode::Off, false, with_ready, false);
        CheckDraw("mixed draw, carried by GX whole", mixed, P::Mode::On, true, with_ready, false);
        CheckDraw("all sealed, on", all_sealed, P::Mode::On, false, with_ready, true);
        CheckDraw("none sealed, on", none_sealed, P::Mode::On, false, with_ready, false);
    }
    Expect(P::Hit(true, true, true), "a clean hit with a valid seal is refused");
    Expect(!P::Hit(true, true, false), "a hit on a range the CPU wrote is taken");
    Expect(!P::Hit(true, false, true), "a hit on a retired seal is taken");
    Expect(!P::Hit(false, true, true), "a slot holding another range is taken");
    return g_failures;
}
}

int main(int argc, char** argv) {
#ifdef LANTERN_STREAM_PLAN_MUTANTS
    if (argc > 1 && !std::strcmp(argv[1], "--mutants")) {
        g_quiet = true;
        if (RunAll()) {
            std::printf("gx-stream-plan mutants: the unmutated run failed\n");
            return 1;
        }
        int survivors = 0;
        for (int m = 1; m < int(P::Mutant::kCount); ++m) {
            P::g_mutant = m;
            const int found = RunAll();
            std::printf("gx-stream-plan mutants: mutant %d %s (%d checks)\n", m, found ? "caught" : "survived", found);
            survivors += found ? 0 : 1;
        }
        P::g_mutant = 0;
        if (survivors) return 1;
        std::printf("gx-stream-plan mutants: every mutant caught (%d)\n", int(P::Mutant::kCount) - 1);
        return 0;
    }
    std::printf("gx-stream-plan-test: the mutant build runs with --mutants\n");
    return 1;
#else
    (void)argc;
    (void)argv;
    const int failures = RunAll();
    std::printf("gx-stream-plan-test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
#endif
}
