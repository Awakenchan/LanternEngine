// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "gpu/shim/engine/gx_objects.h"
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
    Expect(r.RetiredCount(ByOverlap) == 1, "counted as an overlapping seal, not as one of our own sites");
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

    {
        u32 combinations = 0, sealable = 0;
        for (std::uint8_t kind : {std::uint8_t(0), kKindBuffer, std::uint8_t(2)}) {
            for (unsigned usage = 0; usage < 5; ++usage) {
                for (unsigned bind = 0; bind < 256; ++bind) {
                    for (const bool dynamic : {false, true}) {
                        for (const bool data : {false, true}) {
                            BufferTraits t;
                            t.kind = kind;
                            t.usage = std::uint8_t(usage);
                            t.bind = std::uint8_t(bind);
                            t.dynamic = dynamic;
                            t.has_data = data;
                            const bool expected = kind == 1 && (bind & 3) && !(bind & 0x80) && !(bind & 0x10) && !dynamic &&
                                                  (usage == 1 || (usage == 0 && data));
                            const Refusal got = SealRule(t);
                            ++combinations;
                            sealable += got == Sealable;
                            if ((got == Sealable) != expected) {
                                std::printf("FAIL: rule kind %u usage %u bind %#x dynamic %d data %d gave %s\n", kind, usage, bind,
                                            dynamic, data, kRefusalNames[got]);
                                ++failures;
                            }
                        }
                    }
                }
            }
        }
        Expect(combinations == 3 * 5 * 256 * 4 && sealable == 48 * 3, "every flag combination checked: 48 binds by 3 usage and data pairs");
        const auto rule = [](std::uint8_t usage, std::uint8_t bind, bool dynamic, bool data) {
            BufferTraits t;
            t.kind = kKindBuffer;
            t.usage = usage;
            t.bind = bind;
            t.dynamic = dynamic;
            t.has_data = data;
            return SealRule(t);
        };
        Expect(rule(UsageImmutable, kBindVertex, false, true) == Sealable, "an immutable vertex buffer is sealed");
        Expect(rule(UsageDefault, kBindIndex, false, true) == Sealable, "a default index buffer with data is sealed");
        Expect(rule(UsageDefault, kBindIndex, false, false) == RefusedNoData, "a default index buffer without data is not");
        Expect(rule(UsageDefault, kBindVertex | kBindShaderResource | kBindUnorderedAccess, false, true) == RefusedUnorderedAccess,
               "a particle buffer the GPU writes (bind 0x89) is not");
        Expect(rule(UsageImmutable, kBindConstant, false, true) == RefusedNotGeometry, "a constant buffer is not");
        Expect(rule(UsageDefault, kBindVertex | kBindStreamOutput, false, true) == RefusedStreamOutput, "a stream output target is not");
        Expect(rule(UsageDynamic, kBindVertex, true, false) == RefusedDynamic, "a dynamic vertex buffer is not");
        Expect(rule(UsageStaging, kBindVertex, false, true) == RefusedUsage, "a staging buffer is not");
        BufferTraits texture;
        texture.kind = 2;
        texture.usage = UsageImmutable;
        texture.bind = kBindVertex;
        Expect(SealRule(texture) == RefusedKind, "only kind 1 is a buffer");

        BufferTraits h;
        h.usage = UsageImmutable;
        Expect(HolderOf(h) == HolderImmutable, "immutable holder");
        h.usage = UsageDefault;
        h.has_data = true;
        Expect(HolderOf(h) == HolderDefaultData, "default with data");
        h.has_data = false;
        Expect(HolderOf(h) == HolderDefaultEmpty, "default without data");
        h.usage = UsageDynamic;
        h.dynamic = true;
        Expect(HolderOf(h) == HolderDynamic, "dynamic holder");
        h.usage = UsageStaging;
        h.dynamic = false;
        Expect(HolderOf(h) == HolderOtherGx, "a staging buffer is another GX holder");

        auto tally = std::make_unique<SealTally>();
        tally->sealed[UsageImmutable][kBindVertex] = 40;
        tally->sealed[UsageImmutable][kBindIndex] = 12;
        tally->sealed[UsageDefault][kBindVertex | kBindShaderResource] = 3;
        tally->refused[RefusedUnorderedAccess] = 5;
        tally->refused[RefusedNotGeometry] = 7;
        tally->faults_in_sealed = 9;
        tally->faults_left_sealed = 2;
        char line[1024];
        FormatSeals(line, sizeof line, "so far", *tally);
        std::printf("%s\n", line);
        Expect(std::strstr(line, "GX seals (so far): 55 sealed, 12 refused; sealed by usage and bind: default VB|SRV 3; immutable VB 40; "
                                 "immutable IB 12; refused: neither VB nor IB 7; UAV 5; guest write faults in a sealed range 9, "
                                 "still sealed after the fault 2") != nullptr,
               "seal line");
        FormatSeals(line, 30, "x", *tally);
        Expect(std::strlen(line) == 29, "a short buffer is cut, not overrun");
    }

    {
        using namespace BbEngine::GxObjects;
        HeldLinks held;
        Expect(UseHeld(held.At(0x200000), 7) == HeldNothing, "nothing held at a new texture");
        held.Hold(0x200000, 3, 0x1003, 7);
        Expect(UseHeld(held.At(0x200000), 7) == HeldServe, "served while no image was made or freed");
        Expect(held.At(0x200000).index == 3 && held.At(0x200000).uid == 0x1003, "the held image");
        Expect(UseHeld(held.At(0x200000), 8) == HeldSearch, "an image rendered into those bytes later forces the full search");
        held.Hold(0x200000, 3, 0x1003, 8);
        Expect(UseHeld(held.At(0x200000), 8) == HeldServe, "held again after the search");
        Expect(held.Drop(0x1003) && UseHeld(held.At(0x200000), 8) == HeldNothing, "freeing the image drops its link");
        Expect(!held.Drop(0x1003), "a freed image is dropped once");
        held.Hold(0x200000, 4, 0x1004, 9);
        held.Hold(0x200000, 5, 0x1005, 9);
        Expect(!held.Drop(0x1004) && held.At(0x200000).uid == 0x1005, "a texture holds one image; the one it replaced has no link left");
        held.Hold(0x300000, 5, 0x1005, 9);
        Expect(UseHeld(held.At(0x200000), 9) == HeldNothing && held.At(0x300000).uid == 0x1005, "an image is held by one texture");
        held.Forget(0x300000);
        Expect(held.Size() == 0 && !held.Drop(0x1005), "a released texture forgets its image");
        held.Hold(0x400000, 6, 0, 9);
        Expect(held.Size() == 0, "no image, no link");
    }

    if (failures) {
        std::printf("gx-sealed-test: %d failed\n", failures);
        return 1;
    }
    std::printf("gx-sealed-test: ok\n");
    return 0;
}
