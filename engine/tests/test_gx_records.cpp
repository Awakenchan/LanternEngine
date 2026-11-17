// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/gx_bindings.h"
#include "gpu/shim/engine/gx_record_hash.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace G = BbEngine::GxBindings;
namespace H = BbEngine::GxRecordHash;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

namespace {
int g_failed = 0;
bool g_quiet = false;

void Check(bool ok, const char* what) {
    if (!ok) {
        if (!g_quiet) std::printf("FAILED: %s\n", what);
        ++g_failed;
    }
}

u32 Input(u32 kind, u32 reg, u32 slot, u32 top = 0) {
    return kind | reg << 8 | slot << 16 | top << 24;
}

std::unique_ptr<G::State> Fresh() {
    auto state = std::make_unique<G::State>();
    std::memset(static_cast<void*>(state.get()), 0, sizeof *state);
    state->reason = G::Covered;
    for (G::Stage& stage : state->stages) G::Clear(stage);
    return state;
}

u32 View(u32 slot, u32 word) {
    return 0x10000000u | slot << 8 | word;
}

void BindView(G::State& state, G::Stage& stage, u32 slot) {
    u32 words[8];
    for (u32 w = 0; w < 8; ++w) words[w] = View(slot, w);
    stage.views[slot] = G::Add(state, words, 8);
}

struct Filled {
    G::Reason reason;
    std::vector<G::Fill> fills;
};

Filled Prepare(const G::State& state, const G::Plan& plan, u32 size) {
    Filled out{G::Covered, std::vector<G::Fill>(size)};
    out.reason = G::Prepare(state.stages[0], plan, size, out.fills.data());
    return out;
}

G::Reason Value(const G::State& state, const Filled& filled, const G::Plan& plan, u32 flat, u32& value) {
    const G::Fill& fill = filled.fills[flat];
    if (fill.from == G::FromNone) return G::NoInput;
    return G::Value(state, 0, fill, flat < 16 ? 0 : plan.entries[flat - 16].offset, value);
}

void Direct() {
    auto state = Fresh();
    G::Stage& stage = state->stages[0];
    stage.shader = 0x1000;
    stage.inputs[0] = Input(G::View8, 2, 5);
    stage.inputs[1] = Input(G::Sampler, 10, 3);
    stage.inputs[2] = Input(G::Constant, 14, 1);
    stage.inputs[3] = Input(G::View4, 0, 6);
    stage.count = 4;
    BindView(*state, stage, 5);
    const u32 sampler[4] = {0xa0, 0xa1, 0xa2, 0xa3};
    stage.samplers[3] = G::Add(*state, sampler, 4);
    u32 constant[4];
    G::ConstantWords(0x123456789abcull, 0x100, constant);
    stage.constants[1] = G::Add(*state, constant, 4);
    const G::Plan none{nullptr, 0, false};
    const Filled filled = Prepare(*state, none, 16);
    Check(filled.reason == G::Covered, "user data alone needs no plan");
    u32 value = 0;
    for (u32 w = 0; w < 8; ++w) {
        Check(Value(*state, filled, none, 2 + w, value) == G::Covered && value == View(5, w), "a direct view's words");
    }
    Check(Value(*state, filled, none, 11, value) == G::Covered && value == 0xa1, "a direct sampler's second word");
    Check(Value(*state, filled, none, 14, value) == G::Covered && value == 0x56789abcu, "a constant buffer's address");
    Check(Value(*state, filled, none, 15, value) == G::Covered && value == (0x234u | 0x108000u), "a constant buffer's second word");
    Check(Value(*state, filled, none, 0, value) == G::Unbound, "a view slot with no view");
    Check(Prepare(*state, none, 17).reason == G::NoPlan, "a table word with no plan");
    state->stages[0].shader = 0;
    Check(Prepare(*state, none, 16).reason == G::NoShader, "a stage with no shader");
}

void Tables() {
    auto state = Fresh();
    G::Stage& stage = state->stages[0];
    stage.shader = 0x2000;
    stage.inputs[0] = Input(G::ViewTable, 0, 0);
    stage.inputs[1] = Input(G::SamplerTable, 2, 0);
    stage.inputs[2] = Input(G::ConstantTable, 4, 0);
    stage.inputs[3] = Input(G::Shared, 6, 0);
    stage.count = 4;
    BindView(*state, stage, 3);
    const u32 sampler[4] = {0xb0, 0xb1, 0xb2, 0xb3};
    stage.samplers[9] = G::Add(*state, sampler, 4);
    const u32 constant[4] = {0xc0, 0xc1, 0xc2, 0xc3};
    stage.constants[15] = G::Add(*state, constant, 4);
    for (u32 i = 0; i < G::kSharedWords; ++i) state->shared[i] = 0xe000 + i;
    const G::Entry entries[] = {{0, 0, 3 * 8 + 2}, {2, 0, 9 * 4 + 3}, {4, 0, 15 * 4}, {6, 0, 1}, {0, 0, 129 * 8}, {0, 0, 7 * 8}};
    const G::Plan plan{entries, sizeof entries / sizeof entries[0], true};
    const Filled filled = Prepare(*state, plan, 16 + plan.count);
    Check(filled.reason == G::Covered, "every table word has a source");
    u32 value = 0;
    Check(Value(*state, filled, plan, 16, value) == G::Covered && value == View(3, 2), "a view table entry");
    Check(Value(*state, filled, plan, 17, value) == G::Covered && value == 0xb3, "a sampler table entry");
    Check(Value(*state, filled, plan, 18, value) == G::Covered && value == 0xc0, "the global constant buffer through a table");
    Check(Value(*state, filled, plan, 19, value) == G::SharedBlock, "GX's shared block not captured");
    state->shared_valid = 1;
    Check(Value(*state, filled, plan, 19, value) == G::Covered && value == 0xe001, "a word of GX's shared block");
    Check(Value(*state, filled, plan, 20, value) == G::Outside, "past the view table");
    Check(Value(*state, filled, plan, 21, value) == G::Unbound, "a view table entry with no view");
    Check(G::Value(*state, 0, filled.fills[16], 8 * 3 + 7, value) == G::Covered && value == View(3, 7), "a computed offset in a table");
    const G::Entry dynamic[] = {{0, 0, 0}, {16, 2, 0}};
    Check(Prepare(*state, {dynamic, 2, true}, 18).reason == G::NoInput, "a table behind a view word");
    const G::Entry missing[] = {{8, 0, 0}};
    Check(Prepare(*state, {missing, 1, true}, 17).reason == G::NoInput, "a register no input names");
    Check(Prepare(*state, {missing, 1, true}, 18).reason == G::NoPlan, "a plan shorter than the flat words");
}

void Spilled() {
    auto state = Fresh();
    G::Stage& stage = state->stages[0];
    stage.shader = 0x3000;
    stage.inputs[0] = Input(G::Upload, 12, 0, 8);
    stage.inputs[1] = Input(G::SpillViewTable, 16 + 2, 0);
    stage.inputs[2] = Input(G::SpillShared, 16 + 6, 0);
    stage.count = 3;
    for (u32 i = 0; i < 8; ++i) stage.spill[i] = 0xf000 + i;
    stage.spill_count = 8;
    BindView(*state, stage, 1);
    state->shared_valid = 1;
    for (u32 i = 0; i < G::kSharedWords; ++i) state->shared[i] = 0xe000 + i;
    const G::Entry entries[] = {{12, 0, 4}, {12, 0, 2}, {12, 0, 3}, {17, 0, 9}, {12, 0, 6}, {12, 0, 7}, {20, 0, 5}, {12, 0, 1}};
    const G::Plan plan{entries, sizeof entries / sizeof entries[0], true};
    const Filled filled = Prepare(*state, plan, 16 + plan.count);
    Check(filled.reason == G::Covered, "every spilled word has a source");
    u32 value = 0;
    Check(Value(*state, filled, plan, 16, value) == G::Covered && value == 0xf004, "a spilled word through the upload");
    Check(Value(*state, filled, plan, 17, value) == G::Covered && value == 0xf002, "a spilled pointer's word");
    Check(Value(*state, filled, plan, 19, value) == G::Covered && value == View(1, 1), "a view table behind a spilled pointer");
    Check(Value(*state, filled, plan, 22, value) == G::Covered && value == 0xe005, "the shared block behind a spilled pointer");
    stage.spill_count = 4;
    Check(Value(*state, filled, plan, 16, value) == G::Outside, "a spill word not captured");
    const G::Entry past[] = {{12, 0, 8}};
    const Filled outside = Prepare(*state, {past, 1, true}, 17);
    Check(Value(*state, outside, {past, 1, true}, 16, value) == G::Outside, "past the upload's words");
    const G::Entry nested[] = {{12, 0, 5}, {16, 0, 0}};
    Check(Prepare(*state, {nested, 2, true}, 18).reason == G::NoInput, "a spilled word that is no table");
}

void States() {
    auto state = Fresh();
    state->stages[0].shader = 0x4000;
    const u32 words[8] = {};
    for (u32 i = 0; i < G::kMaxDescriptors; ++i) Check(G::Add(*state, words, 8) == i, "a descriptor's index");
    Check(G::Add(*state, words, 8) == G::kUnbound && state->reason == G::ManyDescriptors, "too many descriptors");
    for (u32 r = 0; r < G::kReasons; ++r) Check(G::ReasonName(static_cast<G::Reason>(r))[0] != '?', "a reason's name");
}

u32 Find(const G::Register* list, u32 count, std::uint16_t index) {
    for (u32 i = 0; i < count; ++i) {
        if (list[i].index == index) return list[i].value;
    }
    return 0xdeadbeef;
}

void Registers() {
    G::Render render{};
    render.raster[0] = 0xffffffff;
    render.raster[1] = 0xffffffff;
    render.blend[0] = 0xf;
    for (u32 i = 0; i < 8; ++i) render.blend[1 + i] = 0xffffffff - i;
    render.blend_factor[0] = 0.25f;
    render.blend_factor[3] = 1.0f;
    render.depth_stencil[0] = 0xffffffff;
    render.depth_stencil[1] = 0xffffffff;
    render.depth_stencil[2] = 0x12345678;
    render.stencil_ref = 0x1ab;
    G::Register list[G::kMaxRegisters];
    u32 count = G::Registers(render, list);
    Check(count == 19, "registers with no viewport or scissor");
    Check(Find(list, count, 0x8e) == 0xf, "CB_TARGET_MASK");
    Check(Find(list, count, 0x1e0) == (0xffffffffu & 0xffff1fff), "CB_BLEND0_CONTROL");
    Check(Find(list, count, 0x1e7) == ((0xffffffffu - 7) & 0xffff1fff), "CB_BLEND7_CONTROL");
    Check(Find(list, count, 0x105) == std::bit_cast<u32>(0.25f) && Find(list, count, 0x108) == std::bit_cast<u32>(1.0f), "blend color");
    Check(Find(list, count, 0x200) == 0xc07007ffu, "DB_DEPTH_CONTROL");
    Check(Find(list, count, 0x10b) == 0xffffffu, "DB_STENCIL_CONTROL");
    Check(Find(list, count, 0x10c) == 0xab3456abu && Find(list, count, 0x10d) == 0xab3456abu, "DB_STENCILREFMASK");
    Check(Find(list, count, 0x204) == 0xf7fe03fu && Find(list, count, 0x205) == 0x393fffu, "clip and mode control");
    Check(Find(list, count, 0x10f) == 0xdeadbeef && Find(list, count, 0x94) == 0xdeadbeef, "no viewport or scissor");

    render.viewports = 1;
    render.viewport[0] = 16;
    render.viewport[1] = 8;
    render.viewport[2] = 1920;
    render.viewport[3] = 1080;
    render.viewport[4] = 0.25f;
    render.viewport[5] = 1;
    render.scissors = 1;
    render.scissor[0] = -4;
    render.scissor[1] = 2;
    render.scissor[2] = 1936;
    render.scissor[3] = 0x9000;
    count = G::Registers(render, list);
    Check(count == 29 - 2, "a scissor rectangle the rasterizer state leaves off");
    render.raster[5] = 0x100;
    count = G::Registers(render, list);
    Check(count == 29, "every register");
    const auto f = [&](std::uint16_t index) { return std::bit_cast<float>(Find(list, count, index)); };
    Check(f(0x10f) == 960 && f(0x110) == 976 && f(0x111) == -540 && f(0x112) == 548, "viewport scale and offset");
    Check(f(0x113) == 0.75f && Find(list, count, 0x114) == 0 && f(0xb4) == 0 && f(0xb5) == 1, "viewport depth");
    Check(Find(list, count, 0x94) == (1u << 31 | 2u << 16 | 0u), "scissor top left");
    Check(Find(list, count, 0x95) == ((0x9000u & 0x7fff) << 16 | 1936u), "scissor bottom right");
    for (u32 i = 0; i < count; ++i) Check(G::RegisterName(list[i].index)[0] != '?', "a register's name");

    for (u32 w = 0; w < 11; ++w) render.targets[2][w] = 0x2000 + w;
    for (u32 w = 0; w < 12; ++w) render.depth_target[w] = 0x3000 + w;
    render.color_valid = 1u << 2;
    render.depth_valid = 1;
    count = G::Registers(render, list);
    Check(count == 29 + 11 + 12, "a color target and a depth target");
    Check(Find(list, count, 0x318 + 30) == 0x2000 && Find(list, count, 0x318 + 30 + 10) == 0x200a, "CB_COLOR2 from the view's words");
    Check(Find(list, count, 0x318) == 0xdeadbeef, "an invalid color target is left alone");
    Check(Find(list, count, 0x10) == 0x3000 && Find(list, count, 0x17) == 0x3007, "the depth target's first eight words");
    Check(Find(list, count, 0x2) == 0x3008 && Find(list, count, 0x5) == 0x3009 && Find(list, count, 0x2af) == 0x300a &&
              Find(list, count, 0xf) == 0x300b, "the depth target's view, htile and info words");
    for (u32 i = 0; i < count; ++i) Check(G::RegisterName(list[i].index)[0] != '?', "a target register's name");
}

void Vertex() {
    auto state = Fresh();
    state->vertex_first = 2;
    state->vertex_count = 3;
    for (u32 e = 0; e < G::kMaxVertex; ++e) {
        for (u32 w = 0; w < 4; ++w) state->vertex[e][w] = e << 8 | w;
    }
    u32 words[4];
    Check(G::VertexWords(*state, 8, words) && words[0] == 0x200 && words[3] == 0x203, "the first vertex buffer of the table");
    Check(G::VertexWords(*state, 16, words) && words[1] == 0x401, "the last vertex buffer of the table");
    Check(!G::VertexWords(*state, 4, words), "before the table");
    Check(!G::VertexWords(*state, 20, words), "past the table");
    Check(!G::VertexWords(*state, 9, words), "not at a buffer's start");
}
}

