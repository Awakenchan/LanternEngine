// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_object_motion.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <vk_mem_alloc.h>
#include <xxhash.h>
#include "bbport_settings.h"
#include "bbport_threads.h"
#include "bbport_toggles.h"
#include "common/thread.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

bool CreateBuffer(const Instance& instance, vk::DeviceSize size, bool host, vk::Buffer& buffer,
                  VmaAllocation& allocation, void** mapped, u64& address,
                  VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo alloc_ci{
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    if (host) {
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &raw, &allocation, &info) !=
        VK_SUCCESS) {
        return false;
    }
    buffer = raw;
    if (mapped) {
        *mapped = info.pMappedData;
    }
    address = instance.GetDevice().getBufferAddress({.buffer = buffer});
    return address != 0;
}

} // namespace

vk::UniqueDescriptorSetLayout ObjectMotion::CreateAddressLayout(vk::Device device) {
    const vk::DescriptorSetLayoutBinding binding{
        .binding = Shader::MotionVectors::AddressBinding,
        .descriptorType = vk::DescriptorType::eUniformBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eVertex,
    };
    auto [result, layout] = device.createDescriptorSetLayoutUnique(
        {.bindingCount = 1, .pBindings = &binding});
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error("Cannot create object-motion address layout");
    }
    return std::move(layout);
}

void ObjectMotion::BindAddresses(vk::PipelineLayout layout) {
    ASSERT(enabled && address_set);
    scheduler.Record([layout, set = address_set](vk::CommandBuffer command) {
        command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout,
                                   Shader::MotionVectors::DescriptorSet, set, {});
    });
}

ObjectMotion::ObjectMotion(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    // Pipeline selection limits the extra attachment and vertex stores to likely
    // animated draws. A setting or environment override can still disable the path.
    if (!BbSettings::Get().object_motion ||
        BbSettings::Get().upscaler == BbSettings::UpscalerOff) {
        std::puts("Object motion: off (enable in menu or BB_OBJECT_MOTION=1)");
        return;
    }
    const auto& features = instance.GetPhysicalDevice().getFeatures();
    if (!features.vertexPipelineStoresAndAtomics) {
        std::printf("Object motion: vertexPipelineStoresAndAtomics unsupported, off\n");
        return;
    }
    void* mapped = nullptr;
    u64 params_address = 0, positions_address = 0;
    if (!CreateBuffer(instance, vk::DeviceSize(1 + FrameSlots * ParamsPerFrame) * 32, true,
                      params_buffer, params_allocation, &mapped, params_address) ||
        !CreateBuffer(instance, vk::DeviceSize(1 + 2 * PositionsPerFrame) * 16, false,
                      positions_buffer, positions_allocation, nullptr, positions_address)) {
        std::printf("Object motion: buffer creation failed, off\n");
        return;
    }
    params_mapped = static_cast<u32*>(mapped);
    std::memset(params_mapped, 0, 32);
    // Immutable until destruction, after scheduler.Finish(). Parameters and position history
    // retain their existing rings/barriers; only their base addresses move out of SPIR-V.
    // A small UBO avoids a >128 MiB SSBO descriptor (not portable on all supported devices).
    u64 unused_address{};
    if (!CreateBuffer(instance, sizeof(Shader::MotionVectors::Addresses), true, address_buffer,
                      address_allocation, &mapped, unused_address, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
        std::puts("Object motion: address table allocation failed, off");
        return;
    }
    const Shader::MotionVectors::Addresses addresses{params_address, positions_address};
    address_table = addresses;
    std::memcpy(mapped, &addresses, sizeof(addresses));
    const auto device = instance.GetDevice();
    try {
        address_layout = CreateAddressLayout(device);
    } catch (const std::exception&) {
        // Complete construction in the disabled state so our destructor owns the buffers
        // already allocated above, just as on the other optional-motion allocation failures.
        std::puts("Object motion: descriptor layout allocation failed, off");
        return;
    }
    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eUniformBuffer, 1};
    auto [pool_result, pool] = device.createDescriptorPoolUnique(
        {.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size});
    if (pool_result != vk::Result::eSuccess) {
        std::puts("Object motion: descriptor pool allocation failed, off");
        return;
    }
    address_pool = std::move(pool);
    const vk::DescriptorSetLayout layout = *address_layout;
    auto [set_result, sets] = device.allocateDescriptorSets(
        {.descriptorPool = *address_pool, .descriptorSetCount = 1, .pSetLayouts = &layout});
    if (set_result != vk::Result::eSuccess) {
        std::puts("Object motion: descriptor allocation failed, off");
        return;
    }
    address_set = sets[0];
    const vk::DescriptorBufferInfo buffer_info{
        .buffer = address_buffer, .offset = 0, .range = sizeof(addresses),
    };
    const vk::WriteDescriptorSet write{
        .dstSet = address_set,
        .dstBinding = Shader::MotionVectors::AddressBinding,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eUniformBuffer,
        .pBufferInfo = &buffer_info,
    };
    device.updateDescriptorSets(write, {});
    // bbport: BB_OBJECT_MOTION_HOLD=<frames> (default 30, a quarter second at 120 FPS): how long
    // a small skeleton (weapon, prop) keeps its history after its bone palette last changed, so
    // bones the game moves on some frames only still get object vectors (Motion::History::
    // Moving). 0 is the old rule: history only on frames whose palette changed.
    static const u32 hold = [] {
        const char* value = std::getenv("BB_OBJECT_MOTION_HOLD");
        return value && value[0] ? u32(std::strtoul(value, nullptr, 10)) : 30u;
    }();
    history.SetHold(hold);
    enabled = true;
    static const bool worker_wanted = [] {
        const char* env = std::getenv("LANTERN_MOTION_WORKER");
        if (env && env[0]) {
            return env[0] == '1';
        }
        return BbThreads::Available() >= 12;
    }();
    if (worker_wanted) {
        jobs = std::make_unique<Job[]>(JobCapacity);
        async = true;
        worker = std::jthread([this](std::stop_token stop) { WorkerLoop(stop); });
    }
    std::printf("Object motion: on (%u vertices per frame, moved small skeletons held %u frames, "
                "bookkeeping on %s)\n",
                PositionsPerFrame, hold, async ? "its own thread" : "the recording thread");
}

