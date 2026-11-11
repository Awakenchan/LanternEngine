#include "../bbnet/room/room_service.h"
#include "bbport_write_log.h"
#include <memory>
#include "bbport_gnm_hooks.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_copy.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "bbport_free_check.h"
#include "bbport_sampler.h"
#include "bbport_freeze.h"
#include "bbport_platform.h"
#include "bbport_toggles.h"
#include "engine/engine_hooks.h"
#include "engine/batch.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../bbgpu.h"
#include "common/elf_info.h"
#include "shader_recompiler/native/table_plan.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "common/signal_context.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
const uint64_t* runtime_memory_generation(void);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
void runtime_memory_set_note_write_hook(RuntimeGpuRange note);
void runtime_memory_set_cpu_write_hook(RuntimeGpuRange hook);
int runtime_memory_restore_access(void* context, uintptr_t address, int repeated);
}
namespace Shader { void RegisterSrtFaultHandler(); }

Frontend::WindowSDL* g_window = nullptr;
extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
extern "C" void lantern_av_decode_start(void);
namespace Libraries::VideoOut { void Start(); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
}

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
}

namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
        const char* app0 = std::getenv("BB_GAME_DIR");
        if (app0 && *app0) {
            info.game_folder = std::filesystem::path(app0);
            info.app_ver = "01.09";
            std::ifstream file(info.game_folder / "sce_sys/npbind.dat", std::ios::binary);
            if (file) {
                file.seekg(0, std::ios::end);
                const auto length = file.tellg();
                if (length >= 0x80 && length <= 1024 * 1024) {
                    file.seekg(0);
                    std::vector<u8> bytes(static_cast<size_t>(length));
                    if (file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
                        info.npCommIds.clear();
                        for (size_t at = 0x80; at + 4 <= bytes.size();) {
                            const u16 tag = u16(bytes[at] << 8 | bytes[at + 1]);
                            const u16 size = u16(bytes[at + 2] << 8 | bytes[at + 3]);
                            at += 4;
                            if (size > bytes.size() - at) break;
                            if (tag == 0x10 && size) {
                                const auto end = std::find(bytes.begin() + at, bytes.begin() + at + size, 0);
                                info.npCommIds.emplace_back(bytes.begin() + at, end);
                            }
                            at += size;
                        }
                    }
                }
            }
        }
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    Shader::Native::TablePlan::guest_readable = [](u64 address, u64 bytes) {
        return Memory::Instance()->ClampRangeSize(address, bytes) >= bytes;
    };
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
    runtime_memory_set_note_write_hook([](uintptr_t address, uint64_t size) {
        Memory::Instance()->GetRasterizer()->NoteAssetWrite(address, size);
    });
    runtime_memory_set_cpu_write_hook([](uintptr_t address, uint64_t size) {
        Memory::Instance()->GetRasterizer()->OnCpuWrite(address, size);
    });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
namespace {
struct RegionCache {
    u64 generation = 1;
    uintptr_t start = 0, end = 0;
};
thread_local RegionCache region_cache;

bool CachedMapped(VAddr address, u64 size) {
    static const uint64_t* const generation_ptr = runtime_memory_generation();
    const u64 generation = __atomic_load_n(generation_ptr, __ATOMIC_ACQUIRE);
    if (generation & 1) {
        return false;
    }
    auto& cache = region_cache;
    if (cache.generation == generation && address >= cache.start && address + size <= cache.end) {
        return true;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    cache = {generation, start, end};
    return address + size <= end;
}
}

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    if (size && CachedMapped(virtual_addr, size)) {
        return size;
    }
    return runtime_memory_clamp(virtual_addr, size);
}
bool MemoryManager::IsValidMapping(VAddr virtual_addr, u64 size) {
    const u64 wanted = size ? size : 1;
    return virtual_addr && runtime_memory_clamp(virtual_addr, wanted) >= wanted;
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    static const bool precise =
        EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise;
    if (precise) {
        runtime_memory_read_backing(source, dest, size);
        return;
    }
    if (size && CachedMapped(source, size)) {
        std::memcpy(dest, reinterpret_cast<const void*>(source), size);
        return;
    }
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !BbCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    BbCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    BbWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, BbWriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    BbStats::Timer timer{BbStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    BbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    BbStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        BbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        BbStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
}

namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; }
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
}

