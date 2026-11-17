// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native+ HDR (docs/nativeplus.md): the post-composite chain in RGBA16F, the composite's tone
// mapped scene (Vanilla+ or PsychoV) added to the game's target as a delta, and the replacements
// of the chain's game shaders built on their interface.

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Shader {
struct Info;
struct RuntimeInfo;
namespace Backend {
struct Bindings;
}
} // namespace Shader

namespace Vulkan {

class Instance;
class Scheduler;

class HdrChannel {
public:
    /// A game pixel shader as compiled: what it was compiled from (`start`: its first bindings),
    /// which gives its interface (the recompiler and the lifter lay it out the same way, and only
    /// the recompiler names it), and its SPIR-V, the game's own.
    struct GameShader {
        const Shader::Info& info;
        const Shader::RuntimeInfo& runtime;
        const Shader::Backend::Bindings& start;
        std::span<const u32> spv;
    };

    // The 16-bit chain. The game's 8-bit targets after its composite (1920x1080 RGBA8: the composite's target, its
    // copy for the AA, and the display buffers) become RGBA16F at the port's output-size images
    // (scene proxies, UI and display images: only with a scaled output). The composite draw is
    // followed by bbport's, which adds the HDR part (hdr/bb_composite_hdr.frag, BB_OUTPUT_DELTA),
    // so the frame carries light above the game's white to the final pass.
    static bool Native16();
    /// Native+ with the startup-patched render size (BB_RENDER_RES, "Live resolution changes"
    /// off): the game's targets are already at the render size and the scene has no proxies, so
    /// the chain's targets get same-size RGBA16F proxies of their own (SceneTargets::OwnSize).
    static bool Native16Fixed();
    /// Native+ and the port's output-size images in use (set by the scene proxies, each frame).
    static bool Native16Active();
    static void SetNative16Active(bool active);
    /// GPU thread: the color target of a draw with this pixel shader (guest address, size,
    /// format): composite variants and the shaders of the chain mark their 1920x1080 RGBA8
    /// target as 16-bit from the next frame on.
    static void NoteTarget(u64 ps_hash, VAddr address, u32 width, u32 height, vk::Format format);
    /// Native+ with the startup patch: the destination of a copy of a chain target (the game
    /// copies its finished image for its color grading with a compute copy, not a draw) joins
    /// the chain from the next frame on, so the copy is done between RGBA16F proxies.
    static void NoteCopyTarget(VAddr address, u32 width, u32 height, vk::Format format);
    /// Whether this guest color target is drawn as RGBA16F (pipeline formats, proxies, views).
    static bool Upgraded(VAddr address, u32 width, u32 height, vk::Format format);
    /// The display buffers (the game's last copy) when the port draws them at output size.
    static void SetDisplayRedirect(bool redirect);
    static bool DisplayUpgraded(VAddr address, vk::Format format);
    static constexpr vk::Format Native16Format = vk::Format::eR16G16B16A16Sfloat;
    /// The game's ColorGrading pass (a color table), replaced by Native+; its strength (the menu's
    /// Color Grade Strength) goes to the shader in bb_hdr.w (hdr/bb_color_grading.frag).
    static constexpr u64 ColorGradingHash = 0xed4c7668;
    /// The game's second glow, added after its tone mapping (a blur of the 960x540 glow targets,
    /// additive); with Native+ weighted by the menu's Bloom (blend constant).
    static constexpr u64 GlowAddHash = 0x8c08444c;
    /// Native+'s version of a game shader of the chain (glow source, color table, display copy)
    /// with the game shader's bindings and locations, or empty to keep the game's.
    static std::vector<u32> ChainReplacement(const GameShader& game);

    /// Called for every pixel shader compiled or loaded. For a composite variant, builds
    /// hdr/bb_composite_hdr.frag with that variant's interface (descriptor bindings, input
    /// locations, constant buffer) and options.
    static void NoteGameShader(const GameShader& game);
    /// The tone mapping shader for this composite variant (null if none).
    static vk::ShaderModule CompositeModule(const Instance& instance, u64 pgm_hash);
    /// The display's peak in nits: the display's (hdr_peak_auto) or hdr_peak, 200..10000; live.
    static float PeakNits();
    /// Native+ showing the game's own picture (the menu's preset Off, or Compare on SDR): the
    /// chain's HDR changes are left out (no composite delta, the game's bloom, second glow and
    /// color table) so the 16-bit chain draws the game's own.
    static bool VanillaChain();

    /// The game retains its original 128-byte prefix; only Native+ needs the HDR extension.
    static u32 PushConstantSize() { return Native16() ? 176u : 128u; }
};

} // namespace Vulkan
