// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::Pm4Walk {
using u32 = std::uint32_t;

inline constexpr u32 kPadType = 2;
inline constexpr u32 kComputeShFirst = 0x200;

enum class Step : std::uint8_t { Packet, Pad, Split };

#ifdef LANTERN_DECODER_MUTANTS
enum class Mutant : std::uint8_t { None, SplitBeforePad, KeptNestedFrame, UnmarkedComputeWrite, kCount };
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

inline Step Classify(u32 type, std::size_t packet_dwords, std::size_t remaining_dwords) {
#ifdef LANTERN_DECODER_MUTANTS
    if (Mutated(Mutant::SplitBeforePad) && packet_dwords > remaining_dwords) return Step::Split;
#endif
    if (type == kPadType) return Step::Pad;
    return packet_dwords > remaining_dwords ? Step::Split : Step::Packet;
}

struct ComputeShRoute {
    bool cs_state = false;
    u32 graphics_word = 0;
    u32 graphics_dwords = 0;
};

inline ComputeShRoute RouteComputeSh(u32 sh_word_offset, u32 cs_window_dwords, u32 reg_offset, u32 dwords) {
    if (reg_offset >= kComputeShFirst && reg_offset <= kComputeShFirst + cs_window_dwords) return {true, 0, 0};
#ifdef LANTERN_DECODER_MUTANTS
    if (Mutated(Mutant::UnmarkedComputeWrite)) return {false, 0, 0};
#endif
    return {false, sh_word_offset + reg_offset, dwords};
}

template <typename Handle>
class OwnedFrame {
public:
    explicit OwnedFrame(Handle handle) noexcept : handle_(handle) {}
    OwnedFrame(const OwnedFrame&) = delete;
    OwnedFrame& operator=(const OwnedFrame&) = delete;
    ~OwnedFrame() {
        Free();
    }

    bool Free() noexcept {
        if (!handle_) return false;
#ifdef LANTERN_DECODER_MUTANTS
        if (Mutated(Mutant::KeptNestedFrame)) {
            handle_ = Handle{};
            return false;
        }
#endif
        handle_.destroy();
        handle_ = Handle{};
        return true;
    }

private:
    Handle handle_;
};
}
