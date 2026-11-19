// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx_bindings.h"
#include <array>
#include <atomic>
#include <bit>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <type_traits>

namespace BbEngine::GxWhole {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace Gx = GxBindings;

enum class Mode : u8 { Off, Feed, Compare };
inline constexpr const char* kSwitch = "LANTERN_GX_WHOLE";

inline Mode ModeOf(const char* value) {
    if (value && value[0] == '2') return Mode::Compare;
    return value && value[0] == '1' ? Mode::Feed : Mode::Off;
}

inline constexpr u32 kContext = 0xA000, kUconfig = 0xC000, kSh = 0x2C00, kRegisterWords = 0xD000;
inline constexpr u32 kColorTargets = 8, kColorStride = 15, kColorWords = 11, kColorInfoWord = 4;
inline constexpr u32 kDepthWords = 8, kDepthViewWord = 8, kDepthHtileWord = 9;
inline constexpr u32 kTopologies = 0x2a, kIndexFormat32 = 0x2a;
inline constexpr u32 kViewportControl = 0x43f, kStagesVertexPixel = 0;
inline constexpr u32 kClipMask = 0xf7fe03f, kModeMask = 0x393fff, kBlendMask = 0xffff1fff;
inline constexpr u32 kDepthStencilEnableBits = 0x3;
inline constexpr u32 kDepthControlMask = 0xc07007ff, kStencilControlMask = 0xffffff, kStencilMaskBits = 0xffff00;
inline constexpr u32 kModifierKeep = 0xfcfffc3f, kScissorMask = 0x7fff, kWindowOffsetDisable = 1u << 31;
inline constexpr u32 kIndexBaseHighMask = 0xff, kIndexBaseLowMask = 0xfffffffe, kPrimitiveMask = 0x3f, kByteMask = 0xff;
inline constexpr u32 kSemanticMissing = 0x20, kSemanticOffsetMask = 0x3f, kSemanticDefaultMask = 3;
inline constexpr u32 kScissorEnableByte = 8, kPixelInputs = 32;
inline constexpr float kPolyOffsetScale = 16.0f;
inline constexpr u32 kSampleEvery = 64, kMaxValues = 256, kEvidencePerDraw = 8;

inline constexpr std::array<u8, kTopologies> kPrimitiveOfTopology = {
    0, 1, 2, 3, 4, 6, 10, 11, 12, 13, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

enum class Source : u8 { Record, Kept, Token };

enum Field : u16 {
    CbTargetMask, CbBlendControl, CbBlendColor, DbDepthControl, DbStencilControl, DbStencilRefMask, DbDepthBounds,
    PaClClipCntl, PaSuScModeCntl, PaSuPolyOffset, PaScModeCntl0, PaClVport0, PaScVportZ0, PaClVteCntl,
    PaScScreenScissor, PaScVportScissor0, CbColorTarget, CbColorUnbound, DbDepthTarget, DbDepthView, DbHtileBase,
    DbDepthClear, DbStencilClear, VgtIndexType, VgtIndexBase, VgtPrimitiveType, VgtShaderStages, VsProgram,
    PaClVsOutCntl, PsProgram, SpiShaderFormats, SpiPsInputEnable, SpiPsInControl, SpiBarycentric, DbShaderControl,
    CbShaderMask, SpiPsInputCntl,
    DbRenderControl, DbRenderOverride, PaScWindowOffset, PaScWindowScissor, PaScGenericScissor, PaScVportScissorRest,
    PaScVportZRest, PaClVportRest, VgtRestartIndex, CbColorControl, PaSuLineCntl, VgtGsMode, VgtGsOutPrimType,
    VgtRestartEnable, VgtInstanceStepRate, VgtEsGsItemSize, VgtGsMaxVertOut, VgtLsHsConfig, VgtGsVertItemSize,
    VgtTessConfig, VgtGsInstanceCnt, VgtStrmoutConfig, PaScAaConfig,
    VgtIndexOffset, VgtNumIndices, VgtDmaMaxSize, VgtNumInstances,
    kFields
};

struct FieldInfo {
    const char* name;
    Source source;
    u32 first;
    u32 count;
};

inline constexpr std::array<FieldInfo, kFields> kFieldInfo = {{
    {"CB_TARGET_MASK", Source::Record, kContext + 0x08e, 1},
    {"CB_BLEND_CONTROL", Source::Record, kContext + 0x1e0, 8},
    {"CB_BLEND_COLOR", Source::Record, kContext + 0x105, 4},
    {"DB_DEPTH_CONTROL", Source::Record, kContext + 0x200, 1},
    {"DB_STENCIL_CONTROL", Source::Record, kContext + 0x10b, 1},
    {"DB_STENCILREFMASK", Source::Record, kContext + 0x10c, 2},
    {"DB_DEPTH_BOUNDS", Source::Record, kContext + 0x008, 2},
    {"PA_CL_CLIP_CNTL", Source::Record, kContext + 0x204, 1},
    {"PA_SU_SC_MODE_CNTL", Source::Record, kContext + 0x205, 1},
    {"PA_SU_POLY_OFFSET", Source::Record, kContext + 0x2df, 5},
    {"PA_SC_MODE_CNTL_0", Source::Record, kContext + 0x292, 1},
    {"PA_CL_VPORT_0", Source::Record, kContext + 0x10f, 6},
    {"PA_SC_VPORT_ZMIN_MAX_0", Source::Record, kContext + 0x0b4, 2},
    {"PA_CL_VTE_CNTL", Source::Record, kContext + 0x206, 1},
    {"PA_SC_SCREEN_SCISSOR", Source::Record, kContext + 0x00c, 2},
    {"PA_SC_VPORT_SCISSOR_0", Source::Record, kContext + 0x094, 2},
    {"CB_COLOR", Source::Record, kContext + 0x318, kColorTargets * kColorStride},
    {"CB_COLOR_INFO unbound", Source::Record, kContext + 0x318, kColorTargets * kColorStride},
    {"DB_DEPTH_TARGET", Source::Record, kContext + 0x010, kDepthWords},
    {"DB_DEPTH_VIEW", Source::Record, kContext + 0x002, 1},
    {"DB_HTILE_DATA_BASE", Source::Record, kContext + 0x005, 1},
    {"DB_DEPTH_CLEAR", Source::Record, kContext + 0x00b, 1},
    {"DB_STENCIL_CLEAR", Source::Record, kContext + 0x00a, 1},
    {"VGT_INDEX_TYPE", Source::Record, kContext + 0x29f, 1},
    {"VGT_INDEX_BASE", Source::Record, kContext + 0x1f9, 2},
    {"VGT_PRIMITIVE_TYPE", Source::Record, kUconfig + 0x242, 1},
    {"VGT_SHADER_STAGES_EN", Source::Record, kContext + 0x2d5, 1},
    {"SPI_SHADER_PGM_VS", Source::Record, kSh + 0x048, 4},
    {"PA_CL_VS_OUT_CNTL", Source::Record, kContext + 0x207, 1},
    {"SPI_SHADER_PGM_PS", Source::Record, kSh + 0x008, 4},
    {"SPI_SHADER_Z_COL_FORMAT", Source::Record, kContext + 0x1c4, 2},
    {"SPI_PS_INPUT_ENA_ADDR", Source::Record, kContext + 0x1b3, 2},
    {"SPI_PS_IN_CONTROL", Source::Record, kContext + 0x1b6, 1},
    {"SPI_BARYC_CNTL", Source::Record, kContext + 0x1b8, 1},
    {"DB_SHADER_CONTROL", Source::Record, kContext + 0x203, 1},
    {"CB_SHADER_MASK", Source::Record, kContext + 0x08f, 1},
    {"SPI_PS_INPUT_CNTL", Source::Record, kContext + 0x191, kPixelInputs},
    {"DB_RENDER_CONTROL", Source::Kept, kContext + 0x000, 1},
    {"DB_RENDER_OVERRIDE", Source::Kept, kContext + 0x003, 1},
    {"PA_SC_WINDOW_OFFSET", Source::Kept, kContext + 0x080, 1},
    {"PA_SC_WINDOW_SCISSOR", Source::Kept, kContext + 0x081, 2},
    {"PA_SC_GENERIC_SCISSOR", Source::Kept, kContext + 0x090, 2},
    {"PA_SC_VPORT_SCISSOR_1_15", Source::Kept, kContext + 0x096, 2 * (Gx::kViewports - 1)},
    {"PA_SC_VPORT_ZMIN_MAX_1_15", Source::Kept, kContext + 0x0b6, 2 * (Gx::kViewports - 1)},
    {"PA_CL_VPORT_1_15", Source::Kept, kContext + 0x115, Gx::kViewportFloats * (Gx::kViewports - 1)},
    {"VGT_MULTI_PRIM_IB_RESET_INDX", Source::Kept, kContext + 0x103, 1},
    {"CB_COLOR_CONTROL", Source::Kept, kContext + 0x202, 1},
    {"PA_SU_LINE_CNTL", Source::Kept, kContext + 0x282, 1},
    {"VGT_GS_MODE", Source::Kept, kContext + 0x290, 1},
    {"VGT_GS_OUT_PRIM_TYPE", Source::Kept, kContext + 0x29b, 1},
    {"VGT_MULTI_PRIM_IB_RESET_EN", Source::Kept, kContext + 0x2a5, 1},
    {"VGT_INSTANCE_STEP_RATE", Source::Kept, kContext + 0x2a8, 2},
    {"VGT_ESGS_RING_ITEMSIZE", Source::Kept, kContext + 0x2ab, 1},
    {"VGT_GS_MAX_VERT_OUT", Source::Kept, kContext + 0x2ce, 1},
    {"VGT_LS_HS_CONFIG", Source::Kept, kContext + 0x2d6, 1},
    {"VGT_GS_VERT_ITEMSIZE", Source::Kept, kContext + 0x2d7, 4},
    {"VGT_TF_PARAM", Source::Kept, kContext + 0x2db, 1},
    {"VGT_GS_INSTANCE_CNT", Source::Kept, kContext + 0x2e4, 1},
    {"VGT_STRMOUT_CONFIG", Source::Kept, kContext + 0x2e5, 1},
    {"PA_SC_AA_CONFIG", Source::Kept, kContext + 0x2f8, 1},
    {"VGT_INDX_OFFSET", Source::Token, kContext + 0x102, 1},
    {"VGT_NUM_INDICES", Source::Token, kUconfig + 0x24c, 1},
    {"VGT_DMA_MAX_SIZE", Source::Token, kContext + 0x29e, 1},
    {"VGT_NUM_INSTANCES", Source::Token, kUconfig + 0x24d, 1},
}};
static_assert(kFieldInfo.size() == kFields);
static_assert(kFieldInfo[SpiPsInputCntl].source == Source::Record && kFieldInfo[DbRenderControl].source == Source::Kept);
static_assert(kFieldInfo[PaScAaConfig].source == Source::Kept && kFieldInfo[VgtIndexOffset].source == Source::Token && kFieldInfo[VgtDmaMaxSize].source == Source::Token);

inline constexpr u32 KeptWords() {
    u32 words = 0;
    for (const FieldInfo& info : kFieldInfo) words += info.source == Source::Kept ? info.count : 0;
    return words;
}
inline constexpr u32 kKeptWords = KeptWords();

#ifdef LANTERN_GX_WHOLE_MUTANTS
enum class Mutant : u8 {
    None, ViewportYScaleSign, IndexTypeInverted, TopologyShifted, BlendMaskDropped, SemanticDefaultZero,
    CompareIgnoresMask, KeptMemoryKept, SampleEveryDraw, SuspectMissed, ScissorEnableIgnored, ModifierIgnored,
    DepthUnboundSkipped, KeyIgnoresStamp, ConsumeKeepsSlot, UnboundAlwaysCleared, WrittenIgnoresShaderMask,
    DepthUnboundAlwaysCleared, UnusedIgnoresTargetMask, StencilAlwaysOff, SlotZeroUnread, BoundsAlwaysOff, kCount
};
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#endif

struct Value {
    u16 field;
    u32 reg;
    u32 value;
    u32 mask;
};

struct Difference {
    u16 field;
    u32 reg;
    u32 ours;
    u32 registers;
};

enum class Unused : u8 { Read, SlotNotWritten, StencilOff, BoundsOff, kCount };
inline constexpr u32 kUnusedClasses = static_cast<u32>(Unused::kCount);
inline constexpr std::array<const char*, kUnusedClasses> kUnusedNames = {
    "read", "slot outside CB_TARGET_MASK", "stencil off", "depth bounds off"};

struct Counts {
    u64 total = 0, covered = 0, no_payload = 0, no_whole = 0, overwritten = 0;
    u64 unused_differ = 0;
    u64 unused_by[kUnusedClasses]{};
    u64 field_unused[kFields]{};
    u64 checked = 0, skipped = 0, differ = 0, fields = 0, fields_differ = 0;
    u64 kept_checks = 0, kept_words = 0, kept_changed = 0;
    u64 other_writers = 0;
    u64 field_differ[kFields]{};
    u64 field_changed[kFields]{};
};
static_assert(std::is_trivially_copyable_v<Counts> && sizeof(Counts) % sizeof(u64) == 0);
inline constexpr std::size_t kCountWords = sizeof(Counts) / sizeof(u64);

struct KeptMemory {
    bool valid = false;
    u32 words[kKeptWords]{};
};

inline u32 Bits(float value) {
    return std::bit_cast<u32>(value);
}

inline u32 Truncate(float value) {
    constexpr float kLow = -2147483648.0f, kHigh = 2147483648.0f;
    if (!(value >= kLow && value < kHigh)) return 0x80000000u;
    return static_cast<u32>(static_cast<std::int32_t>(value));
}

inline u32 ClampScissor(std::int32_t value) {
    return static_cast<u32>(value < 0 ? 0 : value) & kScissorMask;
}

inline u32 PrimitiveFromTopology(u32 topology) {
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::TopologyShifted) && topology + 1 < kTopologies) return kPrimitiveOfTopology[topology + 1];
#endif
    return topology < kTopologies ? kPrimitiveOfTopology[topology] : 0;
}