void ObjectMotion::StopWorker() {
    if (!async) {
        return;
    }
    Sync();
    worker.request_stop();
    worker_wake.fetch_add(1, std::memory_order_seq_cst);
    worker_wake.notify_one();
    worker.join();
    async = false;
}

ObjectMotion::~ObjectMotion() {
    StopWorker();
    scheduler.Finish();
    const auto allocator = instance.GetAllocator();
    if (address_buffer) {
        vmaDestroyBuffer(allocator, address_buffer, address_allocation);
    }
    if (params_buffer) {
        vmaDestroyBuffer(allocator, params_buffer, params_allocation);
    }
    if (positions_buffer) {
        vmaDestroyBuffer(allocator, positions_buffer, positions_allocation);
    }
}

void ObjectMotion::InvalidateHistory() {
    if (async) {
        Post({.kind = JobKind::Invalidate});
        return;
    }
    Run({.kind = JobKind::Invalidate});
}

void ObjectMotion::AdvanceHistory() {
    if (BbStats::enabled && (history_frame % 600) == 0 && history.stats.draws) {
        const auto& s = history.stats;
        // bbport: "held" counts small-skeleton draws kept in history though their palette did
        // not change this frame (BB_OBJECT_MOTION_HOLD); they are part of "draws".
        std::printf("Object motion: %llu draws, %llu stored, %llu with history, %llu unmatched, "
                    "%llu capacity skips, %llu invalid, %llu still, %llu held (600 frames); last frame "
                    "%u/%u vertices; index ranges %llu reused, %llu scanned (%llu found changed), "
                    "%zu cached\n",
                    (unsigned long long)s.draws, (unsigned long long)s.stored,
                    (unsigned long long)s.loaded, (unsigned long long)s.unmatched,
                    (unsigned long long)s.exhausted, (unsigned long long)s.invalid,
                    (unsigned long long)s.still, (unsigned long long)s.held,
                    history.Used(), PositionsPerFrame, (unsigned long long)index_ranges.stats.hits,
                    (unsigned long long)index_ranges.stats.scans,
                    (unsigned long long)index_ranges.stats.stale, index_ranges.Size());
        index_ranges.stats = {};
        history.stats = {};
    }
    history.NextFrame();
    ++history_frame;
    if (history_frame % Motion::IndexRangeCache::Unused == 0) {
        index_ranges.Trim(history_frame);
    }
}

void ObjectMotion::Run(const Job& job) {
    switch (job.kind) {
    case JobKind::Draw:
        WriteParam(job.param, Evaluate(job.draw));
        break;
    case JobKind::FrameStart:
        AdvanceHistory();
        break;
    case JobKind::Invalidate:
        history.NextFrame();
        history.NextFrame();
        break;
    }
}

