// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

#include "bbport_toggles.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/scene_resolution.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_occlusion.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "lantern_switches.h"

extern "C" void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);

namespace Vulkan {
namespace Model = OcclusionModel;

namespace {
u64 EnvNumber(const char* name, u64 fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::strtoull(value, nullptr, 10) : fallback;
}

u64 FramesSoFar() {
    return BbStats::frame_number.load(std::memory_order_relaxed);
}

using BlockWords = std::array<u64, 2 * Model::MaxDbs>;

BlockWords ReadWords(VAddr block, u32 dbs) {
    BlockWords words{};
    runtime_memory_read_backing(block, words.data(), 16ull * dbs);
    return words;
}

}

struct Occlusion::Resolver {
    enum class Kind : u8 {
        Begin,
        Closed,
        Unpaired,
    };
    struct Record {
        u64 tick;
        u64 number;
        VAddr block;
        u64 frame;
        u64 first_query;
        u64 queries;
        u64 needed_from;
        u32 dbs;
        Kind kind;
        bool complete;
    };
    struct Answered {
        u64 base;
        u64 count;
        u32 dbs;
    };

    std::mutex state_mutex;
    std::mutex inbox_mutex;
    std::deque<Record> inbox;

    bool alive = true;
    vk::Device device;
    vk::QueryPool pool;
    Rasterizer* rasterizer = nullptr;
    bool precise = false;
    u64 max_lag = 6;

    std::array<u32, RingSize> scale{};
    std::vector<u64> raw = std::vector<u64>(2ull * RingSize);
    std::vector<Record> batch;
    u64 reset_below = 0;
    std::atomic<u64> released{0};
    std::atomic<u64> answered_through{0};

    u64 counter = 0;
    std::unordered_map<VAddr, Answered> answered;
    std::unordered_map<VAddr, Model::SteadyCount> steady;

    u64 dumps = 0, queries_read = 0, seen = 0, hidden = 0, plainly = 0, unavailable = 0;
    u64 late = 0, foreign = 0, unmapped = 0, held = 0, late_written = 0, unpaired = 0;
    std::atomic<u64> pool_full{0}, pred_sets{0}, pred_known{0}, pred_skipped{0}, pred_drawn{0};
    std::chrono::steady_clock::time_point report_time = std::chrono::steady_clock::now();
    u64 report_frames = 0, report_draws = 0;
    std::array<u64, 15> reported{};

    void Run(u64 tick);
    void Process(const Record& record);
    u64 Sum(u64 first, u64 count, u64& missing);
    void Release(u64 below);
    bool Store(VAddr block, u32 dbs, u64 count);
    void Report();
};

bool Occlusion::Wanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("BB_OCCLUSION");
        return !value || value[0] != '0';
    }();
    return wanted;
}

Occlusion::Occlusion(const Instance& instance_, Scheduler& scheduler_, Rasterizer& rasterizer_)
    : instance{instance_}, scheduler{scheduler_}, rasterizer{rasterizer_} {}

std::unique_ptr<Occlusion> Occlusion::Create(const Instance& instance, Scheduler& scheduler,
                                             Rasterizer& rasterizer) {
    if (!Wanted()) {
        std::printf("Occlusion: off (BB_OCCLUSION=0): every query the game makes reads visible\n");
        return nullptr;
    }
    std::unique_ptr<Occlusion> occlusion{new Occlusion(instance, scheduler, rasterizer)};
    if (!occlusion->Init()) {
        return nullptr;
    }
    return occlusion;
}

