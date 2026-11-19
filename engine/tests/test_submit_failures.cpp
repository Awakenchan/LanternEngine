// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/vk_submit_result.h"

namespace {

using namespace Vulkan::SubmitResult;

int failures = 0;
bool quiet = false;

void Expect(bool ok, const std::string& what) {
    if (!ok) {
        if (!quiet) {
            std::printf("FAIL: %s\n", what.c_str());
        }
        ++failures;
    }
}

struct Stopped {
    Site site;
    Outcome outcome;
};

struct FakeSink {
    int device_lost = 0;
    int reports = 0;
    int stops = 0;
    Site last_site = Site::QueueSubmit;
    Outcome last_outcome = Outcome::Submitted;
    std::int32_t last_code = 0;
    std::vector<std::string> events;

    void DeviceLost(Site site) {
        ++device_lost;
        last_site = site;
        events.push_back("device lost");
    }
    void Report(Site site, Outcome outcome, std::int32_t code) {
        ++reports;
        last_site = site;
        last_outcome = outcome;
        last_code = code;
        events.push_back("report");
    }
    int retry_lines = 0;
    int stuck = 0;
    Relief last_relief = Relief::NotRelieved;
    std::int32_t last_retry_code = 0;
    void Retry(Site site, Outcome first, Relief relief, std::int32_t code, const RetryCounters&) {
        ++retry_lines;
        last_site = site;
        last_outcome = first;
        last_relief = relief;
        last_retry_code = code;
        events.push_back("retry");
    }
    void Stuck(Site site) {
        ++stuck;
        last_site = site;
        events.push_back("stuck");
    }
    void Stop(Site site, Outcome outcome) {
        ++stops;
        events.push_back("stop");
        throw Stopped{site, outcome};
    }
};

struct Fault {
    std::int32_t code;
    Outcome outcome;
};

constexpr std::int32_t kInitializationFailed = -3;
constexpr std::int32_t kUnknown = -13;
constexpr std::int32_t kNotReady = 1;
constexpr std::int32_t kIncomplete = 5;

const Fault kFaults[] = {
    {kOutOfHostMemory, Outcome::OutOfHostMemory},
    {kOutOfDeviceMemory, Outcome::OutOfDeviceMemory},
    {kDeviceLost, Outcome::DeviceLost},
    {kInitializationFailed, Outcome::Failed},
    {kUnknown, Outcome::Failed},
    {kNotReady, Outcome::Failed},
    {kIncomplete, Outcome::Failed},
};

const Site kSites[] = {Site::QueueSubmit, Site::QueueSubmitAsync, Site::SparseBind};

struct FaultPlan {
    std::int32_t bind = kSuccess;
    std::int32_t submit = kSuccess;
};

struct FakeGpu {
    std::uint64_t memory_signalled = 0;
    std::uint64_t submitted_tick = 0;
    std::vector<std::uint64_t> waits;
    std::vector<std::string> order;
};

struct SubmitRun {
    FakeSink sink;
    FakeGpu gpu;
    bool stopped = false;
    Outcome outcome = Outcome::Submitted;
};

SubmitRun RunSubmission(const FaultPlan& plan, Site submit_site, std::atomic<std::uint32_t>& reports) {
    SubmitRun run;
    const std::uint64_t bind_tick = 1;
    std::vector<std::function<void()>> before_submit;
    before_submit.push_back([&] {
        run.gpu.order.push_back("bind");
        if (Handle(Site::SparseBind, plan.bind, reports, run.sink) != Outcome::Submitted) {
            return;
        }
        run.gpu.memory_signalled = bind_tick;
    });
    RetryCounters counters;
    Recovery recovery{false, false, counters, [] { return Relief::Relieved; }};
    try {
        run.outcome = SubmitAfter(
            before_submit, submit_site,
            [&](Attempt) {
                run.gpu.order.push_back("submit");
                if (plan.submit == kSuccess) {
                    run.gpu.waits.push_back(bind_tick);
                }
                return plan.submit;
            },
            recovery, reports, run.sink);
        if (run.outcome == Outcome::Submitted) {
            run.gpu.submitted_tick = 1;
        }
    } catch (const Stopped& stopped) {
        run.stopped = true;
        run.outcome = stopped.outcome;
    }
    return run;
}

bool EveryWaitSignalled(const FakeGpu& gpu) {
    for (const std::uint64_t wait : gpu.waits) {
        if (wait > gpu.memory_signalled) {
            return false;
        }
    }
    return true;
}

void CheckClassify() {
    Expect(Classify(kSuccess) == Outcome::Submitted, "success is submitted");
    for (const Fault& fault : kFaults) {
        Expect(Classify(fault.code) == fault.outcome,
               "code " + std::to_string(fault.code) + " is " + OutcomeName(fault.outcome));
    }
}

void CheckHandle() {
    for (const Site site : kSites) {
        std::atomic<std::uint32_t> reports{0};
        FakeSink sink;
        Expect(Handle(site, kSuccess, reports, sink) == Outcome::Submitted,
               std::string{SiteName(site)} + ": success passes");
        Expect(sink.events.empty() && reports.load() == 0,
               std::string{SiteName(site)} + ": success reports nothing");
        for (const Fault& fault : kFaults) {
            std::atomic<std::uint32_t> fresh{0};
            FakeSink failing;
            bool stopped = false;
            Outcome seen = Outcome::Submitted;
            try {
                (void)Handle(site, fault.code, fresh, failing);
            } catch (const Stopped& stop) {
                stopped = true;
                seen = stop.outcome;
                Expect(stop.site == site, "the stop names its site");
            }
            const std::string what =
                std::string{SiteName(site)} + " code " + std::to_string(fault.code);
            Expect(stopped && failing.stops == 1, what + ": stops once");
            Expect(seen == fault.outcome, what + ": stops with its outcome");
            Expect(failing.reports == 1, what + ": one report");
            Expect(failing.last_code == fault.code && failing.last_site == site &&
                       failing.last_outcome == fault.outcome,
                   what + ": the report carries the code, the site and the outcome");
            Expect(failing.device_lost == (fault.outcome == Outcome::DeviceLost ? 1 : 0),
                   what + ": the device-lost report only for device lost");
            Expect(!failing.events.empty() && failing.events.back() == "stop",
                   what + ": the report comes before the stop");
        }
    }
}

void CheckOnce() {
    std::atomic<std::uint32_t> reports{0};
    FakeSink sink;
    for (int i = 0; i < 3; ++i) {
        try {
            (void)Handle(Site::QueueSubmitAsync, kOutOfDeviceMemory, reports, sink);
        } catch (const Stopped&) {
        }
    }
    Expect(sink.reports == 1, "three failures print one report");
    Expect(sink.stops == 3, "every failure still stops");
}

void CheckSubmissions() {
    for (const Site site : {Site::QueueSubmit, Site::QueueSubmitAsync}) {
        const std::string name = SiteName(site);
        {
            std::atomic<std::uint32_t> reports{0};
            const SubmitRun run = RunSubmission({}, site, reports);
            Expect(!run.stopped && run.outcome == Outcome::Submitted, name + ": clean run submits");
            Expect(run.gpu.order == std::vector<std::string>{"bind", "submit"},
                   name + ": the binds go out before the submission");
            Expect(run.gpu.submitted_tick == 1 && EveryWaitSignalled(run.gpu),
                   name + ": clean run is published and its wait signalled");
        }
        for (const Fault& fault : kFaults) {
            const std::string what = name + ", bind code " + std::to_string(fault.code);
            std::atomic<std::uint32_t> reports{0};
            const SubmitRun run = RunSubmission({.bind = fault.code}, site, reports);
            Expect(run.stopped && run.sink.stops == 1, what + ": stops");
            Expect(run.sink.last_site == Site::SparseBind && run.sink.last_code == fault.code,
                   what + ": the report names the sparse bind");
            Expect(run.gpu.order == std::vector<std::string>{"bind"},
                   what + ": the submission never goes out");
            Expect(EveryWaitSignalled(run.gpu), what + ": no submission waits on an unsignalled bind");
            Expect(run.gpu.submitted_tick == 0, what + ": nothing is published");
        }
        for (const Fault& fault : kFaults) {
            const std::string what = name + ", submit code " + std::to_string(fault.code);
            std::atomic<std::uint32_t> reports{0};
            const SubmitRun run = RunSubmission({.submit = fault.code}, site, reports);
            Expect(run.stopped && run.sink.stops == 1, what + ": stops");
            Expect(run.sink.last_site == site && run.sink.last_code == fault.code,
                   what + ": the report names the submission");
            Expect(run.gpu.submitted_tick == 0, what + ": the tick is not published");
        }
    }
}

struct RetryPlan {
    std::vector<std::int32_t> codes;
    bool enabled = true;
    bool has_fence = false;
    Relief relief = Relief::Relieved;
};

struct FakeQueue {
    std::vector<std::int32_t> codes;
    std::vector<Attempt> attempts;
    int binary_waits = 1;
    int binary_waits_consumed = 0;
    int signals = 0;

