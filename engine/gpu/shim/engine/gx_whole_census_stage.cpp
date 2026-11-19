// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_whole_census_stage.h"
#include "gx_bindings.h"
#include "gx_draws.h"
#include "gx_records.h"
#include "gx_whole_census.h"
#include "bbport_native_calls.h"
#include "bbport_sections.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#include <chrono>
#include <cstdio>

namespace BbEngine::GxWholeCensus {
namespace {
static_assert(kBindingsCovered == GxBindings::Covered);
static_assert(kBindingsLost == GxBindings::NoState);
static_assert(GxBindings::kReasons < kBindingsNotApplied);

constexpr auto kReportEvery = std::chrono::seconds(5);
constexpr u64 kDrawsPerClockCheck = 256;
constexpr std::size_t kLineBytes = 2048;

struct Owner {
    Window window;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    u64 since_ticks = BbNativeCalls::Ticks();
    u64 stamped_before = GxRecords::Stamped();
};
Owner g_owner;

void MaybeReport() {
    Owner& owner = g_owner;
    if (Total(owner.window) % kDrawsPerClockCheck != 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - owner.since < kReportEvery) return;
    const u64 ticks = BbNativeCalls::Ticks();
    const double secs = std::chrono::duration<double>(now - owner.since).count();
    const double ticks_per_us =
        BbSections::Enabled() && ticks > owner.since_ticks ? double(ticks - owner.since_ticks) / (secs * 1e6) : 0.0;
    const u64 stamped = GxRecords::Stamped();
    owner.window.stamped = stamped - owner.stamped_before;
    char line[kLineBytes];
    Format(owner.window, ticks_per_us, line, sizeof(line));
    std::printf("%s\n", line);
    Restart(owner.window);
    owner.since = now;
    owner.since_ticks = ticks;
    owner.stamped_before = stamped;
}
}

void OnPacket(const std::uint32_t* packet, bool token_draw) {
    const u32 header = packet[0];
    const u32 opcode = (header >> 8) & 0xff;
    const u32 count = ((header >> 16) & 0x3fff) + 1;
    const bool bind_tag = count > 1 && packet[1] == GxDraws::kBindTag;
    NotePm4(g_owner.window, opcode, count + 1, bind_tag, token_draw);
    if (IsDrawOpcode(opcode) && !token_draw) MaybeReport();
}

void OnTokenDraw(const AmdGpu::Regs& regs, std::uint32_t predicate, std::uint64_t record, Vulkan::Rasterizer& rasterizer) {
    DrawFacts facts;
    facts.token = true;
    facts.records_on = GxRecords::On();
    facts.records_used = GxRecords::Compared() || GxRecords::Used();
    facts.rect_list = regs.primitive_type == AmdGpu::PrimitiveType::RectList;
    facts.hull_or_geometry = regs.stage_enable.hs_en || regs.stage_enable.gs_en;
    facts.predicated = predicate != 0;
    facts.record = record != 0;
    facts.verdict = rasterizer.TakeGxWholeVerdict();
    NoteToken(g_owner.window, facts);
    MaybeReport();
}

void OnPacketTicks(std::uint64_t ticks, bool draw_packet) {
    if (!BbSections::Enabled()) return;
    NoteTicks(g_owner.window, ticks, draw_packet);
}

void OnFlip() {
    ++g_owner.window.frames;
    NoteOutside(g_owner.window);
}

void OnSubmissionEnd() {
    ++g_owner.window.submissions;
    NoteOutside(g_owner.window);
}

void OnConstantEngine(std::size_t dwords) {
    if (!dwords) return;
    ++g_owner.window.ce_buffers;
    g_owner.window.ce_dwords += dwords;
}
}
