// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gpu/shim/engine/graphics.h"
#include "../gpu/shim/bbport_platform.h"
#include "../gpu/shim/bbport_settings.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#endif

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
void Stock(bool enabled) {
    BbSettings::Get().graphics_local_shadow_resolution = enabled ? 0 : 2;
}
void Directional(bool enabled) {
    BbSettings::Get().startup_effects[BbSettings::EffectPcss] = enabled;
}
}

#ifndef _WIN32
void WritableAgain(int, siginfo_t* info, void*) {
    const auto page = reinterpret_cast<std::uintptr_t>(info->si_addr) & ~std::uintptr_t(4095);
    if (mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC)) _exit(2);
}
#endif

int main() {
#ifndef _WIN32
    struct sigaction action{};
    action.sa_sigaction = WritableAgain;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Stock(false);
    constexpr std::size_t pool = 0x22440b0, size_at = 0x224443f, tier = 0x2244cb0;
    constexpr std::uint8_t entry[] = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
        0x48, 0x81, 0xe4, 0xe0, 0xff, 0xff, 0xff, 0x48, 0x81, 0xec, 0x20, 0x01, 0x00, 0x00};
    constexpr std::uint8_t read[] = {
        0x41, 0x8b, 0x44, 0x24, 0x14, 0x4d, 0x89, 0xe6, 0x4c, 0x89, 0x74, 0x24, 0x58,
        0x89, 0x44, 0x24, 0x44};
    constexpr std::uint8_t tiers[] = {0x41, 0xd1, 0xed, 0x48, 0xff, 0xc1, 0x48, 0x83, 0xf9, 0x04};
    std::vector<std::uint8_t> image(tier + sizeof tiers, 0xcc);
    std::memcpy(image.data() + pool, entry, sizeof entry);
    std::memcpy(image.data() + size_at, read, sizeof read);
    std::memcpy(image.data() + tier, tiers, sizeof tiers);
    const auto original = image;
    Stock(false);
    Check(!BbEngine::Graphics::InstallLocalShadows(nullptr, image.size()), "null images are refused");
    Check(!BbEngine::Graphics::InstallLocalShadows(image.data(), tier + sizeof tiers - 1),
          "truncated images are refused");
    Check(image == original, "a truncated image is not modified");
    for (const auto at : {pool, size_at, size_at + sizeof read - 1, tier}) {
        image[at] ^= 1;
        const auto foreign = image;
        Check(!BbEngine::Graphics::InstallLocalShadows(image.data(), image.size()),
              "foreign pool, size-load and tier code are refused");
        Check(image == foreign, "a refused patch changes no bytes");
        image[at] ^= 1;
    }
    Stock(true);
    Check(!BbEngine::Graphics::InstallLocalShadows(image.data(), image.size()), "stock opt-out is honored");
    Check(image == original, "stock mode leaves the whole image unchanged");
    Stock(false);
    Check(BbEngine::Graphics::InstallLocalShadows(image.data(), image.size()), "1.09 pool code is accepted");
    Check(image[size_at] == 0xb8, "the native point base load becomes mov eax,imm32");
    std::uint32_t base = 0;
    std::memcpy(&base, image.data() + size_at + 1, sizeof base);
    Check(base == 2048, "the point base size is 2048");
    const std::array<std::uint32_t, 4> expected{2048, 1024, 512, 256};
    for (unsigned i = 0; i < expected.size(); ++i)
        Check((base >> i) == expected[i], "native halving generates the requested tiers");
    std::memcpy(image.data() + size_at, read, 5);
    Check(image == original, "only the five-byte point-size load changed; counts and other pools stay native");

    constexpr std::size_t spot_size = 0x22440ed, spot_tier = 0x224442a;
    constexpr std::uint8_t spot_read[] = {
        0x41, 0x8b, 0x04, 0x24, 0x89, 0x44, 0x24, 0x68, 0x48, 0x8d, 0x4b, 0x10,
        0x48, 0x89, 0x4c, 0x24, 0x50, 0x49, 0x89, 0xdd, 0x45, 0x31, 0xff, 0x89, 0x44, 0x24, 0x60};
    constexpr std::uint8_t spot_tiers[] = {
        0xd1, 0x6c, 0x24, 0x68, 0xd1, 0x6c, 0x24, 0x60, 0x49, 0xff, 0xc7, 0x49, 0x83, 0xff, 0x04};
    std::memcpy(image.data() + spot_size, spot_read, sizeof spot_read);
    std::memcpy(image.data() + spot_tier, spot_tiers, sizeof spot_tiers);
    const auto spot_original = image;
    Stock(false);
    Check(!BbEngine::Graphics::InstallSpotShadows(nullptr, image.size()), "null spotlight images are refused");
    Check(!BbEngine::Graphics::InstallSpotShadows(image.data(), spot_tier + sizeof spot_tiers - 1),
          "truncated spotlight images are refused");
    Check(image == spot_original, "a truncated spotlight image is not modified");
    for (const auto at : {pool, spot_size, spot_size + sizeof spot_read - 1, spot_tier}) {
        image[at] ^= 1;
        const auto foreign = image;
        Check(!BbEngine::Graphics::InstallSpotShadows(image.data(), image.size()),
              "foreign spotlight initialization and tier code are refused");
        Check(image == foreign, "a refused spotlight patch changes no bytes");
        image[at] ^= 1;
    }
    Stock(true);
    Check(!BbEngine::Graphics::InstallSpotShadows(image.data(), image.size()), "spotlight opt-out is honored");
    Check(image == spot_original, "stock spotlight mode leaves the whole image unchanged");
    Stock(false);
    Check(BbEngine::Graphics::InstallSpotShadows(image.data(), image.size()), "spotlight upgrade installs");
    constexpr std::uint8_t spot_resolution[] = {0x31, 0xc0, 0xb4, 0x10};
    Check(!std::memcmp(image.data() + spot_size, spot_resolution, sizeof spot_resolution),
          "the spotlight base load sets EAX to 4096");
