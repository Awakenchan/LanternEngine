#pragma once
#include <array>
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "lantern_switches.h"

extern "C" std::uint64_t runtime_disabled_optimizations;
extern "C" std::uint64_t runtime_experiment_bits;
#ifdef _WIN32
struct alignas(16) BbRecoverBuf {
    unsigned char registers[256];
};
extern "C" __thread BbRecoverBuf* runtime_fault_recover;
extern "C" int runtime_setjmp(BbRecoverBuf* buf) __attribute__((returns_twice));
#define BB_RECOVER_SET(buf) runtime_setjmp(&(buf))
#else
typedef sigjmp_buf BbRecoverBuf;
extern "C" __thread BbRecoverBuf* runtime_fault_recover;
#define BB_RECOVER_SET(buf) sigsetjmp(buf, 0)
#endif
inline thread_local bool bb_fault_recovers_only = false;

namespace BbToggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    RecorderFences = 1ull << 41,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50,
    UpdateImageFastPath = 1u << 30,
    TextureCollector = 1ull << 31,
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    TaaKeepNearerHistory = 1ull << 55,
    Fsr4MotionYFlip = 1ull << 56,
    SceneMipBias = 1ull << 57,
    ParallelRecording = 1ull << 58,
    ReadbackQueue = 1ull << 59,
    CameraOwnInverse = 1ull << 60,
    ForcedAniso = 1ull << 61,
    CameraYFlip = 1ull << 62,
    Fsr4EncodedInput = 1ull << 63,
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
inline bool Experiment(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_experiment_bits, __ATOMIC_RELAXED) & bit) != 0;
}
}

namespace BbStats {
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::uint64_t> cpu_write_notes{0}, cpu_write_note_bytes{0};
inline std::atomic<std::uint64_t> gpu_range_allocs{0}, gpu_range_alloc_bytes{0};
inline std::atomic<std::uint64_t> bound_in_place_bytes{0}, bound_vram_bytes{0};
inline std::atomic<std::uint64_t> bound_in_place_written_bytes{0};
inline std::atomic<std::uint64_t> gpu_data_updates{0}, copied_back_blocks{0}, readbacks{0};
inline std::atomic<std::uint64_t> late_vram_writes{0};
inline std::atomic<std::uint64_t> shadow_copies{0}, shadow_bytes{0};
inline std::atomic<std::uint64_t> idle_flushes{0};
inline std::atomic<std::uint64_t> eop_decoded{0}, eop_written{0};
inline std::atomic<std::uint64_t> submits_allowed_queries{0}, submits_refused{0}, submit_done_calls{0};
inline std::atomic<std::uint64_t> gnm_frame_waits{0}, gnm_frame_wait_ns{0};
inline std::atomic<std::uint64_t> asc_submits{0};
inline std::array<std::atomic<std::uint64_t>, 4> range_allocators{}, range_live{}, range_bytes{};
inline std::atomic<std::int64_t> hot_pages{0};
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1};
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0};
inline std::atomic<std::uint64_t> gpu_frames{0};
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0};
inline std::atomic<std::uint64_t> t_image_query{0}, t_image_alloc{0}, t_refresh_hash{0},
    t_refresh_obtain{0}, t_refresh_detile{0};
inline const bool& enabled = Lantern::Switches::Load().frame_stats;
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0}, host_copy_waits_skipped{0};
inline std::atomic<std::uint64_t> present_wait_ns{0};
inline std::atomic<std::uint32_t> frame_number{0};
inline std::atomic<std::uint64_t> preupload_bytes{0};
inline std::atomic<std::uint64_t> device_alloc_bytes{0}, device_free_bytes{0};
inline std::atomic<std::uint64_t> live_image_bytes{0}, live_images{0};
inline std::atomic<std::uint64_t> vk_image_bytes{0};
inline std::atomic<std::uint64_t> residency_alloc_bytes{0}, residency_unused_bytes{0};
inline std::atomic<std::uint64_t> gc_used_bytes{0}, gc_trigger_bytes{0}, gc_freed_images{0};
inline std::atomic<std::uint32_t> coarse_second{0};
inline std::atomic<std::uint64_t> vram_idle_bytes{0}, vram_unmapped_bytes{0}, vram_chunks_freed_bytes{0};
inline std::atomic<std::uint64_t> residency_chunk_count{0}, evacuations{0}, evac_kept_gpu{0}, evac_kept_other{0};
inline std::atomic<std::uint64_t> gc_kept_images{0};
inline std::atomic<std::uint64_t> motion_sync_calls{0}, motion_sync_waits{0}, motion_sync_ns{0},
    motion_sync_max_ns{0};
inline std::atomic<std::uint64_t> motion_frame_waits{0}, motion_frame_wait_ns{0},
    motion_frame_wait_max_ns{0};
inline std::atomic<std::uint64_t> sync_recording_calls{0};
inline void NoteWait(std::atomic<std::uint64_t>& count, std::atomic<std::uint64_t>& total,
                     std::atomic<std::uint64_t>& max, std::chrono::steady_clock::time_point start) {
    const std::uint64_t ns = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - start)
                                               .count());
    count.fetch_add(1, std::memory_order_relaxed);
    total.fetch_add(ns, std::memory_order_relaxed);
    for (std::uint64_t seen = max.load(std::memory_order_relaxed);
         seen < ns && !max.compare_exchange_weak(seen, ns, std::memory_order_relaxed);) {
    }
}
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count(),
                        std::memory_order_relaxed);
    }
};
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
enum ProtectKind : unsigned {
    ProtectUpload,
    ProtectTexture,
    ProtectOther,
    ProtectDeferred,
    ProtectKinds,
};
inline std::array<std::atomic<std::uint64_t>, ProtectKinds> stage_b_protect_calls{},
    stage_b_protect_ns{};
inline std::atomic<std::uint64_t> other_protect_calls{0}, other_protect_ns{0};
inline std::atomic<std::uint64_t> deferred_protect_pages{0}, deferred_protect_calls{0},
    deferred_protect_ns{0}, copy_flushes{0}, deferred_cancelled_pages{0};
enum WaitSite : unsigned {
    WaitWriteData,
    WaitDma,
    WaitFlip,
    WaitEos,
    WaitEop,
    WaitOther,
    WaitSites,
};
inline constexpr const char* WaitSiteNames[WaitSites] = {"WriteData", "DMA", "flip", "EOS", "EOP",
                                                         "other"};
inline std::array<std::atomic<std::uint64_t>, WaitSites> signal_waits{}, signal_wait_ns{},
    signal_waits_skipped{}, copy_waits{}, copy_wait_ns{};
inline std::atomic<std::uint64_t> fence_barriers{0};
inline std::atomic<std::uint64_t> dma_on_cpu{0}, dma_on_gpu{0};
inline std::atomic<std::uint64_t> lookahead_packets{0}, lookahead_draws{0}, lookahead_lines{0},
    lookahead_behind{0}, lookahead_waits{0}, lookahead_lead{0}, lookahead_busy_cycles{0},
    lookahead_recovered{0};
}
