// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <thread>
#include <vector>

#include "gpu/shim/engine/gx_sealed.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
}

int main() {
    using namespace BbEngine::GxSealed;
    Registry r(64);
    Hit hit;

    Expect(r.Seal(0x1000, 0) == 0, "an empty buffer is not sealed");
    const u32 a = r.Seal(0x10000, 0x1000);
    const u32 b = r.Seal(0x20000, 0x800);
    Expect(a && b && a != b, "two buffers sealed");

    Expect(r.Find(0x10000, 0x1000, &hit) && hit.id == a && hit.base == 0x10000, "the whole buffer");
    Expect(r.Find(0x10800, 0x100, &hit) && hit.id == a, "a range inside");
    Expect(r.Find(0x10f00, 0x200, &hit) && hit.id == a, "a range starting inside may run past the end (one V# per element)");
    Expect(!r.Find(0x0ff00, 0x200, &hit), "a range starting before");
    Expect(!r.Find(0x11000, 0x10, &hit), "the byte after the buffer");
    Expect(!r.Find(0x30000, 0x10, &hit), "unsealed memory");

    Expect(r.Find(0x20100, 0x10, &hit) && r.Valid(hit), "a hit is valid");
    const Hit kept = hit;
    r.RetireRange(0x20400, 4, ByCpuWrite);
    Expect(!r.Valid(kept), "a CPU write inside retires it");
    Expect(!r.Find(0x20100, 0x10, &hit), "a retired buffer is not found");
    Expect(r.RetiredCount(ByCpuWrite) == 1, "counted by reason");

    Expect(r.Find(0x10000, 0x10, &hit) && r.Valid(hit), "the other buffer is untouched");
    r.RetireRange(0x0f000, 0x1000, ByGpuWrite);
    Expect(r.Valid(hit), "a write ending at its base does not retire it");
    r.RetireRange(0x0f000, 0x1001, ByGpuWrite);
    Expect(!r.Valid(hit), "a write reaching its first byte does");

    const u32 c = r.Seal(0x40000, 0x1000);
    Expect(r.Find(0x40000, 0x10, &hit) && r.Valid(hit), "a new seal");
    const Hit before = hit;
    const u32 d = r.Seal(0x40800, 0x1000);
    Expect(!r.Valid(before) && d && c, "sealing over a sealed range retires the old one");
    Expect(r.Find(0x40800, 0x1000, &hit) && hit.id == d && r.Valid(hit), "the new one is found");

    const u32 reused = r.Seal(0x50000, 0x100);
    Expect(reused == b || reused == a || reused == c, "slots are reused");
    Expect(!r.Valid(kept) && !r.Valid(before), "an old hit on a reused slot stays invalid");

    Registry small(4);
    Expect(small.Seal(0x1000, 1) && small.Seal(0x2000, 1) && small.Seal(0x3000, 1), "fills");
    Expect(small.Seal(0x4000, 1) == 0, "a full registry seals nothing");

    Registry shared(1u << 12);
    for (u64 i = 0; i < 1000; ++i) shared.Seal(0x100000 + i * 0x1000, 0x800);
    std::thread writer([&] {
        for (u64 i = 0; i < 1000; i += 2) shared.RetireRange(0x100000 + i * 0x1000 + 0x10, 4, ByCpuWrite);
    });
    u64 found = 0;
    for (int round = 0; round < 50; ++round) {
        for (u64 i = 0; i < 1000; ++i) {
            Hit h;
            if (shared.Find(0x100000 + i * 0x1000, 0x10, &h) && shared.Valid(h)) ++found;
        }
    }
    writer.join();
    for (u64 i = 0; i < 1000; ++i) {
        Hit h;
        const bool live = shared.Find(0x100000 + i * 0x1000, 0x10, &h);
        if (live == (i % 2 == 0)) {
            Expect(false, "after the writer, exactly the odd buffers stay sealed");
            break;
        }
    }
    Expect(shared.Live() == 500 && shared.RetiredCount(ByCpuWrite) == 500, "500 retired, 500 live");
    (void)found;

    if (failures) {
        std::printf("gx-sealed-test: %d failed\n", failures);
        return 1;
    }
    std::printf("gx-sealed-test: ok\n");
    return 0;
}