#ifdef _WIN32
    auto* code = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    void* memory = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    auto* code = memory == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(memory);
#endif
    Check(code != nullptr, "executable memory for spotlight instruction regression");
    if (code) {
        constexpr std::uint8_t incoming[] = {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff};
        std::memcpy(code, incoming, sizeof incoming);
        std::memcpy(code + sizeof incoming, image.data() + spot_size, sizeof spot_resolution);
        code[sizeof incoming + sizeof spot_resolution] = 0xc3;
        using Size = std::uint64_t (__attribute__((sysv_abi)) *)();
        const auto spot_base = reinterpret_cast<Size>(code)();
        constexpr std::array<std::uint32_t, 4> spot_expected{4096, 2048, 1024, 512};
        for (unsigned i = 0; i < spot_expected.size(); ++i)
            Check((spot_base >> i) == spot_expected[i], "executed spotlight base generates all four tiers");
#ifdef _WIN32
        VirtualFree(code, 0, MEM_RELEASE);
#else
        munmap(code, 4096);
#endif
    }
    std::memcpy(image.data() + spot_size, spot_read, sizeof spot_resolution);
    Check(image == spot_original, "only the four-byte spotlight load changed");
    Check(BbEngine::Graphics::InstallLocalShadows(image.data(), image.size()) &&
          BbEngine::Graphics::InstallSpotShadows(image.data(), image.size()),
          "point and spotlight upgrades install together");

    constexpr std::size_t point_radius = 0x4929c80, spot_radius = 0x4929c84;
    constexpr std::size_t point_load = 0x22b7011, spot_load = 0x22b7220, spot_fallback = 0x22b7298;
    constexpr std::uint8_t point_code[] = {0xc5, 0xfa, 0x10, 0x05, 0x67, 0x2c, 0x67, 0x02};
    constexpr std::uint8_t spot_code[] = {0xc5, 0xfa, 0x10, 0x05, 0x5c, 0x2a, 0x67, 0x02};
    constexpr std::uint8_t fallback_code[] = {0xc5, 0xfa, 0x10, 0x05, 0xe4, 0x29, 0x67, 0x02};
    image.resize(spot_radius + sizeof(float), 0xcc);
    std::memcpy(image.data() + point_load, point_code, sizeof point_code);
    std::memcpy(image.data() + spot_load, spot_code, sizeof spot_code);
    std::memcpy(image.data() + spot_fallback, fallback_code, sizeof fallback_code);
    const auto reset_radii = [&] {
        const float radius = 3.2f;
        std::memcpy(image.data() + point_radius, &radius, sizeof radius);
        std::memcpy(image.data() + spot_radius, &radius, sizeof radius);
    };
    const auto radius_at = [&](std::size_t at) {
        float radius = 0;
        std::memcpy(&radius, image.data() + at, sizeof radius);
        return radius;
    };
    reset_radii();
    Check(BbEngine::Graphics::InstallLocalShadowSoftness(image.data(), image.size(), false, false) &&
          radius_at(point_radius) == 3.2f && radius_at(spot_radius) == 3.2f,
          "stock allocation keeps stock softness for both light types");
    Check(!BbEngine::Graphics::InstallLocalShadowSoftness(nullptr, image.size(), true, true),
          "null filter images are refused");
    Check(!BbEngine::Graphics::InstallLocalShadowSoftness(image.data(), image.size() - 1, true, true) &&
          radius_at(point_radius) == 3.2f, "a truncated spotlight radius changes neither filter");
    for (const auto at : {point_load, spot_load, spot_fallback, point_radius, spot_radius}) {
        image[at] ^= 1;
        const auto altered = image[at];
        Check(!BbEngine::Graphics::InstallLocalShadowSoftness(image.data(), image.size(), true, true),
              "foreign radius constants and all three load sites are refused");
        Check(image[at] == altered, "filter refusal preserves foreign bytes");
        image[at] ^= 1;
        Check(radius_at(point_radius) == 3.2f && radius_at(spot_radius) == 3.2f,
              "filter guard failure leaves both radii unchanged");
    }
    Check(BbEngine::Graphics::InstallLocalShadowSoftness(image.data(), image.size(), true, false) &&
          radius_at(point_radius) == 6.4f && radius_at(spot_radius) == 3.2f,
          "point-only resolution upgrade compensates only point softness");
    reset_radii();
    Check(BbEngine::Graphics::InstallLocalShadowSoftness(image.data(), image.size(), false, true) &&
          radius_at(point_radius) == 3.2f && radius_at(spot_radius) == 6.4f,
          "spotlight-only resolution upgrade compensates only spotlight softness");
    reset_radii();
    Check(BbEngine::Graphics::InstallLocalShadowSoftness(image.data(), image.size(), true, true),
          "both upgraded light types accept softness compensation");
    for (unsigned tier_index = 0; tier_index < 4; ++tier_index) {
        Check(radius_at(point_radius) / float(2048u >> tier_index) == 0.25f * 3.2f / float(256u >> tier_index),
              "point filter UV footprint is one quarter of stock at every tier");
        Check(radius_at(spot_radius) / float(4096u >> tier_index) == 0.25f * 3.2f / float(512u >> tier_index),
              "spotlight filter UV footprint is one quarter of stock at every tier");
    }
    constexpr std::size_t range = 0xeac540, tail = 0xeac657;

    std::memcpy(image.data() + size_at, read, 5);
    std::memcpy(image.data() + spot_size, spot_read, 4);
    reset_radii();
    const auto local_original = image;
    for (int quality = 0; quality < 3; ++quality) {
        BbSettings::Get().graphics_local_shadow_resolution = quality;
        const bool point = BbEngine::Graphics::InstallLocalShadows(image.data(), image.size());
        const bool spot = BbEngine::Graphics::InstallSpotShadows(image.data(), image.size());
        Check(point == (quality != 0) && spot == (quality != 0),
              "local dropdown enables both upgrades only for High and Very high");
        Check(BbEngine::Graphics::InstallLocalShadowSoftness(image.data(), image.size(), point, spot),
              "selected local quality accepts matching softness");
        if (quality == 0) {
            Check(image == local_original, "Default keeps native allocation instructions and filters byte-for-byte");
        } else {
            const unsigned point_base = quality == 1 ? 1024 : 2048;
            const unsigned spot_base = point_base * 2;
            std::uint32_t immediate = 0;
            std::memcpy(&immediate, image.data() + size_at + 1, 4);
            Check(immediate == point_base && image[size_at] == 0xb8, "selected point base immediate");
            Check(image[spot_size] == 0x31 && image[spot_size + 1] == 0xc0 &&
                  image[spot_size + 2] == 0xb4 &&
                  unsigned(image[spot_size + 3]) * 256 == spot_base, "selected spotlight base instruction");
            for (unsigned i = 0; i < 4; ++i) {
                Check(radius_at(point_radius) / float(point_base >> i) == 0.25f * 3.2f / float(256u >> i),
                      "selected point tiers preserve quarter-stock normalized softness");
                Check(radius_at(spot_radius) / float(spot_base >> i) == 0.25f * 3.2f / float(512u >> i),
                      "selected spotlight tiers preserve quarter-stock normalized softness");
            }
        }
        std::memcpy(image.data() + size_at, read, 5);
        std::memcpy(image.data() + spot_size, spot_read, 4);
        reset_radii();
        Check(image == local_original, "local dropdown changes only sizes and successful light-type radii");
    }
    BbSettings::Get().graphics_local_shadow_resolution = 2;
    constexpr std::uint8_t range_entry[] = {
        0xc5, 0xf8, 0x28, 0x02, 0xc5, 0xf8, 0x29, 0x06, 0xc5, 0xf8, 0x28, 0x42, 0x10,
        0xc5, 0xf8, 0x29, 0x46, 0x10};
    constexpr std::uint8_t range_stores[] = {
        0xc5, 0xfa, 0x11, 0x46, 0x54, 0xc5, 0xf2, 0x5d, 0x42, 0x58, 0xc5, 0xfa, 0x11, 0x46, 0x58, 0xc3};
    std::memcpy(image.data() + range, range_entry, sizeof range_entry);
    std::memcpy(image.data() + tail, range_stores, sizeof range_stores);
    Check(BbEngine::Graphics::DirectionalShadowBlend() == 0, "blending is off before native overlap");
    Directional(true);
    Check(!BbEngine::Graphics::InstallDirectionalShadows(nullptr, image.size()), "null cascade image refused");
    Check(!BbEngine::Graphics::InstallDirectionalShadows(image.data(), tail + 14),
          "truncated cascade stores refused");
    for (const auto at : {range, tail, tail + 14}) {
        image[at] ^= 1;
        Check(!BbEngine::Graphics::InstallDirectionalShadows(image.data(), image.size()) &&
              BbEngine::Graphics::DirectionalShadowBlend() == 0, "foreign cascade code cannot enable blending");
        image[at] ^= 1;
    }
    Directional(false);
    Check(!BbEngine::Graphics::InstallDirectionalShadows(image.data(), image.size()),
          "overlap stays off unless an effect asks for it");
    Directional(true);
    Check(BbEngine::Graphics::InstallDirectionalShadows(image.data(), image.size()) &&
          BbEngine::Graphics::DirectionalShadowBlend() == 0.1f, "guarded overlap enables matching blend band");
    auto* tail_page = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(image.data() + tail) & ~std::uintptr_t(4095));