void Constants() {
    G::Stage stage;
    std::memset(&stage, 0xab, sizeof stage);
    G::Clear(stage);
    bool zero = true;
    for (u32 v : stage.constant_versions) zero = zero && v == 0;
    Check(zero && stage.constants[3] == G::kUnbound, "a cleared stage has no constant buffers and no versions");
    const std::uint64_t address = 0x10a2345670ull;
    u32 words[4];
    G::ConstantWords(address, 0x330, words);
    const std::uint64_t back = std::uint64_t(words[0]) | std::uint64_t(words[1] & 0xfff) << 32;
    Check(back == address, "a constant buffer's address reads back from its words");
    Check(words[2] * 16 >= 0x330 && words[2] * 16 < 0x330 + 16, "a constant buffer's size reads back from its words");
}

void Keys() {
    auto state = Fresh();
    state->vertex_first = 2;
    state->vertex_count = 2;
    state->vertex_keys[2] = G::BufferKey(0x1000, 1);
    state->vertex_keys[3] = G::BufferKey(0x2000, 1);
    Check(G::VertexKey(*state, 8) == G::BufferKey(0x1000, 1), "a vertex stream's key comes from its table entry");
    Check(G::VertexKey(*state, 4) == 0 && G::VertexKey(*state, 16) == 0 && G::VertexKey(*state, 9) == 0, "no key outside the table or off a buffer's start");
    Check(G::BufferKey(0x1000, 1) != G::BufferKey(0x1000, 2), "a buffer's key changes with its version");
    Check(G::BufferKey(0x1000, 1) != G::BufferKey(0x1008, 1), "two buffers have different keys");
    Check(G::BufferKey(0, 5) == 0 && (G::BufferKey(0x1000, 0) & 1), "no object has no key, an object always has one");
}

