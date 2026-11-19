// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <bit>
#include <vector>

#include "lantern_gpu_stamps.h"
#include "lantern_meter.h"
#include "video_core/renderer_vulkan/vk_gpu_busy.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan::GpuBusy {
namespace {
namespace Stamps = Lantern::GpuStamps;
namespace Rule = BbEngine::BottleneckMeter;

constexpr std::size_t kWordsPerQuery = 2;

struct Pool {
    vk::Device device;
    vk::QueryPool pool;
    double period_ns = 0.0;
    u64 valid_mask = 0;
    u64 range = 0;
    const Scheduler* watched = nullptr;
    Semaphore* semaphore = nullptr;
    Stamps::Slots slots;
    Rule::SpanUnion union_ticks;
    std::vector<u64> words;
    std::vector<Stamps::Result> results;
    std::vector<Rule::Span> spans;
    bool failed = false;
};

std::atomic<Pool*> g_pool{nullptr};

u64 Handle(vk::CommandBuffer cmdbuf) {
    return u64(reinterpret_cast<std::uintptr_t>(static_cast<VkCommandBuffer>(cmdbuf)));
}

bool ReadRange(Pool& p, u32 first_slot, u32 slots, std::size_t at) {
    const u32 queries = slots * Stamps::kQueriesPerSlot;
    const vk::Result result = p.device.getQueryPoolResults(
        p.pool, first_slot * Stamps::kQueriesPerSlot, queries, queries * kWordsPerQuery * sizeof(u64),
        p.words.data(), kWordsPerQuery * sizeof(u64), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
    if (result != vk::Result::eSuccess && result != vk::Result::eNotReady) {
        Lantern::Log(Lantern::LogChannel::Always, "Limit: GPU timestamps could not be read (%s); GPU busy reads ?",
                     vk::to_string(result).c_str());
        return false;
    }
    for (u32 i = 0; i < slots; ++i) {
        const u64* q = p.words.data() + std::size_t(i) * Stamps::kQueriesPerSlot * kWordsPerQuery;
        Stamps::Result& r = p.results[at + i];
        r.available = q[1] != 0 && q[3] != 0;
        r.begin = q[0] & p.valid_mask;
        r.end = q[2] & p.valid_mask;
        if (p.range && r.end < r.begin) r.end += p.range;
    }
    return true;
}

void Collect() {
    Pool* p = g_pool.load(std::memory_order_acquire);
    if (!p || p->failed) return;
    const Stamps::Pending pending = p->slots.Outstanding();
    if (!pending.count) return;
    const u32 head = std::min(pending.count, Stamps::kSlots - pending.first);
    if (!ReadRange(*p, pending.first, head, 0) ||
        (pending.count > head && !ReadRange(*p, 0, pending.count - head, head))) {
        p->failed = true;
        Lantern::Meter::PublishGpuUnknown();
        return;
    }
    p->spans.clear();
    const u32 ready = p->slots.Ready(pending, std::span<const Stamps::Result>(p->results.data(), pending.count),
                                     Lantern::Meter::SteadyNs(), p->spans);
    p->union_ticks.Add(p->spans, p->range);
    if (ready) p->slots.Release(ready);
    (void)p->slots.Reclaim(p->semaphore->KnownGpuTick());
    Lantern::Meter::PublishGpuBusy(u64(double(p->union_ticks.busy) * p->period_ns));
}
}

void Init(const Instance& instance, Scheduler& watched) {
    if (!Lantern::Meter::On() || g_pool.load(std::memory_order_acquire)) return;
    const vk::PhysicalDevice gpu = instance.GetPhysicalDevice();
    const auto families = gpu.getQueueFamilyProperties();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();
    const u32 bits = family < families.size() ? families[family].timestampValidBits : 0;
    if (!bits) {
        Lantern::Log(Lantern::LogChannel::Always, "Limit: GPU busy not measured (the graphics queue has no timestamps)");
        return;
    }
    auto* p = new Pool;
    p->device = instance.GetDevice();
    const vk::QueryPoolCreateInfo info = {
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = Stamps::kQueries,
    };
    auto [result, pool] = p->device.createQueryPool(info);
    if (result != vk::Result::eSuccess) {
        Lantern::Log(Lantern::LogChannel::Always, "Limit: GPU busy not measured (query pool: %s)",
                     vk::to_string(result).c_str());
        delete p;
        return;
    }
    p->pool = pool;
    if (instance.IsHostQueryResetSupported()) p->device.resetQueryPool(p->pool, 0, Stamps::kQueries);
    p->period_ns = gpu.getProperties().limits.timestampPeriod;
    p->valid_mask = bits >= 64 ? ~u64{0} : (u64{1} << bits) - 1;
    p->range = bits >= 64 ? 0 : u64{1} << bits;
    p->watched = &watched;
    p->semaphore = watched.GetWorkSemaphore();
    p->words.resize(std::size_t(Stamps::kQueries) * kWordsPerQuery);
    p->results.resize(Stamps::kSlots);
    g_pool.store(p, std::memory_order_release);
    Lantern::Meter::SetGpuCollect(&Collect);
}

void Shutdown() {
    Pool* p = g_pool.exchange(nullptr, std::memory_order_acq_rel);
    if (!p) return;
    p->device.destroyQueryPool(p->pool);
    p->pool = vk::QueryPool{};
}

void Begin(const Scheduler* owner, vk::CommandBuffer cmdbuf) {
    Pool* p = g_pool.load(std::memory_order_acquire);
    if (!p || owner != p->watched) return;
    if (const auto slot = p->slots.Begin(Handle(cmdbuf), Lantern::Meter::SteadyNs())) {
        cmdbuf.resetQueryPool(p->pool, *slot * Stamps::kQueriesPerSlot, Stamps::kQueriesPerSlot);
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, p->pool, *slot * Stamps::kQueriesPerSlot);
    }
}

void End(const Scheduler* owner, vk::CommandBuffer cmdbuf) {
    Pool* p = g_pool.load(std::memory_order_acquire);
    if (!p || owner != p->watched) return;
    if (const auto slot = p->slots.End(Handle(cmdbuf), p->semaphore->CurrentTick())) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, p->pool, *slot * Stamps::kQueriesPerSlot + 1);
    }
}
}
