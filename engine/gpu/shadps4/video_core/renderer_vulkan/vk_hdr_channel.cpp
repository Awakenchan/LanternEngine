// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/host_shaders/bb_color_grading_frag.h"
#include "video_core/host_shaders/bb_color_grading_hw_frag.h"
#include "video_core/host_shaders/bb_composite_hdr_frag.h"
#include "video_core/host_shaders/bb_composite_hdr_hw_frag.h"
#include "video_core/host_shaders/bb_composite_hdr_hw_ubo_frag.h"
#include "video_core/host_shaders/bb_composite_hdr_ubo_frag.h"
#include "video_core/host_shaders/bb_display_copy_frag.h"
#include "video_core/host_shaders/bb_glow_source_frag.h"
#include "video_core/host_shaders/bb_glow_source_hw_frag.h"
#include "video_core/renderer_vulkan/bb_tonemap_variants.h"
#include "video_core/renderer_vulkan/vk_hdr_channel.h"

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
void SetSpirvSpecConstant(std::vector<u32>& spv, u32 spec_id, u32 value) {
    u32 target = 0;
    for (size_t i = 5; i < spv.size();) {
        const u32 count = spv[i] >> 16, opcode = spv[i] & 0xffff;
        if (count == 0 || count > spv.size() - i) return;
        if (opcode == 71 && count >= 4 && spv[i + 2] == 1 && spv[i + 3] == spec_id) {
            target = spv[i + 1];
        } else if (opcode == 50 && count == 4 && target && spv[i + 2] == target) {
            spv[i + 3] = value;
            return;
        }
        i += count;
    }
}

const TonemapVariant* FindVariant(u64 hash) {
    if (hash == 0 || hash > 0xffffffffULL) {
        return nullptr;
    }
    const auto it = std::lower_bound(
        TonemapVariants.begin(), TonemapVariants.end(), u32(hash),
        [](const TonemapVariant& v, u32 h) { return v.hash < h; });
    return it != TonemapVariants.end() && it->hash == u32(hash) ? &*it : nullptr;
}

// SPIR-V reading and patching for the replacements: the names (OpName) and the Binding and
// Location decorations of a module.
constexpr u32 SpvOpName = 5, SpvOpDecorate = 71, SpvOpSpecConstantTrue = 48,
              SpvOpSpecConstantFalse = 49, SpvDecorationSpecId = 1, SpvDecorationLocation = 30,
              SpvDecorationBinding = 33;

struct SpvInterface {
    std::unordered_map<std::string, std::vector<u32>> ids; ///< name -> ids
    std::unordered_map<u32, u32> binding;                    ///< id -> Binding
    std::unordered_map<u32, u32> location;                   ///< id -> Location

    std::optional<u32> Binding(const std::string& name) const {
        return Find(name, binding);
    }
    std::optional<u32> Location(const std::string& name) const {
        return Find(name, location);
    }

private:
    std::optional<u32> Find(const std::string& name,
                            const std::unordered_map<u32, u32>& decorations) const {
        const auto it = ids.find(name);
        if (it == ids.end()) {
            return std::nullopt;
        }
        for (const u32 id : it->second) {
            if (const auto d = decorations.find(id); d != decorations.end()) {
                return d->second;
            }
        }
        return std::nullopt;
    }
};

SpvInterface ReadInterface(std::span<const u32> spv) {
    SpvInterface out;
    for (size_t i = 5; i < spv.size();) {
        const u32 count = spv[i] >> 16, opcode = spv[i] & 0xffff;
        if (count == 0 || i + count > spv.size()) {
            break;
        }
        if (opcode == SpvOpName && count >= 3) {
            const char* text = reinterpret_cast<const char*>(&spv[i + 2]);
            out.ids[std::string(text, strnlen(text, (count - 2) * 4))].push_back(spv[i + 1]);
        } else if (opcode == SpvOpDecorate && count >= 4) {
            if (spv[i + 2] == SpvDecorationBinding) {
                out.binding[spv[i + 1]] = spv[i + 3];
            } else if (spv[i + 2] == SpvDecorationLocation) {
                out.location[spv[i + 1]] = spv[i + 3];
            }
        }
        i += count;
    }
    return out;
}