inline u32 IndexType(u32 format) {
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::IndexTypeInverted)) return format == kIndexFormat32 ? 0 : 1;
#endif
    return format == kIndexFormat32 ? 1 : 0;
}

inline u32 PixelInputControl(const Gx::Whole& whole, u32 input) {
    const u32 pixel = whole.pixel_semantics[input];
    u32 offset = kSemanticMissing;
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::SemanticDefaultZero)) offset = 0;
#endif
    for (u32 v = 0; v < whole.vertex_semantics_count && v < Gx::kMaxSemantics; ++v) {
        if (whole.vertex_semantics[v][0] == (pixel & kByteMask)) {
            offset = whole.vertex_semantics[v][1];
            break;
        }
    }
    u32 control = (((pixel >> 7) & 0x1e0) | offset) & kSemanticOffsetMask;
    control |= ((pixel >> 8) & kSemanticDefaultMask) << 8;
    control |= (((pixel >> 12) | (pixel >> 10)) & 1) << 10;
    return control;
}

class Writer {
public:
    explicit Writer(std::span<Value, kMaxValues> out) : out_(out) {}
    void Put(Field field, u32 offset, u32 value, u32 mask = ~0u) {
        if (count_ < kMaxValues) out_[count_++] = {field, kFieldInfo[field].first + offset, value, mask};
    }
    u32 Count() const {
        return count_;
    }

private:
    std::span<Value, kMaxValues> out_;
    u32 count_ = 0;
};

