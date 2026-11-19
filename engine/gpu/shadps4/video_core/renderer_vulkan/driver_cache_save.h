// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

#ifdef LANTERN_DECISION_MUTANTS
#define DRIVER_CACHE_SAVE_MUTANT(id, wrong, right) (::Vulkan::DriverCacheSave::active_mutant == (id) ? (wrong) : (right))
#else
#define DRIVER_CACHE_SAVE_MUTANT(id, wrong, right) (right)
#endif

namespace Vulkan::DriverCacheSave {

#ifdef LANTERN_DECISION_MUTANTS
inline int active_mutant = 0;
inline constexpr int MutantCount = 16;
#endif

inline constexpr std::uint64_t NsPerSecond = 1'000'000'000;
inline constexpr std::uint64_t MinNewPipelines = 16;
inline constexpr std::uint64_t MinIntervalNs = 60 * NsPerSecond;
inline constexpr std::uint64_t CompileQuietNs = 5 * NsPerSecond;
inline constexpr std::uint64_t PollIntervalNs = 5 * NsPerSecond;
inline constexpr std::uint64_t MaxProcessId = 0xFFFFFFFFull;
inline constexpr std::string_view TemporaryMarker = ".tmp.";

enum class Verdict : std::uint8_t { Save, NothingNew, TooFew, TooSoon, Compiling, Closed };

struct Inputs {
    std::uint64_t now_ns = 0;
    std::uint64_t last_save_ns = 0;
    std::uint64_t last_new_pipeline_ns = 0;
    std::uint64_t new_pipelines = 0;
    std::uint64_t compiles_unfinished = 0;
    bool closed = false;
};

inline Verdict Decide(const Inputs& in) noexcept {
    if (DRIVER_CACHE_SAVE_MUTANT(1, false, in.closed)) {
        return Verdict::Closed;
    }
    if (DRIVER_CACHE_SAVE_MUTANT(2, false, in.new_pipelines == 0)) {
        return Verdict::NothingNew;
    }
    if (DRIVER_CACHE_SAVE_MUTANT(3, in.new_pipelines + 1 < MinNewPipelines,
                                 in.new_pipelines < MinNewPipelines)) {
        return Verdict::TooFew;
    }
    if (DRIVER_CACHE_SAVE_MUTANT(4, in.now_ns < in.last_save_ns + MinIntervalNs / 2,
                                 in.now_ns < in.last_save_ns + MinIntervalNs)) {
        return Verdict::TooSoon;
    }
    if (DRIVER_CACHE_SAVE_MUTANT(5, in.now_ns < in.last_new_pipeline_ns,
                                 in.now_ns < in.last_new_pipeline_ns + CompileQuietNs)) {
        return Verdict::Compiling;
    }
    if (DRIVER_CACHE_SAVE_MUTANT(11, false, in.compiles_unfinished != 0)) {
        return Verdict::Compiling;
    }
    return Verdict::Save;
}

inline bool WriteAllowed(bool closed, bool shutting_down) noexcept {
    return DRIVER_CACHE_SAVE_MUTANT(12, !closed, !closed && !shutting_down);
}

inline bool ExitSaveNeeded(bool saved_in_play, std::uint64_t new_since_save,
                           std::uint64_t compiles_unfinished) noexcept {
    if (!saved_in_play) {
        return true;
    }
    return DRIVER_CACHE_SAVE_MUTANT(13, new_since_save != 0, new_since_save != 0 || compiles_unfinished != 0);
}

enum class Step : std::uint8_t { Waited, Saved, Dropped, Failed, Stop };

template <class Host>
Step PlaySave(Host& host) {
    std::unique_lock lock{host.SaveMutex()};
    if (DRIVER_CACHE_SAVE_MUTANT(14, false, host.DeviceLost())) {
        return Step::Stop;
    }
    const Inputs inputs = host.Read();
    const Verdict verdict = Decide(inputs);
    if (verdict != Verdict::Save) {
        host.Skipped(verdict);
        return Step::Waited;
    }
    if (DRIVER_CACHE_SAVE_MUTANT(15, false, true)) {
        lock.unlock();
    }
    auto data = host.Snapshot();
    if (!lock.owns_lock()) {
        lock.lock();
    }
    if (DRIVER_CACHE_SAVE_MUTANT(14, false, host.DeviceLost())) {
        return Step::Stop;
    }
    if (DRIVER_CACHE_SAVE_MUTANT(16, false, !WriteAllowed(host.Closed(), host.ShuttingDown()))) {
        host.Dropped();
        return Step::Dropped;
    }
    const Step step = host.Write(data) ? Step::Saved : Step::Failed;
    host.Record(step, inputs);
    lock.unlock();
    host.Report(step, inputs, data);
    return step;
}

inline std::optional<std::uint64_t> ParseDecimal(std::string_view digits, std::uint64_t max) noexcept {
    if (digits.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const std::uint64_t digit = std::uint64_t(c - '0');
        if (DRIVER_CACHE_SAVE_MUTANT(6, false, value > (max - digit) / 10)) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

inline std::optional<std::uint64_t> TemporaryOwner(std::string_view file_name,
                                                   std::string_view target_name) noexcept {
    if (target_name.empty() ||
        DRIVER_CACHE_SAVE_MUTANT(7, false, !file_name.starts_with(target_name))) {
        return std::nullopt;
    }
    auto rest = file_name.substr(std::min(target_name.size(), file_name.size()));
    if (!rest.starts_with(TemporaryMarker)) {
        return std::nullopt;
    }
    rest.remove_prefix(TemporaryMarker.size());
    const auto dot = rest.find('.');
    if (dot == std::string_view::npos) {
        return DRIVER_CACHE_SAVE_MUTANT(8, ParseDecimal(rest, MaxProcessId),
                                        std::optional<std::uint64_t>{});
    }
    if (DRIVER_CACHE_SAVE_MUTANT(9, false,
                                 !ParseDecimal(rest.substr(dot + 1), ~std::uint64_t{0}).has_value())) {
        return std::nullopt;
    }
    return ParseDecimal(rest.substr(0, dot), MaxProcessId);
}

inline bool RemoveTemporary(std::uint64_t owner, std::uint64_t own_process, bool owner_alive) noexcept {
    return DRIVER_CACHE_SAVE_MUTANT(10, !owner_alive, owner != own_process && !owner_alive);
}

} // namespace Vulkan::DriverCacheSave

#undef DRIVER_CACHE_SAVE_MUTANT