/// Sets the `decoration` literal of every variable called `name`. False if there is none.
bool SetDecoration(std::vector<u32>& spv, const SpvInterface& names, const std::string& name,
                   u32 decoration, u32 value) {
    const auto it = names.ids.find(name);
    if (it == names.ids.end()) {
        return false;
    }
    bool done = false;
    for (size_t i = 5; i < spv.size();) {
        const u32 count = spv[i] >> 16, opcode = spv[i] & 0xffff;
        if (count == 0) {
            break;
        }
        if (opcode == SpvOpDecorate && count >= 4 && spv[i + 2] == decoration &&
            std::ranges::find(it->second, spv[i + 1]) != it->second.end()) {
            spv[i + 3] = value;
            done = true;
        }
        i += count;
    }
    return done;
}

/// Sets a boolean specialization constant (OpSpecConstantTrue / False).
void SetSpecBool(std::vector<u32>& spv, u32 spec_id, bool value) {
    u32 target = 0;
    for (size_t i = 5; i < spv.size();) {
        const u32 count = spv[i] >> 16, opcode = spv[i] & 0xffff;
        if (count == 0) {
            return;
        }
        if (opcode == SpvOpDecorate && count >= 4 && spv[i + 2] == SpvDecorationSpecId &&
            spv[i + 3] == spec_id) {
            target = spv[i + 1];
        } else if ((opcode == SpvOpSpecConstantTrue || opcode == SpvOpSpecConstantFalse) &&
                   count == 3 && target && spv[i + 2] == target) {
            spv[i] = (count << 16) | (value ? SpvOpSpecConstantTrue : SpvOpSpecConstantFalse);
            return;
        }
        i += count;
    }
}

// The tone mapping shader built for each composite variant (by pgm_hash). Never destroyed,
// like the channel (the device is gone before static objects at exit).
struct CompositeShader {
    std::vector<u32> spv;
    vk::ShaderModule module{};
    bool conflicting = false; ///< two permutations with different interfaces: not used
};
std::mutex composite_mutex;
std::unordered_map<u64, CompositeShader>* composites = new std::unordered_map<u64, CompositeShader>;

/// A game pixel shader input: its location, and whether the shader reads it per vertex (the
/// recompiler's manual interpolation, fs_in_attrN_p) or as the GPU interpolates it (fs_in_attrN:
/// Lantern's hardware interpolation pass, or no fragment-shader barycentrics).
struct GameInput {
    u32 location;
    bool per_vertex;
};

/// A game pixel shader's interface, from what it was compiled from rather than from its names:
/// Lantern's compilers (the recompiler's EmitContext, and the lifter, given the same Bindings) bind
/// its buffers from start.unified, then its images, then its samplers; input N at the location of
/// its param_index (one further with clip distance emulation); render target 0 at location 0. The
/// lifter's SPIR-V has no names; both lay a shader out the same (985 shaders of a cache, 0.9.39
/// beta 26: every binding the lifter declares is the recompiler's, of the same kind). Only what a
/// cache record keeps is read (InfoPersistent, the specialization's RuntimeInfo and Bindings): a
/// loaded Info has neither `stores` nor `fs_interpolation`. Whether an input is read per vertex
/// is in the SPIR-V's own decorations, which both compilers write.
struct GameLayout {
    std::vector<u32> buffers; ///< bindings
    u32 first_buffer = 0;     ///< the first buffer's index (Bindings::buffer: ubo_N's N)
    bool uniform = false;     ///< a uniform buffer (else storage buffers, the legacy compiler's)
    std::vector<u32> images, samplers;
    std::vector<std::optional<GameInput>> inputs; ///< by attribute (fs_in_attrN)
    bool mip_bindings = false; ///< an image with a binding per mip level: not laid out here

    std::optional<GameInput> Input(int attr) const {
        return attr >= 0 && size_t(attr) < inputs.size() ? inputs[attr] : std::nullopt;
    }
    /// The binding of the game's fs_imgN or fs_sampN (the names of bbport's shaders too).
    std::optional<u32> Binding(std::string_view name) const {
        const auto at = [&](std::string_view prefix,
                            const std::vector<u32>& list) -> std::optional<u32> {
            if (!name.starts_with(prefix)) {
                return std::nullopt;
            }
            const size_t index =
                std::strtoul(std::string(name.substr(prefix.size())).c_str(), nullptr, 10);
            return index < list.size() ? std::optional<u32>{list[index]} : std::nullopt;
        };
        const auto image = at("fs_img", images);
        return image ? image : at("fs_samp", samplers);
    }
};

