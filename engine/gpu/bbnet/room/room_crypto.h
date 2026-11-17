// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace BbRoom {

using Key = std::array<std::uint8_t, 32>;

struct Secret {
    std::string room_id;
    Key key{};
};

constexpr std::size_t kNonceLen = 12;
constexpr std::size_t kTagLen = 16;
constexpr std::size_t kSealOverhead = kNonceLen + 1 + kTagLen;

std::optional<Secret> Derive(const std::string& name, const std::string& password, int iterations = 200000);
std::vector<std::uint8_t> Seal(const Key& key, std::uint8_t sender, const std::uint8_t* data, std::size_t size);
std::optional<std::vector<std::uint8_t>> Open(const Key& key, std::uint8_t sender, const std::uint8_t* data,
                                              std::size_t size);
bool RandomBytes(std::span<std::uint8_t> out);
void WipeKey(Key& key);

#ifdef LANTERN_ROOM_RANDOM_SEAM
using RandomFill = int (*)(unsigned char* out, int size);
void SetRandomFill(RandomFill fill);
#endif

} // namespace BbRoom