inline void PutOutputMerger(const Gx::Render& render, const Gx::Whole& whole, Writer& out) {
    out.Put(CbTargetMask, 0, render.blend[0]);
    u32 blend_mask = kBlendMask;
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::BlendMaskDropped)) blend_mask = ~0u;
#endif
    for (u32 i = 0; i < kColorTargets; ++i) out.Put(CbBlendControl, i, render.blend[1 + i] & blend_mask);
    for (u32 i = 0; i < 4; ++i) out.Put(CbBlendColor, i, Bits(render.blend_factor[i]));
    out.Put(DbDepthControl, 0, render.depth_stencil[0] & kDepthControlMask);
    out.Put(DbStencilControl, 0, render.depth_stencil[1] & kStencilControlMask);
    const u32 ref = render.stencil_ref & kByteMask;
    const u32 masks = (render.depth_stencil[2] & kStencilMaskBits) | ref | ref << 24;
    out.Put(DbStencilRefMask, 0, masks);
    out.Put(DbStencilRefMask, 1, masks);
    out.Put(DbDepthBounds, 0, Bits(whole.depth_bounds[0]));
    out.Put(DbDepthBounds, 1, Bits(whole.depth_bounds[1]));
}

inline void PutViewport(const Gx::Render& render, Writer& out) {
    if (!render.viewports) return;
    const float x = render.viewport[0], y = render.viewport[1];
    const float right = x + render.viewport[2];
    const float bottom = y + render.viewport[3];
    const float height = bottom - y;
    const float xscale = (right - x) * 0.5f;
    float yscale = height * -0.5f;
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::ViewportYScaleSign)) yscale = -yscale;
#endif
    const float half_height = height * 0.5f;
    out.Put(PaScVportZ0, 0, Bits(0.0f));
    out.Put(PaScVportZ0, 1, Bits(1.0f));
    out.Put(PaClVport0, 0, Bits(xscale));
    out.Put(PaClVport0, 1, Bits(x + xscale));
    out.Put(PaClVport0, 2, Bits(yscale));
    out.Put(PaClVport0, 3, Bits(y + half_height));
    out.Put(PaClVport0, 4, Bits(render.viewport[5] - render.viewport[4]));
    out.Put(PaClVport0, 5, 0);
    out.Put(PaClVteCntl, 0, kViewportControl);
    out.Put(PaScScreenScissor, 0, Truncate(y) << 16 | (Truncate(x) & 0xffff));
    out.Put(PaScScreenScissor, 1, Truncate(bottom) << 16 | (Truncate(right) & 0xffff));
}

