// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbnet_test_symbols.h"

#include "core/libraries/libs.h"

#include <cstdint>
#include <cstdlib>
#include <thread>

extern "C" {
int32_t runtime_thread_spawn(void** out, void* (*entry)(void*), void* argument, uint64_t, const char*) {
    using GuestEntry = void* (__attribute__((sysv_abi)) *)(void*);
    const auto run = reinterpret_cast<GuestEntry>(entry);
    std::thread([run, argument] { run(argument); }).detach();
    if (out) *out = nullptr;
    return 0;
}
[[noreturn]] void runtime_thread_exit(void*) { std::abort(); }
void runtime_blocked_report() {}
int sys_pad_type() { return 0; }
int sys_pad_button(std::uint32_t, int) { return 0; }
}

namespace BbNetTest {
namespace {
std::vector<Registered>& Table() {
    static std::vector<Registered> table;
    return table;
}
}

const std::vector<Registered>& Symbols() { return Table(); }
void ClearSymbols() { Table().clear(); }

std::uint64_t Find(const std::string& nid) {
    for (const Registered& r : Table())
        if (r.nid == nid) return r.address;
    return 0;
}

}

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType, u64 address) {
    BbNetTest::Table().push_back({nid, library, module, address});
}
}
