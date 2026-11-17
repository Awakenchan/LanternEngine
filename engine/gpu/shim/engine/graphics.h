// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::Graphics {
bool Install(std::uint8_t* image, std::size_t size);
bool InstallLocalShadows(std::uint8_t* image, std::size_t size);
bool InstallSpotShadows(std::uint8_t* image, std::size_t size);
bool InstallLocalShadowSoftness(std::uint8_t* image, std::size_t size, bool point, bool spot);
bool InstallDirectionalShadows(std::uint8_t* image, std::size_t size);
float DirectionalShadowBlend();
bool InstallDirectionalShadowResolution(std::uint8_t* image, std::size_t size);
bool Enabled();
std::uint64_t YebisRecords();
}
