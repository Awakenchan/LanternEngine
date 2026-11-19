// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gpu/shim/engine/batch.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<std::pair<std::string, std::string>> g_vars;

const char* Lookup(const char* name, void*) {
    for (const auto& [n, v] : g_vars) {
        if (n == name) return v.c_str();
    }
    return nullptr;
}

void Use(std::vector<std::pair<std::string, std::string>> vars) {
    g_vars = std::move(vars);
    Lantern::SwitchInput input;
    input.env = Lookup;
    Lantern::ReloadSwitches(input);
    Lantern::Steps::Start(Lantern::Switches::Get());
}

std::vector<std::string> g_logged;
void Sink(std::string_view line, void*) {
    g_logged.emplace_back(line);
}
}

int main() {
    using namespace BbEngine::Batch;
    Use({});
    Expect(Switch("LANTERN_VERTEX_MEMO") == Off, "off by default");
    Use({{"LANTERN_VERIFY", "1"}});
    Expect(Switch("LANTERN_PIPELINE_MEMO") == Compare, "LANTERN_VERIFY=1 compares every step");
    Expect(Switch("LANTERN_RING_CONSTANTS") == Compare, "LANTERN_VERIFY=1 now reaches steps outside Batch too");
    Use({{"LANTERN_BATCH", "1"}});
    Expect(Switch("LANTERN_GX_OWN_BUFFERS") == On, "LANTERN_BATCH=1 turns every step on");
    Use({{"LANTERN_VERIFY", "1"}, {"LANTERN_BATCH", "1"}});
    Expect(Switch("LANTERN_VERTEX_MEMO") == Compare, "verify wins over batch");
    Use({{"LANTERN_VERIFY", "1"}, {"LANTERN_VERTEX_MEMO", "0"}});
    Expect(Switch("LANTERN_VERTEX_MEMO") == Off, "a step's own =0 wins");
    Use({{"LANTERN_VERTEX_MEMO", "1"}});
    Expect(Switch("LANTERN_VERTEX_MEMO") == On, "a step's own =1 wins");
    Use({{"LANTERN_BATCH", "1"}, {"LANTERN_PIPELINE_MEMO", "2"}});
    Expect(Switch("LANTERN_PIPELINE_MEMO") == Compare, "a step's own =2 wins");
    Use({{"LANTERN_BATCH", "1"}, {"LANTERN_VERTEX_MEMO", ""}});
    Expect(Switch("LANTERN_VERTEX_MEMO") == On, "an empty own value falls back");
    Use({{"LANTERN_VERTEX_MEMO", "1"}, {"LANTERN_STEPS_OFF", "vertex-memo"}});
    Expect(Switch("LANTERN_VERTEX_MEMO") == Off, "LANTERN_STEPS_OFF reaches the Batch users");
    Expect(Switch("BB_NO_SUCH_SWITCH") == Off, "an unknown name is off");

    const char a1[] = "a", a2[] = "a";
    Step* a = Register(a1, Compare);
    Step* again = Register(a2, Compare);
    Step* b = Register("b", On);
    Expect(a && a == again && b && a != b, "steps are registered once, by their text");
    Note(a, true);
    Note(a, false);
    Note(a, true);
    Expect(a->checked.load() == 3 && a->differ.load() == 1, "checked and differ counts");
    Lantern::SetLogSinkForTest(Sink, nullptr);
    Report("test");
    Expect(a->shown_checked == 3 && a->shown_differ == 1, "a report moves the window");
    Expect(b->shown_checked == 0, "a step that does not compare is not reported");
    Expect(!g_logged.empty() && g_logged[0] == "Verify a (test): checked 3, differ 1; in all 3, differ 1",
           "the report line");

    std::vector<std::thread> threads;
    static const char* names[] = {"c", "d", "e", "f", "g", "h"};
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 1000; ++i) Register(names[(t + i) % 6], Off);
        });
    }
    for (auto& t : threads) t.join();
    Expect(g_count.load() == kMaxSteps, "concurrent registration fills each slot once");
    bool unique = true;
    for (int i = 0; i < kMaxSteps; ++i) {
        for (int j = i + 1; j < kMaxSteps; ++j) unique = unique && g_steps[i].name != g_steps[j].name;
    }
    Expect(unique, "no name is registered twice");
    g_logged.clear();
    Expect(Register("x", Off) == nullptr && Register("y", Off) == nullptr, "no step past the table");
    Expect(g_refused.load() == 2 && g_logged.size() == 1, "a full table is logged once and refusals counted");
    Lantern::SetLogSinkForTest(nullptr, nullptr);

    if (failures) {
        std::printf("batch-test: %d failed\n", failures);
        return 1;
    }
    std::printf("batch-test: ok\n");
    return 0;
}
