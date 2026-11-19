// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/gx_census.h"
#include "video_core/amdgpu/regs.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
namespace C = BbEngine::GxFlushCensus;
using u32 = std::uint32_t;
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

AmdGpu::Regs& Registers() {
    static AmdGpu::Regs regs{};
    return regs;
}

struct Field {
    const char* name;
    u32 first;
    u32 words;
};

template <typename F> Field FieldOf(const char* name, const F& field) {
    const auto& regs = Registers();
    const u32 first = u32(reinterpret_cast<const u32*>(&field) - regs.reg_array.data());
    return {name, first, u32((sizeof(F) + 3) / 4)};
}

std::vector<Field> ReaderFields() {
    auto& regs = Registers();
    std::vector<Field> fields;
#define LANTERN_FIELD(name) fields.push_back(FieldOf(#name, regs.name));
    LANTERN_GX_READER_FIELDS(LANTERN_FIELD)
#undef LANTERN_FIELD
    return fields;
}

void TestCheck() {
    const C::Result result = C::Check();
    if (result.finding != C::Finding::None) {
        std::printf("FAIL: census check: %s (entry %zu)\n", C::kFindingName[static_cast<std::size_t>(result.finding)], result.row);
        ++failures;
    }
}

void TestReadersAreRecordRows() {
    for (const Field& field : ReaderFields()) {
        if (!C::RecordOfSkippedFlush(field.first)) {
            std::printf("FAIL: reader field %s (register 0x%x) is not a Record row of a skipped flush\n", field.name, field.first);
            ++failures;
        }
    }
}

void TestNotReadRowsMissReaders() {
    const auto fields = ReaderFields();
    for (std::size_t i = 0; i < C::kRowCount; ++i) {
        if (!C::Present(i)) continue;
        const C::Row row = C::RowAt(i);
        if (row.tag != C::Tag::NotRead) continue;
        for (const Field& field : fields) {
            if (C::Overlaps(row, field.first, field.words)) {
                std::printf("FAIL: NotRead row %s overlaps reader field %s\n", row.name, field.name);
                ++failures;
            }
        }
    }
}

void TestKnownRegisters() {
    Expect(C::RecordOfSkippedFlush(FieldOf("primitive_type", Registers().primitive_type).first), "VGT_PRIMITIVE_TYPE from the index flush");
    Expect(FieldOf("primitive_type", Registers().primitive_type).first == 0xC000 + 0x242, "primitive_type is uconfig 0x242");
    Expect(FieldOf("clipper_control", Registers().clipper_control).first == 0xA000 + 0x204, "clipper_control is context 0x204");
    Expect(C::Covered(0xA000 + 0x204, C::Writer::Raster, C::Tag::Record), "0x1076b30 writes PA_CL_CLIP_CNTL");
    Expect(!C::Covered(0xA000 + 0x16f, C::Writer::Raster, C::Tag::Record), "the rasterizer flush writes no user clip plane");
    Expect(C::Covered(0xA000 + 0x202, C::Writer::OutputMerger, C::Tag::Kept), "CB_COLOR_CONTROL only on the resolve path");
    Expect(C::Covered(0xA000 + 0x2f8, C::Writer::Target, C::Tag::Kept), "AA config from the target flush");
}

void TestShadowSelection() {
    const std::size_t colour[4] = {0x60, 0xa8, 0x60, 0xa8};
    const std::size_t depth[4] = {0xa0, 0xa0, 0xe8, 0xe8};
    for (u32 bits = 0; bits < 4; ++bits) {
        Expect(C::Layout::ColorShadow(bits, 0) == colour[bits], "colour shadow set from bit 0");
        Expect(C::DepthShadowOf(bits) == depth[bits], "depth shadow set from bit 1");
    }
    Expect(C::Layout::ColorShadow(1, 7) == 0xa8 + 7 * 8, "colour slot stride 8");
}

void TestLayout() {
    const C::Result result = C::CheckLayout();
    Expect(result.finding == C::Finding::None, "flush objects, sync list and command buffer");
    Expect(C::ObjectStart(C::Object::Raster) + C::ObjectSize(C::Object::Raster) <= C::ObjectStart(C::Object::OutputMerger),
           "rasterizer object ends at or before ctx+0xb558");
    Expect(C::CommandFlags() == 0x28, "command buffer flags at +0x28");
}

void TestOutput() {
    Expect(C::MaxDwords(C::Writer::OutputMerger) == 50, "output merger value path 50 dwords");
    Expect(C::MaxDwords(C::Writer::Index) == 8, "index flush 8 dwords");
    Expect(C::MaxDwords(C::Writer::Raster) == 301, "rasterizer flush 301 dwords");
    Expect(C::MaxDwords(C::Writer::Target) == 213, "target flush 213 dwords");
    Expect(C::kSideBufferDwords == 602, "side buffer twice the largest skipped flush");
    Expect(C::CheckOutput().finding == C::Finding::None, "output constants match the rows");
}

int RunSuite() {
    failures = 0;
    TestCheck();
    TestReadersAreRecordRows();
    TestNotReadRowsMissReaders();
    TestKnownRegisters();
    TestShadowSelection();
    TestLayout();
    TestOutput();
    return failures;
}
}

int main() {
#ifdef LANTERN_GX_FLUSH_CENSUS_MUTANTS
    using C::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        C::selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("FAIL: %d failures without a mutant\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("FAIL: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("mutant %d caught (%d checks)\n", m, found);
        }
    }
    C::selected_mutant = Mutant::None;
    if (survivors) return 1;
    std::printf("gx-flush-census-mutants: all %d mutants caught\n", static_cast<int>(Mutant::kCount) - 1);
    return 0;
#else
    const int found = RunSuite();
    if (found) {
        std::printf("gx-flush-census-test: %d failures\n", found);
        return 1;
    }
    std::printf("gx-flush-census-test: ok, %zu rows\n", C::kRowCount);
    return 0;
#endif
}