bool Occlusion::Init() {
    if (!instance.IsHostQueryResetSupported()) {
        std::printf("Occlusion: off - the device cannot reset queries from the host; every query "
                    "the game makes reads visible\n");
        return false;
    }
    const vk::Device device = instance.GetDevice();
    const vk::QueryPoolCreateInfo ring_info = {
        .queryType = vk::QueryType::eOcclusion,
        .queryCount = RingSize,
    };
    const auto [result, pool] = device.createQueryPool(ring_info);
    if (result != vk::Result::eSuccess) {
        std::printf("Occlusion: off - no query pool (%s); every query the game makes reads "
                    "visible\n",
                    vk::to_string(result).c_str());
        return false;
    }
    device.resetQueryPool(pool, 0, RingSize);
    const bool precise = instance.IsOcclusionQueryPreciseSupported();
    if (precise) {
        query_flags = vk::QueryControlFlagBits::ePrecise;
    }
    draw_budget = std::clamp<u64>(EnvNumber("BB_OCCLUSION_CAP", 64), 1, 1024);
    resolver = std::make_shared<Resolver>();
    resolver->device = device;
    resolver->pool = pool;
    resolver->rasterizer = &rasterizer;
    resolver->precise = precise;
    resolver->max_lag = EnvNumber("BB_OCCLUSION_MAX_LAG", 6);
    std::printf("Occlusion: conservative visibility (BB_OCCLUSION=0: always visible) - "
                "each query block the game opens is measured over up to %llu draws and answered "
                "once the GPU has run them (at most %llu frames late), %s; only a complete, fresh "
                "query with no samples hides an object; SET_PREDICATION skips only on known "
                "answers\n",
                static_cast<unsigned long long>(draw_budget),
                static_cast<unsigned long long>(resolver->max_lag),
                precise ? "precise counts" : "not precise (any nonzero count reads plainly visible)");
    return true;
}

Occlusion::~Occlusion() {
    if (!resolver) {
        return;
    }
    std::scoped_lock lock{resolver->state_mutex};
    resolver->alive = false;
    resolver->device.destroyQueryPool(resolver->pool);
    resolver->pool = nullptr;
}

Occlusion::DumpTask Occlusion::MakeDump(VAddr va, u32 num_pipes) const noexcept {
    return {va, decoded_draws, FramesSoFar(), std::clamp(num_pipes, 1u, Model::MaxDbs), 0};
}

void Occlusion::Dump(const DumpTask& task) {
    const VAddr block = task.va & ~VAddr{15};
    open_blocks.Expire(task.frame, OpenFrames);

    Resolver::Record record{};
    record.tick = scheduler.CurrentTick();
    record.number = ++dumps_recorded;
    record.block = block;
    record.frame = task.frame;
    record.dbs = task.pipes;
    if ((task.va & 8) == 0) {
        record.kind = Resolver::Kind::Begin;
        open_blocks.Begin(block, task.issued, next_query, task.frame);
    } else if (const auto closed = open_blocks.End(block, task.issued, next_query)) {
        record.kind = Resolver::Kind::Closed;
        record.frame = closed->frame;
        record.first_query = closed->first_query;
        record.queries = closed->queries;
        record.complete = closed->complete;
    } else {
        record.kind = Resolver::Kind::Unpaired;
    }
    record.needed_from = open_blocks.FirstNeeded(next_query);

    if (last_dump.size() > 65536) {
        last_dump.clear();
    }
    last_dump[block] = {record.number, task.pipes};
    {
        std::scoped_lock lock{resolver->inbox_mutex};
        resolver->inbox.push_back(record);
    }
    if (record.tick != last_dump_tick) {
        last_dump_tick = record.tick;
        scheduler.DeferPriorityOperation(
            [resolver = resolver, tick = record.tick] { resolver->Run(tick); });
    }
}

