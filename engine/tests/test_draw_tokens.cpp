// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "gpu/shim/bbport_native_calls.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool Same(const BbNativeCalls::DrawRecord& a, const BbNativeCalls::DrawRecord& b) {
    return a.kind == b.kind && a.predicate == b.predicate && a.index_count == b.index_count &&
           a.index_offset == b.index_offset && a.index_addr == b.index_addr && a.flags == b.flags &&
           a.base_vertex == b.base_vertex && a.instances == b.instances;
}

void RoundTrip(BbNativeCalls::DrawRecord r) {
    using namespace BbNativeCalls;
    u32 out[16];
    std::memset(out, 0xab, sizeof out);
    const u32 n = EncodeRecordNop(r, out);
    Expect(n == RecordDwords(r.kind, r.flags), "record size");
    Expect(((out[0] >> 16) & 0x3fff) == n - 2, "header count");
    Expect(out[n] == 0xababababu, "nothing written past the record");
    DrawRecord back;
    Expect(DecodeRecordNop(out, back), "decodes");
    Expect(Same(r, back), "decodes to the same record");
    for (u32 word = 1; word < n; ++word) {
        u32 bad[16];
        std::memcpy(bad, out, sizeof bad);
        bad[word] ^= 0x10u;
        DrawRecord ignored;
        Expect(!DecodeRecordNop(bad, ignored), "a changed word is rejected");
    }
}

void OldFormatUnchanged() {
    using namespace BbNativeCalls;
    DrawRecord r;
    r.kind = DrawIndexOffset;
    r.predicate = 1;
    r.index_count = 36;
    r.index_offset = 12;
    u32 out[8]{};
    Expect(EncodeRecordNop(r, out) == 7, "an old record keeps its 7 words");
    u32 expected[7] = {0xc0051000u, TagMagic | 0x100u | DrawIndexOffset, 36, 12, 0, 0, 0};
    expected[6] = RecordCheck(expected + 1, 5);
    Expect(std::memcmp(out, expected, sizeof expected) == 0, "an old record is encoded as before");
}

void ReservedBitsRejected() {
    using namespace BbNativeCalls;
    DrawRecord r;
    r.kind = DrawAuto;
    r.index_count = 3;
    u32 out[8]{};
    EncodeRecordNop(r, out);
    out[1] |= 0x800u;
    out[4] = RecordCheck(out + 1, 3);
    DrawRecord ignored;
    Expect(!DecodeRecordNop(out, ignored), "an unknown flag bit is rejected");
}

void RegisterPackets() {
    using namespace BbNativeCalls;
    DrawRecord r;
    r.flags = CarriesBaseVertex | CarriesInstances;
    r.base_vertex = 7;
    r.instances = 2;
    u32 out[5]{};
    Expect(EncodeRegisterPackets(r, out) == 5, "both register packets");
    const u32 expected[5] = {0xc0016900u, 0x102u, 7, 0xc0002f00u, 2};
    Expect(std::memcmp(out, expected, sizeof expected) == 0, "the packets setIndexOffset and setNumInstances write");
    r.flags = 0;
    Expect(EncodeRegisterPackets(r, out) == 0, "no packets without carried values");
}

void Sizes() {
    using namespace BbNativeCalls;
    const u32 both = CarriesBaseVertex | CarriesInstances;
    Expect(RecordDwords(DrawIndexOffset, both) < SetIndexOffsetDwords + SetNumInstancesDwords + ReservedDwords(DrawIndexOffset),
           "an indexed token is smaller than the packets it replaces");
    Expect(RecordDwords(DrawAuto, both) < SetIndexOffsetDwords + SetNumInstancesDwords + ReservedDwords(DrawAuto),
           "an auto token is smaller than the packets it replaces");
    Expect(RecordDwords(DrawIndexOffset, both) == ReservedDwords(DrawIndexOffset), "an indexed token fits the draw's own space");
}
}

int main() {
    using namespace BbNativeCalls;
    for (u32 kind : {u32(DrawAuto), u32(DrawIndex), u32(DrawIndexOffset)}) {
        for (u32 flags : {0u, u32(CarriesBaseVertex | CarriesInstances)}) {
            DrawRecord r;
            r.kind = kind;
            r.predicate = flags ? 1 : 0;
            r.index_count = 0x12345u + kind;
            r.index_offset = kind == DrawIndexOffset ? 0x777u : 0;
            r.index_addr = kind == DrawIndex ? 0x10'2345'6780ull : 0;
            r.flags = flags;
            r.base_vertex = flags ? 0xdead0u + kind : 0;
            r.instances = flags ? 5 : 0;
            RoundTrip(r);
        }
    }
    OldFormatUnchanged();
    ReservedBitsRejected();
    RegisterPackets();
    Sizes();
    if (failures) {
        std::printf("draw-tokens-test: %d failures\n", failures);
        return 1;
    }
    std::printf("draw-tokens-test: ok\n");
    return 0;
}