G::Render Sample(u32 seed) {
    G::Render render;
    std::memset(&render, 0, sizeof render);
    auto* bytes = reinterpret_cast<unsigned char*>(&render);
    for (std::size_t i = 0; i < sizeof render; ++i) bytes[i] = static_cast<unsigned char>(i * 31 + seed);
    return render;
}

void Memo() {
    G::RenderMemo memo;
    const G::Render render = Sample(7);
    Check(!G::Unchanged(memo, render), "an empty memo never reports the render state unchanged");
    G::Keep(memo, render, true);
    Check(G::Unchanged(memo, render), "a kept clean render state is unchanged on the next draw");
    G::Keep(memo, render, false);
    Check(!G::Unchanged(memo, render), "a render state kept with differing registers is never skipped");
    G::Keep(memo, render, true);
    bool every = true;
    for (std::size_t i = 0; i < sizeof render; ++i) {
        G::Render other = render;
        reinterpret_cast<unsigned char*>(&other)[i] ^= 0x10;
        every = every && !G::Unchanged(memo, other);
    }
    Check(every, "a change in any byte of the render state is seen");
    G::Render tail = render;
    tail.index_base ^= 1;
    Check(!G::Unchanged(memo, tail), "a changed index base is seen");
    tail = render;
    tail.depth_target[11] ^= 1;
    Check(!G::Unchanged(memo, tail), "a changed depth target word is seen");
    G::Render zero = render, negative = render;
    zero.blend_factor[0] = 0.0f;
    negative.blend_factor[0] = -0.0f;
    G::Keep(memo, zero, true);
    Check(!G::Unchanged(memo, negative), "a negative zero blend factor counts as a change");
    G::Keep(memo, Sample(9), true);
    Check(!G::Unchanged(memo, render) && G::Unchanged(memo, Sample(9)), "the memo holds only the last render state");
}