GameLayout ReadLayout(const HdrChannel::GameShader& game) {
    GameLayout out;
    u32 binding = game.start.unified;
    out.first_buffer = game.start.buffer;
    for (const auto& buffer : game.info.buffers) {
        out.buffers.push_back(binding++);
        out.uniform = buffer.is_uniform;
    }
    for (const auto& image : game.info.images) {
        if (image.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
            out.mip_bindings = true;
            return out;
        }
        out.images.push_back(binding++);
    }
    for (size_t i = 0; i < game.info.samplers.size(); ++i) {
        out.samplers.push_back(binding++);
    }
    // Inputs read per vertex (PerVertexKHR, or AMD's ExplicitInterpAMD): their locations.
    constexpr u32 SpvDecorationPerVertexKHR = 5285, SpvDecorationExplicitInterpAMD = 4999;
    std::unordered_map<u32, u32> location_of;
    std::unordered_set<u32> per_vertex_ids;
    for (size_t i = 5; i < game.spv.size();) {
        const u32 count = game.spv[i] >> 16, opcode = game.spv[i] & 0xffff;
        if (count == 0 || i + count > game.spv.size()) {
            break;
        }
        if (opcode == SpvOpDecorate && count >= 3) {
            const u32 decoration = game.spv[i + 2];
            if (decoration == SpvDecorationLocation && count >= 4) {
                location_of[game.spv[i + 1]] = game.spv[i + 3];
            } else if (decoration == SpvDecorationPerVertexKHR ||
                       decoration == SpvDecorationExplicitInterpAMD) {
                per_vertex_ids.insert(game.spv[i + 1]);
            }
        }
        i += count;
    }
    std::unordered_set<u32> per_vertex_locations;
    for (const u32 id : per_vertex_ids) {
        if (const auto it = location_of.find(id); it != location_of.end()) {
            per_vertex_locations.insert(it->second);
        }
    }
    const auto& fs = game.runtime.hw.fs;
    const bool clip = fs.clip_distance_emulation;
    const s32 count = std::min<s32>(s32(fs.num_inputs) - (clip ? 1 : 0), s32(fs.inputs.size()));
    for (s32 i = 0; i < count; ++i) {
        const auto& input = fs.inputs[i];
        if (input.IsDefault()) {
            out.inputs.emplace_back();
            continue;
        }
        const u32 location = u32(input.param_index) + (clip ? 1u : 0u);
        out.inputs.push_back(GameInput{location, per_vertex_locations.contains(location)});
    }
    return out;
}

/// The recompiler names what it declares (the lifter does not): where it does, its names must
/// agree with the layout, or Lantern's compilers no longer lay shaders out as ReadLayout does.
void CheckLayout(u64 pgm_hash, const GameLayout& layout, std::span<const u32> spv) {
    const SpvInterface in = ReadInterface(spv);
    if (in.ids.empty()) {
        return;
    }
    std::string differs;
    const auto check = [&](const std::string& name, std::optional<u32> expected, bool location) {
        const auto found = location ? in.Location(name) : in.Binding(name);
        if (found && found != expected) {
            differs += fmt::format(" {} {} (layout {})", name, *found,
                                   expected ? s64(*expected) : s64(-1));
        }
    };
    for (size_t i = 0; i < layout.buffers.size(); ++i) {
        const u32 index = layout.first_buffer + u32(i);
        check(fmt::format("{}_{}", layout.uniform ? "ubo" : "ssbo", index), layout.buffers[i],
              false);
    }
    for (size_t i = 0; i < layout.images.size(); ++i) {
        check(fmt::format("fs_img{}", i), layout.images[i], false);
    }
    for (size_t i = 0; i < layout.samplers.size(); ++i) {
        check(fmt::format("fs_samp{}", i), layout.samplers[i], false);
    }
    for (size_t i = 0; i < layout.inputs.size(); ++i) {
        const auto& input = layout.inputs[i];
        const auto location = input ? std::optional<u32>{input->location} : std::nullopt;
        check(fmt::format("fs_in_attr{}", i), location, true);
        check(fmt::format("fs_in_attr{}_p", i), location, true);
    }
    check("frag_color0", 0u, true);
    if (!differs.empty()) {
        LOG_WARNING(Render_Vulkan, "Native+: shader {:08x}: its names and its layout differ:{}",
                    u32(pgm_hash), differs);
    }
}

