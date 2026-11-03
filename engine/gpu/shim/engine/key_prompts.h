// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace KeyPrompts {

constexpr std::size_t kIconBytes = 0x400, kHudBytes = 0x80000;
constexpr std::uint32_t kHudWidth = 1024, kHudHeight = 512;

int Identify(const void* data, std::size_t size, std::uint32_t width, std::uint32_t height);

bool Draw(int texture, std::uint8_t* data, std::size_t size);

/// The HUD's sheet (kHudBytes of BC7 in the game's tiling) as kHudWidth x kHudHeight RGBA, rows
/// top down.
void DecodeHud(const std::uint8_t* data, std::uint8_t* rgba);

struct HudRect {
    int x, y, w, h;
};
/// Where Draw puts the key prompts in the HUD's sheet, in its texels.
std::array<HudRect, 3> HudPrompts();
/// XXH3 of the HUD's sheet with the key prompts' blocks zeroed; `prompts`: of each prompt's blocks.
std::uint64_t HudHashes(const std::uint8_t* data, std::array<std::uint64_t, 3>& prompts);

bool Changed();

}
