// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "room_status.h"

namespace BbRoom {

struct Vitals {
    int slot = 0;
    std::string name;
    std::int32_t hp = 0, hp_max = 0, stamina = 0, stamina_max = 0;
    std::uint16_t revives = 0;
    float pos[3] = {};
    std::uint32_t map = 0;
    std::int64_t age_ms = 0;
};

void SendVitals(const Vitals& own);
std::vector<Vitals> Party(int max_age_ms);
void SendTravel(std::uint32_t lamp, const std::vector<std::uint8_t>& world_flags = {});
bool TakeTravel(std::uint32_t& lamp, std::vector<std::uint8_t>* world_flags = nullptr);
void SendStageTravel(std::uint32_t map, std::int32_t point, const std::vector<std::uint8_t>& world_flags = {});
bool TakeStageTravel(std::uint32_t& map, std::int32_t& point, std::vector<std::uint8_t>* world_flags = nullptr);
void SendWorldFlags(const std::vector<std::uint8_t>& part, int target = 0);
std::vector<std::vector<std::uint8_t>> TakeWorldFlags();
void SendWorldAsk();
std::vector<int> TakeWorldAsks();
void SendProgress(const std::vector<std::uint32_t>& flags, int target = 0);
std::vector<std::uint32_t> TakeProgress();
void SendBossLot(std::int32_t lot, int target);
std::vector<std::int32_t> TakeBossLots();
void SendBossTrophy(std::int32_t trophy, int target);
std::vector<std::int32_t> TakeBossTrophies();
enum class PartyCall : std::uint8_t { Wipe = 1, Rejoin = 2 };
void SendPartyCall(PartyCall call, int target = 0);
std::vector<std::pair<int, PartyCall>> TakePartyCalls();

struct Dungeon {
    std::uint64_t channel = 0;
    std::array<std::uint32_t, 43> record{};
    std::vector<std::uint32_t> flags;
};
void PutDungeonForm(std::uint64_t channel, const std::vector<std::uint8_t>& form);
void SendDungeon(const Dungeon& d, int target = 0);
bool TakeDungeon(Dungeon& out);
void SendChalices(const std::vector<std::uint32_t>& goods);
bool TakeChalices(std::vector<std::uint32_t>& out);
int HostSlot();
bool SignsWaiting();
void SendPortrait(const std::vector<std::uint8_t>& packed);
std::vector<std::pair<int, std::vector<std::uint8_t>>> TakePortraits();

bool Active();
std::int64_t ConnectedForMs();
bool SponsoredRelay();
bool CurrentNotice(std::string& text);
bool CurrentAnnouncement(std::string& text, float& alpha);
std::vector<std::pair<int, std::string>> PartyMembers();
bool Kick(int slot);
std::string NetworkId();
std::string PlainName(const std::string& network_id);
bool LeaveParty();
void Shutdown();
bool IsDevSlot(int slot);
void SetOpenRoom(bool on);
bool OpenRoomAllowed();
bool OpenRoomListed();
bool IsVisitorSlot(int slot);
void TakeDownSign();
void NoteSentHome();
bool TakeSentHome();
void SendBring(int target);
bool TakeBring();
std::vector<PeerStatus> Connections();
std::vector<PartyLine> PartyLines();
bool CurrentReconnect(ReconnectNotice& out);
bool LinkUp();
void SendWarpAsk();
bool WarpWaiting();
void SetWarpRequests(bool on);
void SendLampAsk(std::uint32_t lamp);
bool PendingLampAsk(int& slot, std::string& who, std::uint32_t& lamp);
void AnswerLampAsk(bool yes);
void NoteBringFar();
void NoteTravelLost();
void NoteStayedWithParty();
void SetVoice(bool on);
void SendVoice(const std::vector<std::uint8_t>& packet);
std::vector<std::pair<int, std::vector<std::uint8_t>>> TakeVoice();
bool IsHost();
std::string PlayerName();
void Start(int game_p2p_port);
void SetGamePort(int port);
bool Call(const std::string& path, const nlohmann::json& body, nlohmann::json& reply, std::string& error,
          int timeout_ms);

} // namespace BbRoom