constexpr std::size_t kBlockWords = 0x4f0 / 8;

u64 Next(u64& state) {
    u64 z = state += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

u64 Serial(u64 hash, const u64* words, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) hash = H::Mix(hash, words[i]);
    return hash;
}

void Hash() {
    u64 seed = 1;
    std::vector<u64> block(kBlockWords);
    for (u64& word : block) word = Next(seed);
    const u64 base = H::Block(0x1234, block.data(), kBlockWords);
    Check(base == H::Block(0x1234, block.data(), kBlockWords), "the block hash is the same for the same words");
    Check(base != H::Block(0x1235, block.data(), kBlockWords), "the incoming hash changes the block hash");
    Check(base != H::Block(0x1234, block.data(), kBlockWords - 1), "the block length changes the block hash");
    bool every_low = true, every_high = true, every_swap = true;
    for (std::size_t i = 0; i < kBlockWords; ++i) {
        std::vector<u64> other = block;
        other[i] ^= 1;
        every_low = every_low && H::Block(0x1234, other.data(), kBlockWords) != base;
        other = block;
        other[i] ^= u64(1) << 63;
        every_high = every_high && H::Block(0x1234, other.data(), kBlockWords) != base;
        if (i + 1 < kBlockWords) {
            other = block;
            std::swap(other[i], other[i + 1]);
            every_swap = every_swap && H::Block(0x1234, other.data(), kBlockWords) != base;
        }
    }
    Check(every_low, "the lowest bit of every word changes the block hash");
    Check(every_high, "the highest bit of every word changes the block hash");
    Check(every_swap, "swapping two neighbouring words changes the block hash");
    std::vector<u64> same(kBlockWords, 0);
    const u64 empty = H::Block(0x1234, same.data(), kBlockWords);
    bool zeros = true;
    for (std::size_t i = 0; i < kBlockWords; ++i) {
        same[i] = 1;
        zeros = zeros && H::Block(0x1234, same.data(), kBlockWords) != empty;
        same[i] = 0;
    }
    Check(zeros, "one set word in an empty block changes the block hash");
}

