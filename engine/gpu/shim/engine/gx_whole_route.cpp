// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_whole_route.h"
#include "engine_state.h"
#include "gx_bindings.h"
#include "gx_records.h"
#include "gx_whole_stage_a.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <thread>

namespace Vulkan {
class GxWholeKeyAccess {
public:
    static const GraphicsPipeline* Find(const PipelineCache& cache, const GraphicsPipelineKey& key) {
        const auto it = cache.graphics_pipelines.find(key);
        return it == cache.graphics_pipelines.end() ? nullptr : it->second.get();
    }
    static bool Remembered(const PipelineCache& cache, std::uint64_t record) {
        return cache.gx_pipelines.contains(record);
    }
};
}

namespace BbEngine::GxWholeRoute {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace StageA = GxWhole::StageA;
using Key = Vulkan::GraphicsPipelineKey;

constexpr u32 kPublishEvery = 256;
constexpr std::size_t kEvidenceBytes = 512;
constexpr u32 kEvidenceKeyBytes = 8;

constexpr u32 kKeyTail = offsetof(Key, motion_vectors) + sizeof(u32);
constexpr std::array<StageA::ByteRange, 9> kKeyGroups = {{
    {offsetof(Key, stage_hashes), offsetof(Key, vertex_buffer_formats) - offsetof(Key, stage_hashes)},
    {offsetof(Key, vertex_buffer_formats), offsetof(Key, patch_control_points) - offsetof(Key, vertex_buffer_formats)},
    {offsetof(Key, patch_control_points), offsetof(Key, color_buffers) - offsetof(Key, patch_control_points)},
    {offsetof(Key, color_buffers), offsetof(Key, blend_controls) - offsetof(Key, color_buffers)},
    {offsetof(Key, blend_controls), offsetof(Key, write_masks) - offsetof(Key, blend_controls)},
    {offsetof(Key, write_masks), offsetof(Key, cb_shader_mask) - offsetof(Key, write_masks)},
    {offsetof(Key, cb_shader_mask), offsetof(Key, mrt_mask) - offsetof(Key, cb_shader_mask)},
    {offsetof(Key, mrt_mask), kKeyTail - offsetof(Key, mrt_mask)},
    {kKeyTail, sizeof(Key) - kKeyTail},
}};
static_assert(StageA::Partitions(kKeyGroups, sizeof(Key)));
constexpr std::array<const char*, kKeyGroups.size()> kGroupNames = {
    "stages", "vertex formats", "patch points and color count", "color targets", "blend",
    "write masks", "shader mask, logic op and samples", "MRT mask and motion", "depth, primitive and raster"};
static_assert(kGroupNames.size() <= StageA::kMaxGroups);

struct KeyBuilder {
    Vulkan::PipelineSelection selection{};
    Vulkan::PrepWorker worker{};
};

struct Owner {
    StageA::Mode mode = StageA::Mode::Off;
    StageA::Chooser chooser;
    u32 unpublished = 0;
    std::array<bool, static_cast<std::size_t>(StageA::Outcome::kCount)> logged{};
    std::thread::id writer{};
    KeyBuilder record_key, register_key;
};

std::unique_ptr<Owner> g_owner;
std::atomic<bool> g_on{false};
std::atomic<bool> g_disabled{false};
std::atomic<StageA::Mode> g_mode{StageA::Mode::Off};
std::atomic<u64> g_marked{0};
std::array<std::atomic<u64>, StageA::kCountWords> g_published{};

StageA::Mode ModeOf(GxWhole::Mode mode) {
    return mode == GxWhole::Mode::Compare ? StageA::Mode::Compare
           : mode == GxWhole::Mode::Feed  ? StageA::Mode::Use
                                          : StageA::Mode::Off;
}

std::span<const u8> Bytes(const Key& key) {
    return {reinterpret_cast<const u8*>(&key), sizeof key};
}

void Publish(Owner& o) {
    if (++o.unpublished < kPublishEvery) return;
    o.unpublished = 0;
    std::array<u64, StageA::kCountWords> words;
    std::memcpy(words.data(), &o.chooser.counts, sizeof o.chooser.counts);
    for (std::size_t i = 0; i < words.size(); ++i) g_published[i].store(words[i], std::memory_order_relaxed);
}

void NoteWriter(Owner& o) {
    if (o.mode != StageA::Mode::Compare) return;
    const std::thread::id self = std::this_thread::get_id();
    if (o.writer == std::thread::id{}) o.writer = self;
    else if (o.writer != self) ++o.chooser.counts.other_writers;
}

void WriteLine(const char* text, int length) {
    if (length <= 0) return;
    const std::size_t bytes = std::min<std::size_t>(std::size_t(length), kEvidenceBytes - 1);
    if (std::fwrite(text, 1, bytes, stdout) != bytes) return;
}

u32 FirstDifferingByte(const Key& ours, const Key& theirs) {
    const auto a = Bytes(ours), b = Bytes(theirs);
    for (u32 i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return i;
    }
    return 0;
}

u64 Window(const Key& key, u32 at) {
    u64 value = 0;
    const u32 start = std::min<u32>(at, sizeof(Key) - kEvidenceKeyBytes);
    std::memcpy(&value, Bytes(key).data() + start, kEvidenceKeyBytes);
    return value;
}

void ReportFirst(Owner& o, StageA::Outcome outcome, u32 groups, u64 record) {
    auto& logged = o.logged[static_cast<std::size_t>(outcome)];
    if (logged) return;
    logged = true;
    const Key& ours = o.record_key.selection.graphics_key;
    const Key& theirs = o.register_key.selection.graphics_key;
    const u32 at = FirstDifferingByte(ours, theirs);
    const char* group = "none";
    for (u32 g = 0; g < kKeyGroups.size(); ++g) {
        if ((groups >> g) & 1) {
            group = kGroupNames[g];
            break;
        }
    }
    char line[kEvidenceBytes];
    const int length = std::snprintf(
        line, sizeof line,
        "GX whole stage A: first difference (%s) at covered draw %llu, record %016llx: key groups %#x (first: %s), first byte %u, record key %016llx, register key %016llx (LANTERN_GX_WHOLE=%d)\n",
        StageA::kOutcomeNames[static_cast<std::size_t>(outcome)], static_cast<unsigned long long>(o.chooser.covered_seen),
        static_cast<unsigned long long>(record), groups, group, at, static_cast<unsigned long long>(Window(ours, at)),
        static_cast<unsigned long long>(Window(theirs, at)), StageA::ModeNumber(o.mode));
    WriteLine(line, length);
    if (o.mode == StageA::Mode::Use && !g_disabled.exchange(true, std::memory_order_acq_rel)) {
        ++o.chooser.counts.disabled;
        const int off = std::snprintf(line, sizeof line,
                                      "GX whole stage A: the record route is off for this session after that difference; covered draws take the register path\n");
        WriteLine(line, off);
    }
}

bool BuildKey(Vulkan::PipelineCache& cache, KeyBuilder& builder, const AmdGpu::Regs& regs) {
    builder.selection.regs = &regs;
    builder.selection.worker = &builder.worker;
    builder.selection.draw_indirect_params = {};
    builder.worker.stages.clear();
    builder.worker.failed = false;
    return cache.PrepareGraphicsPipeline(builder.selection);
}

void Compare(Owner& o, Vulkan::PipelineCache& cache, AmdGpu::Regs& regs, std::span<const GxWhole::Value> values, u64 record,
             const Vulkan::GraphicsPipeline* used, StageA::Route route) {
    std::array<u32, StageA::kMaxOverlay> saved;
    const std::span<u32> file(regs.reg_array);
    const u32 applied = StageA::Overlay<GxWhole::Value>(file, values, saved);
    const bool record_ok = BuildKey(cache, o.record_key, regs);
    StageA::Restore<GxWhole::Value>(file, values, saved, applied);
    const bool register_ok = BuildKey(cache, o.register_key, regs);
    const Key& ours = o.record_key.selection.graphics_key;
    const Key& theirs = o.register_key.selection.graphics_key;
    const u32 groups = record_ok && register_ok ? StageA::DifferingGroups(Bytes(ours), Bytes(theirs), kKeyGroups) : 0;
    const Vulkan::GraphicsPipeline* other = route == StageA::Route::Registers
                                                ? (record_ok ? Vulkan::GxWholeKeyAccess::Find(cache, ours) : nullptr)
                                                : (register_ok ? Vulkan::GxWholeKeyAccess::Find(cache, theirs) : nullptr);
    const StageA::Outcome outcome = StageA::Judge(record_ok, register_ok, groups, other == used);
    StageA::CountOutcome(o.chooser.counts, outcome, groups);
    if (StageA::IsDifference(outcome)) ReportFirst(o, outcome, groups, record);
}

const Vulkan::GraphicsPipeline* SelectByRecord(Vulkan::PipelineCache& cache, AmdGpu::Regs& regs, std::span<const GxWhole::Value> values,
                                               u64 record) {
    const bool remembered = GxRecords::On() && !GxRecords::Checked() && Vulkan::GxWholeKeyAccess::Remembered(cache, record);
    if (remembered) return cache.GetGraphicsPipeline({}, nullptr, record);
    std::array<u32, StageA::kMaxOverlay> saved;
    const std::span<u32> file(regs.reg_array);
    const u32 applied = StageA::Overlay<GxWhole::Value>(file, values, saved);
    const Vulkan::GraphicsPipeline* pipeline = cache.GetGraphicsPipeline({}, nullptr, record);
    StageA::Restore<GxWhole::Value>(file, values, saved, applied);
    return pipeline;
}

struct Port {
    using Value = GxWhole::Value;
    Owner& owner;
    Vulkan::PipelineCache& cache;
    AmdGpu::Regs& regs;
    const GxBindings::State* Taken(u64 record) {
        return GxRecords::Taken(record);
    }
    StageA::Facts FactsOf(bool record, const GxBindings::State* state, bool capture_on) {
        return {record, state != nullptr, state && state->whole.captured != 0, state && state->whole.vs_bound != 0, capture_on};
    }
    bool Disabled() {
        return g_disabled.load(std::memory_order_relaxed);
    }
    u32 Values(const GxBindings::State& state, bool indexed, std::span<Value, GxWhole::kMaxValues> out) {
        return GxWhole::RegistersFromRecord(state, indexed, out);
    }
    const Vulkan::GraphicsPipeline* Registers(const Vulkan::PreparedDraw* prepared, u64 record) {
        return cache.GetGraphicsPipeline({}, prepared, record);
    }
    const Vulkan::GraphicsPipeline* ByRecord(std::span<const Value> values, u64 record) {
        return SelectByRecord(cache, regs, values, record);
    }
    void Compare(std::span<const Value> values, u64 record, const Vulkan::GraphicsPipeline* used, StageA::Route route) {
        BbEngine::GxWholeRoute::Compare(owner, cache, regs, values, record, used, route);
    }
};
}

