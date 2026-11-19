// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace Vulkan::SubmitResult {

inline constexpr std::int32_t kSuccess = 0;
inline constexpr std::int32_t kOutOfHostMemory = -1;
inline constexpr std::int32_t kOutOfDeviceMemory = -2;
inline constexpr std::int32_t kDeviceLost = -4;
inline constexpr std::int32_t kTimeout = 2;

inline constexpr std::uint32_t kMaxReports = 1;
inline constexpr int kStopExitCode = 23;
inline constexpr std::uint32_t kMaxRetries = 1;
inline constexpr std::uint64_t kReliefWaitNs = 2'000'000'000;
inline constexpr std::uint32_t kMaxRetryLines = 8;
inline constexpr std::uint32_t kMaxReliefLines = 8;

enum class Site : std::uint8_t { QueueSubmit, QueueSubmitAsync, SparseBind };

enum class Outcome : std::uint8_t { Submitted, DeviceLost, OutOfHostMemory, OutOfDeviceMemory, Failed };

enum class Attempt : std::uint8_t { First, Retry };

enum class Relief : std::uint8_t { Relieved, NotRelieved, Stuck, DeviceLost };

struct RetryCounters {
    std::atomic<std::uint32_t> retries{0};
    std::atomic<std::uint32_t> recovered{0};
    std::atomic<std::uint32_t> relief_requests{0};
    std::atomic<std::uint32_t> retry_lines{0};
};

struct RetryRequest {
    bool enabled;
    Site site;
    Outcome outcome;
    std::uint32_t retries_done;
    bool has_fence;
};

template <class Relieve>
struct Recovery {
    bool enabled;
    bool has_fence;
    RetryCounters& counters;
    Relieve relieve;
};

#ifdef LANTERN_TEST_MUTANTS
enum class Mutant : int {
    None,
    OnlyDeviceLostFails,
    PositiveIsSuccess,
    DeviceMemoryAsHost,
    NoStopUnlessDeviceLost,
    NoReport,
    NoLimit,
    NoDeviceLostReport,
    SubmitBeforeOps,
    RetryWhenOff,
    RetrySparseBind,
    RetryAnyFailure,
    RetryUnbounded,
    RetryWithFence,
    RetryWithoutRelief,
    RetryRerunsOps,
    RetryNotCounted,
    ReliefDeviceLostNotRelieved,
    ReliefTimeoutNotStuck,
    ReliefWaitsOnTickZero,
    ReliefNotRequested,
    DeviceLostKeepsOom,
    StuckNotReported,
    RetryLinesUnlimited,
    Count,
};
inline Mutant mutant = Mutant::None;
#endif

inline const char* SiteName(Site site) {
    switch (site) {
    case Site::QueueSubmit:
        return "queue submit";
    case Site::QueueSubmitAsync:
        return "queue submit (recording thread)";
    case Site::SparseBind:
        return "sparse bind";
    }
    return "?";
}

inline const char* OutcomeName(Outcome outcome) {
    switch (outcome) {
    case Outcome::Submitted:
        return "submitted";
    case Outcome::DeviceLost:
        return "device lost";
    case Outcome::OutOfHostMemory:
        return "out of system memory";
    case Outcome::OutOfDeviceMemory:
        return "out of video memory";
    case Outcome::Failed:
        return "failed";
    }
    return "?";
}

inline Outcome Classify(std::int32_t code) {
#ifdef LANTERN_TEST_MUTANTS
    if (mutant == Mutant::OnlyDeviceLostFails && code != kDeviceLost) {
        return Outcome::Submitted;
    }
    if (mutant == Mutant::PositiveIsSuccess && code > kSuccess) {
        return Outcome::Submitted;
    }
    if (mutant == Mutant::DeviceMemoryAsHost && code == kOutOfDeviceMemory) {
        return Outcome::OutOfHostMemory;
    }
#endif
    switch (code) {
    case kSuccess:
        return Outcome::Submitted;
    case kDeviceLost:
        return Outcome::DeviceLost;
    case kOutOfHostMemory:
        return Outcome::OutOfHostMemory;
    case kOutOfDeviceMemory:
        return Outcome::OutOfDeviceMemory;
    default:
        return Outcome::Failed;
    }
}