/// The variant's HDR shader, or why there is none. `per_vertex` says which build of it.
std::optional<std::vector<u32>> BuildComposite(const TonemapVariant& v, const GameLayout& in,
                                               std::string* why, bool* per_vertex) {
    // The constant buffer: the game shader's only buffer: a uniform buffer from the native
    // shader compiler, a storage buffer from the legacy one.
    if (in.buffers.size() != 1) {
        *why = in.buffers.empty() ? "no constant buffer" : "several buffers";
        return std::nullopt;
    }
    if (in.mip_bindings) {
        *why = "an image with a binding per mip level";
        return std::nullopt;
    }
    const std::optional<u32> buffer_index = in.first_buffer, buffer_binding = in.buffers[0];
    const bool uniform = in.uniform;
    if (size_t(v.img_scene) >= in.images.size() || size_t(v.img_scene) >= in.samplers.size()) {
        *why = "unexpected images";
        return std::nullopt;
    }
    // One sampler per image, in the same order (each fetch uses its own S#).
    if (in.images.size() != in.samplers.size()) {
        *why = fmt::format("{} images but {} samplers", in.images.size(), in.samplers.size());
        return std::nullopt;
    }
    const auto image = [&](int index) { return in.Binding(fmt::format("fs_img{}", index)); };
    const auto sampler = [&](int index) { return in.Binding(fmt::format("fs_samp{}", index)); };
    // Inputs: per vertex when the game's shader reads all of them so (the HDR shader then computes
    // the game's values bit for bit); else the build that reads them as the GPU interpolates them,
    // at the same locations (the game's vertex shader writes them the same either way).
    const auto scene_in = in.Input(v.attr_scene);
    const auto mult_in = in.Input(v.attr_mult);
    const auto bloom_in = in.Input(v.attr_bloom);
    const auto vig_in = v.attr_vig >= 0 ? in.Input(v.attr_vig) : scene_in;
    const auto bindings = {image(v.img_scene), image(v.img_mult), image(v.img_bloom),
                           sampler(v.img_scene), sampler(v.img_mult), sampler(v.img_bloom)};
    if (std::ranges::any_of(bindings, [](const auto& b) { return !b.has_value(); })) {
        *why = "image bindings not found";
        return std::nullopt;
    }
    if (!scene_in || !mult_in || !bloom_in || !vig_in) {
        *why = "inputs not found";
        return std::nullopt;
    }
    *per_vertex = scene_in->per_vertex && mult_in->per_vertex && bloom_in->per_vertex &&
                  vig_in->per_vertex;
    const auto scene_loc = std::optional<u32>{scene_in->location};
    const auto mult_loc = std::optional<u32>{mult_in->location};
    const auto bloom_loc = std::optional<u32>{bloom_in->location};
    const auto vig_loc = std::optional<u32>{vig_in->location};

    const auto build = [](std::span<const u32> words) {
        return std::vector<u32>(words.begin(), words.end());
    };
    std::vector<u32> spv = uniform ? (*per_vertex ? build(BB_COMPOSITE_HDR_UBO_FRAG)
                                                  : build(BB_COMPOSITE_HDR_HW_UBO_FRAG))
                                   : (*per_vertex ? build(BB_COMPOSITE_HDR_FRAG)
                                                  : build(BB_COMPOSITE_HDR_HW_FRAG));
    const SpvInterface ours = ReadInterface(spv);
    const auto it = bindings.begin();
    const bool ok =
        SetDecoration(spv, ours, "bb_ssbo", SpvDecorationBinding, *buffer_binding) &&
        SetDecoration(spv, ours, "bb_img_scene", SpvDecorationBinding, *it[0]) &&
        SetDecoration(spv, ours, "bb_img_mult", SpvDecorationBinding, *it[1]) &&
        SetDecoration(spv, ours, "bb_img_bloom", SpvDecorationBinding, *it[2]) &&
        SetDecoration(spv, ours, "bb_samp_scene", SpvDecorationBinding, *it[3]) &&
        SetDecoration(spv, ours, "bb_samp_mult", SpvDecorationBinding, *it[4]) &&
        SetDecoration(spv, ours, "bb_samp_bloom", SpvDecorationBinding, *it[5]) &&
        SetDecoration(spv, ours, "bb_in_scene", SpvDecorationLocation, *scene_loc) &&
        SetDecoration(spv, ours, "bb_in_mult", SpvDecorationLocation, *mult_loc) &&
        SetDecoration(spv, ours, "bb_in_bloom", SpvDecorationLocation, *bloom_loc) &&
        SetDecoration(spv, ours, "bb_in_vig", SpvDecorationLocation, *vig_loc);
    if (!ok) {
        *why = "the HDR shader's decorations were not found";
        return std::nullopt;
    }
    // The variant's options; the tone mapper and the menu's settings come live (push constants).
    SetSpirvSpecConstant(spv, 10, v.curve);
    SetSpecBool(spv, 11, v.luminance);
    SetSpirvSpecConstant(spv, 12, v.pre);
    SetSpecBool(spv, 13, v.pre_luminance);
    SetSpecBool(spv, 14, v.cos4);
    SetSpecBool(spv, 15, v.vig888);
    SetSpecBool(spv, 16, v.gamma);
    SetSpecBool(spv, 17, v.additive);
    SetSpirvSpecConstant(spv, 18, *buffer_index);
    SetSpecBool(spv, 20, v.gain);
    SetSpecBool(spv, 21, v.floor);
    SetSpecBool(spv, 22, v.scene_mat);
    SetSpecBool(spv, 23, v.glow_mat);
    SetSpecBool(spv, 24, v.grade);
    return spv;
}