inline void PutRasterizer(const Gx::Render& render, Writer& out) {
    out.Put(PaClClipCntl, 0, render.raster[0] & kClipMask);
    out.Put(PaSuScModeCntl, 0, render.raster[1] & kModeMask);
    const u32 scale = Bits(std::bit_cast<float>(render.raster[4]) * kPolyOffsetScale);
    out.Put(PaSuPolyOffset, 0, render.raster[3]);
    out.Put(PaSuPolyOffset, 1, scale);
    out.Put(PaSuPolyOffset, 2, render.raster[2]);
    out.Put(PaSuPolyOffset, 3, scale);
    out.Put(PaSuPolyOffset, 4, render.raster[2]);
    const u32 scissor_enable = (render.raster[5] >> kScissorEnableByte) & 1;
    out.Put(PaScModeCntl0, 0, scissor_enable << 1 | (render.raster[5] & 1));
    PutViewport(render, out);
    bool scissor = scissor_enable != 0;
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::ScissorEnableIgnored)) scissor = true;
#endif
    if (scissor) {
        out.Put(PaScVportScissor0, 0, kWindowOffsetDisable | ClampScissor(render.scissor[1]) << 16 | ClampScissor(render.scissor[0]));
        out.Put(PaScVportScissor0, 1, ClampScissor(render.scissor[3]) << 16 | ClampScissor(render.scissor[2]));
    }
}

