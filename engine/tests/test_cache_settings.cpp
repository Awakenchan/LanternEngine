// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/cache_settings.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace C = BbEngine::CacheSettings;
using u8 = std::uint8_t;

namespace {
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++g_failed;
    }
}

constexpr std::size_t kProfileBytes = 0x58;
constexpr u8 kOff = 0, kOn = 1, kCompare = 2;

std::vector<u8> Profile() {
    std::vector<u8> blob(kProfileBytes);
    for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = u8(i * 7);
    return blob;
}

std::vector<u8> Stamped(const C::Values& values) {
    auto blob = Profile();
    C::Append(blob, values);
    return blob;
}

std::vector<u8> WithField(const C::Values& values, std::size_t field, u8 value) {
    auto blob = Stamped(values);
    blob[kProfileBytes + 8 + field] = value;
    return blob;
}

C::Verdict VerdictOf(const std::vector<u8>& blob, const C::Values& now) {
    return C::Open(blob, kProfileBytes, now).verdict;
}

bool IsProfile(std::span<const u8> slice) {
    const auto profile = Profile();
    return slice.size() == kProfileBytes && std::memcmp(slice.data(), profile.data(), kProfileBytes) == 0;
}

void TestRoundTrip() {
    const C::Values now = C::Make(kOn, true, false, true);
    const auto blob = Stamped(now);
    Check(blob.size() == kProfileBytes + C::kRecordBytes, "the record follows the profile");
    C::Values stored;
    Check(C::Split(blob, kProfileBytes, stored) == C::Read::Valid && stored == now, "the record reads back");
    const auto opened = C::Open(blob, kProfileBytes, now);
    Check(opened.verdict == C::Verdict::Keep, "the same settings keep the cache");
    Check(IsProfile(opened.profile), "the profile slice is the profile without the record");
}

void TestWholeRebuilds() {
    const auto lifted = Stamped(C::Make(kOn, false, false, false));
    Check(VerdictOf(lifted, C::Make(kOff, false, false, false)) == C::Verdict::Rebuild,
          "LANTERN_LIFTER off after lifted shaders rebuilds");
    Check(VerdictOf(Stamped(C::Make(kOff, false, false, false)), C::Make(kOn, false, false, false)) == C::Verdict::Rebuild,
          "LANTERN_LIFTER on after recompiled shaders rebuilds");
    Check(VerdictOf(lifted, C::Make(kOn, true, false, false)) == C::Verdict::Rebuild,
          "BB_DIRECT_MEMORY_ACCESS changed rebuilds");
    Check(VerdictOf(Stamped(C::Make(kOff, true, false, false)), C::Make(kOff, false, false, false)) == C::Verdict::Rebuild,
          "BB_DIRECT_MEMORY_ACCESS changed rebuilds with the lifter off too");
    const auto opened = C::Open(lifted, kProfileBytes, C::Make(kOff, false, false, false));
    Check(opened.profile.empty(), "a rebuilt cache gives no profile");
    Check(C::FirstDifference(opened.stored, C::Make(kOff, false, false, false)) == C::Lifter,
          "the log names the setting that changed");
}

void TestEffectsWhileLifting() {
    const C::Values base = C::Make(kOn, false, false, false);
    const auto blob = Stamped(base);
    Check(VerdictOf(blob, C::Make(kOn, false, true, false)) == C::Verdict::Drop,
          "Modern SSR changed while lifting drops only the shaders it decides");
    Check(VerdictOf(blob, C::Make(kCompare, false, false, true)) == C::Verdict::Drop,
          "POM cavity changed while lifting in compare mode drops only the shaders it decides");
    const auto opened = C::Open(blob, kProfileBytes, C::Make(kOn, false, false, true));
    Check(IsProfile(opened.profile), "a cache that drops some shaders keeps the profile");
    Check(C::FirstDifference(opened.stored, C::Make(kOn, false, false, true)) == C::PomCavity,
          "the drop names the setting that changed");
}

