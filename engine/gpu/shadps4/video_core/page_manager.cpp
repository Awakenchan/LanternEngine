// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include "bbport_copy.h"
#include "bbport_sections.h"
#include "bbport_threads.h"
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include "bbport_platform.h"
#ifndef _WIN32
#include <dlfcn.h>
#include <ucontext.h>
#include <unistd.h>
#endif
#include <string>
#include <fmt/format.h>
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#endif

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif
#include "lantern_switches.h"

extern "C" void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch);

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

namespace {
/// bbport: write fault sites (guest code), an open-addressing table keyed by the instruction.
struct FaultSite {
    std::atomic<u64> rip{0};
    std::atomic<u64> caller{0};
    std::atomic<u64> count{0};
    std::atomic<u64> last_address{0};
};
std::array<FaultSite, 512> fault_sites;
std::atomic<u64> fault_sites_dropped{0};
constexpr u64 GuestImage = 0x800000000ull, GuestImageEnd = 0x810000000ull;
constexpr u64 HostSite = 1ull << 63;
constexpr u64 ReadSite = 1ull << 62; ///< a read fault (BB_READBACKS=2 protects GPU data from reads)

/// "+offset" for guest code, the symbol for host code.
std::string SiteName(u64 key) {
    const char* access = key & ReadSite ? "read:" : "";
    key &= ~ReadSite;
    if (!(key & HostSite)) {
        return fmt::format("{}+{:#x}", access, key);
    }
    const u64 address = key & ~HostSite;
#ifdef _WIN32
    char text[512];
    BbPlatform::DescribeAddress(reinterpret_cast<void*>(address), text, sizeof(text));
    return fmt::format("{}host:{}", access, text);
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_sname) {
        return fmt::format("{}host:{}+{:#x}", access, info.dli_sname, address - u64(info.dli_saddr));
    }
    if (info.dli_fname) {
        return fmt::format("{}host:{}+{:#x}", access, info.dli_fname, address - u64(info.dli_fbase));
    }
    return fmt::format("{}host:{:#x}", access, address);
#endif
}

/// The write fault this thread is handling (guest offsets; 0: none).
thread_local u64 current_fault_rip = 0, current_fault_caller = 0;

/// bbport (BB_FRAME_STATS): what the protection changes this thread makes are for
/// (BbStats::ProtectKind), set by PageManager's entry points.
thread_local u32 protect_kind = BbStats::ProtectOther;
struct ProtectKindScope {
    explicit ProtectKindScope(u32 kind) : saved{protect_kind} {
        protect_kind = kind;
    }
    ~ProtectKindScope() {
        protect_kind = saved;
    }
    u32 saved;
};

/// Guest code that wrote image (texture) memory: keyed by instruction and image.
struct ImageFaultSite {
    std::atomic<u64> key{0}; // a mix of the site and the image address
    std::atomic<u64> rip{0}, caller{0}, count{0}, image{0}, size{0};
    std::atomic<u32> width{0}, height{0}, format{0}, tiling{0};
};
std::array<ImageFaultSite, 256> image_fault_sites;

