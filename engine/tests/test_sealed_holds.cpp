// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>

#include "gpu/shim/engine/gx_sealed.h"

namespace {
namespace Sealed = BbEngine::GxSealed;
int failures = 0;
bool quiet = false;

void Expect(bool ok, const char* what) {
    if (!ok) {
        if (!quiet) std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool Bindable(const Sealed::Registry& r, Sealed::u64 address, Sealed::u64 size) {
    Sealed::Hit hit;
    return r.Find(address, size, &hit) && Sealed::Holds(hit.base, hit.size, address, size);
}

int RunAll() {
    failures = 0;
    Expect(Sealed::Holds(0x1000, 0x100, 0x1000, 0x100), "the whole buffer");
    Expect(Sealed::Holds(0x1000, 0x100, 0x1080, 0x80), "a range ending at the last byte");
    Expect(Sealed::Holds(0x1000, 0x100, 0x1010, 0x10), "a range inside");
    Expect(!Sealed::Holds(0x1000, 0x100, 0x10f0, 0x20), "a range running past the end");
    Expect(!Sealed::Holds(0x1000, 0x100, 0x1000, 0x101), "one byte past the end");
    Expect(!Sealed::Holds(0x1000, 0x100, 0x0ff0, 0x20), "a range starting before");
    Expect(!Sealed::Holds(0x1000, 0x100, 0x1100, 0), "an empty range at the end");
    Expect(!Sealed::Holds(0x1000, 0x100, 0x1100, 0x10), "the byte after the buffer");

    Sealed::Registry r(16);
    r.Seal(0x10000, 0x1000);
    Expect(Bindable(r, 0x10000, 0x1000), "SealedLookup's decision binds the whole buffer");
    Expect(Bindable(r, 0x10f00, 0x100), "and a range ending at its end");
    Expect(!Bindable(r, 0x10f00, 0x200), "a V# running past the sealed end is refused, though Find finds its start");
    Expect(!Bindable(r, 0x11000, 0x10), "past the buffer");
    return failures;
}
}

int main(int argc, char** argv) {
    const bool mutants = argc > 1 && std::strcmp(argv[1], "--mutants") == 0;
    if (!mutants) {
        const int failed = RunAll();
        std::printf("sealed-holds-test: %s (%d failures)\n", failed ? "FAILED" : "passed", failed);
        return failed ? 1 : 0;
    }
#ifdef LANTERN_SEALED_MUTANTS
    quiet = true;
    int survivors = 0;
    for (int mutant = 1; mutant <= Sealed::kMutants; ++mutant) {
        Sealed::g_mutant = mutant;
        const int failed = RunAll();
        std::printf("mutant %d: %s (%d failures)\n", mutant, failed ? "caught" : "SURVIVED", failed);
        survivors += failed == 0;
    }
    Sealed::g_mutant = 0;
    quiet = false;
    const int failed = RunAll();
    std::printf("sealed-holds-mutants: %d survivors, shipped lines %s\n", survivors, failed ? "FAILED" : "passed");
    return survivors || failed ? 1 : 0;
#else
    std::printf("sealed-holds-test: --mutants needs a build with LANTERN_SEALED_MUTANTS\n");
    return 1;
#endif
}