namespace {
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;

long EnvMs(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char* end = nullptr;
    const long ms = std::strtol(value, &end, 10);
    return (end && *end == '\0' && ms >= 0 && ms <= 60'000) ? ms : fallback;
}

[[noreturn]] void TerminateNow(int code) {
    if (BbRoom::Active()) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread([done] {
            BbRoom::Shutdown();
            done->store(true);
        }).detach();
        for (int i = 0; i < 150 && !done->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fflush(stdout);
    const char* mode = std::getenv("BB_EXIT_MODE");
    if (mode && std::strcmp(mode, "exit") == 0) {
        std::_Exit(code);
    }
    BbPlatform::TerminateProcessNow(static_cast<unsigned>(code));
}
}

#ifdef BB_PGO_GENERATE
#ifdef __clang__
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_reset_counters(void);
static void ProfileDump() {
    __llvm_profile_write_file();
}
static void ProfileReset() {
    __llvm_profile_reset_counters();
}
#else
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
static void ProfileDump() {
    __gcov_dump();
}
static void ProfileReset() {
    __gcov_reset();
}
#endif
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            ProfileDump();
            ProfileReset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int bbgpu_init(const BbGpuConfig* config) {
    BbSettings::Load();
    Shader::RegisterSrtFaultHandler();
#ifdef BB_PGO_GENERATE
    StartProfileWriter();
#endif
    BbSampler::Start();
    BbFreeze::SetStateReporter([] {
        if (presenter) {
            presenter->ReportState();
        }
    });
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir && !std::getenv("BB_GPU_USER_DIR")) _putenv_s("BB_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("BB_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "Bloodborne";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("bb:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_WaitEventTimeout(nullptr, 2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        const auto exit_started = std::chrono::steady_clock::now();
        const long watchdog_ms = EnvMs("BB_EXIT_WATCHDOG_MS", 4000);
        std::thread([watchdog_ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(watchdog_ms));
            std::printf("Exit: watchdog ended the process after %ld ms\n", watchdog_ms);
            TerminateNow(0);
        }).detach();
        std::atomic<bool> cache_saved{false};
        std::thread([&cache_saved] {
            Vulkan::PipelineCache::SaveAllForShutdown();
            cache_saved.store(true, std::memory_order_release);
            cache_saved.notify_all();
        }).detach();
        if (SDL_Window* sdl_window = window->GetSDLWindow()) {
            SDL_SetWindowFullscreen(sdl_window, false);
            SDL_HideWindow(sdl_window);
        }
        if (presenter) {
            presenter->WaitForSubmittedWork(u64(EnvMs("BB_EXIT_GPU_WAIT_MS", 1500)) * 1'000'000ull);
        }
        bool saved = cache_saved.load(std::memory_order_acquire);
        while (!saved) {
            cache_saved.wait(false, std::memory_order_acquire);
            saved = cache_saved.load(std::memory_order_acquire);
        }
        BbEngine::Batch::Report("exit");
        std::printf("Exit: clean shutdown in %lld ms\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - exit_started)
                                               .count()));
        Common::Log::Flush();
        TerminateNow(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::Start();
    return 0;
}

extern "C" void bbnet_register(void);
extern "C" void bbnet_initialize(void);
extern "C" void bbgpu_start_online(void) {
    const char* online = std::getenv("BB_ONLINE");
    if (online && online[0] == '1') bbnet_initialize();
}
extern "C" void bbgpu_register_kernel(void) {
    Core::Loader::SymbolsResolver resolver;
    lantern_av_decode_start();
    const char* online = std::getenv("BB_ONLINE");
    if (online && online[0] == '1') bbnet_register();
}

extern "C" uintptr_t bbgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

namespace {
struct RepeatedFault {
    u64 rip = 0, address = 0, last_tsc = 0;
    u32 count = 0;
};
thread_local RepeatedFault repeated_fault;

bool FaultRepeats(void* ucontext, void* address) {
    constexpr u64 RepeatCycles = 4'000'000;
    constexpr u32 RepeatLimit = 64;
    auto& r = repeated_fault;
    const u64 rip = reinterpret_cast<u64>(Common::GetRip(ucontext));
    const u64 now = Common::FencedRDTSC();
    if (rip != r.rip || reinterpret_cast<u64>(address) != r.address ||
        now - r.last_tsc > RepeatCycles) {
        r = {rip, reinterpret_cast<u64>(address), now, 0};
        return false;
    }
    r.last_tsc = now;
    if (++r.count < RepeatLimit) {
        return false;
    }
    r.count = 0;
    return true;
}
}

extern "C" int bbgpu_handle_fault(void* ucontext, void* address) {
    if (BbFreeCheck::OnTrapFault(ucontext, reinterpret_cast<std::uint64_t>(address))) {
        return 1;
    }
    if (bb_fault_recovers_only && runtime_fault_recover) {
        return 0;
    }
    if (Core::Signals::Instance()->DispatchAccessViolation(ucontext, address)) {
        if (FaultRepeats(ucontext, address)) {
            runtime_memory_restore_access(ucontext, reinterpret_cast<uintptr_t>(address), 1);
        }
        return 1;
    }
    if (BbFreeCheck::OnStaleTrapFault(reinterpret_cast<std::uint64_t>(address))) {
        return 1;
    }
    return runtime_memory_restore_access(ucontext, reinterpret_cast<uintptr_t>(address), 0);
}

extern "C" void bbnet_set_image(unsigned char* image, uint64_t size);
extern "C" void bbgpu_patch_image(unsigned char* image, uint64_t size) {
    bbnet_set_image(image, size);
    BbGnmHooks::PatchImage(image, size);
    BbEngine::Install(image, size);
}

extern "C" unsigned bbgpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

namespace Libraries::Kernel {
int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
}

namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
}

extern "C" int bbgpu_overlay_captures_input(void) {
    return BbOverlay::CapturesInput() ? 1 : 0;
}

extern "C" int bbgpu_window_in_background(void) {
    return Frontend::WindowInBackground() ? 1 : 0;
}

extern "C" int bbgpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int bbgpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
