// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx_whole.h"
#include <array>
#include <cstddef>
#include <cstdint>

#define LANTERN_GX_READER_FIELDS(X) X(primitive_type) X(index_buffer_type) X(index_base_address) X(viewports) X(viewport_control) X(viewport_depths) X(viewport_scissors) X(screen_scissor) X(clipper_control) X(polygon_control) X(poly_offset) X(mode_control) X(blend_control) X(color_target_mask) X(blend_constants) X(depth_control) X(stencil_control) X(stencil_ref_front) X(stencil_ref_back) X(depth_bounds_min) X(depth_bounds_max)

namespace BbEngine::GxFlushCensus {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using GxWhole::Source;

enum class Writer : u8 { OutputMerger, Index, Raster, Target, Vertex, Bind, kCount };
inline constexpr std::size_t kWriters = static_cast<std::size_t>(Writer::kCount);
inline constexpr std::array<u32, kWriters> kWriterAddress = {0x26d2230, 0x26bba30, 0x26d3370, 0x26d2790, 0x26cfcf0, 0x26bc320};
inline constexpr std::array<const char*, kWriters> kWriterName = {"output merger", "index", "rasterizer", "target", "vertex", "bind"};

enum class Slice : u8 { Never, T5a, T5b };
inline constexpr Slice SkippedIn(Writer writer) {
    switch (writer) {
    case Writer::OutputMerger:
    case Writer::Index:
    case Writer::Raster: return Slice::T5a;
    case Writer::Vertex:
    case Writer::Bind: return Slice::T5b;
    default: return Slice::Never;
    }
}

enum class Tag : u8 { Record, Token, Kept, NotRead, Packet };
enum class Space : u8 { Context, Sh, Uconfig, UserData, Packet };
enum class Path : u8 { Value, Resolve, Geometry, Alternative };

struct Row {
    Writer writer;
    u32 emitter;
    Space space;
    u32 first;
    u32 count;
    u32 stride;
    u32 repeat;
    u32 dwords;
    u32 calls;
    Tag tag;
    Path path;
    const char* name;
};

inline constexpr u32 SpaceBase(Space space) {
    switch (space) {
    case Space::Context: return GxWhole::kContext;
    case Space::Sh: return GxWhole::kSh;
    case Space::Uconfig: return GxWhole::kUconfig;
    default: return 0;
    }
}
inline constexpr u32 SpaceWords(Space space) {
    switch (space) {
    case Space::Context: return 0x400;
    case Space::Sh: return 0x400;
    case Space::Uconfig: return 0x1000;
    default: return 0;
    }
}

using W = Writer;
using S = Space;
using T = Tag;
using P = Path;
inline constexpr std::array kRows = std::to_array<Row>({
    {W::OutputMerger, 0x1073030, S::Context, 0x2dc, 1, 0, 1, 3, 1, T::NotRead, P::Value, "DB_ALPHA_TO_MASK"},
    {W::OutputMerger, 0x1072fd0, S::Context, 0x08e, 1, 0, 1, 3, 1, T::Record, P::Value, "CB_TARGET_MASK"},
    {W::OutputMerger, 0x1073090, S::Context, 0x1e0, 1, 1, 8, 3, 8, T::Record, P::Value, "CB_BLEND0-7_CONTROL"},
    {W::OutputMerger, 0x1073100, S::Context, 0x105, 4, 0, 1, 6, 1, T::Record, P::Value, "CB_BLEND_RED..ALPHA"},
    {W::OutputMerger, 0x10733a0, S::Context, 0x200, 1, 0, 1, 3, 1, T::Record, P::Value, "DB_DEPTH_CONTROL"},
    {W::OutputMerger, 0x1073400, S::Context, 0x10b, 1, 0, 1, 3, 1, T::Record, P::Value, "DB_STENCIL_CONTROL"},
    {W::OutputMerger, 0x1073190, S::Context, 0x10c, 2, 0, 1, 4, 1, T::Record, P::Value, "DB_STENCILREFMASK(_BF)"},
    {W::OutputMerger, 0x1073520, S::Context, 0x008, 2, 0, 1, 4, 1, T::Record, P::Value, "DB_DEPTH_BOUNDS_MIN/MAX"},
    {W::OutputMerger, 0x1073320, S::Context, 0x202, 1, 0, 1, 3, 2, T::Kept, P::Resolve, "CB_COLOR_CONTROL"},
    {W::OutputMerger, 0x1073840, S::Context, 0x323, 2, 15, 8, 4, 8, T::Kept, P::Resolve, "CB_COLORn_CLEAR_WORD0/1"},

    {W::Index, 0x1073fe0, S::Uconfig, 0x242, 1, 0, 1, 3, 1, T::Record, P::Value, "VGT_PRIMITIVE_TYPE"},
    {W::Index, 0x10752c0, S::Context, 0x1f9, 2, 0, 1, 3, 1, T::Record, P::Value, "INDEX_BASE packet"},
    {W::Index, 0x1075270, S::Context, 0x29f, 1, 0, 1, 2, 1, T::Record, P::Value, "INDEX_TYPE packet"},

    {W::Raster, 0x1076b30, S::Context, 0x204, 1, 0, 1, 3, 1, T::Record, P::Value, "PA_CL_CLIP_CNTL"},
    {W::Raster, 0x1076e00, S::Context, 0x205, 1, 0, 1, 3, 1, T::Record, P::Value, "PA_SU_SC_MODE_CNTL"},
    {W::Raster, 0x1077070, S::Context, 0x2e0, 2, 0, 1, 4, 1, T::Record, P::Value, "PA_SU_POLY_OFFSET_FRONT"},
    {W::Raster, 0x10770e0, S::Context, 0x2e2, 2, 0, 1, 4, 1, T::Record, P::Value, "PA_SU_POLY_OFFSET_BACK"},
    {W::Raster, 0x1076fa0, S::Context, 0x2df, 1, 0, 1, 3, 1, T::Record, P::Value, "PA_SU_POLY_OFFSET_CLAMP"},
    {W::Raster, 0x1077000, S::Context, 0x2de, 1, 0, 1, 3, 1, T::NotRead, P::Value, "PA_SU_POLY_OFFSET_DB_FMT_CNTL"},
    {W::Raster, 0x1077150, S::Context, 0x292, 1, 0, 1, 3, 1, T::Record, P::Value, "PA_SC_MODE_CNTL_0"},
    {W::Raster, 0x10765e0, S::Context, 0x0b4, 2, 0, 1, 4, 1, T::Record, P::Value, "PA_SC_VPORT_ZMIN/MAX_0"},
    {W::Raster, 0x10765e0, S::Context, 0x10f, 6, 0, 1, 8, 1, T::Record, P::Value, "PA_CL_VPORT_0"},
    {W::Raster, 0x1076980, S::Context, 0x206, 1, 0, 1, 3, 1, T::Record, P::Value, "PA_CL_VTE_CNTL"},
    {W::Raster, 0x1076790, S::Context, 0x00c, 2, 0, 1, 4, 1, T::Record, P::Value, "PA_SC_SCREEN_SCISSOR"},
    {W::Raster, 0x1073650, S::Context, 0x08d, 1, 0, 1, 3, 1, T::NotRead, P::Value, "PA_SU_HARDWARE_SCREEN_OFFSET"},
    {W::Raster, 0x10736d0, S::Context, 0x2fa, 1, 2, 2, 6, 1, T::NotRead, P::Value, "PA_CL_GB_VERT/HORZ_CLIP_ADJ"},
    {W::Raster, 0x1073750, S::Context, 0x2fb, 1, 2, 2, 6, 1, T::NotRead, P::Value, "PA_CL_GB_VERT/HORZ_DISC_ADJ"},
    {W::Raster, 0x10766d0, S::Context, 0x094, 2, 0, 1, 4, 1, T::Record, P::Value, "PA_SC_VPORT_SCISSOR_0"},
    {W::Raster, 0x10765e0, S::Context, 0x0b6, 2, 2, 15, 4, 15, T::Kept, P::Geometry, "PA_SC_VPORT_ZMIN/MAX_1-15"},
    {W::Raster, 0x10765e0, S::Context, 0x115, 6, 6, 15, 8, 15, T::Kept, P::Geometry, "PA_CL_VPORT_1-15"},
    {W::Raster, 0x10766d0, S::Context, 0x096, 2, 2, 15, 4, 15, T::Kept, P::Geometry, "PA_SC_VPORT_SCISSOR_1-15"},

    {W::Target, 0x10738b0, S::Context, 0x318, 11, 15, 8, 13, 8, T::Record, P::Value, "CB_COLORn"},
    {W::Target, 0x10738b0, S::Packet, 0, 0, 0, 1, 2, 8, T::Packet, P::Value, "colour target NOP"},
    {W::Target, 0x10738b0, S::Context, 0x31c, 1, 15, 8, 3, 8, T::Record, P::Alternative, "CB_COLORn_INFO unbound"},
    {W::Target, 0x10739b0, S::Context, 0x010, 8, 0, 1, 10, 1, T::Record, P::Value, "DB_Z_INFO..DB_DEPTH_SLICE"},
    {W::Target, 0x10739b0, S::Context, 0x00f, 1, 0, 1, 3, 1, T::NotRead, P::Value, "DB_DEPTH_INFO"},
    {W::Target, 0x10739b0, S::Context, 0x002, 1, 0, 1, 3, 1, T::Record, P::Value, "DB_DEPTH_VIEW"},
    {W::Target, 0x10739b0, S::Context, 0x005, 1, 0, 1, 3, 1, T::Record, P::Value, "DB_HTILE_DATA_BASE"},
    {W::Target, 0x10739b0, S::Context, 0x2af, 1, 0, 1, 3, 1, T::NotRead, P::Value, "DB_HTILE_SURFACE"},
    {W::Target, 0x10739b0, S::Packet, 0, 0, 0, 1, 2, 1, T::Packet, P::Value, "depth target NOP"},
    {W::Target, 0x10739b0, S::Context, 0x010, 2, 0, 1, 6, 1, T::Record, P::Alternative, "DB_Z/STENCIL_INFO unbound"},
    {W::Target, 0x10773b0, S::Context, 0x2fe, 16, 0, 1, 18, 1, T::NotRead, P::Value, "PA_SC_AA_SAMPLE_LOCS"},
    {W::Target, 0x10773b0, S::Context, 0x2f5, 2, 0, 1, 4, 1, T::NotRead, P::Value, "PA_SC_CENTROID_PRIORITY"},
    {W::Target, 0x1077240, S::Context, 0x2f8, 1, 0, 1, 3, 1, T::Kept, P::Value, "PA_SC_AA_CONFIG"},
    {W::Target, 0x1073460, S::Context, 0x201, 1, 0, 1, 3, 1, T::NotRead, P::Value, "DB_EQAA"},
    {W::Target, 0x1073840, S::Context, 0x323, 2, 15, 8, 4, 8, T::Kept, P::Value, "CB_COLORn_CLEAR_WORD0/1"},
    {W::Target, 0x1073590, S::Context, 0x00b, 1, 0, 1, 3, 1, T::Record, P::Value, "DB_DEPTH_CLEAR"},
    {W::Target, 0x10735f0, S::Context, 0x00a, 1, 0, 1, 3, 1, T::Record, P::Value, "DB_STENCIL_CLEAR"},
    {W::Target, 0x1076ab0, S::Context, 0x001, 1, 0, 1, 3, 1, T::NotRead, P::Value, "DB_COUNT_CONTROL"},

    {W::Vertex, 0x10761a0, S::Packet, 0, 0, 0, 1, 2, 256, T::Packet, P::Value, "V# NOP 0x68750004"},
    {W::Vertex, 0x10761a0, S::UserData, 0, 4, 0, 1, 6, 256, T::Record, P::Value, "V# in user data"},
    {W::Vertex, 0x1076390, S::UserData, 0, 2, 0, 1, 4, 1, T::Record, P::Value, "fetch table pointer"},

    {W::Bind, 0x1075480, S::Sh, 0x048, 2, 0, 1, 4, 1, T::Record, P::Value, "SPI_SHADER_PGM_LO/HI_VS"},
    {W::Bind, 0x1075480, S::Sh, 0x04a, 2, 0, 1, 4, 1, T::Record, P::Value, "SPI_SHADER_PGM_RSRC1/2_VS"},
    {W::Bind, 0x1075480, S::Context, 0x207, 1, 0, 1, 3, 1, T::Record, P::Value, "PA_CL_VS_OUT_CNTL"},
    {W::Bind, 0x1075480, S::Context, 0x1b1, 1, 0, 1, 3, 1, T::NotRead, P::Value, "SPI_VS_OUT_CONFIG"},
    {W::Bind, 0x1075480, S::Context, 0x1c3, 1, 0, 1, 3, 1, T::NotRead, P::Value, "SPI_SHADER_POS_FORMAT"},
    {W::Bind, 0x1075480, S::Packet, 0, 0, 0, 1, 12, 1, T::Packet, P::Value, "VS trailing NOP"},
    {W::Bind, 0x10753c0, S::Sh, 0x008, 2, 0, 1, 4, 1, T::Record, P::Value, "SPI_SHADER_PGM_LO/HI_PS"},
    {W::Bind, 0x10753c0, S::Sh, 0x00a, 2, 0, 1, 4, 1, T::Record, P::Value, "SPI_SHADER_PGM_RSRC1/2_PS"},
    {W::Bind, 0x10753c0, S::Context, 0x1c4, 2, 0, 1, 4, 1, T::Record, P::Value, "SPI_SHADER_Z/COL_FORMAT"},
    {W::Bind, 0x10753c0, S::Context, 0x1b3, 2, 0, 1, 4, 1, T::Record, P::Value, "SPI_PS_INPUT_ENA/ADDR"},
    {W::Bind, 0x10753c0, S::Context, 0x1b6, 1, 0, 1, 3, 1, T::Record, P::Value, "SPI_PS_IN_CONTROL"},
    {W::Bind, 0x10753c0, S::Context, 0x1b8, 1, 0, 1, 3, 1, T::Record, P::Value, "SPI_BARYC_CNTL"},
    {W::Bind, 0x10753c0, S::Context, 0x203, 1, 0, 1, 3, 1, T::Record, P::Value, "DB_SHADER_CONTROL"},
    {W::Bind, 0x10753c0, S::Context, 0x08f, 1, 0, 1, 3, 1, T::Record, P::Value, "CB_SHADER_MASK"},
    {W::Bind, 0x10753c0, S::Packet, 0, 0, 0, 1, 12, 1, T::Packet, P::Value, "PS trailing NOP"},
    {W::Bind, 0x1075000, S::Context, 0x191, 32, 0, 1, 34, 1, T::Record, P::Value, "SPI_PS_INPUT_CNTL_0-31"},
    {W::Bind, 0x1076030, S::Context, 0x2d5, 1, 0, 1, 3, 1, T::Record, P::Value, "VGT_SHADER_STAGES_EN"},
    {W::Bind, 0x1075960, S::Context, 0x2aa, 1, 0, 1, 3, 1, T::NotRead, P::Value, "IA_MULTI_VGT_PARAM"},
    {W::Bind, 0x10756b0, S::Context, 0x290, 1, 0, 1, 3, 1, T::Kept, P::Value, "VGT_GS_MODE"},
    {W::Bind, 0x10756b0, S::Context, 0x2e5, 1, 0, 1, 3, 1, T::Kept, P::Value, "VGT_STRMOUT_CONFIG"},
    {W::Bind, 0x1073f50, S::Packet, 0, 0, 0, 1, 7, 2, T::Packet, P::Value, "prefetch DMA_DATA"},
    {W::Bind, 0x1076390, S::UserData, 0, 2, 0, 1, 4, 1, T::Record, P::Value, "fetch table pointer"},
});
inline constexpr std::size_t kRowCount = kRows.size();

inline constexpr std::array<u32, 31> kResolveEmitters = {
    0x1072fd0, 0x1073090, 0x1073320, 0x10733a0, 0x1073400, 0x1073590, 0x10735f0, 0x1073650, 0x10736d0, 0x1073750, 0x10738b0,
    0x10739b0, 0x1073fe0, 0x10741b0, 0x1075370, 0x10753c0, 0x1075480, 0x1075da0, 0x1075e10, 0x1076030, 0x10761a0, 0x1076240,
    0x10762f0, 0x1076390, 0x1076420, 0x10765e0, 0x1076790, 0x1076980, 0x10769e0, 0x1077150, 0x1077f40};

namespace Layout {
inline constexpr std::size_t kContextCommands = 0x10;
inline constexpr std::size_t kCommandBegin = 0x0, kCommandEnd = 0x8, kCommandCursor = 0x10, kCommandReserve = 0x18,
                             kCommandUser = 0x20, kCommandFlags = 0x28, kCommandSize = 0x30;
inline constexpr u32 kCommandShaderTypeBit = 0x2;

inline constexpr std::size_t kContextIndex = 0xb518, kIndexSize = 0x10;
inline constexpr std::size_t kIndexBase = 0x0, kIndexPrimitive = 0x8, kIndexType = 0xc;

inline constexpr std::size_t kContextRaster = 0xb528, kRasterSize = 0x30;
inline constexpr std::size_t kRasterClip = 0x0, kRasterMode = 0x4, kRasterPolyFront = 0x8, kRasterPolyBack = 0xc,
                             kRasterPolyClamp = 0x10, kRasterDbFormat = 0x14, kRasterScMode = 0x18, kRasterScissorEnable = 0x19,
                             kRasterState = 0x20, kRasterViewportDirty = 0x28, kRasterScissorDirty = 0x2c;

inline constexpr std::size_t kContextOutput = 0xb558, kOutputSize = 0x1a0;
inline constexpr std::size_t kOutputAlphaToMask = 0x0, kOutputTargetMask = 0x8, kOutputBlend = 0x10, kOutputBlendColor = 0x30,
                             kOutputDepthControl = 0x48, kOutputStencilControl = 0x4c, kOutputStencilRef = 0x50,
                             kOutputDepthBounds = 0x58, kOutputSets = 0x60, kOutputSetStride = 0x48, kOutputSetDepth = 0x40,
                             kOutputColorSamples = 0xf0, kOutputDepthSamples = 0xf4, kOutputDepthClear = 0xf8,
                             kOutputStencilClear = 0xfc, kOutputPendingCount = 0xfe, kOutputPendingFlags = 0xff,
                             kOutputClearWords = 0x100, kOutputWrittenDepthClear = 0x140, kOutputWrittenStencilClear = 0x144,
                             kOutputShadowCount = 0x146, kOutputShadowFlags = 0x147, kOutputWrittenClearWords = 0x148,
                             kOutputBits = 0x188, kOutputBlendCache = 0x190, kOutputDepthCache = 0x198;
inline constexpr u32 kBitColorSet = 0x1, kBitDepthSet = 0x2, kBitDepthClear = 0x4, kBitStencilClear = 0x8, kBitsClearWords = 0xff0;
inline constexpr u32 kFlagsTargets = 0x1, kFlagsDepth = 0x2, kFlagsCountControl = 0x1c;
inline constexpr std::size_t kContextDynamicRenames = 0xb6f8;

inline constexpr std::size_t ColorShadow(u32 bits, u32 slot) {
    return kOutputSets + (bits & kBitColorSet) * kOutputSetStride + slot * 8;
}
inline constexpr std::size_t DepthShadow(u32 bits) {
    return kOutputSets + kOutputSetDepth + ((bits >> 1) & 1) * kOutputSetStride;
}

inline constexpr std::size_t kPendingPrimitive = 0x8, kPendingIndexBuffer = 0x10, kPendingIndexFormat = 0x18,
                             kPendingIndexOffset = 0x1c, kPendingRaster = 0x240, kPendingViewports = 0x248,
                             kPendingViewportStride = 0x18, kPendingScissors = 0x3c8, kPendingScissorStride = 0x10,
                             kPendingBlend = 0x4d0, kPendingDepthStencil = 0x4d8, kPendingColorViews = 0x4e0,
                             kPendingDepthView = 0x520, kPendingBlendColor = 0x568, kPendingDepthBounds = 0x578,
                             kPendingStencilRef = 0x584, kPendingGeometryShader = 0x1970;
inline constexpr u32 kViewports = 16, kColorSlots = 8, kIndexFormat32 = 0x2a;
inline constexpr std::size_t kIndexBufferRename = 0x18, kIndexBufferAddress = 0x20, kIndexBufferDynamic = 0x40;

inline constexpr std::size_t kStageVertex = 0x0, kStagePixel = 0x1900;
inline constexpr std::size_t kStageDirtyViewsLow = 0x18, kStageDirtyViewsHigh = 0x20, kStageDirtyOther = 0x28,
                             kStageDirtyInputs = 0x30, kStageBoundShader = 0x38, kStageUsedViewsLow = 0x60,
                             kStageUsedViewsHigh = 0x68, kStageUsedOther = 0x70;
inline constexpr std::size_t kStateFresh = 0x0, kStateFlag = 0x4, kStatePrimitive = 0x8, kStateTessellation = 0x10,
                             kStateVertexDirty = 0x25a0, kStateLayout = 0x25a8, kStateVertexMasks = 0x25b0,
                             kStateVertexTable = 0xa260, kStateVertexEntry = 0x10, kStateVertexEntries = 256,
                             kStateSharedBlock = 0xb2a0;
}

enum class Object : u8 { OutputMerger, Index, Raster, kCount };
enum class Sync : u8 { Own, Copy, Or };
struct SyncRange {
    Object object;
    u32 offset;
    u32 size;
    Sync sync;
    const char* what;
};
inline constexpr std::array kSync = std::to_array<SyncRange>({
    {Object::OutputMerger, 0x000, 0x060, Sync::Own, "value shadows of 0x26d2230"},
    {Object::OutputMerger, 0x060, 0x090, Sync::Copy, "target shadow sets, written by 0x26d2790"},
    {Object::OutputMerger, 0x0f0, 0x098, Sync::Copy, "sample counts, clear values, counts and flags, written by 0x26d2790"},
    {Object::OutputMerger, 0x188, 0x004, Sync::Copy, "set and one-shot bits"},
    {Object::OutputMerger, 0x18c, 0x004, Sync::Own, "unused"},
    {Object::OutputMerger, 0x190, 0x010, Sync::Own, "blend and depth-stencil object caches"},
    {Object::Index, 0x000, 0x010, Sync::Own, "index base, primitive and index type caches"},
    {Object::Raster, 0x000, 0x028, Sync::Own, "rasterizer value caches and state pointer"},
    {Object::Raster, 0x028, 0x008, Sync::Or, "viewport and scissor dirty masks, ORed in, never cleared on the real object"},
});
inline constexpr std::array<u32, static_cast<std::size_t>(Object::kCount)> kObjectSize = {
    static_cast<u32>(Layout::kOutputSize), static_cast<u32>(Layout::kIndexSize), static_cast<u32>(Layout::kRasterSize)};

inline constexpr u32 kMaxOutputMergerDwords = 50, kMaxIndexDwords = 8, kMaxRasterDwords = 301, kMaxTargetDwords = 213;
inline constexpr u32 kMaxVertexDwords = 2052;
inline constexpr u32 kSideBufferDwords = 2 * kMaxRasterDwords;

enum class Mutant : u8 {
    None, RowDropped, NotReadAsRecord, ResolveAsValue, NotReadOnField, RasterLonger, SyncGap, CallsUndercounted,
    CommandFlagsOnUser, DepthBitZero, kCount
};
#ifdef LANTERN_GX_FLUSH_CENSUS_MUTANTS
inline Mutant selected_mutant = Mutant::None;
inline bool Mutated(Mutant mutant) {
    return selected_mutant == mutant;
}
#else
inline constexpr bool Mutated(Mutant) {
    return false;
}
#endif

inline constexpr std::size_t kDroppedRow = 2;
inline constexpr bool Present(std::size_t index) {
    return !(Mutated(Mutant::RowDropped) && index == kDroppedRow);
}
inline constexpr Row RowAt(std::size_t index) {
    Row row = kRows[index];
    if (Mutated(Mutant::NotReadAsRecord) && row.emitter == 0x1073030) row.tag = Tag::Record;
    if (Mutated(Mutant::ResolveAsValue) && row.emitter == 0x1073320) row.path = Path::Value;
    if (Mutated(Mutant::NotReadOnField) && row.emitter == 0x1073460) row.first = 0x200;
    if (Mutated(Mutant::CallsUndercounted) && row.emitter == 0x1073090) row.calls = 1;
    return row;
}
inline constexpr u32 ObjectStart(Object object) {
    switch (object) {
    case Object::OutputMerger: return static_cast<u32>(Layout::kContextOutput);
    case Object::Index: return static_cast<u32>(Layout::kContextIndex);
    default: return static_cast<u32>(Layout::kContextRaster);
    }
}
inline constexpr u32 ObjectSize(Object object) {
    const u32 size = kObjectSize[static_cast<std::size_t>(object)];
    return Mutated(Mutant::RasterLonger) && object == Object::Raster ? size + 8 : size;
}
inline constexpr bool SyncPresent(std::size_t index) {
    return !(Mutated(Mutant::SyncGap) && index == 3);
}
inline constexpr std::size_t CommandFlags() {
    return Mutated(Mutant::CommandFlagsOnUser) ? Layout::kCommandUser : Layout::kCommandFlags;
}
inline constexpr std::size_t DepthShadowOf(u32 bits) {
    return Mutated(Mutant::DepthBitZero) ? Layout::kOutputSets + Layout::kOutputSetDepth + (bits & 1) * Layout::kOutputSetStride
                                         : Layout::DepthShadow(bits);
}

inline constexpr u32 RegisterAt(const Row& row, u32 k, u32 word) {
    return SpaceBase(row.space) + row.first + k * row.stride + word;
}
inline constexpr bool Overlaps(const Row& row, u32 first, u32 count) {
    if (row.space == Space::Packet || row.space == Space::UserData) return false;
    for (u32 k = 0; k < row.repeat; ++k) {
        const u32 low = RegisterAt(row, k, 0), high = low + row.count;
        if (low < first + count && first < high) return true;
    }
    return false;
}
inline constexpr bool InFields(const Row& row, Source source) {
    for (u32 k = 0; k < row.repeat; ++k) {
        for (u32 word = 0; word < row.count; ++word) {
            const u32 reg = RegisterAt(row, k, word);
            bool found = false;
            for (const auto& info : GxWhole::kFieldInfo) found = found || (info.source == source && reg >= info.first && reg < info.first + info.count);
            if (!found) return false;
        }
    }
    return true;
}
inline constexpr bool OnAnyField(const Row& row) {
    for (const auto& info : GxWhole::kFieldInfo) {
        if (Overlaps(row, info.first, info.count)) return true;
    }
    return false;
}
inline constexpr bool Covered(u32 reg, Writer writer, Tag tag) {
    for (std::size_t i = 0; i < kRowCount; ++i) {
        if (!Present(i)) continue;
        const Row row = RowAt(i);
        if (row.writer == writer && row.tag == tag && Overlaps(row, reg, 1)) return true;
    }
    return false;
}
inline constexpr bool RecordOfSkippedFlush(u32 reg) {
    for (const Writer writer : {Writer::OutputMerger, Writer::Index, Writer::Raster}) {
        if (Covered(reg, writer, Tag::Record)) return true;
    }
    return false;
}

inline constexpr u32 MaxDwords(Writer writer) {
    u32 total = 0;
    for (std::size_t i = 0; i < kRowCount; ++i) {
        if (!Present(i)) continue;
        const Row row = RowAt(i);
        if (row.writer != writer || row.path == Path::Resolve || row.path == Path::Alternative) continue;
        total += row.dwords * row.calls;
    }
    return total;
}

enum class Finding : u8 {
    None, OutOfSpace, RecordNotInFields, TokenNotInFields, KeptOnSkippedPath, NotReadOnField, ResolveOutsideTargets,
    ObjectsOverlap, ObjectPastRenames, SyncNotExact, MaxMismatch, SideBufferSmall, CommandFieldsOverlap, DepthShadowWrong
};
inline constexpr std::array<const char*, 14> kFindingName = {
    "none", "register outside its space", "Record row not a Record field", "Token row not a Token field",
    "Kept row on a value path of a skipped flush", "NotRead row on a field", "resolve row outside output merger",
    "flush objects overlap", "output object runs into the rename table", "sync list not exact", "max output mismatch",
    "side buffer too small", "command buffer fields overlap", "depth shadow not selected by bit 1"};

struct Result {
    Finding finding = Finding::None;
    std::size_t row = 0;
};

inline constexpr Result CheckRows() {
    for (std::size_t i = 0; i < kRowCount; ++i) {
        if (!Present(i)) continue;
        const Row row = RowAt(i);
        if (row.space != Space::Packet && row.space != Space::UserData) {
            const u32 span = row.first + (row.repeat - 1) * row.stride + row.count;
            if (row.count == 0 || span > SpaceWords(row.space)) return {Finding::OutOfSpace, i};
        }
        const bool registers = row.space != Space::Packet && row.space != Space::UserData;
        if (registers && row.tag == Tag::Record && !InFields(row, Source::Record)) return {Finding::RecordNotInFields, i};
        if (registers && row.tag == Tag::Token && !InFields(row, Source::Token)) return {Finding::TokenNotInFields, i};
        if (row.tag == Tag::Kept && SkippedIn(row.writer) == Slice::T5a && row.path == Path::Value) return {Finding::KeptOnSkippedPath, i};
        if (registers && row.tag == Tag::NotRead && OnAnyField(row)) return {Finding::NotReadOnField, i};
        if (row.path == Path::Resolve && row.writer != Writer::OutputMerger) return {Finding::ResolveOutsideTargets, i};
    }
    return {};
}

inline constexpr Result CheckLayout() {
    const Object order[] = {Object::Index, Object::Raster, Object::OutputMerger};
    for (int k = 0; k + 1 < 3; ++k) {
        if (ObjectStart(order[k]) + ObjectSize(order[k]) > ObjectStart(order[k + 1])) return {Finding::ObjectsOverlap, std::size_t(k)};
    }
    if (ObjectStart(Object::OutputMerger) + ObjectSize(Object::OutputMerger) > Layout::kContextDynamicRenames) return {Finding::ObjectPastRenames, 0};
    for (std::size_t o = 0; o < static_cast<std::size_t>(Object::kCount); ++o) {
        u32 next = 0;
        for (std::size_t i = 0; i < kSync.size(); ++i) {
            if (!SyncPresent(i) || static_cast<std::size_t>(kSync[i].object) != o) continue;
            if (kSync[i].offset != next) return {Finding::SyncNotExact, i};
            next = kSync[i].offset + kSync[i].size;
        }
        if (next != ObjectSize(static_cast<Object>(o))) return {Finding::SyncNotExact, o};
    }
    const std::size_t fields[] = {Layout::kCommandBegin, Layout::kCommandEnd, Layout::kCommandCursor, Layout::kCommandReserve,
                                  Layout::kCommandUser, CommandFlags()};
    for (std::size_t a = 0; a < 6; ++a) {
        for (std::size_t b = a + 1; b < 6; ++b) {
            if (fields[a] < fields[b] + 8 && fields[b] < fields[a] + 8) return {Finding::CommandFieldsOverlap, a};
        }
        if (fields[a] + 8 > Layout::kCommandSize) return {Finding::CommandFieldsOverlap, a};
    }
    for (u32 bits = 0; bits < 4; ++bits) {
        const std::size_t want = Layout::kOutputSets + Layout::kOutputSetDepth + ((bits >> 1) & 1) * Layout::kOutputSetStride;
        if (DepthShadowOf(bits) != want || Layout::ColorShadow(bits, 0) != Layout::kOutputSets + (bits & 1) * Layout::kOutputSetStride)
            return {Finding::DepthShadowWrong, bits};
    }
    return {};
}

inline constexpr Result CheckOutput() {
    const u32 want[] = {kMaxOutputMergerDwords, kMaxIndexDwords, kMaxRasterDwords, kMaxTargetDwords, kMaxVertexDwords};
    for (std::size_t w = 0; w < 5; ++w) {
        if (MaxDwords(static_cast<Writer>(w)) != want[w]) return {Finding::MaxMismatch, w};
    }
    for (const Writer writer : {Writer::OutputMerger, Writer::Index, Writer::Raster}) {
        if (2 * MaxDwords(writer) > kSideBufferDwords) return {Finding::SideBufferSmall, static_cast<std::size_t>(writer)};
    }
    return {};
}

inline constexpr Result Check() {
    if (const Result r = CheckRows(); r.finding != Finding::None) return r;
    if (const Result r = CheckLayout(); r.finding != Finding::None) return r;
    return CheckOutput();
}

#ifndef LANTERN_GX_FLUSH_CENSUS_MUTANTS
static_assert(Check().finding == Finding::None);
static_assert(Layout::kContextRaster + Layout::kRasterSize == Layout::kContextOutput);
static_assert(Layout::kContextIndex + Layout::kIndexSize == Layout::kContextRaster);
static_assert(Layout::kContextOutput + Layout::kOutputSize == Layout::kContextDynamicRenames);
static_assert(Layout::kPendingViewports + Layout::kViewports * Layout::kPendingViewportStride == Layout::kPendingScissors);
static_assert(Layout::kPendingColorViews + Layout::kColorSlots * 8 == Layout::kPendingDepthView);
static_assert(Layout::kOutputClearWords + Layout::kColorSlots * 8 == Layout::kOutputWrittenDepthClear);
static_assert(Layout::kOutputWrittenClearWords + Layout::kColorSlots * 8 == Layout::kOutputBits);
static_assert(Layout::kOutputSets + 2 * Layout::kOutputSetStride == Layout::kOutputColorSamples);
static_assert(Layout::kStateVertexTable + Layout::kStateVertexEntries * Layout::kStateVertexEntry <= Layout::kStateSharedBlock);
static_assert(Layout::kStagePixel > Layout::kStageUsedOther + 8);
#endif
}