    std::int32_t Submit(Attempt attempt) {
        const std::size_t index = attempts.size();
        attempts.push_back(attempt);
        const std::int32_t code = index < codes.size() ? codes[index] : kSuccess;
        if (code == kSuccess) {
            if (binary_waits_consumed >= binary_waits) {
                return kDeviceLost;
            }
            ++binary_waits_consumed;
            ++signals;
        }
        return code;
    }
};

struct RetryRun {
    FakeSink sink;
    FakeQueue queue;
    RetryCounters counters;
    int ops = 0;
    int reliefs = 0;
    bool stopped = false;
    Outcome outcome = Outcome::Submitted;
};

void RunRetry(const RetryPlan& plan, Site site, RetryRun& run, RetryCounters& counters) {
    run.queue.codes = plan.codes;
    std::atomic<std::uint32_t> reports{0};
    std::vector<std::function<void()>> before_submit;
    before_submit.push_back([&] { ++run.ops; });
    Recovery recovery{plan.enabled, plan.has_fence, counters, [&] {
                          ++run.reliefs;
                          return plan.relief;
                      }};
    try {
        run.outcome = SubmitAfter(
            before_submit, site, [&](Attempt attempt) { return run.queue.Submit(attempt); },
            recovery, reports, run.sink);
    } catch (const Stopped& stopped) {
        run.stopped = true;
        run.outcome = stopped.outcome;
    }
}

void RunRetry(const RetryPlan& plan, Site site, RetryRun& run) {
    RunRetry(plan, site, run, run.counters);
}

struct FakeWait {
    std::int32_t code = kSuccess;
    std::vector<std::uint64_t> ticks;
    std::int32_t operator()(std::uint64_t tick) {
        ticks.push_back(tick);
        return code;
    }
};

void CheckRelieve() {
    {
        RetryCounters counters;
        FakeWait wait;
        Expect(Relieve(0, wait, counters) == Relief::Relieved,
               "nothing submitted yet: relieved at once");
        Expect(wait.ticks.empty(), "nothing submitted yet: no wait");
        Expect(counters.relief_requests.load() == 1, "every relief asks the caches once");
    }
    struct WaitCase {
        std::int32_t code;
        Relief relief;
        const char* what;
    };
    const WaitCase cases[] = {
        {kSuccess, Relief::Relieved, "the wait finished"},
        {kTimeout, Relief::Stuck, "the wait timed out"},
        {kDeviceLost, Relief::DeviceLost, "the device was lost in the wait"},
        {kOutOfHostMemory, Relief::NotRelieved, "the wait ran out of memory"},
        {kOutOfDeviceMemory, Relief::NotRelieved, "the wait ran out of video memory"},
    };
    for (const WaitCase& c : cases) {
        RetryCounters counters;
        FakeWait wait{.code = c.code, .ticks = {}};
        Expect(Relieve(41, wait, counters) == c.relief, std::string{c.what} + ": its relief");
        Expect(wait.ticks == std::vector<std::uint64_t>{41},
               std::string{c.what} + ": waits once for the last submitted tick");
        Expect(counters.relief_requests.load() == 1, std::string{c.what} + ": one request");
    }
}

void CheckReliefOutcomes() {
    for (const Site site : {Site::QueueSubmit, Site::QueueSubmitAsync}) {
        const std::string name = SiteName(site);
        {
            RetryRun run;
            RunRetry({.codes = {kOutOfDeviceMemory, kSuccess}, .relief = Relief::DeviceLost}, site,
                     run);
            Expect(run.stopped && run.outcome == Outcome::DeviceLost,
                   name + ": device lost in the relief wait stops as device lost");
            Expect(run.sink.device_lost == 1 && run.sink.last_outcome == Outcome::DeviceLost &&
                       run.sink.last_code == kDeviceLost,
                   name + ": the device-lost report runs, not an out of memory one");
            Expect(run.queue.attempts.size() == 1 && run.sink.retry_lines == 0,
                   name + ": device lost in the relief: no retry, no retry line");
        }
        {
            RetryRun run;
            RunRetry({.codes = {kOutOfDeviceMemory, kSuccess}, .relief = Relief::Stuck}, site, run);
            Expect(run.stopped && run.sink.stuck == 1 && run.sink.device_lost == 0,
                   name + ": a stuck GPU in the relief is reported as stuck once");
            Expect(run.sink.last_code == kOutOfDeviceMemory && run.queue.attempts.size() == 1,
                   name + ": a stuck GPU: no retry, stop with the original code");
            Expect(run.sink.last_relief == Relief::Stuck, name + ": the line says stuck");
        }
        {
            RetryRun run;
            RunRetry({.codes = {kOutOfHostMemory, kSuccess}, .relief = Relief::NotRelieved}, site,
                     run);
            Expect(run.stopped && run.queue.attempts.size() == 1 && run.reliefs == 1,
                   name + ": a failed wait: no retry, stop");
            Expect(run.sink.retry_lines == 1 && run.sink.last_relief == Relief::NotRelieved &&
                       run.sink.stuck == 0 && run.counters.retries.load() == 0,
                   name + ": one line says it was not retried");
        }
    }
}

void CheckRetryLineCap() {
    RetryCounters counters;
    int lines = 0;
    const std::uint32_t runs = kMaxRetryLines + 3;
    for (std::uint32_t i = 0; i < runs; ++i) {
        RetryRun run;
        RunRetry({.codes = {kOutOfDeviceMemory, kSuccess}}, Site::QueueSubmitAsync, run, counters);
        lines += run.sink.retry_lines;
    }
    Expect(lines == static_cast<int>(kMaxRetryLines), "retry lines stop at their cap");
    Expect(counters.retries.load() == runs && counters.recovered.load() == runs &&
               counters.retry_lines.load() == runs,
           "retries keep being counted past the line cap");
}

void CheckMayRetry() {
    for (const Site site : kSites) {
        for (const Fault& fault : kFaults) {
            for (const bool enabled : {false, true}) {
                for (const bool fence : {false, true}) {
                    for (const std::uint32_t done : {0u, 1u, 2u}) {
                        const bool expected = enabled && site != Site::SparseBind && !fence &&
                                              done < kMaxRetries &&
                                              (fault.outcome == Outcome::OutOfHostMemory ||
                                               fault.outcome == Outcome::OutOfDeviceMemory);
                        Expect(MayRetry({enabled, site, fault.outcome, done, fence}) == expected,
                               std::string{SiteName(site)} + " " + OutcomeName(fault.outcome) +
                                   (enabled ? " on" : " off") + (fence ? " fence" : "") +
                                   " after " + std::to_string(done) + ": retry decision");
                    }
                }
            }
        }
        Expect(!MayRetry({true, site, Outcome::Submitted, 0, false}),
               std::string{SiteName(site)} + ": a submitted batch is not retried");
    }
}

void CheckRetry() {
    for (const Site site : {Site::QueueSubmit, Site::QueueSubmitAsync}) {
        for (const std::int32_t oom : {kOutOfHostMemory, kOutOfDeviceMemory}) {
            const std::string name = std::string{SiteName(site)} + " code " + std::to_string(oom);
            {
                RetryRun run;
                RunRetry({.codes = {oom, kSuccess}}, site, run);
                Expect(!run.stopped && run.outcome == Outcome::Submitted,
                       name + ": out of memory once, then submitted");
                Expect(run.queue.attempts == std::vector<Attempt>{Attempt::First, Attempt::Retry},
                       name + ": exactly one retry");
                Expect(run.ops == 1, name + ": the binds before the submission go out once");
                Expect(run.reliefs == 1, name + ": one relief before the retry");
                Expect(run.queue.binary_waits_consumed == 1 && run.queue.signals == 1,
                       name + ": the waits are consumed and the signal made once");
                Expect(run.counters.retries.load() == 1 && run.counters.recovered.load() == 1,
                       name + ": one retry and one recovery counted");
                Expect(run.sink.retry_lines == 1 && run.sink.last_relief == Relief::Relieved &&
                           run.sink.last_retry_code == kSuccess && run.sink.reports == 0,
                       name + ": one retry line and no failure report");
            }
            {
                RetryRun run;
                RunRetry({.codes = {oom, oom, kSuccess}}, site, run);
                Expect(run.stopped && run.sink.stops == 1, name + ": out of memory twice stops");
                Expect(run.queue.attempts.size() == 2, name + ": never a second retry");
                Expect(run.sink.reports == 1 && run.sink.last_code == oom,
                       name + ": the stop reports the code of the retry");
                Expect(run.counters.retries.load() == 1 && run.counters.recovered.load() == 0,
                       name + ": one retry, no recovery");
                Expect(run.queue.signals == 0, name + ": nothing signalled");
            }
            {
                RetryRun run;
                RunRetry({.codes = {oom, kSuccess}, .enabled = false}, site, run);
                Expect(run.stopped && run.queue.attempts.size() == 1 && run.reliefs == 0,
                       name + ": switch off stops at once as before");
                Expect(run.sink.retry_lines == 0 && run.counters.retries.load() == 0,
                       name + ": switch off logs and counts no retry");
            }
            {
                RetryRun run;
                RunRetry({.codes = {oom, kSuccess}, .has_fence = true}, site, run);
                Expect(run.stopped && run.queue.attempts.size() == 1 && run.reliefs == 0,
                       name + ": a submission with a fence is not retried");
            }
        }
        for (const Fault& fault : kFaults) {
            if (fault.outcome == Outcome::OutOfHostMemory ||
                fault.outcome == Outcome::OutOfDeviceMemory) {
                continue;
            }
            RetryRun run;
            RunRetry({.codes = {fault.code, kSuccess}}, site, run);
            Expect(run.stopped && run.queue.attempts.size() == 1 && run.reliefs == 0,
                   std::string{SiteName(site)} + " code " + std::to_string(fault.code) +
                       ": no retry without the memory guarantee");
        }
    }
}

int RunChecks() {
    failures = 0;
    CheckClassify();
    CheckHandle();
    CheckOnce();
    CheckSubmissions();
    CheckMayRetry();
    CheckRetry();
    CheckRelieve();
    CheckReliefOutcomes();
    CheckRetryLineCap();
    return failures;
}

#ifdef LANTERN_TEST_MUTANTS
const char* MutantName(Mutant m) {
    switch (m) {
    case Mutant::None:
        return "none";
    case Mutant::OnlyDeviceLostFails:
        return "only device lost fails";
    case Mutant::PositiveIsSuccess:
        return "positive codes pass";
    case Mutant::DeviceMemoryAsHost:
        return "video memory named system memory";
    case Mutant::NoStopUnlessDeviceLost:
        return "no stop unless device lost";
    case Mutant::NoReport:
        return "no report";
    case Mutant::NoLimit:
        return "no report limit";
    case Mutant::NoDeviceLostReport:
        return "no device-lost report";
    case Mutant::SubmitBeforeOps:
        return "submit before the binds";
    case Mutant::RetryWhenOff:
        return "retry with the switch off";
    case Mutant::RetrySparseBind:
        return "retry a sparse bind";
    case Mutant::RetryAnyFailure:
        return "retry any failure";
    case Mutant::RetryUnbounded:
        return "retry without a bound";
    case Mutant::RetryWithFence:
        return "retry a submission with a fence";
    case Mutant::RetryWithoutRelief:
        return "retry when the GPU did not go idle";
    case Mutant::RetryRerunsOps:
        return "retry runs the binds again";
    case Mutant::RetryNotCounted:
        return "recovery not counted";
    case Mutant::ReliefDeviceLostNotRelieved:
        return "device lost in the wait read as not relieved";
    case Mutant::ReliefTimeoutNotStuck:
        return "a wait timeout not read as stuck";
    case Mutant::ReliefWaitsOnTickZero:
        return "waits with nothing submitted";
    case Mutant::ReliefNotRequested:
        return "the caches not asked";
    case Mutant::DeviceLostKeepsOom:
        return "device lost in the relief stops as out of memory";
    case Mutant::StuckNotReported:
        return "a stuck GPU not reported";
    case Mutant::RetryLinesUnlimited:
        return "retry lines without a cap";
    case Mutant::Count:
        break;
    }
    return "?";
}
#endif

}

int main(int argc, char** argv) {
#ifdef LANTERN_TEST_MUTANTS
    if (argc > 1 && std::strcmp(argv[1], "--mutants") == 0) {
        quiet = true;
        int survived = 0;
        int caught = 0;
        for (int m = static_cast<int>(Mutant::None) + 1; m < static_cast<int>(Mutant::Count); ++m) {
            mutant = static_cast<Mutant>(m);
            const int found = RunChecks();
            std::printf("mutant %d (%s): %s, %d checks failed\n", m, MutantName(mutant),
                        found ? "caught" : "SURVIVED", found);
            ++(found ? caught : survived);
        }
        mutant = Mutant::None;
        quiet = false;
        const int base = RunChecks();
        std::printf("submit failures mutants: %d caught, %d survived; unmutated run %s\n", caught,
                    survived, base ? "FAILED" : "passed");
        return survived || base ? 1 : 0;
    }
#endif
    (void)argc;
    (void)argv;
    const int found = RunChecks();
    std::printf("submit failures: %s (%d failed)\n", found ? "FAILED" : "passed", found);
    return found ? 1 : 0;
}