void NoteFaultSite(void* context, VAddr address) {
    current_fault_rip = 0;
    // Collecting a stack for every streamed page adds OS reads to the normal load path.
    const Lantern::Switches& switches = Lantern::Switches::Get();
    if (!switches.frame_stats && !switches.fault_sites) return;
#ifdef _WIN32
    const auto* registers = static_cast<const EXCEPTION_POINTERS*>(context)->ContextRecord;
    const u64 rip = registers->Rip, rbp = registers->Rbp, rsp = registers->Rsp;
#else
    const auto* g = static_cast<const ucontext_t*>(context)->uc_mcontext.gregs;
    const u64 rip = u64(g[REG_RIP]), rbp = u64(g[REG_RBP]), rsp = u64(g[REG_RSP]);
#endif
    const bool guest_code = rip >= GuestImage && rip < GuestImageEnd;
    u64 caller = 0;
    if (guest_code) {
        // The caller: [rbp + 8] when the guest code keeps frames (its memcpy-like leaves do not).
        u64 saved[2] = {};
        if (BbPlatform::ReadProcessMemory(reinterpret_cast<void*>(rbp), saved, sizeof(saved)) &&
            saved[1] >= GuestImage && saved[1] < GuestImageEnd) {
            caller = saved[1];
        }
    } else {
        // Host code (a libc import the port runs natively, the runtime): the first guest return
        // address on the stack is its guest caller.
        std::array<u64, 64> stack{};
        const std::size_t got = BbPlatform::ReadProcessMemory(reinterpret_cast<void*>(rsp), stack.data(), sizeof(stack)) ? stack.size() : 0;
        for (std::size_t i = 0; i < got; ++i) {
            if (stack[i] >= GuestImage && stack[i] < GuestImageEnd) {
                caller = stack[i];
                break;
            }
        }
    }
    // Guest code: its offset in the image; host code: its address with HostSite set.
    const u64 key = (guest_code ? rip - GuestImage : rip | HostSite) |
                    (Common::IsWriteError(context) ? 0 : ReadSite);
    current_fault_rip = key;
    current_fault_caller = caller ? caller - GuestImage : 0;
    for (u64 i = 0, slot = (key * 0x9E3779B97F4A7C15ull) >> 55; i < fault_sites.size(); ++i) {
        auto& site = fault_sites[(slot + i) % fault_sites.size()];
        u64 expected = 0;
        if (site.rip.load(std::memory_order_relaxed) == key ||
            site.rip.compare_exchange_strong(expected, key)) {
            site.caller.store(caller ? caller - GuestImage : 0, std::memory_order_relaxed);
            site.last_address.store(address, std::memory_order_relaxed);
            site.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    fault_sites_dropped.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

static void ReportImageFaultSites() {
    bool any = false;
    for (auto& site : image_fault_sites) {
        const u64 count = site.count.exchange(0, std::memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        std::printf("%s %s (from +%#llx) %llu into image %#llx size %llu %ux%u fmt %u tile %u",
                    any ? ";" : "Image write sites (guest code -> texture, this window):",
                    SiteName(site.rip.load()).c_str(), (unsigned long long)site.caller.load(),
                    (unsigned long long)count, (unsigned long long)site.image.load(),
                    (unsigned long long)site.size.load(), site.width.load(), site.height.load(),
                    site.format.load(), site.tiling.load());
        any = true;
    }
    if (any) {
        std::printf("\n");
    }
}

void PageManager::NoteImageFault(VAddr image_address, u64 image_size, u32 width, u32 height,
                                 u32 format, u32 tiling) {
    if (current_fault_rip == 0) {
        return;
    }
    const u64 key = (current_fault_rip * 0x9E3779B97F4A7C15ull) ^ image_address;
    for (u64 i = 0, slot = (key * 0x9E3779B97F4A7C15ull) >> 56; i < image_fault_sites.size(); ++i) {
        auto& site = image_fault_sites[(slot + i) % image_fault_sites.size()];
        u64 expected = 0;
        if (site.key.load(std::memory_order_relaxed) == key ||
            site.key.compare_exchange_strong(expected, key)) {
            site.rip.store(current_fault_rip, std::memory_order_relaxed);
            site.caller.store(current_fault_caller, std::memory_order_relaxed);
            site.image.store(image_address, std::memory_order_relaxed);
            site.size.store(image_size, std::memory_order_relaxed);
            site.width.store(width, std::memory_order_relaxed);
            site.height.store(height, std::memory_order_relaxed);
            site.format.store(format, std::memory_order_relaxed);
            site.tiling.store(tiling, std::memory_order_relaxed);
            site.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void PageManager::ReportFaultSites() {
    struct Row {
        u64 rip, caller, count, address;
    };
    std::array<Row, 512> rows{};
    std::size_t n = 0;
    u64 total = 0;
    for (auto& site : fault_sites) {
        const u64 count = site.count.exchange(0, std::memory_order_relaxed);
        if (count != 0) {
            rows[n++] = {site.rip.load(), site.caller.load(), count, site.last_address.load()};
            total += count;
        }
    }
    if (total == 0) {
        return;
    }
    ReportImageFaultSites();
    std::sort(rows.begin(), rows.begin() + n, [](const Row& a, const Row& b) { return a.count > b.count; });
    std::printf("Write fault sites (guest code, this window): %llu faults", (unsigned long long)total);
    for (std::size_t i = 0; i < std::min<std::size_t>(n, 12); ++i) {
        std::printf("%s %s (from +%#llx) %llu at %#llx", i ? ";" : ":",
                    SiteName(rows[i].rip).c_str(), (unsigned long long)rows[i].caller,
                    (unsigned long long)rows[i].count, (unsigned long long)rows[i].address);
    }
    std::printf("\n");
}

/// bbport: a watcher count that would leave its range, reported (a few times) instead of the
/// assertion that ended the game (exit 23), or the 7-bit count wrapping to 0 with the page still
/// protected.
static void NoteWatcherCount(u64 page, const char* what) {
    static std::atomic<u32> notes{0};
    if (notes.fetch_add(1, std::memory_order_relaxed) < 8) {
        std::printf("GPU: page tracking: %s at page %#llx; tracking goes on\n", what,
                    static_cast<unsigned long long>(page << 12));
    }
}

struct PageManager::Impl {
    struct PageState {
        u8 num_write_watchers : 7;
        // At the moment only buffer cache can request read watchers.
        // And buffers cannot overlap, thus only 1 can exist per page.
        u8 num_read_watchers : 1;

        static constexpr u8 MaxWriteWatchers = 127;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        /// bbport: `page` (its number) for the counts past the bit fields (SpillWatcher). Called
        /// with the page's lock held.
        template <s32 delta, bool is_read>
        u8 AddDelta(u64 page) {
            if constexpr (is_read) {
                if constexpr (delta == 1) {
                    if (num_read_watchers == 1) [[unlikely]] {
                        NoteWatcherCount(page, "a second read watcher");
                        return num_read_watchers;
                    }
                    return ++num_read_watchers;
                } else if (delta == -1) {
                    if (num_read_watchers == 0) [[unlikely]] {
                        NoteWatcherCount(page, "not enough read watchers");
                        return 0;
                    }
                    return --num_read_watchers;
                } else {
                    return num_read_watchers;
                }
            } else {
                if constexpr (delta == 1) {
                    // 128 or more watchers (images piling up on one page): the count stays at
                    // its maximum, the page protected, and the excess is kept aside.
                    if (num_write_watchers == MaxWriteWatchers) [[unlikely]] {
                        if (!SpillWatcher(page, 1)) {
                            NoteWatcherCount(page, "write watchers beyond the overflow table");
                        }
                        return num_write_watchers;
                    }
                    return ++num_write_watchers;
                } else if (delta == -1) {
                    if (num_write_watchers == MaxWriteWatchers && SpillWatcher(page, -1))
                        [[unlikely]] {
                        return num_write_watchers;
                    }
                    if (num_write_watchers == 0) [[unlikely]] {
                        NoteWatcherCount(page, "not enough write watchers");
                        return 0;
                    }
                    return --num_write_watchers;
                } else {
                    return num_write_watchers;
                }
            }
        }
    };

    /// bbport: write watchers past a page's 7-bit count. +1 keeps one more (false: the table is
    /// full, the watcher is lost and the page may be unprotected early); -1 takes one back
    /// (false: none kept for the page). Nests inside the page locks; nothing else takes it.
    struct SpilledWatchers {
        u64 page;
        u32 extra;
    };
    inline static std::array<SpilledWatchers, 256> spilled_watchers{};
    inline static std::atomic_flag spill_lock{};

    static bool SpillWatcher(u64 page, s32 delta) {
        while (spill_lock.test_and_set(std::memory_order_acquire)) {
        }
        bool done = false;
        SpilledWatchers* free_entry = nullptr;
        for (auto& entry : spilled_watchers) {
            if (entry.extra != 0 && entry.page == page) {
                entry.extra += delta;
                done = true;
                break;
            }
            if (entry.extra == 0 && !free_entry) {
                free_entry = &entry;
            }
        }
        if (!done && delta > 0 && free_entry) {
            *free_entry = {page, 1};
            done = true;
        }
        spill_lock.clear(std::memory_order_release);
        return done;
    }

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;

    Impl() = default;
    virtual ~Impl() {
        for (auto& d : deferred) {
            delete d.load(std::memory_order_relaxed);
        }
    }

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    /// bbport BB_DEFER_PROTECT (see below): per lock region, pages whose write protection is
    /// deferred, and whether the region is listed for bb:Protect. Guarded by locks[region].
    struct Deferred {
        RegionBits pages{};
        bool listed = false;
    };
    struct ProtectRun {
        VAddr address;
        u64 size;
        Core::MemoryPermission perms;
    };
    using ProtectRuns = boost::container::small_vector<ProtectRun, 16>;

    /// bbport: Protect, counted by thread and purpose (BB_FRAME_STATS) and timed on the draw
    /// recording thread (BB_SECTIONS "protect").
    void ApplyProtect(VAddr address, size_t size, Core::MemoryPermission perms) {
        BbSections::Scope scope{BbSections::Protect};
        if (!BbStats::enabled) {
            Protect(address, size, perms);
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        Protect(address, size, perms);
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - start)
                                            .count());
        if (protect_kind == BbStats::ProtectDeferred) {
            BbStats::deferred_protect_calls.fetch_add(1, std::memory_order_relaxed);
            BbStats::deferred_protect_ns.fetch_add(ns, std::memory_order_relaxed);
        } else if (Vulkan::DrawPipe::OnStageB()) {
            BbStats::stage_b_protect_calls[protect_kind].fetch_add(1, std::memory_order_relaxed);
            BbStats::stage_b_protect_ns[protect_kind].fetch_add(ns, std::memory_order_relaxed);
        } else {
            BbStats::other_protect_calls.fetch_add(1, std::memory_order_relaxed);
            BbStats::other_protect_ns.fetch_add(ns, std::memory_order_relaxed);
        }
    }

    // bbport BB_DEFER_PROTECT (on by default; =0 off, or experiment bit 4 of BB_TOGGLE_FILE while
    // the game runs): pages the buffer cache uploaded are watched for writes again
    // (UpdateProtection after ForEachModifiedRange<CPU, true>) by a count change on the calling
    // thread, the draw recording thread; the system call that write-protects them is made by
    // bb:Protect, or by the first upload copy that runs before it did (FlushDeferredProtects).
    // Until then such a page is still writable: a write to it does not fault, and the copy
    // queued for the upload, which reads the page only after the protection is in place, takes
    // it along. Writes after that fault as before.
    //
    // Every path that reads guest memory for the GPU, and how it is covered:
    // - BufferCache::UploadCopies (the VRAM path of ObtainBuffer, SynchronizeMemory): small
    //   uploads via SmallGuestCopy -> RunGuestCopy (Scheduler::RecordHostCopy on a recording
    //   thread, BbCopy::QueueCopy batches, or run inline in direct mode), larger ones as
    //   BbCopy::Async groups, and the synchronous path (toggle DeferredUploads): each flushes
    //   before it copies.
    // - ObtainBufferForImage staging (texture sources): RunGuestCopy / BbCopy::Async running it /
    //   the synchronous path: flushed; its arena path is SynchronizeMemory (above).
    // - ObtainBuffer's stream path (RunGuestCopy, flushed anyway) and stage A's constant ring
    //   (CopySparseMemory, not flushed): snapshots taken when they run, which rely on no page
    //   protection.
    // - Texture hashes and other direct reads of image memory (RefreshImage, KeyPromptData):
    //   images watch their pages through the immediate path, which applies the deferred pages of
    //   its range first (ApplyDeferred).
    // - CPU stores of the command processor (WriteData, DMA, constant RAM dumps) into a page still
    //   writable: no fault, but the copy queued for that page has not read it yet (it flushes
    //   before it reads), so it takes the stored bytes along, as it took a store that faulted.
    //   WriteData and DMA wait for copies reading their range first (WaitHostCopiesFor).
    // - Not deferred: BB_GUEST_IN_PLACE (the GPU reads guest memory chunks itself:
    //   CopyFromGuestChunk), BB_GPU_WRITE_TWINS (copies of kept parts are dropped), precise
    //   readbacks (read watchers): BufferCache allows deferral only without them.
    //
    // A set bit in `deferred` means: the count says watched, the page is still writable. Only
    // write revocations are deferred, so such a page has a count of at least one. The immediate
    // paths reconcile the pages of their range under the page locks: when they add watchers they
    // first apply the deferred pages (a texture then relies on the protection), when they drop a
    // page's last watcher its deferred bit is dropped too (it is writable already). Physical
    // changes of a region are made in order: protect_locks[] (taken before locks[]) is held across
    // the system call by everyone except the deferral itself, which changes no protection, so the
    // draw recording thread never waits for bb:Protect's system calls.

    /// Whether uploads defer their write protection now (BB_DEFER_PROTECT=1, experiment bit 4).
    [[nodiscard]] bool Deferring() const {
        return deferral_allowed && (DeferEnv() || (deferral_possible && BbToggle::Experiment(4)));
    }

    static bool DeferEnv() {
        static const bool on = [] {
            // bbport: off unless BB_DEFER_PROTECT=1. On by default in 0.9.21-0.9.23, it left
            // distant Cathedral Ward textures half-written for good (a write in the deferral
            // window went unnoticed; the path is not found yet).
            const char* env = std::getenv("BB_DEFER_PROTECT");
            return env && env[0] == '1';
        }();
        return on;
    }

    /// Set by the buffer cache: deferral allowed in this memory model; starts bb:Protect when it
    /// may be used (BB_DEFER_PROTECT=1, or BB_TOGGLE_FILE for experiment bit 4).
    void AllowDeferral(bool allowed) {
        deferral_allowed = allowed;
        deferral_possible = DeferEnv() || std::getenv("BB_TOGGLE_FILE") != nullptr;
        if (!allowed || !deferral_possible || protector.joinable()) {
            return;
        }
        deferring_instance = this;
        protector = std::jthread([this](std::stop_token stop) { ProtectorLoop(stop); });
        std::printf("Page tracking: write protection after uploads applied by bb:Protect (%s)\n",
                    DeferEnv() ? "BB_DEFER_PROTECT=1" : "experiment bit 4 of BB_TOGGLE_FILE");
    }

    void StopProtector() {
        if (!protector.joinable()) {
            return;
        }
        protector.request_stop();
        protector_wake.fetch_add(1, std::memory_order_seq_cst);
        protector_wake.notify_all();
        protector.join();
        FlushDeferred(false);
        deferring_instance = nullptr;
    }

    void ProtectorLoop(std::stop_token stop) {
        Common::SetCurrentThreadName("bb:Protect");
        BbThreads::MakeCritical();
        // Uploads come in bursts within a frame: spin a while after each flush, then sleep.
        static const auto spin_time = std::chrono::microseconds([] {
            if (const char* env = std::getenv("BB_PROTECT_SPIN_US")) {
                return std::max(0, std::atoi(env));
            }
            return int(BbThreads::CapSpinUs(BbThreads::Available() >= 12 ? 100 : 20));
        }());
        while (!stop.stop_requested()) {
            const auto spin_until = std::chrono::steady_clock::now() + spin_time;
            for (u32 spins = 1; num_listed.load(std::memory_order_acquire) == 0; ++spins) {
                if (stop.stop_requested()) {
                    return;
                }
                __builtin_ia32_pause();
                if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                    // Announced before the check, which DeferPageWatchersForRegion's count update
                    // and announcement check mirror (both sequentially consistent).
                    const u32 seen = protector_wake.load(std::memory_order_seq_cst);
                    protector_sleeping.store(true, std::memory_order_seq_cst);
                    if (num_listed.load(std::memory_order_seq_cst) == 0 &&
                        !stop.stop_requested()) {
                        protector_wake.wait(seen, std::memory_order_seq_cst);
                    }
                    protector_sleeping.store(false, std::memory_order_relaxed);
                }
            }
            FlushDeferred(false);
        }
    }

    Deferred& DeferredFor(size_t region) {
        Deferred* d = deferred[region].load(std::memory_order_relaxed);
        if (!d) {
            d = new Deferred{};
            deferred[region].store(d, std::memory_order_release);
        }
        return *d;
    }

    /// The deferral itself: counts change now, the pages that become watched are only marked.
    void DeferPageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        const size_t base_page = base_addr >> PM_PAGE_BITS;
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        const size_t region = base_page / PAGES_PER_LOCK;
        u64 pages = 0;
        bool wake = false;
        {
            std::scoped_lock lk(locks[region]);
            Deferred& d = DeferredFor(region);
            deferral_used.store(true, std::memory_order_relaxed);
            for (const auto& [start, end] : mask) {
                for (size_t page = start; page < end; ++page) {
                    PageState& state = cached_pages[base_page + page];
                    const auto old_perms = state.Perms();
                    state.AddDelta<1, false>(base_page + page);
                    if (state.Perms() != old_perms) {
                        d.pages.Set(page); // never set before: such a page counts one at least
                        ++pages;
                    }
                }
            }
            if (pages != 0 && !d.listed) {
                d.listed = true;
                num_listed.fetch_add(1, std::memory_order_seq_cst);
                std::scoped_lock list{list_mutex};
                listed.push_back(static_cast<u32>(region));
                wake = true;
            }
        }
        if (BbStats::enabled && pages != 0) {
            BbStats::deferred_protect_pages.fetch_add(pages, std::memory_order_relaxed);
        }
        if (wake && protector_sleeping.load(std::memory_order_seq_cst)) {
            protector_wake.fetch_add(1, std::memory_order_seq_cst);
            protector_wake.notify_one();
        }
    }

    /// Applies every deferred page listed so far. Any thread, no page lock held. `by_copy`: a
    /// copy about to read guest memory found some (bb:Protect was behind). A flush that finds
    /// nothing listed after waiting for another one: that one applied them.
    void FlushDeferred(bool by_copy) {
        if (num_listed.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock flush{flush_mutex};
        std::vector<u32> regions;
        {
            std::scoped_lock list{list_mutex};
            regions.swap(listed);
        }
        if (regions.empty()) {
            return;
        }
        if (by_copy && BbStats::enabled) {
            BbStats::copy_flushes.fetch_add(1, std::memory_order_relaxed);
        }
        const u32 kind = protect_kind;
        protect_kind = BbStats::ProtectDeferred;
        for (const u32 region : regions) {
            // The system calls are made with the counts unlocked (the draw recording thread may
            // defer more meanwhile; it only adds watchers), in order with every other protection
            // change of the region (protect_locks).
            std::scoped_lock plk(protect_locks[region]);
            ProtectRuns runs;
            {
                std::scoped_lock lk(locks[region]);
                TakeDeferredRuns(region, 0, PAGES_PER_LOCK, runs);
                deferred[region].load(std::memory_order_relaxed)->listed = false;
            }
            for (const auto& run : runs) {
                ApplyProtect(run.address, run.size, run.perms);
            }
        }
        protect_kind = kind;
        // Only now: a copy that saw them listed waits for this flush (flush_mutex) or finds none.
        num_listed.fetch_sub(static_cast<u32>(regions.size()), std::memory_order_release);
    }

    /// The deferred pages of region `region` within [first, end) (page offsets in the region) as
    /// runs of equal permissions (as their counts say); cleared. Locks of the region held.
    void TakeDeferredRuns(size_t region, size_t first, size_t end, ProtectRuns& runs) {
        Deferred* d = deferred[region].load(std::memory_order_relaxed);
        if (!d || first >= end || !d->pages.AnyInRange(first, end)) {
            return;
        }
        const size_t base_page = region * PAGES_PER_LOCK;
        const RegionBits pages(d->pages, first, end);
        for (const auto& [start, stop] : pages) {
            size_t run = start;
            auto perms = cached_pages[base_page + start].Perms();
            for (size_t page = start + 1; page <= stop; ++page) {
                if (page == stop || cached_pages[base_page + page].Perms() != perms) {
                    runs.push_back({(base_page + run) << PM_PAGE_BITS,
                                    (page - run) << PM_PAGE_BITS, perms});
                    if (page != stop) {
                        run = page;
                        perms = cached_pages[base_page + page].Perms();
                    }
                }
            }
            d->pages.UnsetRange(start, stop);
        }
    }

    /// Immediate path adding watchers (or read watchers) to [page, page_end): the deferred pages
    /// there are protected first. protect_locks and locks of the range held.
    void ApplyDeferred(u64 page, u64 page_end) {
        if (!deferral_used.load(std::memory_order_relaxed)) {
            return;
        }
        ProtectRuns runs;
        for (u64 region = page / PAGES_PER_LOCK;
             region < Common::DivCeil(page_end, PAGES_PER_LOCK); ++region) {
            const u64 base = region * PAGES_PER_LOCK;
            TakeDeferredRuns(region, std::max(page, base) - base,
                             std::min<u64>(page_end, base + PAGES_PER_LOCK) - base, runs);
        }
        for (const auto& run : runs) {
            ApplyProtect(run.address, run.size, run.perms);
        }
    }

    /// Immediate path: `page` lost its last write watcher; a deferred protection of it is moot
    /// (it is writable already). Its page lock held.
    void CancelDeferred(u64 page) {
        if (!deferral_used.load(std::memory_order_relaxed)) {
            return;
        }
        Deferred* d = deferred[page / PAGES_PER_LOCK].load(std::memory_order_relaxed);
        const size_t bit = page % PAGES_PER_LOCK;
        if (d && d->pages.Get(bit)) {
            d->pages.Unset(bit);
            if (BbStats::enabled) {
                BbStats::deferred_cancelled_pages.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    /// Switched off while pages are still deferred: applied before the immediate paths go on.
    void FlushIfSwitchedOff() {
        if (num_listed.load(std::memory_order_acquire) != 0 && !Deferring()) {
            FlushDeferred(false);
        }
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        NoteFaultSite(context, addr);
        // bbport: the draw recording thread handles its faults inline too (vk_draw_pipe.h).
        const auto is_gpu_thread = rasterizer->IsGpuSideThread();
        // bbport: copy threads working for a GPU-side thread fault on its behalf, several at once:
        // one at a time, as that thread's own faults are.
        static std::mutex for_gpu_side_mutex;
        std::unique_lock for_gpu_side_lock{for_gpu_side_mutex, std::defer_lock};
        if (BbCopy::ForGpuSide()) {
            for_gpu_side_lock.lock();
        }
        if (is_gpu_thread) {
            BbStats::gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
        }
        if (Common::IsWriteError(context)) {
            BbStats::Timer timer{BbStats::t_write_faults};
            const bool handled = rasterizer->OnWriteFault(
                addr, is_gpu_thread,
                reinterpret_cast<u64>(Common::GetRip(context)));
            current_fault_rip = 0;
            return handled;
        } else {
            BbStats::read_faults.fetch_add(1, std::memory_order_relaxed);
            BbStats::Timer timer{BbStats::t_read_faults};
            // bbport: within the faulting page (see Rasterizer::OnWriteFault).
            return rasterizer->ReadMemory(addr, std::min<u64>(8, GetNextPageAddr(addr) - addr),
                                          is_gpu_thread);
        }
        return false;
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages. bbport: protect_locks first (protection changes
        // of a region in order with bb:Protect's, BB_DEFER_PROTECT).
        const size_t first_lock = page / PAGES_PER_LOCK;
        const size_t last_lock = Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard plk(protect_locks.begin() + first_lock,
                                   protect_locks.begin() + last_lock);
        Common::RangeLockGuard lk(locks.begin() + first_lock, locks.begin() + last_lock);
        if constexpr (track || is_read) {
            ApplyDeferred(page, page_end);
        }

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                ApplyProtect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        // bbport (BB_TRACK_MAPPED_CHECK=1 restores it): the check behind this warning asked the
        // rasterizer's mapped ranges (a shared lock and an interval lookup) on every protection
        // change; a range that is not mapped is skipped by the protection change anyway.
        static const bool check_mapped = [] {
            const char* env = std::getenv("BB_TRACK_MAPPED_CHECK");
            return env && env[0] == '1';
        }();
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (check_mapped && !rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>(page);
            if constexpr (!track && !is_read) {
                if (new_count == 0) {
                    CancelDeferred(page);
                }
            }

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        // bbport: protect_locks first, as UpdatePageWatchers.
        std::scoped_lock plk(protect_locks[base_page / PAGES_PER_LOCK]);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        if constexpr (track || is_read) {
            ApplyDeferred(base_page + start_range.first, base_page + end_range.second);
        }
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                ApplyProtect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>(base_page + page)
                       : state.AddDelta<0, is_read>(base_page + page);
            if constexpr (!track && !is_read) {
                if (update && new_count == 0) {
                    CancelDeferred(base_page + page);
                }
            }

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};

    // bbport BB_DEFER_PROTECT (see Deferring): deferred pages per lock region (allocated on first
    // use), the lock serializing a region's protection changes, the regions listed for bb:Protect.
    std::array<std::atomic<Deferred*>, NUM_ADDRESS_LOCKS> deferred{};
    std::array<LockType, NUM_ADDRESS_LOCKS> protect_locks{};
    std::mutex list_mutex;
    std::vector<u32> listed;              ///< guarded by list_mutex
    std::mutex flush_mutex;               ///< one flush at a time
    inline static std::atomic<u32> num_listed{0}; ///< lowered only once the pages are protected
    inline static Impl* deferring_instance = nullptr;
    std::atomic<bool> deferral_used{false}; ///< some page was deferred once
    bool deferral_allowed = false;
    bool deferral_possible = false;
    std::atomic<u32> protector_wake{0};
    std::atomic<bool> protector_sleeping{false};
    std::jthread protector;
};

#ifdef __linux__
// bbport: write tracking with userfaultfd write-protection instead of mprotect. mprotect takes
// the address space lock for writing and splits mappings; while guest threads fault on
// per-frame buffers and the GPU thread re-protects uploaded pages, every page fault in the
// process (copy threads included) waits for it. UFFDIO_WRITEPROTECT changes page table bits
// under the lock for reading. Read protection (readbacks) still uses mprotect: userfaultfd
// write-protection cannot deny reads. BB_UFFD=1.
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;
    std::mutex read_revoked_mutex;
    std::unordered_set<u64> read_revoked_pages; ///< 4 KiB pages denied reads with mprotect
    std::atomic<size_t> num_read_revoked{0};

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Guest memory is a shared memfd mapping: write-protection there needs the shmem
        // feature, and unpopulated pages must be protectable too.
        uffdio_api api{};
        api.api = UFFD_API;
        api.features =
            UFFD_FEATURE_THREAD_ID | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(uffd, UFFDIO_API, &api) != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            close(uffd);
            throw std::runtime_error("uffdio_api");
        }

        // Read faults (readbacks) still arrive as signals.
        Core::Signals::Instance()->RegisterAccessViolationHandler(
            GuestFaultSignalHandler, std::numeric_limits<u32>::min());

        ufd_thread = std::jthread([this](std::stop_token token) { UffdHandler(token); });
        std::printf("GPU: memory tracking with userfaultfd write-protection\n");
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        uffdio_register reg{};
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio register {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range{};
        range.start = address;
        range.len = size;
        ioctl(uffd, UFFDIO_UNREGISTER, &range);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        auto& address_space = Core::Memory::Instance()->GetAddressSpace();
        const bool allow_read = True(perms & Core::MemoryPermission::Read);
        const bool allow_write = True(perms & Core::MemoryPermission::Write);
        const u64 first = address >> 12, last = (address + size + 4095) >> 12;
        if (!allow_read) {
            // Readbacks: deny all access with mprotect.
            address_space.Protect(address, size, Core::MemoryPermission::None);
            std::scoped_lock lk{read_revoked_mutex};
            for (u64 page = first; page < last; ++page) {
                read_revoked_pages.insert(page);
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            return;
        }
        if (num_read_revoked.load(std::memory_order_acquire) != 0) {
            std::scoped_lock lk{read_revoked_mutex};
            bool restore = false;
            for (u64 page = first; page < last; ++page) {
                restore |= read_revoked_pages.erase(page) != 0;
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            if (restore) {
                address_space.Protect(address, size, Core::MemoryPermission::ReadWrite);
            }
        }
        // Counted like AddressSpace::Protect (BB_FRAME_LOG protect_ms).
        BbStats::Timer timer{BbStats::t_protect};
        BbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
        BbStats::protect_pages.fetch_add(last - first, std::memory_order_relaxed);
        if (!allow_write) {
            BbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
            BbStats::protect_revoke_pages.fetch_add(last - first, std::memory_order_relaxed);
        }
        uffdio_writeprotect wp{};
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio writeprotect {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void UffdHandler(std::stop_token token) {
        Common::SetCurrentThreadName("bb:Uffd");
        while (!token.stop_requested()) {
            pollfd pollfd{};
            pollfd.fd = uffd;
            pollfd.events = POLLIN;
            // Short timeout so the stop request is seen.
            const int pollres = poll(&pollfd, 1, 100);
            if (pollres <= 0 || !(pollfd.revents & POLLIN)) {
                continue;
            }
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret != sizeof(msg)) {
                continue;
            }
            if (msg.event != UFFD_EVENT_PAGEFAULT) {
                continue;
            }
            const VAddr addr = msg.arg.pagefault.address;
            const auto ptid = msg.arg.pagefault.feat.ptid;
            {
                BbStats::Timer timer{BbStats::t_write_faults};
                rasterizer->OnWriteFault(addr, rasterizer->IsGpuSideThreadId(ptid));
            }
            // Protect() clears with DONTWAKE (it may run for pages nobody waits on).
            uffdio_range wake{};
            wake.start = addr & ~u64(PM_PAGE_SIZE - 1);
            wake.len = PM_PAGE_SIZE;
            ioctl(uffd, UFFDIO_WAKE, &wake);
        }
    }
};
#endif // __linux__

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        // bbport BB_GUEST_IN_PLACE: no page protection; the pages are marked in the runtime's write
        // watch instead, and the writers that check it tell the GPU side (WriteTracking).
        if (!VideoCore::WriteTracking()) {
            runtime_memory_set_write_watch(address, size, !True(perms & Core::MemoryPermission::Write));
            if (!VideoCore::WriteVerify()) {
                return;
            }
        }
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        // bbport: write-only (a read watcher on a page without write watchers, BB_READBACKS=2)
        // cannot be expressed; reads must fault, so writes do too (the assertion ended the game).
        if (perms == Core::MemoryPermission::Write) {
            perms = Core::MemoryPermission::None;
        }
        impl.Protect(address, size, perms);
    }

};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef __linux__
    // bbport: dma-buf guest memory (BB_GUEST_GPU_MEMORY=1) cannot be write-protected by userfaultfd
    // (anonymous and shmem only): its registration would fail and writes go unseen.
    const char* guest_gpu_memory = std::getenv("BB_GUEST_GPU_MEMORY");
    const bool dma_buf_guest = guest_gpu_memory && guest_gpu_memory[0] == '1';
    if (std::getenv("BB_UFFD") && std::getenv("BB_UFFD")[0] == '1' && dma_buf_guest) {
        std::printf("GPU: userfaultfd off with BB_GUEST_GPU_MEMORY=1 (dma-buf memory)\n");
    } else if (std::getenv("BB_UFFD") && std::getenv("BB_UFFD")[0] == '1') {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() {
    // bbport: bb:Protect calls Protect, which is the derived implementation's.
    impl->StopProtector();
}

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

template <bool track>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->FlushIfSwitchedOff();
    ProtectKindScope kind{BbStats::ProtectTexture};
    impl->UpdatePageWatchers<track, false>(addr, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->FlushIfSwitchedOff();
    ProtectKindScope kind{track && !is_read ? BbStats::ProtectUpload : BbStats::ProtectOther};
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

bool PageManager::DeferringProtects() const {
    return impl->Deferring();
}

void PageManager::DeferPageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->DeferPageWatchersForRegion(base_addr, mask);
}

void PageManager::FlushDeferredProtects() {
    if (Impl::num_listed.load(std::memory_order_acquire) != 0) {
        if (Impl* impl = Impl::deferring_instance) {
            impl->FlushDeferred(true);
        }
    }
}

void PageManager::AllowDeferredProtects(bool allowed) {
    impl->AllowDeferral(allowed);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
