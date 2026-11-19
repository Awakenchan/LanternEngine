// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/bbnet/room/room_bundle.h"
#include "gpu/bbnet/room/room_frames.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace B = BbRoom::Bundle;
using Bytes = std::vector<std::uint8_t>;

static_assert(BbRoom::Frames::kBundle == 12);
static_assert(B::kSlotBytes == 16384);

int g_failed = 0;

void Expect(bool ok, const char* what) {
    (void)what;
    if (!ok) {
        ++g_failed;
#ifndef LANTERN_ROOM_BUNDLE_MUTANTS
        std::printf("FAIL: %s\n", what);
#endif
    }
}

Bytes Datagram(std::size_t n, std::uint32_t seed) {
    Bytes d(n);
    for (std::size_t i = 0; i < n; ++i) d[i] = static_cast<std::uint8_t>(seed * 131 + i * 7 + (i >> 8));
    return d;
}

Bytes Frame(int slot, const Bytes& bytes) {
    Bytes frame(1, BbRoom::Frames::kBundle);
    frame.push_back(static_cast<std::uint8_t>(slot));
    const auto n = static_cast<std::uint32_t>(bytes.size());
    frame.insert(frame.end(), reinterpret_cast<const std::uint8_t*>(&n), reinterpret_cast<const std::uint8_t*>(&n) + 4);
    frame.insert(frame.end(), bytes.begin(), bytes.end());
    return frame;
}

void Read042(const Bytes& data, int own, std::vector<Bytes>& got) {
    if (data.empty() || data[0] != BbRoom::Frames::kBundle) return;
    for (std::size_t at = 1; data.size() - at >= 5;) {
        const int slot = data[at];
        std::uint32_t n = 0;
        std::memcpy(&n, data.data() + at + 1, 4);
        at += 5;
        if (n > data.size() - at) return;
        for (std::size_t p = at; slot == own && at + n - p >= 2;) {
            const std::size_t m = data[p] | (std::size_t(data[p + 1]) << 8);
            p += 2;
            if (m > at + n - p) break;
            got.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(p), data.begin() + static_cast<std::ptrdiff_t>(p + m));
            p += m;
        }
        at += n;
    }
}

struct Run {
    std::vector<Bytes> bundles;
    std::size_t sends_before_end = 0;
};

Run Bundle(const std::vector<Bytes>& datagrams) {
    Run run;
    Bytes out;
    for (const auto& d : datagrams) {
        B::Add(out, d.data(), d.size(), [&](Bytes& o) {
            run.bundles.push_back(std::move(o));
            ++run.sends_before_end;
        });
    }
    if (!out.empty()) run.bundles.push_back(out);
    return run;
}

Bytes OldStream(const std::vector<Bytes>& datagrams) {
    Bytes s;
    for (const auto& d : datagrams) {
        s.push_back(static_cast<std::uint8_t>(d.size()));
        s.push_back(static_cast<std::uint8_t>(d.size() >> 8));
        s.insert(s.end(), d.begin(), d.end());
    }
    return s;
}

std::size_t Count(const Bytes& bundle) {
    std::size_t c = 0;
    for (std::size_t p = 0; p + 2 <= bundle.size();) {
        p += 2 + (bundle[p] | (std::size_t(bundle[p + 1]) << 8));
        ++c;
    }
    return c;
}

void CheckRun(const std::vector<Bytes>& in, const Run& run, const char* what) {
    bool sizes = true, empty = false;
    Bytes joined;
    for (const auto& b : run.bundles) {
        if (b.empty()) empty = true;
        if (b.size() > B::kSlotBytes && Count(b) != 1) sizes = false;
        joined.insert(joined.end(), b.begin(), b.end());
    }
    Expect(!empty, what);
    Expect(sizes, what);
    Expect(joined == OldStream(in), what);
    std::vector<Bytes> got;
    for (const auto& b : run.bundles) Read042(Frame(2, b), 2, got);
    Expect(got == in, what);
    std::vector<Bytes> other;
    for (const auto& b : run.bundles) Read042(Frame(2, b), 3, other);
    Expect(other.empty(), what);
}

void SmallPieces() {
    std::vector<Bytes> in;
    for (std::uint32_t i = 0; i < 100; ++i) in.push_back(Datagram(1000, i));
    const Run run = Bundle(in);
    CheckRun(in, run, "small pieces round trip");
    Expect(run.bundles.size() == 7, "small pieces: 16 per bundle");
    for (std::size_t i = 0; i + 1 < run.bundles.size(); ++i) Expect(run.bundles[i].size() == 16 * 1002, "small pieces: full bundles hold 16");
    Expect(run.bundles.back().size() == 4 * 1002, "small pieces: tail holds 4");
}

void ExactFill() {
    std::vector<Bytes> in;
    for (std::uint32_t i = 0; i < 8; ++i) in.push_back(Datagram(2046, i));
    const Run run = Bundle(in);
    CheckRun(in, run, "exact fill round trip");
    Expect(run.bundles.size() == 1 && run.bundles[0].size() == B::kSlotBytes, "exact fill: one bundle of 16384");
    Expect(run.sends_before_end == 1, "exact fill: sent as soon as it reaches the cap");
}

