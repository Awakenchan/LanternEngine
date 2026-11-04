// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "bbport_copy.h"

#include <deque>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <array>
#include <memory>
#include <optional>
#include <unordered_map>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

/// bbport: an IntervalList whose every change advances `generation`: lookups of the buffer
/// cache's residency remember their answers for as long as nothing changed (ResidencyMemo).
template <class IV = Interval>
class TrackedIntervalList : public IntervalList<IV> {
public:
    void Add(IV value) {
        ++generation;
        IntervalList<IV>::Add(value);
    }
    void Subtract(u64 start, u64 end) {
        ++generation;
        IntervalList<IV>::Subtract(start, end);
    }
    void Clear() {
        ++generation;
        IntervalList<IV>::Clear();
    }
    static inline u64 generation = 1;
};

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    static constexpr u64 ARENA_PAGE_BITS = 32;
    static constexpr u64 ARENA_PAGE_SIZE = u64{1} << ARENA_PAGE_BITS;
    static constexpr u64 NUM_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr, u64 guest_rip = 0);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// bbport: a new draw or dispatch is being recorded: the per-binding housekeeping of
    /// EnsureResident (unmaps, demotions, assets, late writes) runs once for it, not per binding.
    void NewPacket() noexcept {
        ++packet_epoch;
    }

    /// bbport (BB_FRAME_STATS): ObtainBuffer calls made for the copy shader HLE (set around
    /// them on the draw recording thread), counted by path: [read, written] x [in place, stream
    /// copy, VRAM], with their bytes.
    static inline bool hle_obtain = false;
    static inline std::array<u64, 6> hle_path_calls{}, hle_path_bytes{};
    static inline std::array<u64, 6> obtain_paths{};
    [[nodiscard]] u64 BindGeneration() const noexcept;
    [[nodiscard]] bool Persistent(const Buffer* buffer) const noexcept {
        return buffer != &stream_buffer && buffer != shadow_buffer.get();
    }
    void NoteBound(VAddr address, u64 size) {
        NoteUse(address, size);
    }

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// bbport (BB_DRAWREC_WORKERS, the lookahead thread): asks the cache for what ObtainBuffer
    /// reads first for this range (the arena table entry, the page tracking words); nothing is
    /// changed or read through. Returns the cache lines requested.
    u32 PrefetchBinding(VAddr device_addr, u32 size) const noexcept;

    /// bbport BB_GUEST_IN_PLACE: where a command processor write (WRITE_DATA) the GPU performs in
    /// stream order lands: the arena over the game's memory, if the range is bound there in place.
    /// Not counted as a GPU write of the block (those move blocks to VRAM): it is CPU data, as a PC
    /// game's constant updates. The range counts as GPU-modified until the GPU has done it, so no
    /// copy of it is taken before (IsRegionGpuModified).
    [[nodiscard]] std::optional<std::pair<const Buffer*, u64>> CommandWriteTarget(VAddr device_addr,
                                                                                u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// bbport (BB_PREUPLOAD): notes a guest mapping or unmapping (Rasterizer::MapMemory): the
    /// game's GPU memory (direct memory of type 3 with GPU access) is pre-uploaded.
    void NotePreuploadMapping(VAddr addr, u64 size, bool mapped);
    /// bbport (BB_PREUPLOAD): uploads up to `budget` bytes of that memory which the CPU changed and
    /// then left alone for a few frames into the arena, ahead of its use (draw recording thread).
    void Preupload(u64 budget);
    /// bbport (BB_GPU_WRITE_TWINS): forgets the twins of unmapped guest memory.
    void DropTwins(VAddr addr, u64 size);

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// bbport BB_GUEST_IN_PLACE: the game unmapped [addr, addr + size) (a guest thread): arena
    /// blocks bound to its memory there are unbound before the next binding (GPU side).
    void UnmapInPlace(VAddr addr, u64 size);
    /// Whether the range is bound to the game's own memory (no uploads, tracking or readbacks).
    [[nodiscard]] bool IsInPlace(VAddr addr, u64 size) const;
    /// bbport BB_GUEST_IN_PLACE without write tracking: [addr, addr + size) gets data through a path
    /// the GPU side hears of (file reads, the game's resource loaders, DMA): its blocks may then
    /// keep a VRAM copy (any thread).
    void NoteAssetWrite(VAddr addr, u64 size);
    /// bbport: the game's GPU memory allocator handed out [addr, addr + size) again (any thread): its
    /// blocks lose the asset mark and are bound in place, since what writes them next may be code
    /// the GPU side does not hear of.
    void NoteFreshRange(VAddr addr, u64 size);
    /// bbport, without write tracking: the command processor wrote `data` at addr in decode order.
    /// Where the GPU's data there lives in VRAM (GPU-modified, not in place) the same bytes are
    /// written into it in stream order and true is returned; else the caller invalidates.
    bool UpdateGpuWritten(VAddr addr, std::span<const u8> data);
    /// bbport: copies of [addr, addr + size) in VRAM (ShadowCopy) are stale from here on in stream
    /// order: the GPU or the command processor wrote there (GPU side thread).
    void DropShadows(VAddr addr, u64 size);
    /// bbport, without write tracking (any thread): the command processor wrote these bytes into the
    /// game's memory outside the command stream (labels, timestamps, occlusion results; through
    /// the backing mapping, which nothing sees). Where VRAM holds a copy of them it gets them too, at
    /// the next binding.
    void NoteLateCommandWrite(VAddr addr, const void* data, u64 size);
    /// bbport, without write tracking (any thread): the CPU wrote [addr, addr + size) next to data
    /// the GPU keeps in VRAM. Exactly those bytes are copied into VRAM from the game's memory at the
    /// next binding: no readback of the GPU's data, nothing uploaded over it.
    void NotePreciseUpload(VAddr addr, u64 size);
    /// bbport: the CPU wrote [addr, addr + size) where the GPU side asked to hear of it (no write
    /// tracking): counted per block like a write fault, for moving per-frame data in place.
    void NoteCpuWriteRange(VAddr addr, u64 size);
    /// Whether any part of the range is.
    [[nodiscard]] bool IsAnyInPlace(VAddr addr, u64 size) const {
        return GuestInPlace() && size != 0 &&
               in_place_blocks.Overlaps(addr >> block_shift, ((addr + size - 1) >> block_shift) + 1);
    }

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    /// `in_place`: bind what can be in place even when Garlic (texture data, read once into an
    /// image: a VRAM copy of it would only cost an upload).
    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block,
                        bool in_place = false);
    /// BB_GUEST_IN_PLACE: binds the blocks of [start, end) that are direct memory in a guest memory
    /// chunk to that memory; the others are added to `rest` (VRAM, as before).
    struct ResidentBind {
        u64 start, end; ///< blocks
        vk::DeviceMemory memory;
        u64 offset;
    };
    void BindInPlace(u64 start, u64 end, std::vector<ResidentBind>& out, IntervalList<>& rest,
                     bool any_type = false);
    /// bbport: the housekeeping EnsureResident did before every binding (Process*), once a packet.
    void Maintain();
    u64 packet_epoch = 1, maintained_epoch = 0;
    /// bbport: answers of EnsureResident ("all resident") and IsInPlace for block ranges, valid
    /// while resident_ranges and in_place_blocks keep their generation.
    struct ResidencyMemo {
        u64 first = ~0ull, last = 0, generation = 0;
        bool resident = false, in_place = false;
    };
    std::array<ResidencyMemo, 256> residency_memo{};
    static u64 ResidencyGeneration() noexcept {
        return TrackedIntervalList<Backing>::generation + TrackedIntervalList<>::generation;
    }
    void ProcessPendingUnmaps();
    /// bbport BB_GUEST_IN_PLACE: a write fault (guest thread). Blocks the CPU keeps writing (faults in
    /// 3 frames of 60) move from VRAM to the game's memory in place: dynamic data, like an upload
    /// heap; static data stays in VRAM, like a default heap.
    void NoteCpuWrite(VAddr address, u64 guest_rip);
    /// GPU side: moves the requested blocks once the submission that may still upload into their
    /// VRAM copy was submitted; the rebind waits for it on the GPU (bind_wait_tick).
    void ProcessDemotions();
    void ProcessPendingAssets();
    void ProcessLateWrites();
    /// BB_FRAME_STATS: GPU-written bindings over 64 KiB, reported every 5 s (where they are bound,
    /// which images lie over them).
    void NoteLargeGpuWrite(VAddr addr, u64 size);
    /// bbport BB_GUEST_IN_PLACE on a discrete GPU, BB_SHADOWS=1 (off by default: 85 copies, 20 MB a
    /// frame in Yahar'gul cost more than reading in place, 48 -> 44 FPS): a read-only binding of data the
    /// CPU writes in place is copied once per submission by the GPU into VRAM and bound there, as a
    /// PC game copies an upload heap into a default heap: one sequential copy over PCIe instead of
    /// every shader reading it across the bus.
    std::optional<std::pair<const Buffer*, u64>> ShadowCopy(VAddr addr, u64 size);
    struct Shadow {
        VAddr addr;
        u64 size, offset;
    };
    std::unique_ptr<StreamBuffer> shadow_buffer;
    std::vector<Shadow> shadows; ///< this submission's copies
    u64 shadow_tick = 0;
    /// BB_TRACE_SIZE=bytes (diagnostics): bindings over GPU-written buffers of that size, counted by
    /// the path they take, reported every 5 s.
    void TraceBinding(VAddr addr, u64 size, bool is_written, int kind);
    std::vector<std::pair<VAddr, u64>> traced_ranges;
    std::array<u64, 10> trace_counts{};
    u64 trace_texel = 0, trace_min = ~0ull, trace_max = 0;
    std::chrono::steady_clock::time_point trace_report{};
    struct LargeWrite {
        VAddr addr = 0;
        u64 size = 0, count = 0;
        u64 fresh = 0; ///< ranges handed out again over it in the window
        bool in_place = false;
        std::string images;
    };
    std::unordered_map<u64, LargeWrite> large_writes;
    std::chrono::steady_clock::time_point large_writes_report{};
    /// Without write tracking: [addr, addr + size) is bound for GPU writes (GPU side thread).
    void NoteGpuWrite(VAddr addr, u64 size);
    /// Whether a block may keep a VRAM copy without write tracking: Garlic data only loaders wrote.
    bool VramEligible(u64 block, bool garlic_known = false);
    /// Called as a submission goes out (its tick `submitted`): candidate blocks get VRAM for the next
    /// submission, bound once `submitted` is done on the GPU, filled from the game's memory there.
    /// Called as a submission goes out: blocks partly handed out again whose GPU data is in VRAM get
    /// that data copied back into the game's memory at its end, and are bound in place for the
    /// next submission (once `submitted` is done).
    void QueueCopyBacks(u64 submitted);
    void QueuePromotions(u64 submitted);
    /// Arena residency memory from the 64 MiB blocks: the memory and the byte offset in it.
    std::pair<vk::DeviceMemory, u64> AllocateResidency(u64 bytes);
    /// bbport: BbStats::residency_unused_bytes after the free list or the current block changed.
    void NoteResidencyUnused();
    /// bbport: idle VRAM blocks (see group_use).
    void NoteUse(VAddr address, u64 size);
    void ProcessIdleBlocks();
    /// bbport: vram_level from the driver's usage and budget (BB_VRAM_PRESSURE_PERCENT, 85).
    void UpdateVramLevel(u32 now);
    /// A VRAM slot of a block that left it: reused by the next move to VRAM.
    void ReleaseResidencySlot(vk::DeviceMemory memory, u64 offset);
    /// Unbinds a VRAM block of memory the game unmapped (uploaded again if mapped and used).
    void EvictVramBlock(u64 block);
    /// bbport: without the PC memory model, while VRAM is tight (vram_level): VRAM copies of the
    /// game's data unused for 30 s (10 s when full) leave VRAM; their slots take the next blocks
    /// instead of new VRAM, and they are uploaded again from the game's memory when bound.
    void EvictIdleCopies(u32 now);
    u32 last_cleanup_scan = 0; ///< bbport: EvictIdleCopies with room in VRAM, every 10 s
    /// Residency chunks without blocks for 5 s, the rebinds away from them done: back to the driver.
    void FreeEmptyChunks(u32 now);
    [[nodiscard]] static u32 VramIdleSeconds();
    /// BB_CHUNK_TEXTURES: texture data read by the GPU straight from the guest memory chunk it is
    /// in (no CPU copy, no VRAM buffer copy), when the range is contiguous there.
    std::optional<std::pair<const Buffer*, u64>> GuestChunkSource(VAddr address, u64 size);
    /// bbport BB_CHUNK_BUFFERS: how a binding of the game's memory in place reaches the GPU.
    enum ChunkRoute : u8 {
        RouteChunk,        ///< its chunk's own buffer
        RouteOutside,      ///< in place, not within one chunk (or one mapping): the arena
        RoutePartly,       ///< partly in place, partly in VRAM: the arena
        RouteImageArena,   ///< texture data in place, not within one chunk: the arena
        RouteCommandArena, ///< a command processor write in place, not within one chunk: the arena
        ChunkRoutes,
    };
    /// The in-place range [addr, addr + size) of the arena: through `chunk` (its chunk's buffer)
    /// when there is one, else through the arena, where the runtime keeps barriers on both sides
    /// of it (Runtime::NoteAliasedAccess). Counted, and with BB_CHUNK_VERIFY checked.
    std::pair<const Buffer*, u64> RouteInPlace(ChunkRoute arena_route, const Buffer* arena,
                                               VAddr addr, u64 size, bool is_written);
    void CountRoute(ChunkRoute route, u64 size, bool is_written) noexcept {
        chunk_route_calls[route].fetch_add(1, std::memory_order_relaxed);
        chunk_route_bytes[route].fetch_add(size, std::memory_order_relaxed);
        if (is_written) {
            chunk_route_written[route].fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::array<std::atomic<u64>, ChunkRoutes> chunk_route_calls{}, chunk_route_bytes{},
        chunk_route_written{};
    /// BB_CHUNK_VERIFY: the arena's binding of the range's blocks must be the chunk's memory at the
    /// chunk route's offsets (every binding); some ranges (16 a second, up to 4 KiB) are copied on
    /// the GPU through the arena, the chunk and the arena again, and compared when done
    /// (ProcessChunkChecks; a range the CPU changed between the two arena copies is left out).
    void VerifyChunkRoute(const Buffer* arena, VAddr addr, u64 size,
                          std::pair<const Buffer*, u64> chunk);
    void ProcessChunkChecks();
    struct ChunkCheck {
        u64 tick;
        VAddr addr;
        u32 size;
        u32 slot;
    };
    static constexpr u32 VerifySlots = 64, VerifyBytes = 4096;
    std::unique_ptr<Buffer> verify_buffer;
    std::vector<ChunkCheck> chunk_checks;
    u64 verify_free_slots = ~0ull; ///< a bit per slot of verify_buffer
    u32 verify_second = 0, verify_in_second = 0;
    u64 verify_compared = 0, verify_differed = 0, verify_changed = 0, verify_unmapped = 0;
    u32 verify_reports = 0;

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    /// The image over [device_addr, device_addr + size), if any, written into `buffer` at `offset`
    /// (the arena, or the range's chunk with BB_CHUNK_BUFFERS).
    bool SynchronizeMemoryFromImage(const Buffer* buffer, u64 offset, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    // bbport (BB_IMAGE_TILE_MEMO): the last fills of texel buffer ranges from the image over
    // them. A fill is left out while the image, its scene proxy, the buffer range and its
    // residency are all as they were after the previous one.
    struct TileMemo {
        VAddr address = 0;
        u32 size = 0;
        const Buffer* arena = nullptr;
        u64 arena_offset = 0;
        u64 image_uid = 0;
        const void* backing = nullptr;
        u64 write_seq = 0;
        u32 flags = 0;
        u64 watch = 0;
        u64 residency = 0;
        u64 write_tick = 0;
    };
    std::array<TileMemo, 8> tile_memos{};
    u32 tile_memo_next = 0;
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, NUM_ARENA_PAGES> address_space{};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    std::mutex preupload_mutex;               ///< the map hooks come from guest threads
    std::map<VAddr, VAddr> preupload_ranges; ///< start -> end of pre-uploaded guest memory
    VAddr preupload_cursor = 0;
    u64 preupload_first_bytes = 0, preupload_again_bytes = 0; ///< BB_FRAME_STATS report
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset; ///< in blocks, like start and end (SubRange, CanMergeWith)
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    TrackedIntervalList<Backing> resident_ranges;
    /// bbport BB_GUEST_IN_PLACE: blocks bound to the game's own memory, and guest unmaps to apply.
    TrackedIntervalList<> in_place_blocks;
    std::mutex pending_unmaps_mutex;
    std::vector<std::pair<VAddr, u64>> pending_unmaps;
    std::atomic<bool> unmaps_pending{false};
    std::mutex dynamic_mutex;
    std::unordered_map<u64, std::pair<u32, u32>> cpu_write_frames; ///< block -> last frame, frames
    std::unordered_map<u64, std::pair<u32, u32>> gpu_write_frames; ///< written range -> last frame, frames
    std::vector<u64> demote_requests;
    std::atomic<bool> demotes_requested{false};
    struct Demotion {
        u64 block;
        u64 tick;         ///< requested at
        bool cpu_written; ///< written by the CPU often: in place from now on (else: handed out again)
        bool idle = false;  ///< bbport: not bound for BB_VRAM_IDLE_SECONDS (ProcessIdleBlocks)
        bool force = false; ///< bbport: its chunk is emptied, idle or not (back once bound)
    };
    std::vector<Demotion> demotions_waiting;
    IntervalList<> dynamic_blocks;                     ///< bound in place even when Garlic
    IntervalList<> gx_upload_blocks;
    /// Bytes last written through a path the GPU side hears of (file reads, the game's resource
    /// loaders, DMA) and not handed out again by the game's GPU memory allocator since. Only blocks
    /// made of such bytes keep a VRAM copy: anything else in a block may be written by code we do
    /// not hear of.
    IntervalList<> asset_bytes;
    IntervalList<> promote_candidates; ///< in-place blocks that may get a VRAM copy again
    IntervalList<> copy_back_blocks;   ///< VRAM blocks with GPU data, handed out again in part
    IntervalList<> gpu_written_bytes;  ///< bound for GPU writes since handed out (known data too)
    u64 promoted_bytes = 0;
    /// VRAM slots of blocks that left VRAM, for the next blocks moved there (in their chunks).
    u64 free_slot_count = 0;
    std::mutex pending_assets_mutex;
    struct PendingRange {
        VAddr addr;
        u64 size;
        bool fresh; ///< handed out again by the game's GPU memory allocator (else: an asset)
    };
    std::vector<PendingRange> pending_assets;
    std::atomic<bool> assets_pending{false};
    std::mutex late_writes_mutex;
    std::vector<std::pair<VAddr, std::vector<u8>>> late_writes;
    std::vector<std::pair<VAddr, u64>> precise_uploads;
    std::atomic<bool> late_writes_pending{false};
    u64 bind_wait_tick = 0;                            ///< the next arena binds wait for it
    std::array<std::unique_ptr<Buffer>, 256> chunk_buffers{}; ///< per guest memory chunk (Chunk::index)

    u32 arena_memory_type_index{};
    /// bbport: a memory type the arena takes outside the card's own heaps (discrete GPUs): new
    /// blocks of the game's data go there while VRAM is tight or full (AllocateResidency).
    std::optional<u32> system_memory_type_index;
    u64 system_residency_bytes = 0;
    u32 system_residency_reports = 0;
    vk::DeviceMemory residency_memory{}; ///< bbport: the 64 MiB block arena residency comes from
    u64 residency_size = 0, residency_used = 0;
    /// bbport: the next block, allocated in the background. Stage B waits for it at most 50 ms
    /// (TakeSpareResidency): a std::future here once never became ready and froze the game.
    struct SpareBlock {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false, abandoned = false;
        vk::DeviceMemory memory{};
    };
    std::shared_ptr<SpareBlock> spare_residency;
    u32 spare_residency_timeouts = 0;
    vk::DeviceMemory TakeSpareResidency();
    /// bbport: VRAM blocks no longer needed. Per 2 MiB of guest addresses, the second
    /// (BbStats::coarse_second) it was last bound: blocks in VRAM unused for BB_VRAM_IDLE_SECONDS
    /// (60, 0: never) go back to the game's memory, and residency chunks left without blocks
    /// go back to the driver (ProcessIdleBlocks). Blocks of unmapped memory leave VRAM too.
    static constexpr u64 USE_GROUP_BITS = 21;
    std::vector<u32> group_use;
    /// Groups with blocks moved in place for idleness, and those blocks: bound again, they get a
    /// VRAM copy again (QueuePromotions). Else the GPU kept reading them over PCIe (FPS 90 -> 55).
    std::vector<u8> group_demoted;
    IntervalList<> idle_demoted;
    IntervalList<> evacuated_demoted; ///< the part of idle_demoted an evacuation moved, idle or not
    /// bbport BB_DYNAMIC_RETRY: blocks moved in place for CPU writes (dynamic_blocks) get a VRAM copy
    /// again once their wait is over and the GPU still reads them (ProcessDynamicRetries); moved in
    /// place again, they wait twice as long. Without it they stayed in place for good: a block the
    /// game's code wrote in 3 frames, once, was read over PCIe for the rest of the session (1.1 GiB
    /// of them after five minutes, FPS 90 -> 55, RX 9070 XT, 2026-10-21).
    struct DynamicRetry {
        u32 since = 0;   ///< the coarse second it went in place (or was last retried)
        u32 strikes = 0; ///< times it went in place for CPU writes
    };
    std::unordered_map<u64, DynamicRetry> dynamic_retry;
    void ProcessGxUploadArena();
    void NoteDynamic(u64 block);
    void ProcessDynamicRetries(u32 now);
    /// Blocks QueuePromotions gave a VRAM copy: uploaded whole at the next packet. Their slot may
    /// hold another block's bytes, and vertex buffers are bound without a size and shaders read
    /// through the page table (EnsureResident clears reused slots for the same reason).
    std::vector<u64> promote_fills;
    void ProcessPromoteFills();
    /// bbport: what moved between VRAM and the game's memory and why, reported once a minute with
    /// the PC memory model (ReportResidency).
    enum PromoteSource : u8 {
        PromoteAsset,
        PromoteGpuWritten,
        PromoteIdle,
        PromoteEvacuated,
        PromoteRetry,
        PromoteSources,
    };
    enum DemoteReason : u8 {
        DemoteCpuWritten,
        DemoteHandedOut,
        DemoteIdle,
        DemoteEvacuated,
        DemoteCopiedBack,
        DemoteReasons,
    };
    std::unordered_map<u64, u8> promote_source; ///< why each of promote_candidates is there
    std::array<u64, PromoteSources> promoted_by{};
    std::array<u64, DemoteReasons> demoted_by{};
    std::atomic<u64> bound_in_place_bytes{0}, bound_dynamic_bytes{0};
    /// Sparse binds before submissions (recording threads): calls, calls that waited for a
    /// submission to finish on the GPU, ranges, nanoseconds in vkQueueBindSparse and the longest.
    std::atomic<u64> bind_calls{0}, bind_waits{0}, bind_ranges{0}, bind_ns{0}, bind_max_ns{0};
    u32 residency_report_second = 0;
    u64 residency_report_frame = 0;
    void ReportResidency(u32 now);
    struct ResidencyChunk {
        u64 size;
        u64 used;         ///< bytes handed out and not returned
        u32 empty_second; ///< when `used` dropped to 0
        u64 empty_tick;   ///< the submission then being recorded (it carries the rebinds)
        std::vector<u64> free_offsets; ///< slots of blocks that left (byte offsets)
        u32 evacuated_second = 0;      ///< when its blocks were last sent away (ProcessIdleBlocks)
    };
    std::unordered_map<VkDeviceMemory, ResidencyChunk> residency_chunks;
    u32 idle_scan_second = 0;
    u32 last_evacuation = 0; ///< ProcessIdleBlocks: when a chunk was last emptied
    /// bbport: how full VRAM is against the driver's budget, once a second (UpdateVramLevel):
    /// 0 room, 1 tight (idle VRAM copies go back sooner), 2 full (no new ones either).
    std::atomic<int> vram_level{0};
    u32 vram_full_until = 0; ///< a failed VRAM allocation counts as full until then
    u32 vram_level_reports = 0;
    /// bbport: the second a shader last read the game's memory through the page table
    /// (SynchronizeDmaBuffers). Those reads do not mark blocks as used, so nothing leaves VRAM
    /// while such shaders run (EvictIdleCopies).
    std::atomic<u32> dma_second{0};
    u64 evicted_bytes = 0;
    /// Blocks EvictIdleCopies took out of VRAM: they come back with their whole 2 MiB group.
    TrackedIntervalList<> evicted_blocks;
    u32 evict_reports = 0, dma_kept_reports = 0, dma_kept_second = 0;
    /// The seconds a VRAM copy may go unused before it goes back in place, at this level.
    u32 idle_seconds_now = 60;
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    /// bbport: arena pages a new arena covers at most (an aligned group, within the driver's
    /// largest buffer): the game's memory in one arena, never migrated (GetArena).
    u64 arena_group_pages{1};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
