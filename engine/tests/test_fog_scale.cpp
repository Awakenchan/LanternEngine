// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/fog_scale.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace F = BbEngine::FogScale;

namespace {
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    ++g_failed;
#ifndef LANTERN_FOG_SCALE_MUTANTS
    std::printf("FAIL: %s\n", what);
#else
    (void)what;
#endif
}

float At(const std::vector<std::uint8_t>& bytes, std::size_t block, int index) {
    float v = 0;
    std::memcpy(&v, bytes.data() + block * F::kBlockBytes + std::size_t(index) * 4, sizeof v);
    return v;
}
float At(const std::uint8_t* bytes, std::size_t block, int index) {
    float v = 0;
    std::memcpy(&v, bytes + block * F::kBlockBytes + std::size_t(index) * 4, sizeof v);
    return v;
}

std::vector<std::uint8_t> Table(std::uint32_t count, std::size_t extra = 0) {
    std::vector<std::uint8_t> t(count * F::kBlockBytes + extra);
    for (std::size_t i = 0; i < count * F::kBlockFloats; ++i) {
        const float v = 0.25f + 0.01f * float(i % 50);
        std::memcpy(t.data() + i * 4, &v, 4);
    }
    for (std::size_t i = count * F::kBlockBytes; i < t.size(); ++i) t[i] = std::uint8_t(i);
    return t;
}

bool Opacity(int index) { return index == 0 || index == 8 || index == 12; }

void TestDefaultPath() {
    Check(F::Untouched(true, 1.0f), "fog 1.0 with picture tuning on takes the early return");
    Check(F::Untouched(false, 1.0f), "fog 1.0 with picture tuning off takes the early return");
    Check(F::Untouched(false, 0.5f), "picture tuning off takes the early return at any fog");
    Check(!F::Untouched(true, 0.99f), "fog 0.99 scales");
    Check(!F::Untouched(true, 1.01f), "fog 1.01 scales");
    Check(!F::Untouched(true, 0.0f), "fog 0 scales");
}

void TestSourceUntouched() {
    for (const float k : {0.0f, 0.5f, 2.0f, 5.0f}) {
        auto table = Table(7);
        const auto before = table;
        std::vector<std::uint8_t> scratch(table.size(), 0xee);
        const std::uint8_t* out = F::ScaleCopy(table.data(), 7, table.size(), k, scratch.data());
        Check(table == before, "the game's table keeps its own values");
        Check(out == scratch.data(), "the upload reads the scratch copy");
    }
}

void TestScaled() {
    const std::uint32_t count = 5;
    auto table = Table(count);
    std::vector<std::uint8_t> scratch(table.size());
    const float k = 0.5f;
    const std::uint8_t* out = F::ScaleCopy(table.data(), count, table.size(), k, scratch.data());
    bool scaled = true, kept = true;
    for (std::size_t b = 0; b < count; ++b) {
        for (int i = 0; i < int(F::kBlockFloats); ++i) {
            const float was = At(table, b, i), now = At(out, b, i);
            if (Opacity(i)) scaled &= now == was * k;
            else kept &= now == was;
        }
    }
    Check(scaled, "floats 0, 8 and 12 of every block, block 0 included, are scaled once");
    Check(kept, "the other floats are copied unchanged");

    std::vector<std::uint8_t> again(table.size());
    const std::uint8_t* second = F::ScaleCopy(table.data(), count, table.size(), k, again.data());
    Check(std::memcmp(second, out, table.size()) == 0, "a second upload scales the game's values again, not the last result");

    auto bright = Table(2);
    std::vector<std::uint8_t> up(bright.size());
    const std::uint8_t* raised = F::ScaleCopy(bright.data(), 2, bright.size(), 3.0f, up.data());
    Check(At(raised, 1, 0) == std::min(At(bright, 1, 0) * 3.0f, 1.0f), "fog above 1 stops at an opacity of 1");
}