void OneOver() {
    std::vector<Bytes> in;
    for (std::uint32_t i = 0; i < 7; ++i) in.push_back(Datagram(2046, i));
    in.push_back(Datagram(2047, 7));
    const Run run = Bundle(in);
    CheckRun(in, run, "one byte over round trip");
    Expect(run.bundles.size() == 2, "one byte over: the last piece starts a new bundle");
    Expect(!run.bundles.empty() && run.bundles[0].size() == 7 * 2048, "one byte over: first bundle holds 7");
    for (const auto& b : run.bundles) Expect(b.size() <= B::kSlotBytes, "one byte over: no bundle past the cap");
}

void BigAlone() {
    std::vector<Bytes> in{Datagram(40000, 1)};
    Run run = Bundle(in);
    CheckRun(in, run, "big first round trip");
    Expect(run.bundles.size() == 1 && run.bundles[0].size() == 40002, "big first: goes alone");
    Expect(run.sends_before_end == 1, "big first: sent at once");

    in = {Datagram(100, 2), Datagram(20000, 3), Datagram(65507, 4), Datagram(50, 5)};
    run = Bundle(in);
    CheckRun(in, run, "big between round trip");
    Expect(run.bundles.size() == 4, "big between: four bundles");
    if (run.bundles.size() == 4) {
        Expect(run.bundles[0].size() == 102, "big between: small one goes before");
        Expect(run.bundles[1].size() == 20002, "big between: 20000 alone");
        Expect(run.bundles[2].size() == 65509, "big between: largest datagram alone");
        Expect(run.bundles[3].size() == 52, "big between: small one after");
    }
    Expect(run.sends_before_end == 3, "big between: big ones leave at once");
}

void Format() {
    Bytes out;
    std::vector<Bytes> sent;
    const Bytes a{0xAA, 0xBB};
    const Bytes b = Datagram(258, 9);
    B::Add(out, a.data(), a.size(), [&](Bytes& o) { sent.push_back(o); });
    B::Add(out, b.data(), b.size(), [&](Bytes& o) { sent.push_back(o); });
    Expect(sent.empty(), "format: small pieces wait");
    Bytes want{2, 0, 0xAA, 0xBB, 2, 1};
    want.insert(want.end(), b.begin(), b.end());
    Expect(out == want, "format: u16 little-endian length then bytes");
    const Bytes frame = Frame(5, out);
    Bytes head{12, 5, static_cast<std::uint8_t>(want.size()), static_cast<std::uint8_t>(want.size() >> 8), 0, 0};
    Expect(frame.size() == 6 + want.size() && std::equal(head.begin(), head.end(), frame.begin()), "format: frame header");
}

void ManyFramesOneMessage() {
    std::vector<Bytes> in;
    for (std::uint32_t i = 0; i < 40; ++i) in.push_back(Datagram(900 + i * 13, i));
    const Run run = Bundle(in);
    Bytes message;
    for (const auto& b : run.bundles) {
        const Bytes f = Frame(4, b);
        message.insert(message.end(), f.begin() + (message.empty() ? 0 : 1), f.end());
    }
    std::vector<Bytes> got;
    Read042(message, 4, got);
    Expect(run.bundles.size() > 1, "many: more than one bundle");
    Expect(got == in, "many: one reader recovers every datagram in order");
}

void Random() {
    std::uint32_t r = 12345;
    auto next = [&] { r = r * 1103515245u + 12345u; return r >> 8; };
    for (int round = 0; round < 50; ++round) {
        std::vector<Bytes> in;
        const int count = 1 + int(next() % 120);
        for (int i = 0; i < count; ++i) {
            const std::uint32_t k = next() % 100;
            const std::size_t n = k < 70 ? 1 + next() % 1400 : k < 95 ? 1 + next() % 16000 : 1 + next() % 65507;
            in.push_back(Datagram(n, next()));
        }
        const Run run = Bundle(in);
        CheckRun(in, run, "random round trip");
    }
}

void All() {
    SmallPieces();
    ExactFill();
    OneOver();
    BigAlone();
    Format();
    ManyFramesOneMessage();
    Random();
}

}

int main() {
#ifdef LANTERN_ROOM_BUNDLE_MUTANTS
    for (int m = 0; m <= B::kMutants; ++m) {
        B::g_mutant = m;
        g_failed = 0;
        All();
        if (m == 0 && g_failed) {
            std::printf("room bundle mutants: the unmutated run failed %d checks\n", g_failed);
            return 1;
        }
        if (m > 0 && !g_failed) {
            std::printf("room bundle mutants: mutant %d survived\n", m);
            return 1;
        }
    }
    std::printf("room bundle mutants: every mutant caught\n");
    return 0;
#else
    All();
    if (g_failed) {
        std::printf("room-bundle-test: %d failed\n", g_failed);
        return 1;
    }
    std::printf("room-bundle-test: ok\n");
    return 0;
#endif
}