template <class Sink>
[[nodiscard]] Outcome Handle(Site site, std::int32_t code, std::atomic<std::uint32_t>& reports,
                             Sink& sink) {
    const Outcome outcome = Classify(code);
    if (outcome == Outcome::Submitted) {
        return outcome;
    }
#ifdef LANTERN_TEST_MUTANTS
    if (mutant == Mutant::NoStopUnlessDeviceLost && outcome != Outcome::DeviceLost) {
        return outcome;
    }
#endif
    std::uint32_t cap = kMaxReports;
#ifdef LANTERN_TEST_MUTANTS
    if (mutant == Mutant::NoLimit) {
        cap = ~0u;
    }
#endif
    if (reports.fetch_add(1, std::memory_order_relaxed) < cap) {
#ifdef LANTERN_TEST_MUTANTS
        if (mutant == Mutant::NoDeviceLostReport) {
            sink.Report(site, outcome, code);
        } else if (mutant == Mutant::NoReport) {
            if (outcome == Outcome::DeviceLost) {
                sink.DeviceLost(site);
            }
        } else
#endif
        {
            if (outcome == Outcome::DeviceLost) {
                sink.DeviceLost(site);
            }
            sink.Report(site, outcome, code);
        }
    }
    sink.Stop(site, outcome);
    return outcome;
}

inline bool OutOfMemory(Outcome outcome) {
    return outcome == Outcome::OutOfHostMemory || outcome == Outcome::OutOfDeviceMemory;
}

[[nodiscard]] inline bool MayRetry(const RetryRequest& request) {
#ifdef LANTERN_TEST_MUTANTS
    if (mutant == Mutant::RetryWhenOff && !request.enabled && OutOfMemory(request.outcome) &&
        request.site != Site::SparseBind && request.retries_done < kMaxRetries) {
        return true;
    }
    if (mutant == Mutant::RetrySparseBind && request.site == Site::SparseBind && request.enabled &&
        OutOfMemory(request.outcome) && request.retries_done < kMaxRetries) {
        return true;
    }
    if (mutant == Mutant::RetryAnyFailure && request.enabled && request.site != Site::SparseBind &&
        request.outcome != Outcome::Submitted && request.retries_done < kMaxRetries &&
        !request.has_fence) {
        return true;
    }
    if (mutant == Mutant::RetryUnbounded && request.enabled && request.site != Site::SparseBind &&
        OutOfMemory(request.outcome) && !request.has_fence) {
        return true;
    }
    if (mutant == Mutant::RetryWithFence && request.enabled && request.site != Site::SparseBind &&
        OutOfMemory(request.outcome) && request.retries_done < kMaxRetries) {
        return true;
    }
#endif
    if (!request.enabled || request.site == Site::SparseBind) {
        return false;
    }
    if (!OutOfMemory(request.outcome) || request.has_fence) {
        return false;
    }
    return request.retries_done < kMaxRetries;
}

inline Relief ReliefOf(std::int32_t wait_code) {
#ifdef LANTERN_TEST_MUTANTS
    if (mutant == Mutant::ReliefDeviceLostNotRelieved && wait_code == kDeviceLost) {
        return Relief::NotRelieved;
    }
    if (mutant == Mutant::ReliefTimeoutNotStuck && wait_code == kTimeout) {
        return Relief::NotRelieved;
    }
#endif
    switch (wait_code) {
    case kSuccess:
        return Relief::Relieved;
    case kDeviceLost:
        return Relief::DeviceLost;
    case kTimeout:
        return Relief::Stuck;
    default:
        return Relief::NotRelieved;
    }
}

