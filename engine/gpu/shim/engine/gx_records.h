// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx_bindings.h"
#include <cstddef>
#include <cstdint>

namespace BbEngine::GxRecords {
bool Install(std::uint8_t* image, std::size_t size);
bool On();
bool Checked();
bool Compared();
bool Used();
void Stamp(std::uint64_t record_address, std::uint32_t count);
std::uint64_t Take(std::uint64_t record_address);
const GxBindings::State* Taken(std::uint64_t record);
bool Intact();
void NoteDraw(GxBindings::Reason bindings, std::uint32_t kept, bool render);
void NoteWord(std::uint32_t stage, std::uint32_t flat, std::uint32_t ours, std::uint32_t old, std::uint64_t program);
void NoteRegister(std::uint16_t index, std::uint32_t ours, std::uint32_t old);
void NoteTarget(int target, std::uint64_t ours, std::uint64_t old);
bool ConstantsTracked();
bool BindsByObject();
void NoteBind(bool reused, bool checked, bool same, std::uint64_t address, std::uint64_t size);
void NoteConstant(bool reused, bool checked, bool same, std::uint64_t address, std::uint32_t size);
}
