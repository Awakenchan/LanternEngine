// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <map>

#include "gpu/shim/engine/const_census.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct Entry {
    std::uint64_t bytes;
    bool buffer;
};

using Census = BbEngine::ConstCensus::Class;
using BbEngine::ConstCensus::Range;

std::map<std::uint64_t, Entry> g_objects;

bool BufferAt(std::uint64_t address) {
    Range found;
    return BbEngine::ConstCensus::ContainingRange(
        g_objects, address, [](const Entry& e) { return e.buffer ? e.bytes : 0; }, &found);
}

Census Classify(std::uint64_t address, Range ring) {
    return BbEngine::ConstCensus::Classify(address, ring, BufferAt);
}
}

int main() {
    using namespace BbEngine::ConstCensus;
    const Range ring{0x10000000, 0x100000};

    Expect(Inside(0x10000000, ring), "the ring's first byte is inside");
    Expect(Inside(0x100fffff, ring), "the ring's last byte is inside");
    Expect(!Inside(0x10100000, ring), "the byte after the ring is outside");
    Expect(!Inside(0x0fffffff, ring), "the byte before the ring is outside");
    Expect(!Inside(0x10000000, Range{0x10000000, 0}), "an empty range holds nothing");
    Expect(!Inside(0, Range{}), "an unknown ring holds nothing");

    g_objects[0x20000000] = {0x100, true};
    g_objects[0x20000100] = {0x1000, false};
    g_objects[0x20002000] = {0x40, true};
    g_objects[0x30000000] = {0x80, true};
    g_objects[0x10000800] = {0x200, true};

    Expect(Classify(0x10000000, ring) == UploadRing, "ring start");
    Expect(Classify(0x10000900, ring) == UploadRing, "the ring wins over a buffer inside it");
    Expect(Classify(0x20000000, ring) == GxBuffer, "buffer start");
    Expect(Classify(0x200000ff, ring) == GxBuffer, "buffer last byte");
    Expect(Classify(0x20000100, ring) == Other, "inside a texture, not a buffer");
    Expect(Classify(0x20001200, ring) == Other, "past the texture, the buffer before it does not reach");
    Expect(Classify(0x2000203f, ring) == GxBuffer, "second buffer last byte");
    Expect(Classify(0x20002040, ring) == Other, "one past the second buffer");
    Expect(Classify(0x1fffffff, ring) == Other, "before every object");
    Expect(Classify(0x30000040, ring) == GxBuffer, "a buffer far away");
    Expect(Classify(0x10000900, Range{}) == GxBuffer, "without the ring, the buffer");
    Expect(Classify(0x10000000, Range{}) == Other, "without the ring, nothing");

    std::map<std::uint64_t, Entry> textures;
    textures[0x40000000] = {0x100, true};
    for (std::uint64_t i = 1; i <= 8; ++i) textures[0x40000000 + 0x100 * i] = {0x100, false};
    Range found;
    const auto bytes = [](const Entry& e) { return e.buffer ? e.bytes : 0; };
    Expect(ContainingRange(textures, 0x40000010, bytes, &found) && found.base == 0x40000000 && found.bytes == 0x100,
           "the range found");
    Expect(!ContainingRange(textures, 0x40000910, bytes, &found), "the walk back stops after eight steps");

    Window last;
    Count(UploadRing, 64);
    Count(UploadRing, 128);
    Count(Other, 256);
    Window w = Take(last);
    Expect(w.copies[UploadRing] == 2 && w.bytes[UploadRing] == 192, "ring totals");
    Expect(w.copies[GxBuffer] == 0 && w.bytes[GxBuffer] == 0, "no buffer copies");
    Expect(w.copies[Other] == 1 && w.bytes[Other] == 256, "other totals");
    Count(GxBuffer, 32);
    w = Take(last);
    Expect(w.copies[UploadRing] == 0 && w.copies[GxBuffer] == 1 && w.bytes[GxBuffer] == 32 && w.copies[Other] == 0,
           "a window counts only what came after the last one");

    if (failures) {
        std::printf("const-census-test: %d failed\n", failures);
        return 1;
    }
    std::printf("const-census-test: ok\n");
    return 0;
}
