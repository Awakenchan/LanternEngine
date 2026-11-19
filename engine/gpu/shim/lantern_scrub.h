// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <string_view>

namespace Lantern::Scrub {

inline constexpr int kMaxSecrets = 32;
inline constexpr std::size_t kSecretBytes = 256;

enum class Kind : unsigned char { Home, User, OneDrive, Account, Room, Secret, Server };

bool AddSecret(std::string_view value, Kind kind) noexcept;
void ClearSecretsForTest() noexcept;
int SecretCount() noexcept;

std::size_t Text(std::string_view in, char* out, std::size_t size) noexcept;

}