void Start(GxWhole::Mode mode, bool counting) {
    const StageA::Mode stage_mode = ModeOf(mode);
    if (stage_mode == StageA::Mode::Off && !counting) return;
    auto owner = std::make_unique<Owner>();
    owner->mode = stage_mode;
    g_owner = std::move(owner);
    g_mode.store(stage_mode, std::memory_order_relaxed);
    g_on.store(true, std::memory_order_release);
    if (stage_mode == StageA::Mode::Compare) {
        host_log("GX whole stage A: every covered draw's pipeline key built from the record and compared with the register key (LANTERN_GX_WHOLE=2); the register path's pipeline is used");
    } else if (stage_mode == StageA::Mode::Use) {
        host_log("GX whole stage A: covered draws take their pipeline from the record, without a prepared draw; one in %u compared (LANTERN_GX_WHOLE=1)",
                 StageA::kSampleEvery);
    } else {
        host_log("GX whole stage A: count only (LANTERN_GX_WHOLE=0, BB_FRAME_STATS=1)");
    }
}

bool Disabled() {
    return g_disabled.load(std::memory_order_acquire);
}

bool On() {
    return g_on.load(std::memory_order_acquire);
}

bool MarksPrepared() {
    const StageA::Switches switches{g_on.load(std::memory_order_acquire), g_mode.load(std::memory_order_relaxed),
                                    g_disabled.load(std::memory_order_relaxed)};
    return switches.MarksPrepared();
}

