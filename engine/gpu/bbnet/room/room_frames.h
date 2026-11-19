// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

#include "../../shim/engine/insight_sync.h"

namespace BbRoom::Frames {

enum : std::uint8_t { kRequest = 1, kReply = 2, kEvent = 3, kDatagram = 4, kBatch = 5, kVitals = 6, kTravel = 7, kProgress = 8, kDungeon = 9, kChalices = 10, kPortrait = 11, kBundle = 12, kBring = 13, kVoice = 14, kBossLot = 15, kBossTrophy = 16, kPiece = 17, kPartyCall = 18, kTravelFlags = 19, kPing = 20, kPong = 21, kWarpAsk = 22, kWarpAnswer = 23, kLampAsk = 24, kLampAnswer = 25, kWorldFlags = 26, kWorldAsk = 27, kStageTravel = 28, kHostInsight = 29 };
static_assert(kHostInsight == BbEngine::InsightSync::kFrameKind);
constexpr std::size_t kVitalsFixed = 1 + 1 + 4 * 4 + 2 + 3 * 4 + 4;

}
