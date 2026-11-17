// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>
#include <string>

#include "gpu/shim/engine/utf8_wide.h"

namespace {
namespace U = BbEngine::Utf8;

int g_failures = 0;
bool g_quiet = false;

void Expect(bool ok, const char* what) {
    if (ok) return;
    ++g_failures;
    if (!g_quiet) std::printf("FAILED: %s\n", what);
}

int RunAll() {
    g_failures = 0;
    Expect(U::IsAscii("/home/user/Games/CUSA03173"), "an ASCII path reads as not ASCII");
    Expect(!U::IsAscii("/home/\xe8\xa1\x80/CUSA03173"), "a Chinese path reads as ASCII");
    Expect(!U::IsAscii("caf\xc3\xa9"), "a Latin-1 accent in UTF-8 reads as ASCII");
    Expect(!U::IsAscii("a\xa9"), "a stray continuation byte reads as ASCII");
    Expect(U::ToCodePoints("abc/1") == std::u32string(U"abc/1"), "ASCII text changed");
    Expect(U::ToCodePoints("\xe7\x8b\xac\xe7\xab\x8b") == std::u32string{0x72EC, 0x7ACB}, "two 3-byte characters decoded wrong");
    Expect(U::ToCodePoints("caf\xc3\xa9") == std::u32string{U'c', U'a', U'f', 0xE9}, "a 2-byte character decoded wrong");
    Expect(U::ToCodePoints("\xf0\x9f\x8e\xae") == std::u32string{0x1F3AE}, "a 4-byte character decoded wrong");
    Expect(U::ToCodePoints("a\xe8\xa1") == std::u32string{U'a', U::kReplacement}, "a truncated character is not one replacement");
    Expect(U::ToCodePoints("\xe8" "ab") == std::u32string{U::kReplacement, U'a', U'b'},
           "a lead byte followed by ASCII swallowed the ASCII");
    Expect(U::ToCodePoints("\xc0\xaf") == std::u32string{U::kReplacement, U::kReplacement}, "an overlong slash was accepted");
    Expect(U::ToCodePoints("\xed\xa0\x80") == std::u32string{U::kReplacement, U::kReplacement, U::kReplacement},
           "a surrogate was accepted");
    Expect(U::ToCodePoints("\xff" "a") == std::u32string{U::kReplacement, U'a'}, "an invalid byte did not become a replacement");
    return g_failures;
}
}

int main(int argc, char** argv) {
#ifdef LANTERN_UTF8_MUTANTS
    if (argc > 1 && !std::strcmp(argv[1], "--mutants")) {
        g_quiet = true;
        if (RunAll()) {
            std::printf("utf8-wide mutants: the unmutated run failed\n");
            return 1;
        }
        int survivors = 0;
        for (int m = 1; m < int(U::Mutant::kCount); ++m) {
            U::g_mutant = m;
            const int found = RunAll();
            std::printf("utf8-wide mutants: mutant %d %s (%d checks)\n", m, found ? "caught" : "survived", found);
            survivors += found ? 0 : 1;
        }
        U::g_mutant = 0;
        if (survivors) return 1;
        std::printf("utf8-wide mutants: every mutant caught (%d)\n", int(U::Mutant::kCount) - 1);
        return 0;
    }
    std::printf("utf8-wide-test: the mutant build runs with --mutants\n");
    return 1;
#else
    (void)argc;
    (void)argv;
    const int failures = RunAll();
    std::printf("utf8-wide-test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
#endif
}
