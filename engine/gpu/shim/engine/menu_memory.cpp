// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu_memory.h"
#include "engine_hooks.h"
#include <cstdio>
#include <cstring>
#include <span>

namespace BbEngine::MenuMemory {
namespace {
constexpr std::size_t kScaleformHeap = 0x1f5a465, kScaleformPages = 0x1f5a479;
constexpr std::size_t kMenuBudget = 0x4736d40 + 2 * 0x80 + 9 * 8;
constexpr std::uint8_t kMovEdx = 0xba;
constexpr std::uint32_t kGameHeap = 18u << 20, kGamePages = 34u << 20;
constexpr std::uint64_t kGameBudget = 82ull << 20;
constexpr std::uint32_t kMoreHeap = 32u << 20, kMorePages = 64u << 20;
constexpr std::uint64_t kMoreBudget = 96ull << 20;

template <typename T> T At(const std::uint8_t* image, std::size_t at) {
    T v;
    std::memcpy(&v, image + at, sizeof v);
    return v;
}
}

bool Compatible(const std::uint8_t* image, std::size_t size) {
    if (size < kMenuBudget + 8 || size < kScaleformPages + 5) return false;
    return image[kScaleformHeap] == kMovEdx && image[kScaleformPages] == kMovEdx &&
           At<std::uint32_t>(image, kScaleformHeap + 1) == kGameHeap &&
           At<std::uint32_t>(image, kScaleformPages + 1) == kGamePages && At<std::uint64_t>(image, kMenuBudget) == kGameBudget;
}

bool Install(std::uint8_t* image, std::size_t size) {
    if (!Compatible(image, size)) return false;
    const std::uint32_t heap = kGameHeap + kMoreHeap, pages = kGamePages + kMorePages;
    const std::uint64_t budget = kGameBudget + kMoreBudget;
    const CodePatch patches[] = {
        {image, kScaleformHeap + 1, AsCode(std::span(&kGameHeap, 1)), AsCode(std::span(&heap, 1))},
        {image, kScaleformPages + 1, AsCode(std::span(&kGamePages, 1)), AsCode(std::span(&pages, 1))},
        {image, kMenuBudget, AsCode(std::span(&kGameBudget, 1)), AsCode(std::span(&budget, 1))}};
    if (const WriteResult result = WriteCode(patches); result != WriteResult::Applied) {
        std::printf("Engine: Scaleform menu heaps left as the game has them: their code %s\n", Describe(result));
        return false;
    }
    std::printf("Engine: Scaleform menu heaps %u/%u MiB, MENU %llu MiB (native option pages)\n", heap >> 20,
                pages >> 20, static_cast<unsigned long long>(budget >> 20));
    return true;
}
}
