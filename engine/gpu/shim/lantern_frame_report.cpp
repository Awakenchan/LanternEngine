// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#include "bbport_heap_sites.h"
#include "engine/const_census.h"
#include "engine/gx_objects.h"
#include "engine/gx_records.h"
#include "engine/gx_owed_check.h"
#include "engine/gx_whole_route.h"
#include "engine/ring_constants.h"
#include "engine/vertex_census.h"
#include "engine/pipe_census.h"
#include "engine/graphics.h"
#include "engine/batch.h"
#include "bbport_platform.h"
#include "bbport_toggles.h"
#include "bbport_wait_trace.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "lantern_frame_report.h"
#include "lantern_meter.h"
#include "lantern_pacing_stats.h"
#include "lantern_steps.h"
#include "lantern_switches.h"
#include "engine/loading.h"
#include "sys/video_out.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/pipeline_compile_queue.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

extern "C" void runtime_sleep_stats(uint64_t* calls, uint64_t* ns);
extern "C" void runtime_wait_report(double frames);
extern "C" void sys_rwlock_census(uint64_t* live, uint64_t* found, uint64_t* steps);
extern "C" void sys_sema_census(uint64_t* live, uint64_t* found, uint64_t* steps);
extern "C" void runtime_thread_census(uint64_t* made, uint64_t* finished, uint64_t* found, uint64_t* steps);
extern "C" uint64_t runtime_memory_regions(void);
extern "C" uint64_t runtime_heap_growths(void);
extern "C" uint64_t runtime_process_time_us(void);
extern "C" uint64_t runtime_tsc_frequency(void);

namespace Vulkan {
extern std::atomic<u64> g_bb_compile_ns, g_bb_compile_ns_total;
extern std::atomic<u32> g_bb_compiles, g_bb_compiles_total;
}

namespace AmdGpu {
void OcclusionDumpCounts(u64& dumps, u64& in_order);
}

namespace LanternFrameReport {

static void PrintGxWhole() {
    char line[4096];
    bool route_suspect = false;
    if (BbEngine::GxWholeRoute::Line(line, sizeof line, &route_suspect)) {
        std::printf("%s\n", line);
        if (route_suspect) {
            std::printf("GX whole stage A: INSTRUMENT SUSPECT\n");
        }
    }
    bool suspect = false;
    if (!BbEngine::GxRecords::WholeLine(line, sizeof line, &suspect)) {
        return;
    }
    std::printf("%s\n", line);
    if (suspect) {
        std::printf("GX whole: INSTRUMENT SUSPECT\n");
    }
}

static void PrintPipeCensus(u64 frames, double seconds, u64 stage_a_us) {
    namespace Census = BbEngine::PipeCensus;
    static Census::Window last;
    static u64 last_yebis = 0;
    const Census::Window w = Census::Take(last);
    const u64 yebis = BbEngine::Graphics::YebisRecords();
    Census::FrameInputs in;
    in.frames = frames;
    in.seconds = seconds;
    in.tsc_hz = double(runtime_tsc_frequency());
    in.stage_a_cpu_us = double(stage_a_us);
    in.yebis_records = yebis - last_yebis;
    in.commit = Census::BuildCommit();
    last_yebis = yebis;
    char line[2048];
    Census::FormatPipe(line, sizeof line, "5 s", w, in);
    std::printf("%s\n", line);
    Census::FormatPreparation(line, sizeof line, "5 s", w, in);
    std::printf("%s\n", line);
    char names[Census::kWaitSlots][Census::kWaitNameBytes];
    for (u32 slot = 0; slot < Census::kWaitSlots; ++slot) Census::WaitName(slot, names[slot], sizeof names[slot]);
    Census::FormatWaits(line, sizeof line, "5 s", w, in, names);
    std::printf("%s\n", line);
}

static void PrintMemory() {
#ifndef _WIN32
    u64 vram_kib = 0, gtt_kib = 0;
    std::vector<u64> clients;
    if (DIR* dir = opendir("/proc/self/fdinfo")) {
        while (const dirent* entry = readdir(dir)) {
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/self/fdinfo/%s", entry->d_name);
            FILE* file = std::fopen(path, "r");
            if (!file) {
                continue;
            }
            char line[160];
            bool drm = false;
            unsigned long long client = 0, vram = 0, gtt = 0;
            while (std::fgets(line, sizeof(line), file)) {
                drm |= std::strncmp(line, "drm-driver:", 11) == 0;
                std::sscanf(line, "drm-client-id: %llu", &client);
                std::sscanf(line, "drm-memory-vram: %llu", &vram);
                std::sscanf(line, "drm-memory-gtt: %llu", &gtt);
            }
            std::fclose(file);
            if (drm && std::find(clients.begin(), clients.end(), client) == clients.end()) {
                clients.push_back(client);
                vram_kib += vram;
                gtt_kib += gtt;
            }
        }
        closedir(dir);
    }
    unsigned long long size_pages = 0, rss_pages = 0;
    if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(statm, "%llu %llu", &size_pages, &rss_pages) != 2) {
            rss_pages = 0;
        }
        std::fclose(statm);
    }
    const u64 rss = u64(rss_pages) * u64(sysconf(_SC_PAGESIZE));
    const u64 device = BbStats::device_alloc_bytes.load() - BbStats::device_free_bytes.load();
    u64 vma_blocks = 0, vma_used = 0;
    Vulkan::VmaDeviceUsage(vma_blocks, vma_used);
    std::printf("Memory: VRAM %llu MiB, GTT %llu MiB, RSS %llu MiB; Vulkan memory %llu MiB "
                "allocated (VMA blocks %llu MiB, %llu MiB of them used); Vulkan images %llu MiB; "
                "cached images %llu MiB in %llu; guest blocks in VRAM %llu MiB allocated, "
                "%llu MiB of it unused (%llu MiB moved back idle, %llu MiB unmapped, %llu MiB freed; %llu chunks, %llu emptied, blocks kept: %llu GPU-written, %llu not direct memory); texture collector: usage %llu MiB, starts at %llu MiB, %llu images freed, %llu GPU-written kept\n",
                (unsigned long long)(vram_kib >> 10), (unsigned long long)(gtt_kib >> 10),
                (unsigned long long)(rss >> 20), (unsigned long long)(device >> 20),
                (unsigned long long)(vma_blocks >> 20), (unsigned long long)(vma_used >> 20),
                (unsigned long long)(BbStats::vk_image_bytes.load() >> 20),
                (unsigned long long)(BbStats::live_image_bytes.load() >> 20),
                (unsigned long long)BbStats::live_images.load(),
                (unsigned long long)(BbStats::residency_alloc_bytes.load() >> 20),
                (unsigned long long)(BbStats::residency_unused_bytes.load() >> 20),
                (unsigned long long)(BbStats::vram_idle_bytes.exchange(0) >> 20),
                (unsigned long long)(BbStats::vram_unmapped_bytes.exchange(0) >> 20),
                (unsigned long long)(BbStats::vram_chunks_freed_bytes.exchange(0) >> 20),
                (unsigned long long)BbStats::residency_chunk_count.load(),
                (unsigned long long)BbStats::evacuations.exchange(0),
                (unsigned long long)BbStats::evac_kept_gpu.exchange(0),
                (unsigned long long)BbStats::evac_kept_other.exchange(0),
                (unsigned long long)(BbStats::gc_used_bytes.load() >> 20),
                (unsigned long long)(BbStats::gc_trigger_bytes.load() >> 20),
                (unsigned long long)BbStats::gc_freed_images.exchange(0),
                (unsigned long long)BbStats::gc_kept_images.exchange(0));