inline bool SlotWritten(const Gx::Render& render, const Gx::Whole& whole, u32 slot) {
    if (!whole.ps_bound) return false;
    u32 shader_mask = whole.ps_registers[11];
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::WrittenIgnoresShaderMask)) shader_mask = ~0u;
#endif
    return ((render.blend[0] & shader_mask) >> (4 * slot) & 0xf) != 0;
}

inline void PutTargets(const Gx::Render& render, const Gx::Whole& whole, Writer& out) {
    for (u32 i = 0; i < kColorTargets; ++i) {
        if ((render.color_valid >> i) & 1) {
            for (u32 w = 0; w < kColorWords; ++w) out.Put(CbColorTarget, kColorStride * i + w, render.targets[i][w]);
            continue;
        }
        bool written = SlotWritten(render, whole, i);
#ifdef LANTERN_GX_WHOLE_MUTANTS
        if (Mutated(Mutant::UnboundAlwaysCleared)) written = true;
#endif
        if (written) out.Put(CbColorUnbound, kColorStride * i + kColorInfoWord, 0);
    }
    bool depth = render.depth_valid != 0;
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::DepthUnboundSkipped) && !depth) return;
#endif
    if (depth) {
        for (u32 w = 0; w < kDepthWords; ++w) out.Put(DbDepthTarget, w, render.depth_target[w]);
        out.Put(DbDepthView, 0, render.depth_target[kDepthViewWord]);
        out.Put(DbHtileBase, 0, render.depth_target[kDepthHtileWord]);
    } else {
        bool tested = (render.depth_stencil[0] & kDepthStencilEnableBits) != 0;
#ifdef LANTERN_GX_WHOLE_MUTANTS
        if (Mutated(Mutant::DepthUnboundAlwaysCleared)) tested = true;
#endif
        if (tested) {
            out.Put(DbDepthTarget, 0, 0);
            out.Put(DbDepthTarget, 1, 0);
        }
    }
    out.Put(DbDepthClear, 0, Bits(whole.depth_clear));
    out.Put(DbStencilClear, 0, whole.stencil_clear, kByteMask);
}