constexpr const char* CurveNames[] = {"exp curve", "extended Reinhard", "log curve", "no curve"};
} // namespace

void HdrChannel::NoteGameShader(const GameShader& game) {
    if (!Native16()) {
        return;
    }
    const u64 pgm_hash = game.info.pgm_hash;
    const TonemapVariant* v = FindVariant(pgm_hash);
    if (!v || !v->standard || v->additive) {
        return; // additive variants output HDR for a later pass: nothing to do here
    }
    std::string why;
    bool per_vertex = false;
    const GameLayout layout = ReadLayout(game);
    CheckLayout(pgm_hash, layout, game.spv);
    auto built = BuildComposite(*v, layout, &why, &per_vertex);
    std::scoped_lock lock{composite_mutex};
    auto [it, fresh] = composites->try_emplace(pgm_hash);
    if (!built) {
        if (fresh) {
            LOG_WARNING(Render_Vulkan, "HDR pass: composite variant {:08x} left in SDR: {}",
                        u32(pgm_hash), why);
            it->second.conflicting = true;
        }
        return;
    }
    if (fresh) {
        it->second.spv = std::move(*built);
        LOG_INFO(Render_Vulkan, "HDR pass: composite variant {:08x} ready ({}{}, {})",
                 u32(pgm_hash), CurveNames[v->curve < 4 ? v->curve : 3],
                 v->luminance ? " on luminance" : "",
                 per_vertex ? "inputs per vertex" : "inputs interpolated by the GPU");
    } else if (!it->second.conflicting && it->second.spv != *built) {
        it->second.conflicting = true;
        LOG_WARNING(Render_Vulkan,
                    "HDR pass: composite variant {:08x} has permutations with different "
                    "interfaces, left in SDR",
                    u32(pgm_hash));
    }
}

vk::ShaderModule HdrChannel::CompositeModule(const Instance& instance, u64 pgm_hash) {
    std::scoped_lock lock{composite_mutex};
    const auto it = composites->find(pgm_hash);
    if (it == composites->end() || it->second.conflicting || it->second.spv.empty()) {
        return {};
    }
    if (!it->second.module) {
        it->second.module = CompileSPV(it->second.spv, instance.GetDevice());
    }
    return it->second.module;
}

