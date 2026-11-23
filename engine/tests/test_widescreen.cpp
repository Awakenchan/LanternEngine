// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gpu/shim/engine/widescreen.h"
#include "../gpu/shim/bbport_platform.h"
#include "../gpu/shim/bbport_settings.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef _WIN32
namespace BbPlatform {
bool ReadProcessMemory(const void* p, void* out, size_t n) {
    if (!p) return false;
    std::memcpy(out, p, n);
    return true;
}
}
#else
// The runtime's memory map, which BbPlatform::ReadProcessMemory falls back to on Linux: with no
// runtime in the test, every address reads as on Windows above (the test passes its own memory).
extern "C" int runtime_memory_vma_info(uintptr_t, int* prot, int* type, uintptr_t* end) {
    *prot = 1;
    *type = 0;
    *end = UINTPTR_MAX;
    return 1;
}
#endif

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
struct Read { std::size_t at; bool height, ecx; };
constexpr Read kReads[] = {{0x15e83af, 0, 0}, {0x1bfd491, 0, 0}, {0x1bfd4d1, 1, 0}, {0x1644357, 0, 0},
                           {0x1644365, 1, 0}, {0x1644c55, 0, 0}, {0x1644c63, 1, 0}, {0x16452c7, 0, 0},
                           {0x16452d5, 1, 0}, {0x1654df6, 0, 0}, {0x1654e1c, 1, 0}, {0x1655611, 1, 0},
                           {0x1655628, 0, 0}, {0x16b3372, 0, 1}, {0x16b3398, 1, 0}};
std::vector<std::uint8_t> image(0x5130000);
std::uint32_t U32(std::size_t at) { std::uint32_t v; std::memcpy(&v, &image[at], 4); return v; }
float F32(std::size_t at) { float v; std::memcpy(&v, &image[at], 4); return v; }
template <std::size_t N> void Put(std::size_t at, const std::uint8_t (&b)[N]) { std::memcpy(&image[at], b, N); }
std::uint32_t Right(int w, int h) {
    const float rx = std::fmax(1.0f, (float(w) / float(h)) / (16.0f / 9.0f));
    return std::uint32_t(1920 + std::ceil(960 * (rx - 1)));
}
}

int main() {
    for (const auto& r : kReads) {
        std::uint8_t code[9] = {0x48, 0x8d, std::uint8_t(r.ecx ? 0x0d : 0x05), 0, 0, 0, 0, 0x8b,
                                std::uint8_t(r.ecx ? 0x09 : 0x00)};
        const auto d = std::int32_t(std::int64_t(r.height ? 0x51289fc : 0x51289f8) - std::int64_t(r.at + 7));
        std::memcpy(code + 3, &d, 4);
        std::memcpy(&image[r.at], code, 9);
    }
    const std::uint8_t pinned[] = {0xb8, 0x80, 0x07, 0, 0, 0x0f, 0x1f, 0x40, 0};
    Put(0x15e83af, pinned);
    const float plate[] = {0.5f, -0.5f}, bounds[] = {1920, 1080};
    for (std::size_t at : {0x48f9a00, 0x48f9a60, 0x48f9ad0}) std::memcpy(&image[at], plate, 8);
    std::memcpy(&image[0x4926ea4], bounds, 8);
    const std::uint8_t ja30[] = {0x77, 0x30}, ja2f[] = {0x77, 0x2f}, ja5a[] = {0x77, 0x5a}, setbe[] = {0x0f, 0x96, 0xc1},
                       ja14[] = {0x77, 0x14}, ja34[] = {0x77, 0x34}, ja20[] = {0x77, 0x20};
    Put(0x1bfd473, ja30); Put(0x1654dd9, ja2f); Put(0x16b3370, ja5a); Put(0x1642520, setbe);
    Put(0x1bfd4cf, ja14); Put(0x1654e1a, ja14); Put(0x16b3396, ja34); Put(0x164253c, ja20);
    const std::uint8_t entry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                  0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x48};
    Put(0x14368b0, entry);

    auto& s = BbSettings::Get();
    s.widescreen = true;
    const int wide = 5;
    s.output_res = s.startup_output_res = wide;
    const std::uint32_t right = Right(BbSettings::OutputWidth(wide), BbSettings::OutputHeight(wide));

    image[0x1655628] = 0xcc;
    Check(!BbEngine::Widescreen::Install(image.data(), image.size()), "foreign code is refused");
    Check(!std::memcmp(&image[0x14368b0], entry, sizeof entry), "a refusal leaves the camera hook site alone");
    image[0x1655628] = 0x48;

    Check(BbEngine::Widescreen::Install(image.data(), image.size()), "1.09's code is accepted");
    Check(image[0x1655628] == 0xb8 && U32(0x1655628 + 1) == 1920, "a plain read is pinned at 1920");
    Check(image[0x16b3372] == 0xb9 && U32(0x16b3372 + 1) == right, "the ecx right edge gets the widened width");
    Check(U32(0x1bfd491 + 1) == right && U32(0x1bfd4d1 + 1) == 1080, "21:9: right edge widened, bottom native");
    Check(F32(0x48f9a00) > 0.5f && F32(0x48f9a00 + 4) == -0.5f, "21:9: plates widened only");
    Check(F32(0x4926ea4) == float(right), "21:9: the UI's right bound widened");
    Check(image[0x1bfd473] == 0x66 && image[0x1642520] == 0xb1, "21:9: the left clips open");
    Check(image[0x1bfd4cf] == 0x77, "21:9: the top clips stay");

    s.output_res = s.startup_output_res = 2;
    BbEngine::Widescreen::Tick();
    Check(U32(0x1bfd491 + 1) == 1920 && image[0x1bfd473] == 0x77 && image[0x1642520] == 0x0f,
          "16:9: edges and clips are the game's");
    Check(F32(0x48f9a00) == 0.5f && F32(0x4926ea4) == 1920, "16:9: plates and bounds are the game's");

    Check(BbEngine::Widescreen::NarrowFovFactor() == 1.0f, "16:9 needs no Vert+");

    if (failures) return 1;
    std::puts("Widescreen: PASS (1.09 sites and the startup patch's pins accepted, foreign code refused, 21:9 "
              "widened, 16:9 restored)");
    return 0;
}