inline void PutInputAssembly(const Gx::Render& render, const Gx::Whole& whole, bool indexed, Writer& out) {
    out.Put(VgtPrimitiveType, 0, PrimitiveFromTopology(whole.topology), kPrimitiveMask);
    out.Put(VgtShaderStages, 0, kStagesVertexPixel);
    if (!indexed || !whole.index_bound) return;
    out.Put(VgtIndexType, 0, IndexType(render.index_format), 3);
    if (!render.index_known) return;
    out.Put(VgtIndexBase, 0, static_cast<u32>(render.index_base >> 32), kIndexBaseHighMask);
    out.Put(VgtIndexBase, 1, static_cast<u32>(render.index_base), kIndexBaseLowMask);
}

inline void PutShaders(const Gx::Whole& whole, Writer& out) {
    if (whole.vs_bound) {
        const u32* vs = whole.vs_registers;
        bool modified = whole.vs_modifier != 0;
#ifdef LANTERN_GX_WHOLE_MUTANTS
        if (Mutated(Mutant::ModifierIgnored)) modified = false;
#endif
        out.Put(VsProgram, 0, vs[0]);
        out.Put(VsProgram, 1, 0);
        out.Put(VsProgram, 2, modified ? (vs[2] & kModifierKeep) | whole.vs_modifier : vs[2]);
        out.Put(VsProgram, 3, vs[3]);
        out.Put(PaClVsOutCntl, 0, vs[6]);
    }
    if (!whole.ps_bound) {
        out.Put(PsProgram, 0, 0);
        out.Put(PsProgram, 1, 0);
        out.Put(DbShaderControl, 0, 0);
        return;
    }
    const u32* ps = whole.ps_registers;
    out.Put(PsProgram, 0, ps[0]);
    out.Put(PsProgram, 1, 0);
    out.Put(PsProgram, 2, ps[2]);
    out.Put(PsProgram, 3, ps[3]);
    out.Put(SpiShaderFormats, 0, ps[4]);
    out.Put(SpiShaderFormats, 1, ps[5]);
    out.Put(SpiPsInputEnable, 0, ps[6]);
    out.Put(SpiPsInputEnable, 1, ps[7]);
    out.Put(SpiPsInControl, 0, ps[8]);
    out.Put(SpiBarycentric, 0, ps[9]);
    out.Put(DbShaderControl, 0, ps[10]);
    out.Put(CbShaderMask, 0, ps[11]);
    if (!whole.semantics_valid) return;
    for (u32 i = 0; i < whole.pixel_semantics_count && i < kPixelInputs; ++i) out.Put(SpiPsInputCntl, i, PixelInputControl(whole, i));
}

inline u32 RegistersFromRecord(const Gx::State& state, bool indexed, std::span<Value, kMaxValues> out) {
    if (!state.whole.captured) return 0;
    Writer writer(out);
    PutOutputMerger(state.render, state.whole, writer);
    PutRasterizer(state.render, writer);
    PutTargets(state.render, state.whole, writer);
    PutInputAssembly(state.render, state.whole, indexed, writer);
    PutShaders(state.whole, writer);
    return writer.Count();
}

inline constexpr u32 kStencilEnableBit = 0x1, kDepthBoundsEnableBit = 0x8, kFirstUnreadSlot = 1;

struct UnusedFound {
    u32 mask = 0;
    std::array<Difference, kUnusedClasses> first{};
};

inline Unused UnusedBy(const Value& value, const Gx::Render& render) {
    const u32 control = render.depth_stencil[0];
    u32 target_mask = render.blend[0];
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::UnusedIgnoresTargetMask)) target_mask = 0;
#endif
    switch (value.field) {
    case CbBlendControl:
    case CbColorTarget: {
        const u32 offset = value.reg - kFieldInfo[value.field].first;
        const u32 slot = value.field == CbBlendControl ? offset : offset / kColorStride;
        u32 first_unread = kFirstUnreadSlot;
#ifdef LANTERN_GX_WHOLE_MUTANTS
        if (Mutated(Mutant::SlotZeroUnread)) first_unread = 0;
#endif
        return slot >= first_unread && slot < kColorTargets && ((target_mask >> (4 * slot)) & 0xf) == 0 ? Unused::SlotNotWritten
                                                                                                       : Unused::Read;
    }
    case DbStencilControl:
    case DbStencilRefMask: {
        bool off = (control & kStencilEnableBit) == 0;
#ifdef LANTERN_GX_WHOLE_MUTANTS
        if (Mutated(Mutant::StencilAlwaysOff)) off = true;
#endif
        return off ? Unused::StencilOff : Unused::Read;
    }
    case DbDepthBounds: {
        bool off = (control & kDepthBoundsEnableBit) == 0;
#ifdef LANTERN_GX_WHOLE_MUTANTS
        if (Mutated(Mutant::BoundsAlwaysOff)) off = true;
#endif
        return off ? Unused::BoundsOff : Unused::Read;
    }
    default:
        return Unused::Read;
    }
}

