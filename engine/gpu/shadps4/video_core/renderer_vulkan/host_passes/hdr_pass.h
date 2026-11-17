// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: HDR output (hdr in lantern.ini). Encodes the swapchain-sized SDR composite (the game's
// frame letterboxed, plus the menu layer) into the HDR swapchain image: HDR10 PQ or scRGB.
// Native mode (hdr_mode=native) brightens the highlights with the frame's gain map
// (vk_hdr_native.h).

#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::HostPasses {

class HdrOutputPass {
public:
    /// Push constants of hdr_output.frag.
    struct Settings {
        float paper_white = 203.0f; ///< nits of SDR white
        float peak = 1000.0f;       ///< nits the display reaches
        float boost = 0.5f;         ///< highlight expansion, 0..1 (expand mode and fallback)
        u32 flags = 0;              ///< FlagScRgb | FlagOverlay | FlagNative | FlagDebug
        /// FlagNative: the frame's rectangle in the output (top left corner, pixels) and
        /// 1 / its size, mapping output pixels to gain map coordinates.
        std::array<float, 2> gain_offset{};
        std::array<float, 2> gain_scale{};
        /// Native mode: the UI's white (times paper white) and colours (0 sRGB .. 1 DCI-P3).
        float ui_white = 1.0f;
        float ui_vivid = 0.0f;
        /// Native+: the game's black level taken to 0 (Black Floor), in units of paper white.
        float black_floor = 0.0f;
    };
    static constexpr u32 FlagScRgb = 1;   ///< scRGB (linear BT.709) output, else HDR10 PQ
    static constexpr u32 FlagOverlay = 2; ///< the menu layer is drawn
    static constexpr u32 FlagNative = 4;  ///< `gain` is this frame's gain map (native mode)
    static constexpr u32 FlagDebug = 8;   ///< BB_HDR_DEBUG: the gain as a heat map
    static constexpr u32 FlagUiFrame = 16; ///< native mode, a frame without a scene: all UI
    static constexpr u32 FlagCompare = 32; ///< the left half as SDR (the menu's comparison)
    /// Native+: the frame is the 16-bit chain's (RGBA16F, the game's white at 1), its SDR part
    /// encoded as the game's signal, its pre-HUD snapshot bound (`snapshot`), shown as the game's
    /// SDR (the menu's preset Off or Compare SDR).
    static constexpr u32 FlagNativePlus = 64, FlagEncoded = 128, FlagSnapshot = 256,
                         FlagGameLook = 512;

    /// `output_format`: the HDR swapchain's format.
    void Create(vk::Device device, vk::Format output_format);

    bool IsCreated() const {
        return static_cast<bool>(pipeline);
    }

    /// Draws into `output` (layout ColorAttachmentOptimal) of `extent`; `game` and `overlay`
    /// (both ShaderReadOnlyOptimal, `extent` big) are read texel for texel, `gain` (any size,
    /// in `gain_layout`) is filtered.
    void Render(vk::CommandBuffer cmdbuf, vk::ImageView game, vk::ImageView overlay,
                vk::ImageView gain, vk::ImageLayout gain_layout, vk::ImageView output,
                vk::Extent2D extent, const Settings& settings, vk::ImageView snapshot = {});

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
    vk::UniqueSampler gain_sampler{}; ///< bilinear: the map has the display buffer's size
};

} // namespace Vulkan::HostPasses
