// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace LanternSandbox {

constexpr const char* kSwitch = "LANTERN_TEST_SANDBOX";
constexpr const char* kLoopbackSwitch = "LANTERN_TEST_SANDBOX_LOOPBACK";
constexpr unsigned kMostRefusalLines = 32;

enum class Target { Remote, Loopback };
enum class Verdict { Allow, Refuse };

struct Counters {
    std::atomic<unsigned> refused{0};
    std::atomic<unsigned> sockets{0};
    std::atomic<unsigned> recording_opens{0};
    std::atomic<unsigned> voice_refused{0};
    std::atomic<unsigned> rebinds{0};
    std::atomic<unsigned> lines{0};
};
inline Counters counters;

#ifdef LANTERN_SANDBOX_MUTANTS
enum class Mutant : int { None = 0, SkipGate = 1, LoopbackWithoutFlag = 2, AnyHostIsLoopback = 3, KeepAnyAddress = 4 };
inline std::atomic<int> mutant{0};
inline bool Is(Mutant m) { return mutant.load(std::memory_order_relaxed) == int(m); }
#endif

inline Verdict Decide(bool sandbox, bool loopback_allowed, Target target) {
#ifdef LANTERN_SANDBOX_MUTANTS
    if (Is(Mutant::SkipGate)) return Verdict::Allow;
    if (Is(Mutant::LoopbackWithoutFlag)) loopback_allowed = true;
#endif
    if (!sandbox) return Verdict::Allow;
    return target == Target::Loopback && loopback_allowed ? Verdict::Allow : Verdict::Refuse;
}

inline bool SwitchSet(const char* name) {
    const char* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

class Switches {
public:
    Switches() { Reload(); }
    void Reload() {
        const bool on = SwitchSet(kSwitch);
        loopback_.store(on && SwitchSet(kLoopbackSwitch), std::memory_order_relaxed);
        on_.store(on, std::memory_order_relaxed);
        if (on && !told_.exchange(true, std::memory_order_relaxed))
            std::printf("Sandbox: on (%s=1), online and voice off\n", kSwitch);
    }
    bool On() const { return on_.load(std::memory_order_relaxed); }
    bool Loopback() const { return loopback_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> on_{false}, loopback_{false}, told_{false};
};

inline Switches& TheSwitches() {
    static Switches switches;
    return switches;
}

inline void Reload() { TheSwitches().Reload(); }

inline bool On() { return TheSwitches().On(); }

inline Target ClassifyHost(std::string_view host) {
#ifdef LANTERN_SANDBOX_MUTANTS
    if (Is(Mutant::AnyHostIsLoopback)) return Target::Loopback;
#endif
    return host == "127.0.0.1" || host == "::1" || host == "[::1]" ? Target::Loopback : Target::Remote;
}

inline bool Allow(Target target, const char* site, std::string_view what) {
    const Switches& switches = TheSwitches();
    if (!switches.On()) return true;
    if (Decide(true, switches.Loopback(), target) == Verdict::Allow) return true;
    counters.refused.fetch_add(1, std::memory_order_relaxed);
    if (counters.lines.fetch_add(1, std::memory_order_relaxed) < kMostRefusalLines)
        std::printf("Sandbox: %s refused (%.*s): %s=1\n", site, int(what.size()), what.data(), kSwitch);
    return false;
}

inline bool AllowHost(std::string_view host, const char* site) { return Allow(ClassifyHost(host), site, host); }

inline std::string_view HostOfUrl(std::string_view url) {
    if (const auto scheme = url.find("://"); scheme != std::string_view::npos) url.remove_prefix(scheme + 3);
    url = url.substr(0, url.find_first_of("/?#"));
    if (const auto at = url.rfind('@'); at != std::string_view::npos) url.remove_prefix(at + 1);
    if (!url.empty() && url.front() == '[') return url.substr(0, url.find(']') + 1);
    return url.substr(0, url.rfind(':'));
}

inline bool AllowUrl(std::string_view url, const char* site) { return AllowHost(HostOfUrl(url), site); }

inline bool AllowLocalSocket(const char* site) { return Allow(Target::Loopback, site, "a socket with no destination yet"); }

inline void NoteSocket() { counters.sockets.fetch_add(1, std::memory_order_relaxed); }

inline bool AllowVoice() {
    if (!On()) return true;
    if (Decide(true, false, Target::Remote) == Verdict::Allow) return true;
    counters.voice_refused.fetch_add(1, std::memory_order_relaxed);
    return false;
}

}