void TestEquivalentSettings() {
    Check(VerdictOf(Stamped(C::Make(kOn, false, true, false)), C::Make(kCompare, false, true, false)) == C::Verdict::Adopt,
          "compare lifts as on does: kept, with the record rewritten");
    Check(VerdictOf(Stamped(C::Make(kCompare, false, false, false)), C::Make(kOn, false, false, false)) == C::Verdict::Adopt,
          "on after compare: kept, with the record rewritten");
    Check(VerdictOf(Stamped(C::Make(kOff, false, false, false)), C::Make(kOff, false, true, true)) == C::Verdict::Keep,
          "with the lifter off the effects store nothing: kept");
    Check(VerdictOf(WithField(C::Make(kOff, false, false, false), C::ModernSsr, 1), C::Make(kOff, false, false, false)) ==
              C::Verdict::Adopt,
          "a record with effects under the lifter off is kept and rewritten");
}

void TestExistingCachesKept() {
    for (const auto& now : {C::Make(kOn, false, false, false), C::Make(kOff, true, true, true), C::Make(kCompare, false, true, false)}) {
        const auto unstamped = Profile();
        const auto opened = C::Open(unstamped, kProfileBytes, now);
        Check(opened.read == C::Read::Absent && opened.verdict == C::Verdict::Adopt,
              "a cache from before the record is kept and stamped, whatever the settings");
        Check(IsProfile(opened.profile), "a cache from before the record gives its whole profile");
    }
}

void TestFresh() {
    const auto opened = C::Open({}, kProfileBytes, C::Make(kOn, false, false, false));
    Check(opened.read == C::Read::Empty && opened.verdict == C::Verdict::Fresh && opened.profile.empty(),
          "no profile starts a fresh cache");
}

void TestUnreadable() {
    const C::Values now = C::Make(kOn, false, false, false);
    auto blob = Stamped(now);
    blob[kProfileBytes] ^= 1;
    Check(VerdictOf(blob, now) == C::Verdict::Rebuild, "a damaged magic rebuilds");
    C::Values stored;
    Check(C::Split(WithField(now, C::Lifter, 3), kProfileBytes, stored) == C::Read::Unreadable,
          "a lifter value above compare is not a readable record");
    Check(C::Split(WithField(now, C::Lifter, kCompare), kProfileBytes, stored) == C::Read::Valid,
          "the lifter's compare value is readable");
    for (const auto field : {C::DirectMemory, C::ModernSsr, C::PomCavity}) {
        Check(C::Split(WithField(now, field, 2), kProfileBytes, stored) == C::Read::Unreadable,
              "a yes/no field above 1 is not a readable record");
    }
    Check(VerdictOf(WithField(now, C::DirectMemory, 2), now) == C::Verdict::Rebuild, "a value out of range rebuilds");
    blob = Stamped(now);
    blob.back() = 1;
    Check(VerdictOf(blob, now) == C::Verdict::Rebuild, "reserved bytes set rebuild");
    blob = Stamped(now);
    blob.pop_back();
    Check(C::Open(blob, kProfileBytes, now).read == C::Read::ForeignSize, "a short record is a profile of another size");
    Check(VerdictOf(blob, now) == C::Verdict::Rebuild, "a short record rebuilds");
    const std::vector<u8> odd(3);
    const auto foreign = C::Open(odd, kProfileBytes, now);
    Check(foreign.read == C::Read::ForeignSize && foreign.verdict == C::Verdict::Rebuild && foreign.profile.empty(),
          "a blob of another size rebuilds and says so");
}

void TestValues() {
    Check(C::LifterValue(false, false) == kOff && C::LifterValue(true, false) == kOn && C::LifterValue(false, true) == kCompare,
          "lifter off, on and compare are three values");
    const C::Values v = C::Make(kOn, true, true, false);
    Check(v.field[C::DirectMemory] == 1 && v.field[C::ModernSsr] == 1 && v.field[C::PomCavity] == 0, "each setting has its field");
    const C::Values off = C::Make(kOff, true, true, true);
    Check(off.field[C::ModernSsr] == 0 && off.field[C::PomCavity] == 0 && off.field[C::DirectMemory] == 1,
          "the lifter off stores the effects as 0");
}

