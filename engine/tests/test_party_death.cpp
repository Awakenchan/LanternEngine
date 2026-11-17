// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/party_death.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {
int failures = 0;
bool quiet = false;
void Check(bool ok, const char* what) {
    if (ok) return;
    if (!quiet) std::printf("FAIL: %s\n", what);
    ++failures;
}

struct FakeClock {
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<FakeClock, duration>;
};
}

float SpectateYaw(bool fixed, float stick, float rate, float dt, float seconds) {
    float yaw = 0.0f;
    const int frames = static_cast<int>(std::lround(seconds / dt));
    for (int i = 0; i < frames; ++i) {
        const std::uint32_t flag = fixed ? BbEngine::PartyDeath::SpectateCameraInput(1, true) : 1u;
        yaw += ((flag & 0xffu) ? stick : 0.0f) * rate * dt;
    }
    return yaw;
}

void TestSpectateCamera() {
    using BbEngine::PartyDeath::SpectateCameraInput;
    Check(SpectateCameraInput(1, true) == 0, "watching another hunter turns the camera's stick input off");
    Check(SpectateCameraInput(0, true) == 0, "input already off stays off while watching");
    Check(SpectateCameraInput(1, false) == 1, "the own camera keeps the game's input flag");
    Check(SpectateCameraInput(0, false) == 0, "the own camera keeps a menu's input off");
    Check(SpectateCameraInput(0x101, false) == 0x101, "the own camera leaves the whole slot as the game wrote it");

    constexpr float kStick = 0.5f, kRate = 2.0f, kSeconds = 10.0f;
    const float drift = kStick * kRate * kSeconds;
    for (const float dt : {1.0f / 60.0f, 1.0f / 30.0f}) {
        Check(std::fabs(SpectateYaw(false, kStick, kRate, dt, kSeconds) - drift) < 1e-3f,
              "before the fix a remote stick spins the view at the same speed at any frame rate");
        Check(SpectateYaw(true, kStick, kRate, dt, kSeconds) == 0.0f, "after the fix the view stays put");
    }
}

int RunSuite() {
    failures = 0;
    using namespace BbEngine::PartyDeath;
    using std::chrono::milliseconds;
    using std::chrono::seconds;

    {
        Situation fight;
        fight.in_party = true;
        fight.boss_up = true;
        fight.anyone_else_up = true;
        Check(Decide(fight) == Move::Stay, "host or guest in a boss fight spectates while anyone lives");

        Situation last = fight;
        last.anyone_else_up = false;
        Check(Decide(last) == Move::Stay, "the revive loop: the last hunter falling stands nobody up in the arena");
        last.all_down_held = true;
        Check(Decide(last) == Move::PartyWipe, "everyone staying down is a wipe");
        Situation told = fight;
        told.wipe_called = true;
        Check(Decide(told) == Move::PartyWipe, "the host's word wipes a guest even if late HP shows someone up");

        Situation won = fight;
        won.boss_up = false;
        Check(Decide(won) == Move::ToLamp, "the boss's death sends the dead back to the lamp");
        won.anyone_else_up = false;
        won.all_down_held = true;
        Check(Decide(won) == Move::ToLamp, "a boss dying as the last hunter falls is not a wipe");
    }
    {
        Situation out;
        out.in_party = true;
        out.anyone_else_up = true;
        Check(Decide(out) == Move::ToLamp, "outside a boss the fallen hunter alone goes back to the lamp");
        out.anyone_else_up = false;
        out.all_down_held = true;
        Check(Decide(out) == Move::ToLamp, "outside a boss everyone down is still one lamp each, no wipe");

        Situation left;
        Check(Decide(left) == Move::GameLoad, "out of a party a death is the game's own");
    }
    {
        Held<FakeClock> held;
        FakeClock::time_point t{};
        Check(!held.Update(true, t, seconds(2)), "the wipe needs the window");
        t += seconds(1);
        Check(!held.Update(true, t, seconds(2)), "one second is not enough");
        Check(!held.Update(false, t, seconds(2)), "late HP showing a hunter up clears it");
        t += milliseconds(100);
        Check(!held.Update(true, t, seconds(2)), "and the window starts again");
        t += seconds(2);
        Check(held.Update(true, t, seconds(2)), "held for the window it is a wipe");
    }
    {
        FightWatch<FakeClock> fight(milliseconds(1500));
        FakeClock::time_point t{};
        fight.Update(false, t);
        fight.Begin(t);
        Check(!fight.Seen() && !fight.Up(t) && !fight.Over(t), "no bar, no fight");
        fight.Update(true, t);
        Check(fight.Seen() && fight.Up(t), "a bar is a fight");
        t += seconds(1);
        Check(fight.Up(t), "a bar flickering at a phase change is still the fight");
        t += seconds(1);
        Check(!fight.Up(t) && fight.Over(t), "gone for good the fight is over");
        fight.Begin(t);
        Check(!fight.Seen(), "a death after the fight starts clean");
        fight.Update(true, t);
        t += seconds(1);
        fight.Begin(t);
        Check(fight.Seen(), "a death right at a flicker keeps the fight");
    }
    {
        Spectate view;
        Check(view.Update({}, false) == -1, "nobody alive, nobody to follow");
        Check(view.Update({{3, "c"}, {1, "a"}, {2, "b"}}, false) == 1, "starts on the first hunter");
        Check(view.Name() == "a" && view.Count() == 3, "names who is followed");
        Check(view.Update({{1, "a"}, {2, "b"}, {3, "c"}}, false) == 1, "stays without a press");
        Check(view.Update({{1, "a"}, {2, "b"}, {3, "c"}}, true) == 2, "a press takes the next");
        Check(view.Update({{1, "a"}, {2, "b"}, {3, "c"}}, true) == 3, "and the next");
        Check(view.Update({{1, "a"}, {2, "b"}, {3, "c"}}, true) == 1, "and wraps around");
        Check(view.Update({{1, "a"}}, true) == 1, "a press with one hunter alive stays on them");
        view.Update({{1, "a"}, {2, "b"}, {3, "c"}}, true);
        Check(view.Slot() == 2, "back on the second");
        Check(view.Update({{1, "a"}, {3, "c"}}, false) == 3, "the followed hunter dying moves to the next");
        Check(view.Name() == "c", "and the name follows");
        Check(view.Update({{1, "a"}}, false) == 1, "the followed hunter leaving wraps to who is left");
        Check(view.Update({}, false) == -1 && view.Name().empty(), "the last one falling clears it");
    }
    TestSpectateCamera();
    return failures;
}

int main() {
#ifdef LANTERN_PARTY_DEATH_MUTANTS
    int survivors = 0;
    quiet = true;
    for (int m = 0; m <= BbEngine::PartyDeath::kMutants; ++m) {
        BbEngine::PartyDeath::g_mutant = m;
        const int found = RunSuite();
        if (m == 0) {
            if (found) {
                std::printf("party death mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("party death mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("party death mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("party death mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) return 1;
    std::printf("party death: all passed\n");
    return 0;
#endif
}