#endif
}

static void LogRam() {
    BbPlatform::Memory m;
    if (!BbPlatform::GetMemory(m)) {
        return;
    }
    const auto mib = [](u64 bytes) { return static_cast<unsigned long long>(bytes >> 20); };
    std::printf("Memory: game %llu MiB in RAM (%llu MiB committed, peak %llu MiB); PC %llu of %llu "
                "MiB free, %llu MiB commit left; GPU images %llu MiB, %llu MiB cached in %llu\n",
                mib(m.working_set), mib(m.private_bytes), mib(m.peak_working_set), mib(m.free_phys),
                mib(m.total_phys), mib(m.commit_left), mib(BbStats::vk_image_bytes.load()),
                mib(BbStats::live_image_bytes.load()),
                static_cast<unsigned long long>(BbStats::live_images.load()));
    static int reports = 0;
    if (reports++ % 5 == 0) {
        std::thread([mib] {
            BbPlatform::MemoryBreakdown b;
            if (!BbPlatform::GetMemoryBreakdown(b)) return;
            std::printf("Memory by kind: game memory %llu MiB, other mapped %llu MiB, programs and DLLs %llu MiB, "
                        "engine and driver %llu MiB (shared %llu MiB)\n",
                        mib(b.game_pool), mib(b.mapped), mib(b.images), mib(b.priv), mib(b.shared));
            std::fflush(stdout);
        }).detach();
    }
    std::fflush(stdout);
}

static void LogLoad(double seconds, u64 flips, double worst_ms, u64 slow) {
    struct Snapshot {
        u64 draws, dispatches, cpu_us, faults, tracker_faults, protects, protect_pages;
        u64 rw_found, rw_steps, sema_found, sema_steps, thread_found, thread_steps;
        u64 zpass, zpass_in_order;
    };
    static Snapshot last{};
    Snapshot now{};
    now.draws = BbStats::draws.load(std::memory_order_relaxed);
    now.dispatches = BbStats::dispatches.load(std::memory_order_relaxed);
    if (BbPlatform::Usage usage; BbPlatform::GetUsage(false, usage)) {
        now.cpu_us = usage.user_us + usage.sys_us;
        now.faults = usage.minor_faults;
    }
    now.tracker_faults = BbStats::tracker_faults.load(std::memory_order_relaxed);
    now.protects = BbStats::protect_calls.load(std::memory_order_relaxed);
    now.protect_pages = BbStats::protect_pages.load(std::memory_order_relaxed);
    AmdGpu::OcclusionDumpCounts(now.zpass, now.zpass_in_order);
    u64 rwlocks = 0, semas = 0, made = 0, finished = 0;
    sys_rwlock_census(&rwlocks, &now.rw_found, &now.rw_steps);
    sys_sema_census(&semas, &now.sema_found, &now.sema_steps);
    runtime_thread_census(&made, &finished, &now.thread_found, &now.thread_steps);
    const bool first = last.draws == 0 && last.cpu_us == 0;
    const double frames = static_cast<double>(std::max<u64>(flips, 1));
    const double secs = std::max(seconds, 1.0);
    const auto delta = [](u64 a, u64 b) { return a >= b ? a - b : a; };
    const auto per_s = [&](u64 a, u64 b) { return first ? 0.0 : double(delta(a, b)) / secs; };
    const auto steps = [](u64 found, u64 walked) {
        return found ? double(walked) / double(found) : 0.0;
    };
    std::printf("Load: %.1f FPS (%llu over 33 ms, worst %.0f ms), %.0f draws and %.0f dispatches a "
                "frame; CPU %.1f cores, %.0f page faults/s; write faults %.0f/s, protects %.0f/s "
                "(%.0f pages/s), hot pages %lld; threads %llu live of %llu, rwlocks %llu (%.1f "
                "steps, %.0f/s), semaphores %llu (%.1f steps), memory map %llu regions, heap "
                "growths %llu; occlusion dumps %.1f a frame (%.1f in order)\n",
                double(flips) / secs, static_cast<unsigned long long>(slow), worst_ms,
                first ? 0.0 : double(delta(now.draws, last.draws)) / frames,
                first ? 0.0 : double(delta(now.dispatches, last.dispatches)) / frames,
                per_s(now.cpu_us, last.cpu_us) / 1e6, per_s(now.faults, last.faults),
                per_s(now.tracker_faults, last.tracker_faults), per_s(now.protects, last.protects),
                per_s(now.protect_pages, last.protect_pages),
                static_cast<long long>(BbStats::hot_pages.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(made - finished),
                static_cast<unsigned long long>(made), static_cast<unsigned long long>(rwlocks),
                steps(delta(now.rw_found, last.rw_found), delta(now.rw_steps, last.rw_steps)),
                per_s(now.rw_found, last.rw_found), static_cast<unsigned long long>(semas),
                steps(delta(now.sema_found, last.sema_found), delta(now.sema_steps, last.sema_steps)),
                static_cast<unsigned long long>(runtime_memory_regions()),
                static_cast<unsigned long long>(runtime_heap_growths()),
                first ? 0.0 : double(delta(now.zpass, last.zpass)) / frames,
                first ? 0.0 : double(delta(now.zpass_in_order, last.zpass_in_order)) / frames);
    last = now;
}

namespace {
struct StepsReport {
    bool printed_start = false;
    std::chrono::steady_clock::time_point first_flip{};
    std::chrono::steady_clock::time_point last_minute{};
    std::chrono::steady_clock::time_point last_flip{};
    char started[32] = {};
};
StepsReport g_steps_report;

void StepsOnFlip() {
    StepsReport& r = g_steps_report;
    const auto now = std::chrono::steady_clock::now();
    const u64 period_ns = r.printed_start ? u64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - r.last_flip).count()) : 0;
    r.last_flip = now;
    Lantern::Steps::OnFlip({u64(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()),
                            BbEngine::Loading::ScreenUp(), period_ns});
    if (!r.printed_start) {
        r.printed_start = true;
        r.first_flip = r.last_minute = now;
        const std::time_t wall = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &wall);
#else
        localtime_r(&wall, &local);
#endif
        std::strftime(r.started, sizeof r.started, "%Y-%m-%dT%H:%M:%S", &local);
        Lantern::Steps::PrintStepsAtStart();
    }
    Lantern::Steps::PrintNewFirstDifferences();
    if (now - r.last_minute >= std::chrono::seconds(60)) {
        const double seconds = std::chrono::duration<double>(now - r.last_minute).count();
        r.last_minute = now;
        Lantern::Steps::PrintMinuteTable(seconds, false);
        Lantern::Diag::PrintWarnings();
    }
}