namespace {
std::atomic<bool> native16_active{false};
std::atomic<bool> display_redirect{false};
std::mutex native16_mutex;
std::unordered_set<VAddr> native16_targets;
std::atomic<u32> native16_count{0};

// The startup-patched render size (BB_RENDER_RES, "Live resolution changes" off): the game
// allocates its targets at that size, aligned. {0, 0} in a live session.
std::pair<u32, u32> FixedRenderSize() {
    static const std::pair<u32, u32> size = [] {
        u32 w = 0, h = 0;
        const char* res = std::getenv("BB_RENDER_RES");
        if (!res || std::sscanf(res, "%ux%u", &w, &h) != 2) {
            return std::pair<u32, u32>{0, 0};
        }
        return std::pair<u32, u32>{w, h};
    }();
    return size;
}

// The chain's targets, 8-bit RGBA: 1920x1080 (the registers give the height padded to whole
// 8-row tiles), or with the startup patch the patched render size, padded: the width to a
// multiple of 128 (1708 in 1792, the scene of 1440p with DLSS or FSR Quality), the height to 64
// (3840x2176).
bool ChainSize(u32 width, u32 height) {
    const auto [rw, rh] = FixedRenderSize();
    if (rw != 0) {
        return width >= rw && width < rw + 128 && height >= rh && height < rh + 64;
    }
    return width == 1920 && height >= 1080 && height <= 1088;
}
constexpr bool ChainFormat(vk::Format format) {
    return format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb ||
           format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
}

// The shaders after the composite that write its image or the copy the AA reads:
// PointSampleColor1 (copy), EmissiveToGlowPS (copy for the glow, clamped by Native+), DLAA, and
// the glow's addition.
constexpr bool ChainShader(u64 ps_hash) {
    return ps_hash == 0xf7229091 || ps_hash == 0x57f3b8c2 || ps_hash == 0xda1faca9 ||
           ps_hash == 0x8c08444c;
}
} // namespace

std::vector<u32> HdrChannel::ChainReplacement(const GameShader& game) {
    if (!Native16()) {
        return {};
    }
    const u64 pgm_hash = game.info.pgm_hash;
    if (pgm_hash != 0x57f3b8c2 && pgm_hash != ColorGradingHash && pgm_hash != 0x38d65b32) {
        return {};
    }
    // The game shader's input fs_in_attr0 (none for the display copy): per vertex, the build that
    // reads it so; else the one that reads it as the GPU interpolates it (bb_inputs.glsl).
    const GameLayout in = ReadLayout(game);
    CheckLayout(pgm_hash, in, game.spv);
    const auto input = in.Input(0);
    const bool per_vertex = input && input->per_vertex;
    const auto pick = [per_vertex](std::span<const u32> manual, std::span<const u32> hw) {
        return per_vertex ? std::vector<u32>(manual.begin(), manual.end())
                          : std::vector<u32>(hw.begin(), hw.end());
    };
    std::vector<u32> spv;
    std::vector<const char*> bindings;
    bool reads_input = true;
    if (pgm_hash == 0x57f3b8c2) {
        spv = pick(BB_GLOW_SOURCE_FRAG, BB_GLOW_SOURCE_HW_FRAG);
        bindings = {"fs_img0", "fs_samp0"};
    } else if (pgm_hash == ColorGradingHash) {
        spv = pick(BB_COLOR_GRADING_FRAG, BB_COLOR_GRADING_HW_FRAG);
        bindings = {"fs_img0", "fs_img1", "fs_samp0", "fs_samp1"};
    } else if (pgm_hash == 0x38d65b32) {
        spv.assign(std::begin(BB_DISPLAY_COPY_FRAG), std::end(BB_DISPLAY_COPY_FRAG));
        bindings = {"fs_img0", "fs_img1", "fs_samp0"};
        reads_input = false;
    } else {
        return {};
    }
    const SpvInterface ours = ReadInterface(spv);
    bool ok = true;
    for (const char* name : bindings) {
        const auto value = in.Binding(name);
        ok = ok && value && SetDecoration(spv, ours, name, SpvDecorationBinding, *value);
    }
    ok = ok && !in.mip_bindings &&
         SetDecoration(spv, ours, "frag_color0", SpvDecorationLocation, 0);
    if (reads_input) {
        // Ours is called fs_in_attr0 in both builds.
        ok = ok && input &&
             SetDecoration(spv, ours, "fs_in_attr0", SpvDecorationLocation, input->location);
    }
    if (!ok) {
        LOG_WARNING(Render_Vulkan, "Native+: shader {:08x} has another interface, kept as it is",
                    pgm_hash);
        return {};
    }
    LOG_INFO(Render_Vulkan, "Native+: shader {:08x} replaced ({}{})", pgm_hash,
             pgm_hash == 0x57f3b8c2       ? "glow source clamped"
             : pgm_hash == ColorGradingHash ? "color table kept above white"
                                            : "display copy above white",
             !reads_input ? "" : per_vertex ? ", input per vertex" : ", input interpolated by the GPU");
    return spv;
}

