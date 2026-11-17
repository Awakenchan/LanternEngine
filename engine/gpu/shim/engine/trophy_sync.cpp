// SPDX-License-Identifier: GPL-2.0-or-later
#include "trophy_sync.h"
#include "engine_state.h"
#include "online_fixes.h"
#include "trophy_rules.h"
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace BbEngine::TrophySync {
namespace {
TrophyRules::Sync g_sync;
std::chrono::steady_clock::time_point g_next{};
}

void Tick() {
    if (g_sync.Done()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next) return;
    g_next = now + std::chrono::seconds(3);
    std::int32_t mode = -1;
    const bool have_mode = event_flag_mode(&mode);
    const char* role = TrophyRules::RoleName(OnlineFixes::PartyRole());
    g_sync.Read(player_record(), world_player_block(nullptr), have_mode, mode, event_flag_get_own,
                [role](int trophy) { std::printf("Runtime: trophy %d earned in this save as %s\n", trophy, role); });
}
}
