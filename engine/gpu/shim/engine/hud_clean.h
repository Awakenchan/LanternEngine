// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The HUD's sheet (the 1024x512 one the key prompts are drawn into) made fit for filtering: the
// gaps between its sprites are grey, and filtered, the bars' glows showed them as a faint box. Used
// by bb-ui-4k pack (the 4K menus' art for the sheet, written as kArtFile) and by the texture cache
// (the game's own sheet, when there is no such art).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "engine/key_prompts.h"

namespace HudClean {

/// The 4K menus' art for the sheet as the game ships it (its own key prompts), and the cleaned
/// copy bb-ui-4k pack writes beside it.
constexpr std::uint64_t kArtKey = 0x2706953d47319687ull;
constexpr const char* kArtFile = "hud-4k.bbui";

/// The art's file name in the pack (WxH_size_key.bbui).
inline std::string ArtName() {
    char name[96];
    std::snprintf(name, sizeof name, "%ux%u_%zu_%016llx.bbui", unsigned(KeyPrompts::kHudWidth),
                  unsigned(KeyPrompts::kHudHeight), KeyPrompts::kHudBytes, static_cast<unsigned long long>(kArtKey));
    return name;
}
/// The game's sheet the art was made from (KeyPrompts::HudHashes): with its key prompts' blocks
/// zeroed, and each prompt's blocks. Elsewhere the drawn sheet must match it for the art to stand
/// in; a prompt that matches keeps the art's.
constexpr std::uint64_t kSheetMasked = 0x2a50d5bf658d44fbull;
constexpr std::uint64_t kSheetPrompts[3] = {0xf5681001af8a6d1full, 0xd5d7e8ebf85a63f6ull, 0x75f17fcc6aff2036ull};

/// Fills the transparent texels of the rectangle with the colour of the nearest drawn texel in it,
/// so filtering at a sprite's edge blends towards the sprite's own colour.
inline void Fill(std::vector<std::uint8_t>& rgba, int w, int x0, int y0, int rw, int rh) {
    std::vector<std::uint32_t> queue;
    std::vector<std::uint8_t> filled(std::size_t(rw) * rh);
    const auto at = [&](int x, int y) { return (std::size_t(y0 + y) * w + (x0 + x)) * 4; };
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            if (rgba[at(x, y) + 3]) {
                filled[std::size_t(y) * rw + x] = 1;
                queue.push_back(std::uint32_t(y) * rw + x);
            }
        }
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const int x = int(queue[head] % rw), y = int(queue[head] / rw);
        const int around[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& [nx, ny] : around) {
            if (nx < 0 || ny < 0 || nx >= rw || ny >= rh || filled[std::size_t(ny) * rw + nx]) {
                continue;
            }
            filled[std::size_t(ny) * rw + nx] = 1;
            std::memcpy(&rgba[at(nx, ny)], &rgba[at(x, y)], 3);
            queue.push_back(std::uint32_t(ny) * rw + nx);
        }
    }
}

/// Cleans the sheet, `scale` texels per texel of the game's sheet. Between the two glows the
/// sheet's grey is opaque (three columns): a gap as well. `upscaled`: the 4K art, whose grey there
/// varies more, and which has faint grey texels the upscale left at the sprites' edges.
inline void Clean(std::vector<std::uint8_t>& rgba, int w, int h, int scale, bool upscaled) {
    const auto grey = [upscaled](const std::uint8_t* t) {
        const int hi = std::max({t[0], t[1], t[2]}), lo = std::min({t[0], t[1], t[2]});
        return upscaled ? hi - lo <= 10 && lo >= 15 && hi <= 55 : hi - lo <= 2 && t[0] >= 33 && t[0] <= 39;
    };
    for (int y = 97 * scale; y < 113 * scale; ++y) {
        for (int x = 638 * scale; x < 641 * scale; ++x) {
            std::uint8_t* texel = &rgba[(std::size_t(y) * w + x) * 4];
            if (grey(texel)) {
                texel[3] = 0;
            }
        }
    }
    if (upscaled) {
        for (std::size_t i = 0; i < rgba.size(); i += 4) {
            if (rgba[i + 3] < 16 && grey(&rgba[i])) {
                rgba[i + 3] = 0;
            }
        }
    }
    Fill(rgba, w, 0, 0, w, h);
}

} // namespace HudClean