void TestProfileOf() {
    Check(IsProfile(C::ProfileOf(Profile(), kProfileBytes).value_or(std::span<const u8>{})), "an unstamped profile is read");
    Check(IsProfile(C::ProfileOf(Stamped(C::Make(kOn, true, true, true)), kProfileBytes).value_or(std::span<const u8>{})),
          "a stamped profile is read without its record");
    Check(!C::ProfileOf(std::vector<u8>(kProfileBytes + 1), kProfileBytes), "a profile of another size is refused");
    Check(!C::ProfileOf({}, kProfileBytes), "no profile is refused");
}

void TestLabProfile() {
    const auto unstamped = Profile();
    const auto absent = C::OpenForLab(unstamped, kProfileBytes, true, kOn);
    Check(absent && !absent->recorded && IsProfile(absent->profile), "lab: a profile without the record is used");
    Check(absent && absent->compile == C::Make(kOn, true, false, false),
          "lab: without a record it compiles for the current lifter mode, with no effect kept recompiled");
    const C::Values lifted = C::Make(kOn, true, true, false);
    const auto stamped = Stamped(lifted);
    const auto valid = C::OpenForLab(stamped, kProfileBytes, true, kOff);
    Check(valid && valid->recorded && valid->stored == lifted && valid->compile == lifted && IsProfile(valid->profile),
          "lab: a valid record gives its settings and the profile, whatever the lab's own lifter mode");
    Check(valid && C::Lifts(valid->compile), "lab: a record of lifted shaders asks the lab to lift");
    Check(!C::OpenForLab(stamped, kProfileBytes, false, kOn), "lab: another direct memory access setting is refused");
    auto damaged = stamped;
    damaged[kProfileBytes + 4] ^= 1;
    Check(!C::OpenForLab(damaged, kProfileBytes, true, kOn), "lab: an unreadable record is refused");
    Check(!C::OpenForLab(std::vector<u8>(kProfileBytes + 2), kProfileBytes, true, kOn),
          "lab: a profile of another size is refused");
    for (const u8 mode : {kOff, kOn, kCompare}) {
        const auto lab = C::OpenForLab(unstamped, kProfileBytes, false, mode);
        if (!lab) {
            Check(false, "lab: a profile without the record is used");
            continue;
        }
        const auto relabelled = C::LabStamped(lab->profile, lab->compile);
        C::Values stored;
        Check(C::Split(relabelled, kProfileBytes, stored) == C::Read::Valid && stored == lab->compile &&
                  IsProfile(std::span<const u8>(relabelled).first(kProfileBytes)),
              "lab: an unrecorded cache it recompiled is stamped with what it compiled");
        for (const bool ssr : {false, true}) {
            for (const bool pom : {false, true}) {
                const auto verdict = C::Open(relabelled, kProfileBytes, C::Make(mode, false, ssr, pom)).verdict;
                Check(verdict != C::Verdict::Rebuild && verdict != C::Verdict::Fresh,
                      "no record, lab recompile, the engine in the same lifter mode: the cache is not rebuilt");
            }
        }
    }
}

int RunSuite() {
    g_failed = 0;
    TestRoundTrip();
    TestWholeRebuilds();
    TestEffectsWhileLifting();
    TestEquivalentSettings();
    TestExistingCachesKept();
    TestFresh();
    TestUnreadable();
    TestValues();
    TestProfileOf();
    TestLabProfile();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_CACHE_SETTINGS_MUTANTS
    int survivors = 0;
    for (int m = 0; m <= C::kMutants; ++m) {
        C::g_mutant = m;
        const int found = RunSuite();
        if (m == 0) {
            if (found) {
                std::printf("cache settings mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("cache settings mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("cache settings mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("cache settings mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("cache settings: %d failures\n", g_failed);
        return 1;
    }
    std::printf("cache settings: all tests passed\n");
    return 0;
#endif
}