void ObjectMotion::Post(const Job& job) {
    const u64 at = posted.load(std::memory_order_relaxed);
    while (at - done.load(std::memory_order_acquire) >= JobCapacity) {
        __builtin_ia32_pause();
    }
    jobs[at % JobCapacity] = job;
    posted.store(at + 1, std::memory_order_seq_cst);
    if (worker_sleeping.load(std::memory_order_seq_cst)) {
        worker_wake.fetch_add(1, std::memory_order_seq_cst);
        worker_wake.notify_one();
    }
}

void ObjectMotion::Sync() {
    if (!async) {
        return;
    }
    const u64 target = posted.load(std::memory_order_acquire);
    for (u32 spins = 1; done.load(std::memory_order_acquire) < target; ++spins) {
        if (spins < 4096) {
            __builtin_ia32_pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void ObjectMotion::WorkerLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("bb:Motion");
    static const auto spin_time =
        std::chrono::microseconds(BbThreads::CapSpinUs(BbThreads::Available() >= 12 ? 200 : 50));
    u64 next = 0;
    while (true) {
        if (posted.load(std::memory_order_acquire) == next) {
            const auto spin_until = std::chrono::steady_clock::now() + spin_time;
            for (u32 spins = 1; posted.load(std::memory_order_acquire) == next; ++spins) {
                if (stop.stop_requested()) {
                    return;
                }
                __builtin_ia32_pause();
                if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                    const u32 seen = worker_wake.load(std::memory_order_seq_cst);
                    worker_sleeping.store(true, std::memory_order_seq_cst);
                    if (posted.load(std::memory_order_seq_cst) == next && !stop.stop_requested()) {
                        worker_wake.wait(seen, std::memory_order_seq_cst);
                    }
                    worker_sleeping.store(false, std::memory_order_relaxed);
                }
            }
        }
        Run(jobs[next % JobCapacity]);
        ++next;
        done.store(next, std::memory_order_release);
    }
}

Motion::History::Allocation ObjectMotion::Evaluate(const DrawJob& job) {
    if (job.gated && !history.Moving(job.gate, job.palette)) {
        return {};
    }
    Motion::Draw draw = job.draw;
    if (!job.index_size) {
        draw.vertices = {u32(job.base_vertex), draw.index_count};
        return history.Prepare(draw);
    }
    draw.vertices = {};
    if (job.indices_valid) {
        const u32 count = draw.index_count;
        const u64 bytes = u64(count) * job.index_size;
        BbRecoverBuf* const outer_recover = runtime_fault_recover;
        BbRecoverBuf recover;
        if (BB_RECOVER_SET(recover)) {
            runtime_fault_recover = outer_recover;
            return history.Prepare(draw);
        }
        runtime_fault_recover = &recover;
        const auto scanned = index_ranges.Get(
            {job.index_address, count, job.index_size, job.base_vertex, job.restart}, history_frame,
            [&] {
                const auto* data = reinterpret_cast<const void*>(job.index_address);
                const auto range =
                    job.index_size == 2
                        ? Motion::IndexedRange(std::span(static_cast<const u16*>(data), count),
                                               job.base_vertex, job.restart)
                        : Motion::IndexedRange(std::span(static_cast<const u32*>(data), count),
                                               job.base_vertex, job.restart);
                return Motion::IndexRangeCache::Result{range, XXH3_64bits(data, bytes)};
            });
        runtime_fault_recover = outer_recover;
        draw.vertices = scanned.range;
        draw.topology = scanned.topology;
    }
    return history.Prepare(draw);
}

void ObjectMotion::WriteParam(u32 index, const Motion::History::Allocation& allocation) {
    const u32 flags = (allocation.store ? Shader::MotionVectors::FlagStore : 0) |
                      (allocation.load ? Shader::MotionVectors::FlagLoad : 0);
    u32* param = params_mapped + index * 8;
    if (!flags) {
        std::memset(param, 0, 32);
        return;
    }
    param[0] = allocation.store;
    param[1] = allocation.load;
    param[2] = allocation.vertices;
    param[3] = flags;
    param[4] = allocation.first_vertex;
    param[5] = allocation.first_instance;
    param[6] = allocation.instances;
    param[7] = 0;
}

void ObjectMotion::OnFrameStart() {
    if (!enabled) {
        return;
    }
    if (async) {
        Post({.kind = JobKind::FrameStart});
    } else {
        AdvanceHistory();
    }
    ++frame;
    const u32 slot = u32(frame % FrameSlots);
    // Do not overwrite host parameters still referenced by queued GPU work.
    if (params_ticks[slot]) {
        scheduler.Wait(params_ticks[slot]);
        params_ticks[slot] = 0;
    }
    params_used = 0;
    for (auto& target : targets) {
        target.written = false;
    }
    scheduler.EndRendering();
    // Last frame's stores must be visible to this frame's loads. Also finish old reads
    // before the ping-pong half they reference is reused for writes.
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
}

u32 ObjectMotion::Draw(const DrawJob& job) {
    if (!enabled || BbToggle::Disabled(1u << 29) || params_used >= ParamsPerFrame) {
        return 0;
    }
    const u32 slot = u32(frame % FrameSlots);
    if (async) {
        const u32 index = 1 + slot * ParamsPerFrame + params_used++;
        params_ticks[slot] = scheduler.CurrentTick();
        Post({.kind = JobKind::Draw, .param = index, .draw = job});
        return index;
    }
    const auto allocation = Evaluate(job);
    if (!allocation.store && !allocation.load) {
        return 0;
    }
    const u32 index = 1 + slot * ParamsPerFrame + params_used++;
    WriteParam(index, allocation);
    params_ticks[slot] = scheduler.CurrentTick();
    return index;
}

ObjectMotion::Target& ObjectMotion::GetTarget(u32 width, u32 height) {
    for (auto& target : targets) {
        if (target.image && target.width == width && target.height == height) {
            target.used_frame = frame;
            return target;
        }
    }
    // A free slot, else the image used longest ago (not the one being read).
    Target* slot = nullptr;
    for (auto& target : targets) {
        if (!target.image) {
            slot = &target;
            break;
        }
        if (&target != read_target && (!slot || target.used_frame < slot->used_frame)) {
            slot = &target;
        }
    }
    if (slot->image) {
        // Destroyed once the GPU is past the work recorded so far (it may still use it).
        scheduler.DeferOperation([image = std::move(slot->image),
                                  view = std::move(slot->view)]() mutable {
            view.reset();
            image.Destroy();
        });
    }
    static u32 reports = 0;
    if (BbStats::enabled && reports < 16) {
        ++reports;
        std::printf("Object motion: motion image %ux%u%s\n", width, height,
                    slot->width ? " (replacing one)" : "");
    }
    const auto device = instance.GetDevice();
    slot->image = VideoCore::UniqueImage(device, instance.GetAllocator());
    slot->image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR32G32B32A32Sfloat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    slot->view = Check(device.createImageViewUnique({
        .image = vk::Image(slot->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR32G32B32A32Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    slot->width = width;
    slot->height = height;
    slot->written = false;
    slot->layout = vk::ImageLayout::eUndefined;
    slot->used_frame = frame;
    return *slot;
}

void ObjectMotion::Attach(RenderState& state, u32 width, u32 height) {
    constexpr u32 slot = Shader::MotionVectors::Output;
    Target& target = GetTarget(width, height);
    auto& attachment = state.color_attachments[slot];
    attachment = {};
    attachment.image_view = *target.view;
    attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    if (!target.written || target.layout != vk::ImageLayout::eColorAttachmentOptimal) {
        // Clear once per frame; later reads must not erase the validity of this history.
        scheduler.EndRendering();
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .oldLayout = target.layout,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = vk::Image(target.image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        // Recorded in order on the recording thread: CommandBuffer() here switched the rest of
        // the submission (most of the G-buffer) to direct recording on the GPU thread.
        scheduler.Record([barrier](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        });
        target.layout = vk::ImageLayout::eColorAttachmentOptimal;
        attachment.is_clear = !target.written;
        target.written = true;
    }
    state.num_color_attachments = std::max<u16>(state.num_color_attachments, slot + 1);
}

vk::ImageView ObjectMotion::PrepareRead(u32 width, u32 height, bool& valid) {
    // The image of this size; otherwise any image (the pass binds one, its vectors unused).
    Target* target = const_cast<Target*>(Find(width, height));
    if (!target) {
        for (auto& candidate : targets) {
            if (candidate.image && (!target || candidate.used_frame > target->used_frame)) {
                target = &candidate;
            }
        }
    }
    if (!target) {
        target = &GetTarget(1, 1);
    }
    valid = target->written && width == target->width && height == target->height;
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = target->layout,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(target->image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    scheduler.Record([barrier](vk::CommandBuffer cmdbuf) {
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    target->layout = vk::ImageLayout::eGeneral;
    read_target = target;
    return *target->view;
}

} // namespace Vulkan