Occlusion::DrawQuery Occlusion::TakeQuery(u32 scene_size) noexcept {
    if (next_query - resolver->released.load(std::memory_order_acquire) >= RingSize) {
        resolver->pool_full.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const u32 slot = static_cast<u32>(next_query % RingSize);
    u32 scale = 1u << 16;
    if (scene_size != 0) {
        const auto size = SceneResolution::Unpack(scene_size);
        scale = Model::ScaleToNative(size.width, size.height);
    }
    resolver->scale[slot] = scale;
    ++next_query;
    return {resolver->pool, slot, query_flags};
}

void Occlusion::SetPredication(const PredicationTask& task) {
    resolver->pred_sets.fetch_add(1, std::memory_order_relaxed);
    if (task.op == 0) {
        predicate = Predicate::None;
        chain_seen = chain_unknown = false;
        return;
    }
    if (task.op != 1) {
        predicate = Predicate::Draw;
        return;
    }
    if (!task.cont || predicate == Predicate::None) {
        chain_seen = chain_unknown = false;
    }
    bool known = false;
    if (const auto it = last_dump.find(task.block); it != last_dump.end() &&
        resolver->answered_through.load(std::memory_order_acquire) >= it->second.number) {
        const BlockWords words = ReadWords(task.block, it->second.dbs);
        const auto answer = Model::ReadAnswer(words.data(), it->second.dbs);
        if (answer != Model::Answer::Pending) {
            known = true;
            chain_seen |= answer == Model::Answer::Seen;
            resolver->pred_known.fetch_add(1, std::memory_order_relaxed);
        }
    }
    chain_unknown |= !known;
    predicate = Model::DrawPredicated(chain_seen, chain_unknown, task.draw_if_visible != 0)
                    ? Predicate::Draw
                    : Predicate::Skip;
}

bool Occlusion::SkipUnderPredicate() noexcept {
    const bool skip = predicate == Predicate::Skip;
    (skip ? resolver->pred_skipped : resolver->pred_drawn).fetch_add(1, std::memory_order_relaxed);
    return skip;
}

void Occlusion::Resolver::Run(u64 tick) {
    std::scoped_lock lock{state_mutex};
    if (!alive) {
        return;
    }
    batch.clear();
    {
        std::scoped_lock inbox_lock{inbox_mutex};
        while (!inbox.empty() && inbox.front().tick <= tick) {
            batch.push_back(inbox.front());
            inbox.pop_front();
        }
    }
    for (const Record& record : batch) {
        Process(record);
    }
    Report();
}

void Occlusion::Resolver::Process(const Record& record) {
    ++dumps;
    if (record.kind != Kind::Begin) {
        u64 count = Model::PlainlyVisible;
        if (record.kind == Kind::Unpaired) {
            ++unpaired;
        } else if (record.complete) {
            u64 missing = 0;
            count = Sum(record.first_query, record.queries, missing);
            unavailable += missing;
        }
        const u64 now = FramesSoFar();
        const bool fresh = Model::QueryFresh(record.frame, now, max_lag);
        count = Model::VisibilityCount(count, record.queries, record.complete, fresh);
        plainly += count == Model::PlainlyVisible ? 1 : 0;
        if (!Model::LateAnswerWritable(record.frame, now, max_lag,
                                       answered.contains(record.block))) {
            ++late;
        } else {
            late_written += fresh ? 0 : 1;
            if (steady.size() > 65536) {
                steady.clear();
            }
            const u64 report = steady[record.block].Add(count);
            held += count == 0 && report != 0 ? 1 : 0;
            if (Store(record.block, record.dbs, report)) {
                ++(report != 0 ? seen : hidden);
            }
        }
    }
    Release(record.needed_from);
    answered_through.store(record.number, std::memory_order_release);
}

u64 Occlusion::Resolver::Sum(u64 first, u64 count, u64& missing) {
    queries_read += count;
    u64 samples = 0;
    while (count != 0) {
        const u32 start = static_cast<u32>(first % RingSize);
        const u32 run = static_cast<u32>(std::min<u64>(count, RingSize - start));
        const vk::Result result = device.getQueryPoolResults(
            pool, start, run, 16ull * run, raw.data(), 16,
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        const bool readable = result == vk::Result::eSuccess || result == vk::Result::eNotReady;
        for (u32 i = 0; i < run; ++i) {
            const bool available = readable && raw[2 * i + 1] != 0;
            missing += available ? 0 : 1;
            samples += Model::QueryCount(raw[2 * i], available, precise, scale[start + i]);
        }
        first += run;
        count -= run;
    }
    return samples;
}

void Occlusion::Resolver::Release(u64 below) {
    while (reset_below < below) {
        const u32 start = static_cast<u32>(reset_below % RingSize);
        const u32 run = static_cast<u32>(std::min<u64>(below - reset_below, RingSize - start));
        device.resetQueryPool(pool, start, run);
        reset_below += run;
    }
    released.store(reset_below, std::memory_order_release);
}

bool Occlusion::Resolver::Store(VAddr block, u32 dbs, u64 count) {
    const BlockWords now = ReadWords(block, dbs);
    BlockWords ours{};
    const auto previous = answered.find(block);
    const bool have_ours = previous != answered.end() && previous->second.dbs == dbs;
    if (have_ours) {
        Model::FillBlock(ours.data(), dbs, previous->second.base, previous->second.count);
    }
    for (u32 i = 0; i < 2 * dbs; ++i) {
        if (now[i] != 0 && !(have_ours && now[i] == ours[i])) {
            ++foreign;
            return false;
        }
    }
    BlockWords words{};
    Model::FillBlock(words.data(), dbs, counter, count);
    auto* memory = Core::Memory::Instance();
    for (const u32 half : {0u, 1u}) {
        for (u32 db = 0; db < dbs; ++db) {
            const VAddr address = block + 16ull * db + 8ull * half;
            const u64 value = words[2 * db + half];
            if (!memory->TryWriteBacking(reinterpret_cast<u8*>(address), &value, sizeof(value))) {
                ++unmapped;
                answered.erase(block);
                return false;
            }
            rasterizer->NoteLateCommandWrite(address, &value, sizeof(value));
        }
    }
    if (answered.size() > 262144) {
        answered.clear();
    }
    answered[block] = {counter, count, dbs};
    counter += count;
    return true;
}

void Occlusion::Resolver::Report() {
    if (!Lantern::Switches::Get().frame_stats) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_time < std::chrono::seconds(5)) {
        return;
    }
    report_time = now;
    const u64 frames_now = FramesSoFar();
    const u64 draws_now = BbStats::draws.load(std::memory_order_relaxed);
    const std::array<u64, 15> totals{
        dumps,
        queries_read,
        seen,
        hidden,
        plainly,
        unavailable,
        late + foreign + unmapped,
        pool_full.load(std::memory_order_relaxed),
        pred_sets.load(std::memory_order_relaxed),
        pred_known.load(std::memory_order_relaxed),
        pred_skipped.load(std::memory_order_relaxed),
        pred_drawn.load(std::memory_order_relaxed),
        held,
        late_written,
        unpaired,
    };
    const double frames = std::max<double>(1.0, double(frames_now - report_frames));
    std::array<double, 15> per_frame{};
    for (size_t i = 0; i < totals.size(); ++i) {
        per_frame[i] = double(totals[i] - reported[i]) / frames;
    }
    const auto& f = per_frame;
    const double answers = f[2] + f[3];
    std::printf("Occlusion per frame: %.1f dumps, %.1f queries, objects %.1f visible / %.1f "
                "culled (%.0f%%), %.1f blocks read plainly visible (%.1f queries unavailable), "
                "%.1f answers not written (totals: late %llu, reused memory %llu, unmapped %llu), "
                "pool full %.1f; SET_PREDICATION %.1f (%.1f known), predicated draws %.1f skipped "
                "/ %.1f drawn; %.0f draws/frame; held visible %.1f, late written %.1f, unpaired "
                "ends %.1f\n",
                f[0], f[1], f[2], f[3], answers > 0 ? 100.0 * f[3] / answers : 0.0, f[4], f[5],
                f[6], static_cast<unsigned long long>(late),
                static_cast<unsigned long long>(foreign),
                static_cast<unsigned long long>(unmapped), f[7], f[8], f[9], f[10], f[11],
                double(draws_now - report_draws) / frames, f[12], f[13], f[14]);
    std::fflush(stdout);
    reported = totals;
    report_frames = frames_now;
    report_draws = draws_now;
}

}