void NoteMarked(std::uint32_t draws) {
    if (draws) g_marked.fetch_add(draws, std::memory_order_relaxed);
}

const Vulkan::GraphicsPipeline* Select(Vulkan::PipelineCache& cache, AmdGpu::Regs& regs, const Vulkan::PreparedDraw* prepared,
                                       std::uint64_t gx_record, bool indexed) {
    Owner& o = *g_owner;
    NoteWriter(o);
    Port port{o, cache, regs};
    const Vulkan::GraphicsPipeline* pipeline = StageA::Choose(port, o.chooser, o.mode, prepared, gx_record, indexed);
    Publish(o);
    return pipeline;
}

bool Line(char* out, std::size_t size, bool* suspect) {
    if (!On()) return false;
    std::array<u64, StageA::kCountWords> words;
    for (std::size_t i = 0; i < words.size(); ++i) words[i] = g_published[i].load(std::memory_order_relaxed);
    StageA::Counts counts;
    std::memcpy(&counts, words.data(), sizeof counts);
    counts.marked = g_marked.load(std::memory_order_relaxed);
    const StageA::Mode mode = g_mode.load(std::memory_order_relaxed);
    StageA::FormatLine(out, size, mode, counts, kGroupNames);
    if (suspect) *suspect = StageA::InstrumentSuspect(mode, counts);
    return true;
}
}