void Strength() {
    u64 seed = 77;
    std::vector<u64> block(kBlockWords);
    double lane_sum = 0, serial_sum = 0, lane_min = 64, serial_min = 64;
    std::size_t flips = 0;
    constexpr int kBlocks = 8;
    std::vector<double> lane_bits(kBlockWords * 64, 0), serial_bits(kBlockWords * 64, 0);
    for (int b = 0; b < kBlocks; ++b) {
        for (u64& word : block) word = Next(seed);
        const u64 start = Next(seed);
        const u64 lane = H::Block(start, block.data(), kBlockWords), serial = Serial(start, block.data(), kBlockWords);
        for (std::size_t i = 0; i < kBlockWords; ++i) {
            for (u32 bit = 0; bit < 64; ++bit) {
                block[i] ^= u64(1) << bit;
                const int l = std::popcount(lane ^ H::Block(start, block.data(), kBlockWords));
                const int s = std::popcount(serial ^ Serial(start, block.data(), kBlockWords));
                block[i] ^= u64(1) << bit;
                lane_bits[i * 64 + bit] += l;
                serial_bits[i * 64 + bit] += s;
                lane_sum += l;
                serial_sum += s;
                ++flips;
            }
        }
    }
    for (std::size_t k = 0; k < lane_bits.size(); ++k) {
        lane_min = std::min(lane_min, lane_bits[k] / kBlocks);
        serial_min = std::min(serial_min, serial_bits[k] / kBlocks);
    }
    const double lane_mean = lane_sum / double(flips), serial_mean = serial_sum / double(flips);
    if (!g_quiet) {
        std::printf("gx-records-test: one flipped input bit flips %.2f output bits on average (serial %.2f), %.2f at least (serial %.2f)\n",
                    lane_mean, serial_mean, lane_min, serial_min);
    }
    Check(lane_mean >= serial_mean - 0.25 && lane_mean > 30.0 && lane_mean < 34.0, "the block hash flips about half its bits for one input bit");
    Check(lane_min >= serial_min, "no input bit reaches fewer output bits than in the serial hash");
    std::vector<u64> hashes;
    std::vector<u64> pairs(kBlockWords, 0);
    for (std::size_t first : {std::size_t(0), std::size_t(3), std::size_t(150)}) {
        for (std::size_t gap : {std::size_t(1), std::size_t(4)}) {
            for (u64 x = 0; x < 256; ++x) {
                for (u64 y = 0; y < 256; ++y) {
                    pairs[first] = x;
                    pairs[first + gap] = y;
                    hashes.push_back(H::Block(first * 8 + gap, pairs.data(), kBlockWords));
                }
            }
            pairs[first] = pairs[first + gap] = 0;
        }
    }
    std::sort(hashes.begin(), hashes.end());
    Check(std::adjacent_find(hashes.begin(), hashes.end()) == hashes.end(), "no two of 393216 nearby blocks share a hash");
}