void StepsOnExit() {
    StepsReport& r = g_steps_report;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = r.printed_start ? std::chrono::duration<double>(now - r.first_flip).count() : 0.0;
    Lantern::Steps::PrintNewFirstDifferences();
    Lantern::Steps::PrintMinuteTable(seconds, true);
    Lantern::Diag::PrintWarnings();
    Lantern::Diagnosis::Inputs in;
#ifdef _WIN32
    in.pid = long(_getpid());
    in.os = "Windows";
#else
    in.pid = long(getpid());
    in.os = "Linux";
#endif
    in.started = r.started[0] ? r.started : "before the first frame";
    in.engine_id = BbEngine::PipeCensus::BuildCommit();
    in.ended = "exit";
    in.minutes_tenths = u64(seconds / 6.0);
    in.frames = Lantern::g_frame.load(std::memory_order_relaxed);
    in.areas = Lantern::Steps::Area();
    in.fps_tenths = seconds > 0 ? u64(double(in.frames) * 10.0 / seconds) : 0;
    if (!Lantern::Diagnosis::WriteFile(in)) {
        Lantern::Log(Lantern::LogChannel::Always, "Diagnosis: not written at this exit");
    }
}
}

void OnFlip() {
    StepsOnFlip();
    {
        static auto next_ram = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        static auto load_start = std::chrono::steady_clock::now(), load_last = load_start;
        static u64 load_flips = 0, load_slow = 0;
        static double load_worst_ms = 0.0;
        const auto flip_time = std::chrono::steady_clock::now();
        Lantern::Meter::OnFlip(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(flip_time.time_since_epoch()).count()));
        const double flip_ms =
            std::chrono::duration<double, std::milli>(flip_time - load_last).count();
        load_last = flip_time;
        ++load_flips;
        load_slow += flip_ms > 33.0;
        load_worst_ms = std::max(load_worst_ms, flip_ms);
        if (const auto now = flip_time; now >= next_ram) {
            next_ram = now + std::chrono::seconds(60);
            LogRam();
            LogLoad(std::chrono::duration<double>(now - load_start).count(), load_flips,
                    load_worst_ms, load_slow);
            std::fflush(stdout);
            load_start = now;
            load_flips = load_slow = 0;
            load_worst_ms = 0.0;
        }
    }

    if (Lantern::Switches::Get().frame_stats) {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point window_start = Clock::now(), last = window_start;
        static u32 frames;
        static double worst_ms;
        const auto now = Clock::now();
        const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count();
        worst_ms = std::max(worst_ms, frame_ms);
        static std::vector<double> intervals;
        intervals.push_back(frame_ms);
        last = now;
        static u64 last_gpu_ns, last_images, last_image_bytes, last_buffer_bytes;
        static u64 last_t[6], last_minflt, last_sigf, last_pc, last_pp, last_rc, last_rp;
        const u64 pc = BbStats::protect_calls.load(), pp = BbStats::protect_pages.load(),
                  rc = BbStats::protect_revoke_calls.load(), rp = BbStats::protect_revoke_pages.load();
        const u64 minflt = BbStats::gpu_minor_faults.load(), sigf = BbStats::gpu_signal_faults.load();
        static u64 last_copy_cpu, last_copy_sys, last_copy_flt;
        const u64 copy_cpu = BbStats::t_copy_cpu.load(), copy_sys = BbStats::copy_sys_us.load(),
                  copy_flt = BbStats::copy_minflt.load();
        static u64 last_proc_flt, last_copy_ns, last_copy_bytes, last_rf, last_trf, last_twf;
        const u64 rf = BbStats::read_faults.load(), trf = BbStats::t_read_faults.load(),
                  twf = BbStats::t_write_faults.load();
        if (frame_ms > 40.0 && last_gpu_ns != 0) {
            std::printf("       fault handlers: %llu read faults %.1f ms, write faults %.1f ms\n",
                        static_cast<unsigned long long>(rf - last_rf), (trf - last_trf) / 1e6,
                        (twf - last_twf) / 1e6);
        }
        last_rf = rf;
        last_trf = trf;
        last_twf = twf;
        const u64 copy_ns = BbStats::t_copy.load(), copy_bytes = BbStats::copy_bytes.load();
        u64 proc_flt = 0;
        if (BbPlatform::Usage usage; BbPlatform::GetUsage(false, usage)) {
            proc_flt = usage.minor_faults;
        }
        const u64 t_now[6] = {BbStats::t_resident.load(), BbStats::t_protect.load(),
                              BbStats::t_image_create.load(), BbStats::t_refresh.load(),
                              BbStats::t_staging.load(), BbStats::t_host_wait.load()};
        static u64 last_draws, last_dispatches, last_subs, last_sys, last_user, last_invol, last_vol;
        const u64 draws = BbStats::draws.load(), dispatches = BbStats::dispatches.load(),
                  subs = BbStats::submissions.load(), sys_us = BbStats::gpu_sys_us.load(),
                  user_us = BbStats::gpu_user_us.load(), invol = BbStats::gpu_invol_switches.load(),
                  vol = BbStats::gpu_vol_switches.load();
        u64 gpu_ns = 0;
        if (const int clock = BbStats::gpu_thread_clock.load(); clock != -1) {
            gpu_ns = BbPlatform::ReadThreadCpuClockNs(clock);
        }
        const u64 images = BbStats::images_registered.load();
        const u64 image_bytes = BbStats::image_upload_bytes.load();
        const u64 buffer_bytes = BbStats::buffer_upload_bytes.load();
        static u64 last_compile_ns_total;
        static u32 last_compiles_total;
        const u64 compile_ns_total = Vulkan::g_bb_compile_ns_total.load();
        const u32 compiles_total = Vulkan::g_bb_compiles_total.load();
        if (frame_ms > 40.0 && last_gpu_ns != 0) {
            const double frame_compile_ms = (compile_ns_total - last_compile_ns_total) / 1e6;
            std::printf("Stall: %.1f ms frame (%s); GPU thread on CPU %.1f ms; %u pipelines "
                        "built, %.1f ms; %llu images registered, %.1f MB image uploads, %.1f MB "
                        "buffer uploads\n",
                        frame_ms,
                        frame_compile_ms >= frame_ms * 0.5 ? "pipelines built"
                        : frame_compile_ms > 0.0           ? "pipelines built, loading"
                                                           : "loading or other work",
                        (gpu_ns - last_gpu_ns) / 1e6, compiles_total - last_compiles_total,
                        frame_compile_ms, static_cast<unsigned long long>(images - last_images),
                        (image_bytes - last_image_bytes) / 1e6,
                        (buffer_bytes - last_buffer_bytes) / 1e6);
            std::printf("       %llu draws, %llu dispatches, %llu submissions; GPU thread user %.1f ms, "
                        "kernel %.1f ms, %llu preempted, %llu waits\n",
                        static_cast<unsigned long long>(draws - last_draws),
                        static_cast<unsigned long long>(dispatches - last_dispatches),
                        static_cast<unsigned long long>(subs - last_subs), (user_us - last_user) / 1e3,
                        (sys_us - last_sys) / 1e3, static_cast<unsigned long long>(invol - last_invol),
                        static_cast<unsigned long long>(vol - last_vol));
        }
        if (frame_ms > 40.0 && last_gpu_ns != 0) {
            std::printf("       ms in: resident %.1f, protect %.1f, image create %.1f, "
                        "image refresh %.1f, staging %.1f, waiting for host copies %.1f\n",
                        (t_now[0] - last_t[0]) / 1e6, (t_now[1] - last_t[1]) / 1e6,
                        (t_now[2] - last_t[2]) / 1e6, (t_now[3] - last_t[3]) / 1e6,
                        (t_now[4] - last_t[4]) / 1e6, (t_now[5] - last_t[5]) / 1e6);
            static u64 last_parts[5];
            const u64 parts[5] = {BbStats::t_image_query.load(), BbStats::t_image_alloc.load(),
                                  BbStats::t_refresh_hash.load(), BbStats::t_refresh_obtain.load(),
                                  BbStats::t_refresh_detile.load()};
            std::printf("       of which: format query %.1f, allocation %.1f; hashing %.1f, "
                        "source data %.1f, detiling %.1f\n",
                        (parts[0] - last_parts[0]) / 1e6, (parts[1] - last_parts[1]) / 1e6,
                        (parts[2] - last_parts[2]) / 1e6, (parts[3] - last_parts[3]) / 1e6,
                        (parts[4] - last_parts[4]) / 1e6);
            std::copy(std::begin(parts), std::end(parts), std::begin(last_parts));
            std::printf("       GPU thread page faults %llu (process %llu), protection faults %llu; "
                        "protect calls %llu (%llu pages), of which write-revoking %llu (%llu pages)\n",
                        static_cast<unsigned long long>(minflt - last_minflt),
                        static_cast<unsigned long long>(proc_flt - last_proc_flt),
                        static_cast<unsigned long long>(sigf - last_sigf),
                        static_cast<unsigned long long>(pc - last_pc),
                        static_cast<unsigned long long>(pp - last_pp),
                        static_cast<unsigned long long>(rc - last_rc),
                        static_cast<unsigned long long>(rp - last_rp));
        }
        last_pc = pc;
        last_pp = pp;
        last_rc = rc;
        last_rp = rp;
        last_minflt = minflt;
        if (frame_ms > 40.0 && last_gpu_ns != 0 && copy_ns > last_copy_ns) {
            std::printf("       guest copies %.1f MB in %.1f thread-ms, %.1f ms on CPU (kernel %.1f ms, "
                        "%llu page faults in large copies) (%.2f GB/s per thread)\n",
                        (copy_bytes - last_copy_bytes) / 1e6, (copy_ns - last_copy_ns) / 1e6,
                        (copy_cpu - last_copy_cpu) / 1e6, (copy_sys - last_copy_sys) / 1e3,
                        static_cast<unsigned long long>(copy_flt - last_copy_flt),
                        double(copy_bytes - last_copy_bytes) / double(copy_ns - last_copy_ns));
        }
        static FILE* const frame_log = [] () -> FILE* {
            const char* path = std::getenv("BB_FRAME_LOG");
            FILE* file = path && *path ? std::fopen(path, "w") : nullptr;
            if (file) {
                std::fprintf(file, "t_s,frame_ms,present_wait_ms,tick_wait_ms,stage_a_cpu_ms,"
                                   "images,image_mb,buffer_mb,protect_ms,resident_ms,create_ms,"
                                   "refresh_ms,write_fault_ms,read_faults,guest_copy_mb,draws,"
                                   "dispatches,submissions,compiles,preupload_mb,kernel_ms,alloc_mb,"
                                   "free_mb\n");
            }
            return file;
        }();
        if (frame_log) {
            static const auto log_start = now;
            static u64 log_twf, log_tick, log_present, log_compiles, log_rf, log_preupload;
            static u64 log_alloc, log_free;
            const u64 tick = BbStats::tick_wait_ns.load(), present = BbStats::present_wait_ns.load();
            const u64 compiles_now = Vulkan::g_bb_compiles.load();
            const auto delta = [](u64 now_value, u64& last_value) {
                const u64 d = now_value >= last_value ? now_value - last_value : now_value;
                last_value = now_value;
                return d;
            };
            std::fprintf(frame_log,
                         "%.4f,%.2f,%.2f,%.2f,%.2f,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%llu,"
                         "%.2f,%llu,%llu,%llu,%llu,%.2f,%.2f,%.1f,%.1f\n",
                         std::chrono::duration<double>(now - log_start).count(), frame_ms,
                         delta(present, log_present) / 1e6, delta(tick, log_tick) / 1e6,
                         last_gpu_ns ? (gpu_ns - last_gpu_ns) / 1e6 : 0.0,
                         static_cast<unsigned long long>(images - last_images),
                         (image_bytes - last_image_bytes) / 1e6,
                         (buffer_bytes - last_buffer_bytes) / 1e6, (t_now[1] - last_t[1]) / 1e6,
                         (t_now[0] - last_t[0]) / 1e6, (t_now[2] - last_t[2]) / 1e6,
                         (t_now[3] - last_t[3]) / 1e6, delta(twf, log_twf) / 1e6,
                         static_cast<unsigned long long>(delta(rf, log_rf)),
                         (copy_bytes - last_copy_bytes) / 1e6,
                         static_cast<unsigned long long>(draws - last_draws),
                         static_cast<unsigned long long>(dispatches - last_dispatches),
                         static_cast<unsigned long long>(subs - last_subs),
                         static_cast<unsigned long long>(delta(compiles_now, log_compiles)),
                         delta(BbStats::preupload_bytes.load(), log_preupload) / 1e6,
                         last_gpu_ns ? (sys_us - last_sys) / 1e3 : 0.0,
                         delta(BbStats::device_alloc_bytes.load(), log_alloc) / 1e6,
                         delta(BbStats::device_free_bytes.load(), log_free) / 1e6);
            static u32 unflushed = 0;
            if (++unflushed >= 120) {
                std::fflush(frame_log);
                unflushed = 0;
            }
        }
        last_copy_ns = copy_ns;
        last_copy_cpu = copy_cpu;
        last_copy_sys = copy_sys;
        last_copy_flt = copy_flt;
        last_copy_bytes = copy_bytes;
        last_proc_flt = proc_flt;
        last_sigf = sigf;
        std::copy(std::begin(t_now), std::end(t_now), std::begin(last_t));
        last_draws = draws;
        last_dispatches = dispatches;
        last_subs = subs;
        last_sys = sys_us;
        last_user = user_us;
        last_invol = invol;
        last_vol = vol;
        last_gpu_ns = gpu_ns;
        last_images = images;
        last_image_bytes = image_bytes;
        last_buffer_bytes = buffer_bytes;
        last_compile_ns_total = compile_ns_total;
        last_compiles_total = compiles_total;
        ++frames;
        const double window = std::chrono::duration<double>(now - window_start).count();
        if (window >= 5.0) {
            const u32 compiles = Vulkan::g_bb_compiles.exchange(0);
            const u64 compile_ns = Vulkan::g_bb_compile_ns.exchange(0);
            const u64 direct = Vulkan::Scheduler::direct_recordings.exchange(0);
            const u64 faults = BbStats::tracker_faults.exchange(0);
            static u64 window_draws = 0, window_gpu_us = 0;
            const u64 all_draws = BbStats::draws.load();
            const u64 gpu_us = BbStats::gpu_user_us.load() + BbStats::gpu_sys_us.load();
            const double us_per_draw = all_draws > window_draws
                                           ? double(gpu_us - window_gpu_us) / (all_draws - window_draws)
                                           : 0.0;
            const double draws_per_frame = frames ? double(all_draws - window_draws) / frames : 0.0;
            const u64 stage_a_us = gpu_us - window_gpu_us;
            const u64 ring_window_draws = window_draws;
            static u64 last_idle_ns = 0, last_tick_ns = 0, last_frame_wait_ns = 0;
            const auto taken = [](const std::atomic<u64>& total, u64& last) {
                const u64 now_total = total.load(std::memory_order_relaxed);
                const u64 d = now_total >= last ? now_total - last : 0;
                last = now_total;
                return d;
            };
            window_draws = all_draws;
            window_gpu_us = gpu_us;
            std::printf("Frame stats: %.1f FPS, worst frame %.1f ms (vblank %u Hz); "
                        "%u shader/pipeline compiles, %.1f ms; %llu recorder syncs; "
                        "%.0f write faults/s, %lld hot pages; GPU thread %.2f us/draw, "
                        "%.0f draws/frame, idle %.1f%%; blocked: recorder %.1f%%, host copies "
                        "%.1f%% (%.0f/frame, %.0f/frame not needed), copy threads %.1f%%, GPU ticks %.1f%%; "
                        "reduced-size draws %.0f/frame of %.0f in the scene\n",
                        frames / window, worst_ms, EmulatorSettings.GetVblankFrequency(), compiles,
                        compile_ns / 1e6, static_cast<unsigned long long>(direct),
                        faults / window, static_cast<long long>(BbStats::hot_pages.load()),
                        us_per_draw, draws_per_frame,
                        taken(BbStats::gpu_idle_ns, last_idle_ns) / (window * 1e7),
                        BbStats::sync_recording_ns.exchange(0) / (window * 1e7),
                        BbStats::host_copies_wait_ns.exchange(0) / (window * 1e7),
                        frames ? double(BbStats::host_copy_waits.exchange(0)) / frames : 0.0,
                        frames ? double(BbStats::host_copy_waits_skipped.exchange(0)) / frames : 0.0,
                        BbStats::copy_threads_wait_ns.exchange(0) / (window * 1e7),
                        taken(BbStats::tick_wait_ns, last_tick_ns) / (window * 1e7),
                        frames ? double(BbStats::reduced_draws.exchange(0)) / frames : 0.0,
                        frames ? double(BbStats::scene_draws.exchange(0)) / frames : 0.0);
            {
                const double per = frames ? 1.0 / frames : 0.0;
                const u64 sync_calls = BbStats::motion_sync_calls.exchange(0);
                const u64 sync_waits = BbStats::motion_sync_waits.exchange(0);
                const u64 sync_ns = BbStats::motion_sync_ns.exchange(0);
                const u64 sync_max = BbStats::motion_sync_max_ns.exchange(0);
                const u64 start_waits = BbStats::motion_frame_waits.exchange(0);
                const u64 start_ns = BbStats::motion_frame_wait_ns.exchange(0);
                const u64 start_max = BbStats::motion_frame_wait_max_ns.exchange(0);
                const u64 recorder_syncs = BbStats::sync_recording_calls.exchange(0);
                if (sync_calls || start_waits) {
                    std::printf("Object motion waits: submissions %.1f/frame, %.1f/frame waited for bb:Motion, "
                                "%.3f ms/frame, longest %.0f us; frame start waits for the GPU 4 frames back "
                                "%.2f/frame, %.3f ms/frame, longest %.0f us; recorder syncs %.1f/frame, direct "
                                "recordings %.1f/frame\n",
                                sync_calls * per, sync_waits * per, sync_ns * per / 1e6, sync_max / 1e3,
                                start_waits * per, start_ns * per / 1e6, start_max / 1e3,
                                recorder_syncs * per, direct * per);
                }
            }
            BbEngine::Batch::Report("5 s");
            if (BbEngine::VertexCensus::On()) {
                static BbEngine::VertexCensus::Window last;
                BbEngine::VertexCensus::Print("5 s", BbEngine::VertexCensus::Take(last));
                BbEngine::GxObjects::PrintSeals("so far");
            }
            if (BbEngine::PipeCensus::On()) {
                PrintPipeCensus(frames, window, stage_a_us);
            }
            PrintGxWhole();
            BbEngine::GxWhole::OwedCheck::Print(frames);
            {
                namespace Ring = BbEngine::RingConstants;
                static Ring::Counts last;
                const Ring::Counts w = Ring::Take(Ring::g_totals, last);
                const bool counting = Ring::g_totals.counting.load(std::memory_order_relaxed);
                Ring::FrameInputs in;
                in.frames = frames;
                in.draws = all_draws - ring_window_draws;
                in.stage_a_us = stage_a_us;
                std::uint64_t ring_base = 0, ring_bytes = 0;
                in.ring_known = BbEngine::GxObjects::UploadArena(&ring_base, &ring_bytes);
                char line[1024];
                Ring::Format(line, sizeof line, Ring::g_mode, counting, w, in);
                std::printf("%s\n", line);
                if (const char* why = Ring::Suspect(Ring::g_mode, counting, w)) {
                    std::printf("Ring constants: INSTRUMENT SUSPECT (%s)\n", why);
                }
                Ring::Difference first;
                if (Ring::TakeFirst(Ring::g_totals, first)) {
                    Ring::FormatDifference(line, sizeof line, first);
                    std::printf("%s\n", line);
                }
            }
            if (BbEngine::ConstCensus::g_on) {
                namespace Census = BbEngine::ConstCensus;
                static Census::Window last;
                const Census::Window w = Census::Take(last);
                const double f = frames ? double(frames) : 1.0;
                std::uint64_t all = 0, all_bytes = 0;
                for (int c = 0; c < Census::kClasses; ++c) {
                    all += w.copies[c];
                    all_bytes += w.bytes[c];
                }
                std::uint64_t ring_base = 0, ring_bytes = 0;
                const bool ring = BbEngine::GxObjects::UploadArena(&ring_base, &ring_bytes);
                std::printf("Constant copies: %.0f/frame, %.1f KiB/frame (%.0f/s, %.1f MiB/s)",
                            all / f, all_bytes / (f * 1024.0), all / window, all_bytes / (window * 1048576.0));
                for (int c = 0; c < Census::kClasses; ++c) {
                    std::printf("; %s %.0f/frame %.1f KiB/frame (%.1f%%)", Census::kClassNames[c], w.copies[c] / f,
                                w.bytes[c] / (f * 1024.0), all ? 100.0 * w.copies[c] / all : 0.0);
                }
                if (ring) {
                    std::printf("; upload ring %#llx, %llu MiB\n", static_cast<unsigned long long>(ring_base),
                                static_cast<unsigned long long>(ring_bytes >> 20));
                } else {
                    std::printf("; upload ring not seen\n");
                }
            }
            {
                const double f = frames ? double(frames) : 1.0;
                const auto per_frame = [&](std::atomic<u64>& counter) {
                    return double(counter.exchange(0)) / f;
                };
                const auto ms_per_frame = [&](std::atomic<u64>& ns) {
                    return double(ns.exchange(0)) / (f * 1e6);
                };
                std::printf("Stage B waits: fences");
                for (u32 s = 0; s < BbStats::WaitSites; ++s) {
                    const double n = per_frame(BbStats::signal_waits[s]);
                    const double ms = ms_per_frame(BbStats::signal_wait_ns[s]);
                    const double skipped = per_frame(BbStats::signal_waits_skipped[s]);
                    if (n + skipped > 0.0) {
                        std::printf(" %s %.1f/frame %.3f ms", BbStats::WaitSiteNames[s], n, ms);
                        if (skipped > 0.0) {
                            std::printf(" (%.1f/frame not needed)", skipped);
                        }
                    }
                }
                std::printf("; copies");
                for (u32 s = 0; s < BbStats::WaitSites; ++s) {
                    const double n = per_frame(BbStats::copy_waits[s]);
                    const double ms = ms_per_frame(BbStats::copy_wait_ns[s]);
                    if (n > 0.0) {
                        std::printf(" %s %.1f/frame %.3f ms", BbStats::WaitSiteNames[s], n, ms);
                    }
                }
                std::printf("; fence barriers %.1f/frame; DMA into memory stored by the CPU "
                            "%.1f/frame, recorded for the GPU %.1f/frame\n",
                            per_frame(BbStats::fence_barriers), per_frame(BbStats::dma_on_cpu),
                            per_frame(BbStats::dma_on_gpu));
                double stage_b_calls = 0.0, stage_b_ms = 0.0;
                std::array<double, BbStats::ProtectKinds> kind_calls{};
                for (u32 k = 0; k < BbStats::ProtectKinds; ++k) {
                    kind_calls[k] = per_frame(BbStats::stage_b_protect_calls[k]);
                    stage_b_calls += kind_calls[k];
                    stage_b_ms += ms_per_frame(BbStats::stage_b_protect_ns[k]);
                }
                const double other_calls = per_frame(BbStats::other_protect_calls);
                const double other_ms = ms_per_frame(BbStats::other_protect_ns);
                const double deferred_pages = per_frame(BbStats::deferred_protect_pages);
                const double deferred_calls = per_frame(BbStats::deferred_protect_calls);
                const double deferred_ms = ms_per_frame(BbStats::deferred_protect_ns);
                std::printf("Stage B protects: %.1f/frame %.3f ms (uploads %.1f, textures %.1f, "
                            "other %.1f); other threads %.1f/frame %.3f ms; deferred %.0f "
                            "pages/frame, applied in %.1f calls/frame %.3f ms (bb:Protect or "
                            "copies), copies that flushed %.1f/frame, cancelled %.0f pages/frame\n",
                            stage_b_calls, stage_b_ms, kind_calls[BbStats::ProtectUpload],
                            kind_calls[BbStats::ProtectTexture], kind_calls[BbStats::ProtectOther],
                            other_calls, other_ms, deferred_pages, deferred_calls, deferred_ms,
                            per_frame(BbStats::copy_flushes),
                            per_frame(BbStats::deferred_cancelled_pages));
            }
            {
                auto& p = Vulkan::pipeline_compile_stats;
                const u64 background = p.background.exchange(0), taken = p.taken.exchange(0);
                const u64 skipped = p.skipped_draws.exchange(0);
                const u64 skipped_pipelines = p.skipped_pipelines.exchange(0);
                const u64 late = p.late_binds.exchange(0), waits = p.waits.exchange(0);
                const u64 wait_ns = p.wait_ns.exchange(0), longest = p.longest_wait_ns.exchange(0);
                if (background + skipped + late + waits != 0) {
                    std::printf("Pipelines: %llu compiled in the background (%llu by the thread "
                                "that needed them); %llu draws of %llu pipelines left out while "
                                "compiling; %llu draws/dispatches bound on the recording threads; "
                                "%llu waits, %.1f ms, longest %.1f ms\n",
                                static_cast<unsigned long long>(background),
                                static_cast<unsigned long long>(taken),
                                static_cast<unsigned long long>(skipped),
                                static_cast<unsigned long long>(skipped_pipelines),
                                static_cast<unsigned long long>(late),
                                static_cast<unsigned long long>(waits), wait_ns / 1e6,
                                longest / 1e6);
                }
            }
            VideoCore::PageManager::ReportFaultSites();
            if (const u64 notes = BbStats::cpu_write_notes.exchange(0)) {
                std::printf("Write notices: %.0f/s, %.2f MB/s (libc copies over pages the GPU side "
                            "watches)\n",
                            notes / window, BbStats::cpu_write_note_bytes.exchange(0) / (window * 1e6));
            }
            Vulkan::Breadcrumbs::PrintProgress();
            PrintMemory();
            if (const u64 in_place = BbStats::bound_in_place_bytes.exchange(0)) {
                std::printf("Buffer bindings: %.1f MB/frame in place (%.1f GPU-written), %.1f MB/frame "
                            "VRAM copies\n",
                            in_place / (frames * 1e6),
                            BbStats::bound_in_place_written_bytes.exchange(0) / (frames * 1e6),
                            BbStats::bound_vram_bytes.exchange(0) / (frames * 1e6));
            }
            {
                const u64 updates = BbStats::gpu_data_updates.exchange(0);
                const u64 copied_back = BbStats::copied_back_blocks.exchange(0);
                const u64 readbacks = BbStats::readbacks.exchange(0);
                const u64 late = BbStats::late_vram_writes.exchange(0);
                if (updates + copied_back + readbacks + late != 0) {
                    std::printf("GPU data in VRAM: %.1f command writes/s put into it, %.1f exact writes/s "
                                "(labels, CPU bytes beside GPU data), %.1f blocks/s copied back, "
                                "%.1f readbacks/s\n",
                                updates / window, late / window, copied_back / window,
                                readbacks / window);
                }
            }
            sys_equeue_report(frames);
            if (static int heap_reports = 0; ++heap_reports % 6 == 1) {
                BbHeapSites::Report();
            }
            runtime_wait_report(frames);
            for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
                if (const u64 a = BbStats::range_allocators[i].load()) {
                    std::printf("%s %#llx: %llu live, %.1f MB", i ? ";" : "GPU range allocators:",
                                (unsigned long long)a, (unsigned long long)BbStats::range_live[i].load(),
                                BbStats::range_bytes[i].load() / 1e6);
                }
            }
            if (BbStats::range_allocators[0].load()) {
                std::printf("\n");
            }
            std::printf("Gnm: %.1f sceGnmSubmitDone/frame, %.1f sceGnmAreSubmitsAllowed/frame, %.1f refused/frame; "
                        "guest waited for the previous frame %.1fx %.2f ms/frame; %.1f compute queue submissions/frame\n",
                        BbStats::submit_done_calls.exchange(0) / double(frames),
                        BbStats::submits_allowed_queries.exchange(0) / double(frames),
                        BbStats::submits_refused.exchange(0) / double(frames),
                        BbStats::gnm_frame_waits.exchange(0) / double(frames),
                        taken(BbStats::gnm_frame_wait_ns, last_frame_wait_ns) / (1e6 * double(frames)),
                        BbStats::asc_submits.exchange(0) / double(frames));
            {
                static u64 last_decoded = 0, last_written = 0;
                const u64 decoded = BbStats::eop_decoded.load(), written = BbStats::eop_written.load();
                std::printf("EOP fences: %.1f decoded/frame, %.1f labels written/frame, %lld pending\n",
                            frames ? double(decoded - last_decoded) / frames : 0.0,
                            frames ? double(written - last_written) / frames : 0.0,
                            (long long)(decoded - written));
                last_decoded = decoded;
                last_written = written;
            }
            if (const u64 early = BbStats::idle_flushes.exchange(0)) {
                std::printf("Honest labels: %.1f submissions/frame sent early (GPU idle)\n",
                            early / double(frames));
            }
            {
                uint64_t sleeps = 0, sleep_ns = 0;
                runtime_sleep_stats(&sleeps, &sleep_ns);
                if (sleeps != 0 && frames != 0) {
                    std::printf("Guest sleeps per frame: %.1f, %.2f ms\n", sleeps / double(frames),
                                sleep_ns / (frames * 1e6));
                }
            }
            if (const u64 copies = BbStats::shadow_copies.exchange(0)) {
                std::printf("Shadow copies: %.0f/frame, %.1f MB/frame (in-place data the CPU writes, "
                            "copied into VRAM for reading)\n",
                            copies / double(frames), BbStats::shadow_bytes.exchange(0) / (frames * 1e6));
            }
            if (const u64 allocs = BbStats::gpu_range_allocs.exchange(0)) {
                std::printf("Guest GPU memory: %.0f ranges/s handed out, %.2f MB/s\n", allocs / window,
                            BbStats::gpu_range_alloc_bytes.exchange(0) / (window * 1e6));
            }
            if (const u64 submits = Vulkan::Scheduler::recorded_submissions.exchange(0)) {
                const u64 segments = Vulkan::Scheduler::recorded_segments.exchange(0);
                std::printf("Recording: %.1f submissions/frame, %.2f command buffers each; "
                            "recording threads woken %.1f/frame by stage B, %.1f/frame by the "
                            "thread before them\n",
                            frames ? double(submits) / frames : 0.0, double(segments) / submits,
                            frames ? double(Vulkan::Scheduler::producer_wakes.exchange(0)) / frames : 0.0,
                            frames ? double(Vulkan::Scheduler::peer_wakes_done.exchange(0)) / frames : 0.0);
            }
            if (const u32 reliefs = Vulkan::SubmitResult::EngineRetries().relief_requests.load(
                    std::memory_order_relaxed)) {
                const auto& retry = Vulkan::SubmitResult::EngineRetries();
                std::printf("Submit out of memory (LANTERN_SUBMIT_OOM_RETRY): %u so far, %u "
                            "retried, %u recovered\n",
                            reliefs, retry.retries.load(std::memory_order_relaxed),
                            retry.recovered.load(std::memory_order_relaxed));
            }
            BbWaitTrace::Report(window);
            if (intervals.size() > 2) {
                std::vector<double> sorted = intervals;
                std::sort(sorted.begin(), sorted.end());
                const double median = sorted[sorted.size() / 2];
                double sum = 0, sq = 0;
                u32 spikes = 0;
                for (const double ms : intervals) {
                    sum += ms;
                    sq += ms * ms;
                    spikes += ms > 1.5 * median;
                }
                const double mean = sum / intervals.size();
                std::printf("Frame pacing: median %.2f ms, stddev %.2f ms, p99 %.2f ms, "
                            "%u frames over 1.5x median\n",
                            median, std::sqrt(std::max(0.0, sq / intervals.size() - mean * mean)),
                            sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)], spikes);
            }
            {
                char pacing[640];
                if (LanternPacingStats::Line(pacing, sizeof pacing, double(runtime_process_time_us()) / 1e6)) {
                    std::printf("%s\n", pacing);
                }
            }
            intervals.clear();
            window_start = now;
            frames = 0;
            worst_ms = 0;
        }
    }
}

void OnExit() {
    if (BbEngine::VertexCensus::On()) {
        BbEngine::VertexCensus::Print("in all", BbEngine::VertexCensus::Read());
    }
    PrintGxWhole();
    Lantern::Meter::OnExit();
    StepsOnExit();
}

} // namespace LanternFrameReport

extern "C" void bbgpu_pacing_pad_read(void) {
    LanternPacingStats::PadRead();
}