void TestUploadBeyondBlocks() {
    const std::uint32_t count = 3;
    auto table = Table(count, 40);
    std::vector<std::uint8_t> scratch(table.size());
    const std::size_t bytes = F::CopyBytes(count, std::uint32_t(table.size() - 2));
    Check(bytes == table.size(), "the copy covers every byte the upload reads, rounded up to 4");
    const std::uint8_t* out = F::ScaleCopy(table.data(), count, bytes, 0.5f, scratch.data());
    Check(std::memcmp(out + count * F::kBlockBytes, table.data() + count * F::kBlockBytes, 40) == 0,
          "bytes past the blocks are copied as they are");
    Check(F::CopyBytes(count, 0) == count * F::kBlockBytes, "a small upload still copies every block");
}

void TestCap() {
    Check(F::CopyBytes(F::kMaxBlocks, 0) == F::kMaxBytes, "the largest table fits");
    Check(F::CopyBytes(F::kMaxBlocks + 1, 0) == 0, "a table over the cap is not copied");
    Check(F::CopyBytes(1, std::uint32_t(F::kMaxBytes + 4)) == 0, "an upload over the cap is not copied");
}

void TestSnap() {
    Check(F::SnapToOne(0.996f) == 1.0f, "0.996 snaps to 1");
    Check(F::SnapToOne(1.004f) == 1.0f, "1.004 snaps to 1");
    Check(F::SnapToOne(1.0f) == 1.0f, "1 stays 1");
    Check(F::SnapToOne(0.99f) == 0.99f, "0.99 keeps its value");
    Check(F::SnapToOne(1.01f) == 1.01f, "1.01 keeps its value");
    Check(F::Untouched(true, F::SnapToOne(0.997f)), "a snapped value takes the early return");
}

void TestMenuStep() {
    Check(F::MenuStep(1.0f, 0, 0.1f, 10) == 10, "fog 1.0 shows 10");
    Check(F::MenuStep(0.96f, 0, 0.1f, 10) == 9, "fog 0.96 does not show 10");
    Check(F::MenuStep(0.99f, 0, 0.1f, 10) == 9, "fog 0.99 does not show 10");
    Check(F::MenuStep(1.5f, 0, 0.1f, 10) == 9, "fog 1.5 does not show 10");
    Check(F::MenuStep(3.0f, 0, 0.1f, 10) == 9, "fog 3 does not show 10");
    Check(F::MenuStep(0.5f, 0, 0.1f, 10) == 5, "fog 0.5 shows 5");
    Check(F::MenuStep(0.0f, 0, 0.1f, 10) == 0, "fog 0 shows 0");
    Check(F::MenuStep(0.0f + 10 * 0.1f, 0, 0.1f, 10) == 10, "the page's own 10 reads back as 10");
    Check(F::MenuStep(1.03f, 0.5f, 0.1f, 10) == 6, "a neutral step is not shown for a value above it");
    Check(F::MenuStep(0.7f, 1.0f, 0.2f, 10) == 1, "a neutral first step is not shown for a value below it");
    Check(F::MenuStep(2.0f, 1.0f, 0.2f, 10) == 5, "other values round as before");
}

int RunSuite() {
    g_failed = 0;
    TestDefaultPath();
    TestSourceUntouched();
    TestScaled();
    TestUploadBeyondBlocks();
    TestCap();
    TestSnap();
    TestMenuStep();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_FOG_SCALE_MUTANTS
    const char* names[] = {"", "scales in place", "skips block 0", "no cap", "scales at fog 1"};
    static_assert(std::size(names) == F::kMutants + 1);
    int survivors = 0;
    for (int m = 0; m <= F::kMutants; ++m) {
        F::g_mutant = m;
        const int found = RunSuite();
        if (m == 0) {
            if (found) {
                std::printf("fog scale mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("fog scale mutants: mutant %d (%s) survived\n", m, names[m]);
            ++survivors;
        } else {
            std::printf("fog scale mutants: mutant %d (%s) caught by %d checks\n", m, names[m], found);
        }
    }
    if (survivors) return 1;
    std::printf("fog scale mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("fog scale: %d failures\n", g_failed);
        return 1;
    }
    std::printf("fog scale: all tests passed\n");
    return 0;
#endif
}