template <class Wait>
[[nodiscard]] Relief Relieve(std::uint64_t submitted, Wait&& wait, RetryCounters& counters) {
#ifdef LANTERN_TEST_MUTANTS
    if (mutant != Mutant::ReliefNotRequested)
#endif
    {
        counters.relief_requests.fetch_add(1, std::memory_order_relaxed);
    }
    bool nothing_submitted = submitted == 0;
#ifdef LANTERN_TEST_MUTANTS
    if (mutant == Mutant::ReliefWaitsOnTickZero) {
        nothing_submitted = false;
    }
#endif
    if (nothing_submitted) {
        return Relief::Relieved;
    }
    return ReliefOf(wait(submitted));
}

template <class Sink>
void RetryLine(Site site, Outcome first, Relief relief, std::int32_t code,
               const RetryCounters& counters, std::atomic<std::uint32_t>& lines, Sink& sink) {
    std::uint32_t cap = kMaxRetryLines;
#ifdef LANTERN_TEST_MUTANTS
    if (mutant == Mutant::RetryLinesUnlimited) {
        cap = ~0u;
    }
#endif
    if (lines.fetch_add(1, std::memory_order_relaxed) < cap) {
        sink.Retry(site, first, relief, code, counters);
    }
}

template <class Ops, class Submit, class Relieve, class Sink>
[[nodiscard]] Outcome SubmitAfter(Ops& before_submit, Site site, Submit&& submit,
                                  Recovery<Relieve>& recovery, std::atomic<std::uint32_t>& reports,
                                  Sink& sink) {
#ifdef LANTERN_TEST_MUTANTS
    if (mutant == Mutant::SubmitBeforeOps) {
        const std::int32_t code = submit(Attempt::First);
        for (auto& operation : before_submit) {
            operation();
        }
        return Handle(site, code, reports, sink);
    }
#endif
    for (auto& operation : before_submit) {
        operation();
    }
    std::int32_t code = submit(Attempt::First);
    std::uint32_t retries = 0;
    while (true) {
        const Outcome outcome = Classify(code);
        if (!MayRetry({recovery.enabled, site, outcome, retries, recovery.has_fence})) {
            break;
        }
        Relief relief = recovery.relieve();
#ifdef LANTERN_TEST_MUTANTS
        if (mutant == Mutant::RetryWithoutRelief) {
            relief = Relief::Relieved;
        }
        if (mutant == Mutant::DeviceLostKeepsOom && relief == Relief::DeviceLost) {
            relief = Relief::NotRelieved;
        }
#endif
        if (relief == Relief::DeviceLost) {
            code = kDeviceLost;
            break;
        }
        if (relief != Relief::Relieved) {
            bool report_stuck = relief == Relief::Stuck;
#ifdef LANTERN_TEST_MUTANTS
            if (mutant == Mutant::StuckNotReported) {
                report_stuck = false;
            }
#endif
            if (report_stuck) {
                sink.Stuck(site);
            }
            RetryLine(site, outcome, relief, code, recovery.counters,
                      recovery.counters.retry_lines, sink);
            break;
        }
        ++retries;
        recovery.counters.retries.fetch_add(1, std::memory_order_relaxed);
#ifdef LANTERN_TEST_MUTANTS
        if (mutant == Mutant::RetryRerunsOps) {
            for (auto& operation : before_submit) {
                operation();
            }
        }
#endif
        code = submit(Attempt::Retry);
        bool count_success = Classify(code) == Outcome::Submitted;
#ifdef LANTERN_TEST_MUTANTS
        if (mutant == Mutant::RetryNotCounted) {
            count_success = false;
        }
#endif
        if (count_success) {
            recovery.counters.recovered.fetch_add(1, std::memory_order_relaxed);
        }
        RetryLine(site, outcome, Relief::Relieved, code, recovery.counters,
                  recovery.counters.retry_lines, sink);
    }
    return Handle(site, code, reports, sink);
}

struct EngineSink {
    void DeviceLost(Site site);
    void Report(Site site, Outcome outcome, std::int32_t code);
    void Retry(Site site, Outcome first, Relief relief, std::int32_t code,
               const RetryCounters& counters);
    void Stuck(Site site);
    [[noreturn]] void Stop(Site site, Outcome outcome);
};

std::atomic<std::uint32_t>& EngineReports();
RetryCounters& EngineRetries();

}
