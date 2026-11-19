// SPDX-License-Identifier: GPL-3.0-or-later
#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "gpu/shim/engine/pm4_walk.h"

namespace {
using BbEngine::Pm4Walk::u32;
using BbEngine::Pm4Walk::Step;

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr u32 kPadDword = 0x80000000u;
constexpr u32 kShRegWordOffset = 0x2C00;
constexpr u32 kComputeWindowDwords = 0x50;
constexpr u32 kOpcodeNop = 0x10;
constexpr u32 kOpcodeIndirectBuffer = 0x3F;
constexpr u32 kOpcodeSetShReg = 0x76;

constexpr u32 Type3(u32 opcode, u32 body_dwords) {
    return (3u << 30) | ((body_dwords - 1) << 16) | (opcode << 8);
}

constexpr u32 HeaderType(u32 header) {
    return header >> 30;
}

constexpr std::size_t PacketDwords(u32 header) {
    return ((header >> 16) & 0x3fff) + 2;
}

std::vector<u32> WalkHeaders(std::span<const u32> buffer, std::size_t& stopped_at) {
    std::vector<u32> headers;
    std::size_t at = 0;
    while (at < buffer.size()) {
        const u32 header = buffer[at];
        const Step step = BbEngine::Pm4Walk::Classify(HeaderType(header), PacketDwords(header), buffer.size() - at);
        if (step == Step::Split) break;
        headers.push_back(header);
        at += step == Step::Pad ? 1 : PacketDwords(header);
    }
    stopped_at = at;
    return headers;
}

void TestClassify() {
    const std::vector<u32> trailing_pad = {Type3(kOpcodeNop, 2), 0, 0, kPadDword};
    std::size_t stopped = 0;
    const auto headers = WalkHeaders(trailing_pad, stopped);
    Expect(headers.size() == 2 && headers[1] == kPadDword && stopped == trailing_pad.size(),
           "a pad dword as a buffer's last dword is stepped over, not held for the next buffer");

    const std::vector<u32> pads_between = {kPadDword, Type3(kOpcodeSetShReg, 3), 0x10, 1, 2, kPadDword, kPadDword};
    const auto between = WalkHeaders(pads_between, stopped);
    Expect(between.size() == 4 && stopped == pads_between.size(), "pads before, between and after packets");

    const std::vector<u32> split = {kPadDword, Type3(kOpcodeSetShReg, 4), 0x10, 1};
    const auto cut = WalkHeaders(split, stopped);
    Expect(cut.size() == 1 && stopped == 1, "a type-3 packet running past the buffer is held for the next one");

    Expect(BbEngine::Pm4Walk::Classify(BbEngine::Pm4Walk::kPadType, 2, 1) == Step::Pad, "pad with one dword left");
    Expect(BbEngine::Pm4Walk::Classify(3, 3, 3) == Step::Packet, "a packet that ends at the buffer's end fits");
    Expect(BbEngine::Pm4Walk::Classify(3, 4, 3) == Step::Split, "a packet one dword too long is split");
}

void TestRouteComputeSh() {
    using BbEngine::Pm4Walk::RouteComputeSh;
    const auto graphics = RouteComputeSh(kShRegWordOffset, kComputeWindowDwords, 0x0C, 4);
    Expect(!graphics.cs_state && graphics.graphics_word == kShRegWordOffset + 0x0C && graphics.graphics_dwords == 4,
           "a compute-queue write to graphics SH registers names the words the stage B copy must take");
    const auto first = RouteComputeSh(kShRegWordOffset, kComputeWindowDwords, BbEngine::Pm4Walk::kComputeShFirst, 2);
    Expect(first.cs_state && first.graphics_dwords == 0, "COMPUTE_DISPATCH_INITIATOR goes to cs_state");
    const auto user_data_15 = RouteComputeSh(kShRegWordOffset, kComputeWindowDwords, 0x24F, 4);
    Expect(user_data_15.cs_state && user_data_15.graphics_dwords == 0, "USER_DATA_15 goes to cs_state (clamped there)");
    const auto window_end = RouteComputeSh(kShRegWordOffset, kComputeWindowDwords, 0x250, 1);
    Expect(window_end.cs_state, "one past the window stays with cs_state, as ApplyGraphicsRegisterPacket skips it");
    const auto below = RouteComputeSh(kShRegWordOffset, kComputeWindowDwords, 0x1FF, 1);
    Expect(!below.cs_state && below.graphics_word == kShRegWordOffset + 0x1FF && below.graphics_dwords == 1,
           "the last graphics SH register");
    const auto above = RouteComputeSh(kShRegWordOffset, kComputeWindowDwords, 0x251, 2);
    Expect(!above.cs_state && above.graphics_dwords == 2, "past the window is a register file write");
}

int live_frames = 0;
int frames_made = 0;

struct CountedTask {
    struct promise_type {
        promise_type() noexcept {
            ++live_frames;
            ++frames_made;
        }
        promise_type(const promise_type&) = delete;
        promise_type& operator=(const promise_type&) = delete;
        ~promise_type() {
            --live_frames;
        }
        CountedTask get_return_object() {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept {
            return {};
        }
        std::suspend_always final_suspend() noexcept {
            return {};
        }
        std::suspend_always yield_value(int) noexcept {
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {
            std::abort();
        }
    };
    std::coroutine_handle<promise_type> handle;
};

using Nested = BbEngine::Pm4Walk::OwnedFrame<std::coroutine_handle<CountedTask::promise_type>>;

CountedTask Decode(std::span<const u32> buffer, int depth, int& frees) {
    for (const u32 header : buffer) {
        if (((header >> 8) & 0xff) == kOpcodeIndirectBuffer && depth < 3) {
            auto task = Decode(buffer, depth + 1, frees);
            Nested nested{task.handle};
            task.handle.resume();
            while (!task.handle.done()) {
                co_yield 0;
                task.handle.resume();
            }
            if (nested.Free()) ++frees;
        } else {
            co_yield 0;
        }
    }
}

void TestNestedFrames() {
    live_frames = 0;
    frames_made = 0;
    const std::vector<u32> buffer = {Type3(kOpcodeNop, 1), Type3(kOpcodeIndirectBuffer, 3), Type3(kOpcodeIndirectBuffer, 3)};
    int frees = 0;
    auto top = Decode(buffer, 0, frees);
    while (!top.handle.done()) top.handle.resume();
    top.handle.destroy();
    Expect(frames_made == 1 + 2 + 4 + 8, "three levels of two nested buffers each");
    Expect(frees == frames_made - 1, "every nested buffer reports its frame freed");
    Expect(live_frames == 0, "no nested frame outlives its buffer");

    live_frames = 0;
    {
        auto task = Decode(buffer, 2, frees);
        Nested nested{task.handle};
        task.handle.resume();
        task.handle.resume();
        Expect(live_frames == 2, "the outer buffer is suspended inside a nested one");
    }
    Expect(live_frames == 0, "a nested frame left before it finished is freed with its owner");
}

int RunSuite() {
    failures = 0;
    TestClassify();
    TestRouteComputeSh();
    TestNestedFrames();
    return failures;
}
}

int main() {
#ifdef LANTERN_DECODER_MUTANTS
    using BbEngine::Pm4Walk::Mutant;
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        BbEngine::Pm4Walk::selected_mutant = static_cast<Mutant>(m);
        const int found = RunSuite();
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("decoder walk mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("decoder walk mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("decoder walk mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("decoder walk mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("decoder walk: %d failures\n", failures);
        return 1;
    }
    std::printf("decoder walk: all tests passed\n");
    return 0;
#endif
}
