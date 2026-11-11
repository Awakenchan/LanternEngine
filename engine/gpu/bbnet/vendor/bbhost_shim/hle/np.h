// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
std::string hle_np_npid_text(const void* npid);
void hle_register_np_signaling();
void hle_np_signaling_peer_known(std::uint16_t member_id);
void hle_np_signaling_reset();
void hle_np_signaling_peer_dead(const std::string& online_id);
void hle_np_signaling_control(const std::uint8_t* data, std::size_t len, std::uint32_t addr, std::uint16_t port);
bool hle_np_signaling_live(const std::string& online_id);
void hle_np_matching2_peer_closed(const std::string& online_id);
void hle_np_signaling_m2_connect(const std::string& online_id);
int hle_np_signaling_transport_status(const std::string& online_id);
void hle_np_signaling_m2_stop(const std::string& online_id);
void hle_np_matching2_transport_up(const std::string& online_id);
void hle_np_matching2_transport_dead(const std::string& online_id, int error);
