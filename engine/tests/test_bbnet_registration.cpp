// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbnet_test_symbols.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

extern "C" void bbnet_register(void);

namespace {

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::string ReadAll(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    std::string out;
    for (const char c : text.str())
        if (c != '\r') out.push_back(c);
    return out;
}

}

int main(int argc, char** argv) {
    if (argc < 3 || (std::strcmp(argv[1], "thd") != 0 && std::strcmp(argv[1], "room") != 0)) {
        std::printf("usage: bbnet-registration-test thd|room <golden> [--record]\n");
        return 2;
    }
    const std::string mode = argv[1];
    const std::string golden = argv[2];
    const bool record = argc > 3 && std::strcmp(argv[3], "--record") == 0;
    SetEnvironment("BB_ONLINE", "1");
    SetEnvironment("BB_NET_BACKEND", mode.c_str());
    BbNetTest::ClearSymbols();
    bbnet_register();
    std::string text;
    for (const auto& r : BbNetTest::Symbols()) {
        if (!r.address) {
            std::printf("FAIL %s %s registered without an address\n", r.nid.c_str(), r.library.c_str());
            return 1;
        }
        text += r.nid + " " + r.library + " " + r.module + "\n";
    }
    const std::size_t count = BbNetTest::Symbols().size();
    if (record) {
        std::ofstream out(golden, std::ios::binary);
        out << text;
        if (!out) {
            std::printf("FAIL cannot write %s\n", golden.c_str());
            return 1;
        }
        std::printf("bbnet-registration-test %s: recorded %zu symbols\n", mode.c_str(), count);
        return 0;
    }
    if (ReadAll(golden) != text) {
        std::printf("FAIL the %s registration differs from %s (%zu symbols now)\n", mode.c_str(), golden.c_str(), count);
        return 1;
    }
    std::printf("bbnet-registration-test %s: ok (%zu symbols)\n", mode.c_str(), count);
    return 0;
}
