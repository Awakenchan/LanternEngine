// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace BbLooks {
inline constexpr int PresetCount = 5;
const char* PresetName(int preset);
const char* PresetNameRu(int preset);
void Apply(int preset);
bool GoBack();
bool HasPrevious();
bool NeedsRestart();
}