bool HdrChannel::Native16Fixed() {
    return Native16() && FixedRenderSize().first != 0;
}

bool HdrChannel::Native16() {
    static const bool native = EmulatorSettings.IsHdrAllowed() && EmulatorSettings.IsNativePlus();
    return native;
}

bool HdrChannel::Native16Active() {
    return Native16() && native16_active.load(std::memory_order_relaxed);
}

void HdrChannel::SetNative16Active(bool active) {
    if (Native16() && native16_active.exchange(active) != active) {
        LOG_INFO(Render_Vulkan, "Native+: {}", active ? "on (output-size images)"
                                                          : "off (no output-size images)");
    }
}

void HdrChannel::NoteTarget(u64 ps_hash, VAddr address, u32 width, u32 height,
                            vk::Format format) {
    if (!Native16()) {
        return;
    }
    const TonemapVariant* v = FindVariant(ps_hash);
    if (!(v && v->standard) && !ChainShader(ps_hash)) {
        return;
    }
    if (!ChainSize(width, height) || !ChainFormat(format)) {
        static std::mutex logged_mutex;
        static std::unordered_set<u64> logged;
        std::scoped_lock lock{logged_mutex};
        if (logged.insert(ps_hash).second) {
            LOG_INFO(Render_Vulkan, "Native+: target {:#x} of ps {:08x} left 8-bit ({}x{} {})",
                     address, ps_hash, width, height, vk::to_string(format));
        }
        return;
    }
    std::scoped_lock lock{native16_mutex};
    if (native16_targets.insert(address).second) {
        native16_count.store(u32(native16_targets.size()), std::memory_order_release);
        LOG_INFO(Render_Vulkan, "Native+: target {:#x} (from ps {:08x}) is RGBA16F", address,
                 ps_hash);
    }
}

void HdrChannel::NoteCopyTarget(VAddr address, u32 width, u32 height, vk::Format format) {
    if (!Native16Fixed() || !ChainSize(width, height) || !ChainFormat(format)) {
        return;
    }
    std::scoped_lock lock{native16_mutex};
    if (native16_targets.insert(address).second) {
        native16_count.store(u32(native16_targets.size()), std::memory_order_release);
        LOG_INFO(Render_Vulkan, "Native+: target {:#x} (copy of the chain) is RGBA16F",
                 address);
    }
}

bool HdrChannel::Upgraded(VAddr address, u32 width, u32 height, vk::Format format) {
    if (!Native16Active() || !ChainSize(width, height) || !ChainFormat(format) ||
        native16_count.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::scoped_lock lock{native16_mutex};
    return native16_targets.contains(address);
}

void HdrChannel::SetDisplayRedirect(bool redirect) {
    display_redirect.store(redirect, std::memory_order_relaxed);
}

bool HdrChannel::DisplayUpgraded(VAddr address, vk::Format format) {
    (void)address;
    return Native16Active() && display_redirect.load(std::memory_order_relaxed) &&
           ChainFormat(format);
}

float HdrChannel::PeakNits() {
    // The peak in use: the display's (hdr_peak_auto) or lantern.ini's hdr_peak; live.
    return std::clamp(BbSettings::HdrPeakNits(BbSettings::Get()), 200.0f, 10000.0f);
}

bool HdrChannel::VanillaChain() {
    if (!Native16()) {
        return false;
    }
    const auto& s = BbSettings::Get();
    return s.hdr_preset == 0 || s.hdr16_compare == BbSettings::Values::HdrCompareSdr;
}

} // namespace Vulkan