int RunSuite() {
    g_failed = 0;
    Direct();
    Tables();
    Spilled();
    States();
    Registers();
    Vertex();
    Constants();
    Keys();
    Memo();
    Hash();
    Strength();
    return g_failed;
}

#if defined(LANTERN_GX_RECORDS_MUTANTS) && defined(LANTERN_GX_RECORD_HASH_MUTANTS)
template <typename M> bool Survivors(M& selected, int count, const char* name) {
    bool any = false;
    for (int m = 1; m < count; ++m) {
        selected = static_cast<M>(m);
        if (!RunSuite()) {
            std::printf("gx-records-test: %s mutant %d survived\n", name, m);
            any = true;
        }
    }
    selected = static_cast<M>(0);
    return any;
}
#endif

int main() {
#if defined(LANTERN_GX_RECORDS_MUTANTS) && defined(LANTERN_GX_RECORD_HASH_MUTANTS)
    g_quiet = true;
    if (RunSuite()) {
        std::printf("gx-records-test mutants: the unmutated run failed %d checks\n", g_failed);
        return 1;
    }
    const bool memo = Survivors(G::selected_mutant, static_cast<int>(G::Mutant::kCount), "memo");
    const bool hash = Survivors(H::selected_mutant, static_cast<int>(H::Mutant::kCount), "hash");
    if (memo || hash) return 1;
    std::printf("gx-records-test mutants: all %d killed\n", static_cast<int>(G::Mutant::kCount) + static_cast<int>(H::Mutant::kCount) - 2);
    return 0;
#else
    if (RunSuite()) {
        std::printf("gx-records-test: %d failed\n", g_failed);
        return 1;
    }
    std::printf("gx-records-test: ok\n");
    return 0;
#endif
}