#ifdef _WIN32
    DWORD old_protection = 0;
    const bool executable = VirtualProtect(tail_page, 4096, PAGE_EXECUTE_READWRITE, &old_protection) != 0;
#else
    const bool executable = mprotect(tail_page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
    Check(executable, "cascade tail test is executable");
    if (executable) {
        alignas(16) std::array<std::uint8_t, 0x700> owner{};
        alignas(16) std::array<float, 36> camera{}, output{};
        const float split0 = 20, split1 = 40;
        std::memcpy(owner.data() + 0x6dc, &split0, 4);
        std::memcpy(owner.data() + 0x6e0, &split1, 4);
        camera[0x54 / 4] = 0.05f;
        camera[0x58 / 4] = 600;
        using Tail = void (__attribute__((sysv_abi)) *)(void*, void*, void*, void*, unsigned, float, float);
        const auto run = reinterpret_cast<Tail>(image.data() + tail);
        const std::array<float, 4> starts{0.05f, 20, 40, 150};
        const std::array<float, 4> expected_near{0.05f, 18, 38, 139};
        for (unsigned c = 0; c < 4; ++c) {
            run(owner.data(), output.data(), camera.data(), nullptr, c, starts[c], 800);
            Check(output[0x54 / 4] == expected_near[c], "executed hook expands exactly the shader blend interval");
            Check(output[0x58 / 4] == 600, "native far-camera clamp is preserved");
        }
        camera[0x54 / 4] = 19;
        run(owner.data(), output.data(), camera.data(), nullptr, 1, 20, 40);
        Check(output[0x54 / 4] == 19, "overlap cannot extend behind camera near plane");
#ifdef _WIN32
        DWORD ignored = 0;
        VirtualProtect(tail_page, 4096, old_protection, &ignored);
#else
        mprotect(tail_page, 4096, PROT_READ | PROT_WRITE);
#endif
    }
    constexpr std::size_t table = 0x48a9668, selection = 0xeac238;
    constexpr std::uint32_t native_sizes[] = {2048, 1024, 2048, 4096};
    constexpr std::uint8_t select_code[] = {
        0xb8, 0x00, 0x08, 0x00, 0x00, 0x48, 0x83, 0xf9, 0x03, 0x77, 0x0a,
        0x48, 0x8d, 0x05, 0x1e, 0xd4, 0x9f, 0x03, 0x8b, 0x04, 0x88};
    const auto reset_atlas = [&] {
        std::memcpy(image.data() + table, native_sizes, sizeof native_sizes);
        std::memcpy(image.data() + selection, select_code, sizeof select_code);
    };
    reset_atlas();
    Check(!BbEngine::Graphics::InstallDirectionalShadowResolution(nullptr, image.size()),
          "null atlas allocation image refused");
    Check(!BbEngine::Graphics::InstallDirectionalShadowResolution(image.data(), table + sizeof native_sizes - 1),
          "truncated atlas table refused");
    for (const auto at : {table, table + sizeof native_sizes - 1, selection, selection + sizeof select_code - 1}) {
        image[at] ^= 1;
        const auto before = image;
        Check(!BbEngine::Graphics::InstallDirectionalShadowResolution(image.data(), image.size()) &&
              image == before, "foreign atlas table and selection code refused without writes");
        image[at] ^= 1;
    }
    const auto atlas_original = image;
    alignas(4096) std::array<std::uint8_t, 4096> selection_test{};
#ifdef _WIN32
    DWORD selection_protection = 0;
    const bool selection_executable = VirtualProtect(selection_test.data(), selection_test.size(),
        PAGE_EXECUTE_READWRITE, &selection_protection) != 0;
#else
    const bool selection_executable = mprotect(selection_test.data(), selection_test.size(),
        PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
    Check(selection_executable, "atlas selection test is executable");
    reset_atlas();
    BbSettings::Get().graphics_shadow_resolution = 0;
    Check(!BbEngine::Graphics::InstallDirectionalShadowResolution(image.data(), image.size()) &&
          image == atlas_original, "the game's own atlas changes no bytes");
    for (int quality = 1; quality < 3; ++quality) {
        reset_atlas();
        BbSettings::Get().graphics_shadow_resolution = quality;
        Check(BbEngine::Graphics::InstallDirectionalShadowResolution(image.data(), image.size()),
              "atlas quality accepted");
        const std::uint32_t expected_atlas = 2048u << quality;
        std::array<std::uint32_t, 4> actual{};
        std::memcpy(actual.data(), image.data() + table, sizeof actual);
        for (auto dimension : actual) Check(dimension == expected_atlas, "all map quality entries use requested atlas");
        std::uint32_t fallback = 0;
        std::memcpy(&fallback, image.data() + selection + 1, sizeof fallback);
        Check(fallback == expected_atlas, "out-of-range native selector uses requested atlas too");
        if (selection_executable) {
            std::memcpy(selection_test.data(), image.data() + selection, sizeof select_code);
            selection_test[sizeof select_code] = 0xc3;
            std::memcpy(selection_test.data() + 256, actual.data(), sizeof actual);

            const std::int32_t displacement = 256 - 18;
            std::memcpy(selection_test.data() + 14, &displacement, sizeof displacement);
            using Select = unsigned (__attribute__((sysv_abi)) *)(void*, void*, void*, std::int64_t);
            const auto select = reinterpret_cast<Select>(selection_test.data());
            for (std::int64_t profile : {-1, 0, 1, 2, 3, 4})
                Check(select(nullptr, nullptr, nullptr, profile) == expected_atlas,
                      "executed native selection handles every map profile and fallback");
        }
        reset_atlas();
        Check(image == atlas_original, "atlas upgrade changes only resolution table and fallback immediate");
    }
#ifdef _WIN32
    DWORD selection_ignored = 0;
    if (selection_executable)
        VirtualProtect(selection_test.data(), selection_test.size(), selection_protection, &selection_ignored);
#else
    if (selection_executable)
        mprotect(selection_test.data(), selection_test.size(), PROT_READ | PROT_WRITE);
#endif
    if (failures) return 1;
    std::puts("Native local shadows: PASS (point and spotlight code guards, independent opt-outs, "
              "1024/2048 bases, four tiers, instruction execution, isolated writes, combined install, "
              "guarded per-type softness and quarter-stock filter footprints, "
              "directional cascade overlap and 2048/4096/8192 atlas selection)");
    return 0;
}
