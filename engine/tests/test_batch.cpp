// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "gpu/shim/engine/batch.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
}

int main() {
    using namespace BbEngine::Batch;
    Expect(Resolve(nullptr, false, false) == Off, "off by default");
    Expect(Resolve(nullptr, true, false) == Compare, "LANTERN_VERIFY=1 compares every step");
    Expect(Resolve(nullptr, false, true) == On, "LANTERN_BATCH=1 turns every step on");
    Expect(Resolve(nullptr, true, true) == Compare, "verify wins over batch");
    Expect(Resolve("0", true, true) == Off, "a step's own =0 wins");
    Expect(Resolve("1", true, false) == On, "a step's own =1 wins");
    Expect(Resolve("2", false, true) == Compare, "a step's own =2 wins");
    Expect(Resolve("", false, true) == On, "an empty own value falls back");

    Step* a = Register("a", Compare);
    Step* again = Register("a", Compare);
    Step* b = Register("b", On);
    Expect(a && a == again && b && a != b, "steps are registered once");
    Note(a, true);
    Note(a, false);
    Note(a, true);
    Expect(a->checked.load() == 3 && a->differ.load() == 1, "checked and differ counts");
    Report("test");
    Expect(a->shown_checked == 3 && a->shown_differ == 1, "a report moves the window");
    Expect(b->shown_checked == 0, "a step that does not compare is not reported");
    for (int i = 0; i < kMaxSteps + 2; ++i) Register(i % 2 ? "x" : "y", Off);
    static const char* names[kMaxSteps + 2] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    int null_seen = 0;
    for (const char* n : names) null_seen += Register(n, Off) == nullptr;
    Expect(null_seen > 0, "no step past the table");

    if (failures) {
        std::printf("batch-test: %d failed\n", failures);
        return 1;
    }
    std::printf("batch-test: ok\n");
    return 0;
}
