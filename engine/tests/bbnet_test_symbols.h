// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace BbNetTest {

struct Registered {
    std::string nid, library, module;
    std::uint64_t address;
};

const std::vector<Registered>& Symbols();
void ClearSymbols();
std::uint64_t Find(const std::string& nid);

}
