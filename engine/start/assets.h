// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"

namespace start {

std::string VerifyExecutable(const fs::path& elf);
fs::path PrepareMirror(const fs::path& game, const fs::path& elf, const fs::path& cache);
fs::path PrepareMenuAssets(const fs::path& game, const fs::path& elf, const fs::path& cache, const fs::path& tool,
                           bool rebirth, const std::optional<std::string>& host_title = std::nullopt,
                           const std::optional<std::string>& guest_title = std::nullopt);
// Widescreen: the game's menu movies whose full-screen dims, fades and bands stop at the 16:9
// stage's edges, widened by `tool` (bb-engine-assets wide-movies, tools/engine/menu_assets.h
// WideMovie) for the UI viewport `width` x `height` (BB_UI_SIZE) into cache/wide-ui/WxH, and kept
// while the movies, the tool and the size stay the same. `option_movie`, when given, is the PC
// menus' optionsetting.gfx, widened in place of the game's. Each movie made, with what changed.
struct WideMovie {
    std::string relative;  // dvdroot_ps4/menu/<name>.gfx
    fs::path file;
    std::string note;
};
std::vector<WideMovie> PrepareWideMovies(const fs::path& game, const std::optional<fs::path>& option_movie,
                                         const fs::path& cache, const fs::path& tool, int width, int height);

std::vector<std::pair<std::string, fs::path>> PrepareEffectFixes(const fs::path& game, const fs::path& cache,
                                                                 std::vector<std::string>& notes);

std::optional<std::pair<std::string, fs::path>> PrepareJumpKey(const fs::path& game, const fs::path& cache,
                                                              std::string& note);

fs::path PrepareLamp2Lamp(const fs::path& game, const std::optional<fs::path>& update, const fs::path& patch,
                          const fs::path& cache);

int RunProgram(const fs::path& program, const std::vector<std::wstring>& arguments, std::string* output = nullptr,
               void* log = nullptr);
std::wstring CommandLine(const fs::path& program, const std::vector<std::wstring>& arguments);

}