inline u32 CompareRecord(std::span<const u32> registers, std::span<const Value> values, Counts& counts,
                         std::span<Difference, kEvidencePerDraw> evidence, const Gx::Render* render = nullptr,
                         UnusedFound* found = nullptr) {
    u32 differing = 0;
    for (const Value& value : values) {
        if (value.reg >= registers.size()) continue;
        u32 mask = value.mask;
#ifdef LANTERN_GX_WHOLE_MUTANTS
        if (Mutated(Mutant::CompareIgnoresMask)) mask = ~0u;
#endif
        ++counts.fields;
        const u32 decoded = registers[value.reg];
        if (((decoded ^ value.value) & mask) == 0) continue;
        if (render) {
            const Unused unused = UnusedBy(value, *render);
            if (unused != Unused::Read) {
                const u32 u = static_cast<u32>(unused);
                ++counts.unused_differ;
                ++counts.unused_by[u];
                ++counts.field_unused[value.field];
                if (found && !((found->mask >> u) & 1)) {
                    found->mask |= 1u << u;
                    found->first[u] = {value.field, value.reg, value.value & mask, decoded & mask};
                }
                continue;
            }
        }
        ++counts.fields_differ;
        ++counts.field_differ[value.field];
        if (differing < kEvidencePerDraw) evidence[differing] = {value.field, value.reg, value.value & mask, decoded & mask};
        ++differing;
    }
    return differing;
}

inline u32 CheckKept(std::span<const u32> registers, KeptMemory& memory, bool covered, Counts& counts) {
    bool reset = !covered;
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::KeptMemoryKept)) reset = false;
#endif
    if (reset) {
        memory.valid = false;
        return 0;
    }
    if (!covered) return 0;
    const bool compare = memory.valid;
    u32 changed = 0, at = 0;
    for (u32 f = 0; f < kFields; ++f) {
        const FieldInfo& info = kFieldInfo[f];
        if (info.source != Source::Kept) continue;
        bool field_changed = false;
        for (u32 w = 0; w < info.count; ++w, ++at) {
            const u32 reg = info.first + w;
            const u32 now = reg < registers.size() ? registers[reg] : 0;
            field_changed |= compare && memory.words[at] != now;
            memory.words[at] = now;
        }
        if (field_changed) {
            ++counts.field_changed[f];
            ++changed;
        }
    }
    if (compare) {
        ++counts.kept_checks;
        counts.kept_words += kKeptWords;
        counts.kept_changed += changed != 0;
    }
    memory.valid = true;
    return changed;
}

enum class Key : u8 { Taken, Unstamped, Lost, Stale };

inline Key CheckKey(u64 slot_address, u64 slot_sequence, u32 slot_key, u64 token_address, u32 token_key) {
    if (!token_key) return Key::Unstamped;
    if (!slot_sequence || slot_address != token_address) return Key::Lost;
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::KeyIgnoresStamp)) return Key::Taken;
#endif
    return slot_key == token_key ? Key::Taken : Key::Stale;
}

struct RecordSlot {
    std::atomic<u64> address{0}, mixed{0}, sequence{0};
    std::atomic<u32> key{0};
};

struct Taking {
    Key key = Key::Unstamped;
    u64 record = 0, sequence = 0;
};

inline void Publish(RecordSlot& slot, u64 address, u64 record, u64 sequence, u32 key) {
    slot.sequence.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.mixed.store(record ^ address, std::memory_order_relaxed);
    slot.address.store(address, std::memory_order_relaxed);
    slot.key.store(key, std::memory_order_relaxed);
    slot.sequence.store(sequence, std::memory_order_release);
}

inline void Forget(RecordSlot& slot, u64 address) {
    if (slot.address.load(std::memory_order_relaxed) == address) slot.sequence.store(0, std::memory_order_relaxed);
}

