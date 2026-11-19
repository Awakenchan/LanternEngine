// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx_owed_registers.h"
#include "gx_whole.h"
#include <cstdint>
#include <span>

namespace BbEngine::GxWhole::OwedCheck {
inline constexpr std::uint32_t kRegisterWords = GxWhole::kRegisterWords;
inline constexpr std::uint32_t kContextBase = GxWhole::kContext;
using MarkHook = void (*)(void*, std::uint32_t, std::uint32_t);

bool Start(Mode mode);
bool Attach(MarkHook& on_mark, std::span<const std::uint32_t> file);
void OnMark(void* context, std::uint32_t word, std::uint32_t count);
void TokenStart();
void Note(std::uint32_t opcode, const std::uint32_t* packet, bool token, bool indexed, std::uint64_t record,
          std::span<const std::uint32_t> file, std::uint64_t guest_packet);
void Read(Owed::Reader reader, std::span<const std::uint32_t> file, std::uint64_t guest_packet);
void SubmissionEnd(std::span<const std::uint32_t> file);
void Print(double frames);

constexpr bool OpcodesAre(std::uint32_t clear_state, std::uint32_t dispatch_direct, std::uint32_t dispatch_indirect,
                          std::uint32_t draw_indirect, std::uint32_t draw_index_indirect, std::uint32_t draw_index_2,
                          std::uint32_t draw_indirect_multi, std::uint32_t draw_index_auto, std::uint32_t draw_index_offset_2,
                          std::uint32_t draw_index_indirect_multi, std::uint32_t wait_reg_mem, std::uint32_t event_write,
                          std::uint32_t draw_index_indirect_count_multi, std::uint32_t streamout_flush, std::uint32_t wait_on_register) {
    namespace Op = Owed::Op;
    return clear_state == Op::ClearState && dispatch_direct == Op::DispatchDirect && dispatch_indirect == Op::DispatchIndirect &&
           draw_indirect == Op::DrawIndirect && draw_index_indirect == Op::DrawIndexIndirect && draw_index_2 == Op::DrawIndex2 &&
           draw_indirect_multi == Op::DrawIndirectMulti && draw_index_auto == Op::DrawIndexAuto &&
           draw_index_offset_2 == Op::DrawIndexOffset2 && draw_index_indirect_multi == Op::DrawIndexIndirectMulti &&
           wait_reg_mem == Op::WaitRegMem && event_write == Op::EventWrite &&
           draw_index_indirect_count_multi == Op::DrawIndexIndirectCountMulti && streamout_flush == Op::kStreamoutFlushEvent &&
           wait_on_register == 0;
}
}