inline Taking Consume(RecordSlot& slot, u64 address, u32 token_key) {
    Taking taking;
    if (!token_key) return taking;
    u64 sequence = slot.sequence.load(std::memory_order_acquire);
    const u64 at = slot.address.load(std::memory_order_relaxed);
    const u64 mixed = slot.mixed.load(std::memory_order_relaxed);
    const u32 key = slot.key.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    taking.key = CheckKey(at, sequence, key, address, token_key);
    if (taking.key != Key::Taken) return taking;
    bool claimed = slot.sequence.compare_exchange_strong(sequence, 0, std::memory_order_relaxed);
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::ConsumeKeepsSlot)) claimed = slot.sequence.load(std::memory_order_relaxed) == sequence;
#endif
    if (!claimed) {
        taking.key = Key::Stale;
        return taking;
    }
    taking.record = mixed ^ address;
    taking.sequence = sequence;
    return taking;
}

inline bool ShouldCompare(Mode mode, u64 covered_index) {
    if (mode == Mode::Compare) return true;
    if (mode != Mode::Feed) return false;
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::SampleEveryDraw)) return true;
#endif
    return covered_index % kSampleEvery == 0;
}

inline bool InstrumentSuspect(Mode mode, const Counts& counts) {
#ifdef LANTERN_GX_WHOLE_MUTANTS
    if (Mutated(Mutant::SuspectMissed)) return counts.total > 0 && counts.covered == 0;
#endif
    if (counts.total > 0 && counts.covered == 0) return true;
    if (mode == Mode::Off) return false;
    if (counts.covered > 0 && counts.checked == 0) return true;
    return counts.differ > 0 && counts.differ == counts.checked;
}

inline int ModeNumber(Mode mode) {
    return mode == Mode::Compare ? 2 : mode == Mode::Feed ? 1 : 0;
}

class LineBuilder {
public:
    LineBuilder(char* out, std::size_t size) : out_(out), size_(size) {
        if (size_) out_[0] = '\0';
    }
    __attribute__((format(printf, 2, 3))) void Add(const char* format, ...) {
        if (used_ + 1 >= size_) return;
        std::va_list args;
        va_start(args, format);
        const int wrote = std::vsnprintf(out_ + used_, size_ - used_, format, args);
        va_end(args);
        if (wrote <= 0) return;
        const std::size_t room = size_ - used_ - 1;
        used_ += static_cast<std::size_t>(wrote) < room ? static_cast<std::size_t>(wrote) : room;
    }
    std::size_t Used() const {
        return used_;
    }

private:
    char* out_;
    std::size_t size_;
    std::size_t used_ = 0;
};

inline std::size_t FormatLine(char* out, std::size_t size, Mode mode, const Counts& c) {
    using ull = unsigned long long;
    LineBuilder line(out, size);
    line.Add("GX whole (LANTERN_GX_WHOLE=%d): %llu draws, %llu covered by a GX record", ModeNumber(mode), ull(c.total), ull(c.covered));
    if (mode != Mode::Off) {
        line.Add(" (%llu without a payload, %llu without its whole part, %llu overwritten while read); %llu checked, %llu skipped by the sample, %llu differ; %llu fields compared, %llu differ; kept fields %llu checks, %llu draws with a change",
                 ull(c.no_payload), ull(c.no_whole), ull(c.overwritten), ull(c.checked), ull(c.skipped), ull(c.differ), ull(c.fields),
                 ull(c.fields_differ), ull(c.kept_checks), ull(c.kept_changed));
        if (c.other_writers) line.Add("; %llu calls from other writers", ull(c.other_writers));
        bool first = true;
        for (u32 f = 0; f < kFields; ++f) {
            if (!c.field_differ[f]) continue;
            line.Add("%s%s %llu", first ? "; differ by field: " : ", ", kFieldInfo[f].name, ull(c.field_differ[f]));
            first = false;
        }
        if (c.unused_differ) {
            line.Add("; %llu differences in fields the draw does not read (not counted above, no turn-off)", ull(c.unused_differ));
            for (u32 u = 1; u < kUnusedClasses; ++u) {
                if (c.unused_by[u]) line.Add(", %s %llu", kUnusedNames[u], ull(c.unused_by[u]));
            }
            for (u32 f = 0; f < kFields; ++f) {
                if (c.field_unused[f]) line.Add(", %s %llu", kFieldInfo[f].name, ull(c.field_unused[f]));
            }
        }
        first = true;
        for (u32 f = 0; f < kFields; ++f) {
            if (!c.field_changed[f]) continue;
            line.Add("%s%s %llu", first ? "; kept fields changed between covered draws: " : ", ", kFieldInfo[f].name, ull(c.field_changed[f]));
            first = false;
        }
    }
    return line.Used();
}
}
