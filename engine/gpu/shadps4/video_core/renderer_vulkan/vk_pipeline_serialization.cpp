// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>
#include "common/serdes.h"
#include "common/hash.h"
#include <algorithm>
#include <stdexcept>
#include "bbport_threads.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/native/gt7_tonemap.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_effect_shaders.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 7u; // bbport: interpolated integer fix (Pascal)
static constexpr u32 MotionShaderBinaryVersion = 8u; // runtime address descriptor, set 1
static constexpr u32 CaptureShaderBinaryVersion = 9u;
static constexpr u32 ShaderMetaVersion = 8u; // bbport: HwFragmentRuntimeInfo::depth_first
static constexpr u32 PipelineKeyVersion = 6u; // pointer-free pipeline state + generated fragment
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

std::vector<u8> SerializeShaderMeta(const Shader::Info& info, const Shader::StageSpecialization& spec,
                                    size_t perm_hash, size_t perm_idx) {
    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    const bool vertex_motion = info.hw_stage == Shader::HwStage::Vertex &&
                               spec.runtime_info.hw.vs.motion_vectors;
    const bool vertex_capture = vertex_motion && (spec.runtime_info.hw.vs.motion_vectors & 2) != 0;
    meta.Write(vertex_capture ? Serialization::CaptureShaderBinaryVersion
                             : vertex_motion ? Serialization::MotionShaderBinaryVersion
                             : Serialization::ShaderBinaryVersion);
    meta.Write(perm_hash);
    meta.Write(perm_idx);
    spec.Serialize(ar);
    info.Serialize(ar);
    return ar.TakeOff();
}

bool ReadShaderMetaSpec(std::vector<u8>&& blob, Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Archive ar{std::move(blob)};
    Serialization::Reader meta{ar};
    u32 meta_version{}, binary_version{};
    u64 perm_hash{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }
    meta.Read(binary_version);
    meta.Read(perm_hash);
    meta.Read(perm_idx);
    return spec.Deserialize(ar);
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    const bool vertex_motion = info.hw_stage == Shader::HwStage::Vertex &&
                               spec.runtime_info.hw.vs.motion_vectors;
    const bool vertex_capture = vertex_motion && (spec.runtime_info.hw.vs.motion_vectors & 2) != 0;
    meta.Write(vertex_capture ? Serialization::CaptureShaderBinaryVersion
                             : vertex_motion ? Serialization::MotionShaderBinaryVersion
                             : Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx, u64 expected_hash) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion &&
        binary_version != Serialization::MotionShaderBinaryVersion &&
        binary_version != Serialization::CaptureShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    if (!spec.Deserialize(ar) || !info.Deserialize(ar)) {
        return false;
    }
    if (perm_idx >= 4096 || perm_hash_ar != expected_hash ||
        HashCombine(info.pgm_hash, perm_idx) != expected_hash) {
        return false;
    }

    // Only old motion vertex binaries embed process-local addresses. Other v7 shaders remain
    // reusable; v8 motion binaries load the current addresses from a renderer-owned descriptor.
    const bool vertex_motion = info.hw_stage == Shader::HwStage::Vertex &&
                               spec.runtime_info.hw.vs.motion_vectors;
    const bool vertex_capture = vertex_motion && (spec.runtime_info.hw.vs.motion_vectors & 2) != 0;
    if (binary_version != (vertex_capture ? Serialization::CaptureShaderBinaryVersion
                             : vertex_motion ? Serialization::MotionShaderBinaryVersion
                                        : Serialization::ShaderBinaryVersion)) {
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0, compute_key.value) ||
        sel.infos[0]->hw_stage != Shader::HwStage::Compute) {
        return false;
    }

    if (compute_pipelines.contains(compute_key)) {
        return true;
    }
    auto pipeline =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *sel.infos[0], sel.modules[0], sdata, true);
    compute_pipelines.emplace(compute_key, std::move(pipeline));

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer data{ar};
    data.Write(vertex_attributes);
    data.Write(vertex_bindings);
    data.Write(divisors);
    // No pNext, pSampleMask, or Vulkan handles belong in a persistent record.
    ASSERT(!multisampling.pNext && !multisampling.pSampleMask);
    data.Write(multisampling.rasterizationSamples);
    data.Write(multisampling.sampleShadingEnable);
    data.Write(multisampling.minSampleShading);
    data.Write(multisampling.alphaToCoverageEnable);
    data.Write(multisampling.alphaToOneEnable);
    data.Write(tcs);
    data.Write(tes);
    data.Write(fragment);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar, bool legacy) {
    Serialization::Reader data{ar};
    if (legacy) {
        // Read old v5 records without ever using persisted pointers. Current writers use v6.
        data.Read(&vertex_attributes, sizeof(vertex_attributes));
        data.Read(&vertex_bindings, sizeof(vertex_bindings));
        data.Read(&divisors, sizeof(divisors));
        if (vertex_attributes.size() > vertex_attributes.capacity() ||
            vertex_bindings.size() > vertex_bindings.capacity() ||
            divisors.size() > divisors.capacity()) {
            throw std::runtime_error("Invalid cached vertex input count");
        }
        data.Read(multisampling);
        if (multisampling.pNext || multisampling.pSampleMask) {
            return false;
        }
    } else {
        data.Read(vertex_attributes);
        data.Read(vertex_bindings);
        data.Read(divisors);
        data.Read(multisampling.rasterizationSamples);
        data.Read(multisampling.sampleShadingEnable);
        data.Read(multisampling.minSampleShading);
        data.Read(multisampling.alphaToCoverageEnable);
        data.Read(multisampling.alphaToOneEnable);
    }
    data.Read(tcs);
    data.Read(tes);
    if (!legacy) {
        data.Read(fragment);
    }
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar, bool legacy) {
    sel.graphics_key.Deserialize(ar);
    if (sel.graphics_key.motion_vectors &&
        !instance.GetPhysicalDevice().getFeatures().vertexPipelineStoresAndAtomics) {
        return false;
    }

    GraphicsPipeline::SerializationSupport sdata{};
    if (!sdata.Deserialize(ar, legacy)) {
        return false;
    }
    // v5 did not save the generated clip-discard fragment shader. Only this ambiguous
    // legacy variant needs rebuilding; ordinary v5 pipelines remain usable.
    if (legacy && profile.needs_clip_distance_emulation &&
        !sel.graphics_key.stage_hashes[u32(Shader::SwStage::Fragment)]) {
        return false;
    }

    warmup_mip_bindings = false;
    // bbport: each stage's bindings start where the stages before it end (the fragment shader's
    // first, as RefreshGraphicsStages compiles them and BuildDescSetLayout lays them out).
    Shader::Backend::Bindings expected{};
    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx, hash) ||
            u32(sel.infos[stage_idx]->sw_stage) != stage_idx) {
            return false;
        }
        // A key of a module compiled after another fragment shader (engines up to 0.9.37 beta 6
        // reused those: StageSpecialization ignored the start of stages without guest resources)
        // has a layout its shaders do not match, invalid Vulkan that AMD's driver ends the process
        // on. Left out, it compiles again in play. So is one with an image bound per mip level:
        // its binding count needs the draw's descriptor, which the warm-up does not have.
        // The layout is built from the program's Info (the first permutation loaded): it must
        // have the bindings of this permutation's own record.
        const auto& layout_info = *sel.infos[stage_idx];
        if (warmup_mip_bindings || warmup_start[stage_idx].unified != expected.unified ||
            warmup_start[stage_idx].buffer != expected.buffer ||
            layout_info.buffers.size() != warmup_count[stage_idx].buffer ||
            layout_info.buffers.size() + layout_info.images.size() + layout_info.samplers.size() !=
                warmup_count[stage_idx].unified) {
            return false;
        }
        expected.unified += warmup_count[stage_idx].unified;
        expected.buffer += warmup_count[stage_idx].buffer;
    }
    // Without dynamic vertex input (RX 400/500, Vega, older APUs on AMD's legacy driver) the
    // vertex input is baked into the pipeline from the record: a record made where it was dynamic
    // has none, and the vertex shader's attributes would read nothing (validation: Input-07904).
    if (!instance.IsVertexInputDynamicState() && sel.fetch_shader &&
        !sel.fetch_shader->attributes.empty() && sdata.vertex_attributes.empty()) {
        return false;
    }
    // A fragment shader that reads the motion varyings needs a vertex shader that writes them
    // (validation found keys pairing one with a vertex shader without them: undefined motion).
    const auto& fs_runtime = sel.runtime_infos[u32(Shader::SwStage::Fragment)];
    const auto& vs_runtime = sel.runtime_infos[u32(Shader::SwStage::Vertex)];
    if (sel.graphics_key.stage_hashes[u32(Shader::SwStage::Fragment)] && fs_runtime.hw.fs.motion_vectors &&
        !(sel.graphics_key.stage_hashes[u32(Shader::SwStage::Vertex)] &&
          vs_runtime.hw_stage == Shader::HwStage::Vertex && vs_runtime.hw.vs.motion_vectors)) {
        return false;
    }
    static const bool motion_layout_guard = [] {
        const char* env = std::getenv("BB_WARMUP_MOTION_GUARD");
        return !(env && env[0] == '0');
    }();
    if (motion_layout_guard && !sel.graphics_key.motion_vectors &&
        sel.graphics_key.stage_hashes[u32(Shader::SwStage::Vertex)] &&
        vs_runtime.hw_stage == Shader::HwStage::Vertex && vs_runtime.hw.vs.motion_vectors) {
        static std::atomic<u32> left_out{0};
        const u32 count = left_out.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 20) {
            std::printf("GPU cache: pipeline key %u left out, a motion vertex shader without motion in "
                        "its key: vs 0x%016llx, fs 0x%016llx (BB_WARMUP_MOTION_GUARD=0: built)\n",
                        count,
                        (unsigned long long)sel.graphics_key.stage_hashes[u32(Shader::SwStage::Vertex)],
                        (unsigned long long)sel.graphics_key.stage_hashes[u32(Shader::SwStage::Fragment)]);
        }
        return false;
    }

    if (graphics_pipelines.contains(sel.graphics_key)) {
        return true;
    }
    auto pipeline = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);
    graphics_pipelines.emplace(sel.graphics_key, std::move(pipeline));

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage, u64 expected_hash) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    std::optional<Shader::Gcn::FetchShaderData> fetch;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, fetch, spec, perm_idx, expected_hash)) {
        return false;
    }
    // bbport: a composite cached before the GT7 pass compiles again in play (native/gt7_tonemap.h).
    if (!program->info.gt7_checked && !Shader::Native::ToneCurveEnabled() &&
        Shader::Native::IsKnownComposite(program->info.pgm_hash)) {
        return false;
    }

    std::vector<u32> spv;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
        fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx), spv);
    if (spv.size() < 5 || spv[0] != 0x07230203) {
        return false;
    }
    // Reject truncated instructions before passing cached SPIR-V to the driver.
    bool discards = false;
    for (size_t word = 5; word < spv.size();) {
        const u32 count = spv[word] >> 16;
        if (!count || count > spv.size() - word) {
            return false;
        }
        const u32 opcode = spv[word] & 0xffff;
        discards |= opcode == 252u || opcode == 5380u;
        word += count;
    }

    auto it = program_cache.find(program->info.pgm_hash);
    Program* target = it == program_cache.end() ? program.get() : it->second.get();
    program->info.has_discard |= discards;
    target->info.has_discard |= discards;
    vk::ShaderModule module;
    if (perm_idx < target->modules.size() && target->modules[perm_idx].module) {
        // The persisted index is part of the pipeline key. Equal specializations at other
        // indices are not conflicts: equality deliberately ignores some unused bindings.
        if (!(target->modules[perm_idx].spec == spec)) {
            return false;
        }
        module = target->modules[perm_idx].module;
    } else {
        const auto effect_shader = EffectShaders::Replacement(program->info, profile, spec.start);
        const auto effect_patch =
            effect_shader.empty() ? EffectShaders::Patched(program->info, spv) : std::nullopt;
        const std::span<const u32> code = !effect_shader.empty() ? effect_shader
                                          : effect_patch         ? std::span<const u32>(*effect_patch)
                                                                 : std::span<const u32>(spv);
        const auto [result, compiled] = instance.GetDevice().createShaderModule(
            {.codeSize = code.size_bytes(), .pCode = code.data()});
        if (result != vk::Result::eSuccess) {
            throw Serialization::CorruptData{"cached SPIR-V rejected by the driver"};
        }
        module = compiled;
    }
    // bbport: where this permutation's bindings start and how many it has, from its own record
    // (LoadGraphicsPipeline checks them against the stages before it).
    if (stage < MaxShaderStages) {
        const auto& own = program->info;
        warmup_start[stage] = spec.start;
        warmup_count[stage] = {};
        warmup_count[stage].buffer = u32(own.buffers.size());
        warmup_count[stage].unified = u32(own.buffers.size() + own.images.size() + own.samplers.size());
        for (const auto& image : own.images) {
            warmup_mip_bindings |= image.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex;
        }
    }
    // Rebind before moving: a temporary Program may have supplied this permutation's Info.
    // Never retain a pointer to that temporary in a cached StageSpecialization.
    spec.info = &target->info;
    sel.runtime_infos[stage] = spec.runtime_info;
    if (fetch) {
        sel.fetch_shader = std::move(fetch);
    }
    target->InsertPermut(module, std::move(spec), perm_idx);
    if (!target->info_template) {
        target->info_template = std::make_unique<Shader::Info>(target->info);
    }
    sel.infos[stage] = &target->info;
    sel.modules[stage] = module;
    if (it == program_cache.end()) {
        const auto hash = program->info.pgm_hash;
        program_cache.emplace(hash, std::move(program));
    }
    return true;
}

void PipelineCache::WarmUp() {
    {
        std::scoped_lock lock{progress_mutex};
        precache_progress = {};
    }
    const auto complete = [] {
        std::scoped_lock lock{progress_mutex};
        precache_progress.rejected = precache_progress.total - precache_progress.loaded;
        precache_progress.complete = true;
    };
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        complete();
        return;
    }

    Storage::DataBase::Instance().Open();
    if (!Storage::DataBase::Instance().IsOpened()) {
        complete();
        return;
    }
    auto& database = Storage::DataBase::Instance();
    const auto names = database.ListBlobs(Storage::BlobType::PipelineKey);
    {
        std::scoped_lock lock{progress_mutex};
        precache_progress.total = static_cast<u32>(names.size());
    }

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        // Without a profile the remaining records have no proven shader-feature compatibility.
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        complete();
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_WARNING(Render, "Pipeline cache profile has unexpected size ({} != {})",
                    profile_data.size(), sizeof(Shader::Profile));
    }
    Shader::Profile cached_profile{};
    if (profile_data.size() == sizeof(Shader::Profile)) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    }
    if (profile_data.size() != sizeof(Shader::Profile) || cached_profile != profile) {
        // bbport: upstream closed the cache for the session here, so it was never rewritten
        // and every later session compiled every shader again (stutters on each new area).
        // Start a fresh cache for this build and GPU instead.
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        complete();
        return;
    }

    const auto* regs = sel.regs;
    u32 damaged = 0;
    // bbport: a GPU driver that crashes building one cached pipeline (an AMD driver did, at every
    // start) would end every start the same way. The pipelines being built are written to
    // warmup.running, one a line (the OS keeps the write when the process dies); the next start
    // leaves those pipelines out and builds the others. BB_WARMUP_GUARD=0: not used.
    std::unordered_set<std::string> crashed;
    std::filesystem::path guard_path;
    if (const auto folder = database.FolderPath(); !folder.empty()) {
        const char* guard_env = std::getenv("BB_WARMUP_GUARD");
        if (!(guard_env && guard_env[0] == '0')) {
            guard_path = folder / "warmup.running";
            if (std::ifstream last{guard_path}; last) {
                for (std::string line; std::getline(last, line);) {
                    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
                        line.pop_back();
                    }
                    if (!line.empty()) {
                        crashed.insert(line);
                    }
                }
            }
            for (const auto& name : crashed) {
                database.Remove(Storage::BlobType::PipelineKey, name);
                std::printf("GPU cache: the GPU driver crashed building cached pipeline %s at the last "
                            "start (or one built beside it); it is left out (it compiles again when a "
                            "draw needs it)\n",
                            name.c_str());
            }
        }
    }
    // bbport: a driver that crashes in the warm-up may do so on many cached pipelines: on an
    // RX 9070 XT (0.9.37 beta 6, the first engine to read every record) each start ended in it,
    // eight more pipelines left out each time. After one crash the warm-up stays off with this
    // driver and engine build; the pipelines compile in play, as before beta 6. Another driver or
    // engine build tries it again.
    if (!guard_path.empty()) {
        const auto off_path = guard_path.parent_path() / "warmup.off";
        const std::string off_id = "driver " + std::to_string(instance.GetDriverVersion()) +
                                   ", engine built " + __DATE__ + " " + __TIME__;
        bool off = !crashed.empty();
        if (off) {
            std::ofstream{off_path, std::ios::trunc} << off_id << '\n';
        } else if (std::ifstream last{off_path}; last) {
            std::string line;
            std::getline(last, line);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            off = line == off_id;
            last.close();
            if (!off) {
                std::error_code ignored;
                std::filesystem::remove(off_path, ignored);
            }
        }
        if (off) {
            std::error_code ignored;
            std::filesystem::remove(guard_path, ignored);
            {
                std::scoped_lock lock{progress_mutex};
                precache_progress.rejected = precache_progress.total;
            }
            std::printf("GPU cache: not built at the start: the GPU driver crashed in the start's "
                        "warm-up (%s); pipelines compile in play until the driver or the engine "
                        "changes (delete warmup.off in the cache folder to try again)\n",
                        off_id.c_str());
            std::fflush(stdout);
            database.FinishPreload();
            complete();
            return;
        }
    }
    std::mutex guard_mutex;
    std::vector<std::string> building; // under guard_mutex once the driver's threads run
    // Written over in place (spaces over what a longer list left; the reader skips blank lines).
    std::fstream guard;
    if (!guard_path.empty()) {
        guard.open(guard_path, std::ios::out | std::ios::trunc);
    }
    size_t guard_size = 0;
    const auto write_guard = [&] {
        if (!guard.is_open()) {
            return;
        }
        std::string text;
        for (const auto& name : building) {
            text += name;
            text += '\n';
        }
        const size_t size = text.size();
        text.resize(std::max(size, guard_size), ' ');
        guard_size = size;
        guard.seekp(0);
        guard << text;
        guard.flush();
    };
    // bbport (docs/pipeline_compiles.md): how long the start waits for the cached pipelines.
    const auto warmup_start = std::chrono::steady_clock::now();
    // The pipelines' state is rebuilt here, one after the other: selection, programs, SRT walker
    // allocation and archive reads are shared. Their creation by the driver (100-300 ms each with
    // its cache cold, after every compiler or driver change) is collected (WarmupJobs) and runs
    // on BB_WARMUP_THREADS threads below; the game starts once all are there, as before.
    WarmupJobs batch;
    std::vector<const std::string*> job_names;
    warmup_jobs = &batch;
    for (const auto& name : names) {
        bool loaded = false;
        sel = {.regs = regs}; // Clear partial state after rejection, including runtime/fetch.
        if (crashed.contains(name)) {
            std::scoped_lock lock{progress_mutex};
            ++precache_progress.rejected;
            continue;
        }
        building.assign(1, name);
        write_guard();
        const size_t jobs_before = batch.jobs.size();
        try {
            std::vector<u8> data;
            database.Load(Storage::BlobType::PipelineKey, name, data);
            Serialization::Archive ar{std::move(data)};
            Serialization::Reader reader{ar};
            u32 version{}, is_compute{};
            reader.Read(version);
            reader.Read(is_compute);
            if ((version == 5 || version == Serialization::PipelineKeyVersion) && is_compute <= 1) {
                loaded = is_compute ? LoadComputePipeline(ar)
                                    : LoadGraphicsPipeline(ar, version == 5);
            }
        } catch (const std::exception& error) {
            ++damaged;
            LOG_WARNING(Render, "Skipping pipeline {}: {}", name, error.what());
        }
        job_names.resize(batch.jobs.size(), &name);
        if (batch.jobs.size() != jobs_before) {
            continue; // counted when its job has run
        }
        std::scoped_lock lock{progress_mutex};
        loaded ? ++precache_progress.loaded : ++precache_progress.rejected;
    }
    warmup_jobs = nullptr;
    const auto rebuilt_at = std::chrono::steady_clock::now();
    u32 threads = std::clamp<u32>(BbThreads::Available() > 1 ? BbThreads::Available() - 1 : 1, 1, 8);
    if (const char* env = std::getenv("BB_WARMUP_THREADS"); env && env[0]) {
        threads = std::clamp(std::atoi(env), 1, 32);
    }
    threads = u32(std::min<size_t>(threads, std::max<size_t>(batch.jobs.size(), 1)));
    std::atomic<size_t> next_job{0};
    std::atomic<u32> refused{0};
    const auto worker = [&] {
        for (size_t i; (i = next_job.fetch_add(1)) < batch.jobs.size();) {
            {
                std::scoped_lock lock{guard_mutex};
                building.push_back(*job_names[i]);
                write_guard();
            }
            const bool created = batch.jobs[i]();
            {
                std::scoped_lock lock{guard_mutex};
                building.erase(std::find(building.begin(), building.end(), *job_names[i]));
                write_guard();
            }
            if (!created) {
                refused.fetch_add(1);
                LOG_WARNING(Render, "Skipping pipeline {}: the driver refused it", *job_names[i]);
            }
            std::scoped_lock lock{progress_mutex};
            created ? ++precache_progress.loaded : ++precache_progress.rejected;
        }
    };
    building.clear();
    std::vector<std::thread> pool;
    for (u32 t = 1; t < threads; ++t) {
        pool.emplace_back(worker);
    }
    worker();
    for (auto& thread : pool) {
        thread.join();
    }
    // A pipeline the driver refuses is a damaged cache entry, as one whose record fails to load.
    damaged += refused.load();
    sel = {.regs = regs};
    if (guard.is_open()) {
        guard.close();
        std::error_code ignored;
        std::filesystem::remove(guard_path, ignored);
    }
    if (damaged) {
        // A failed reconstruction may have inserted earlier stages of the damaged record.
        // 0.4 discards those too, while retaining the fork's preload progress reporting.
        graphics_pipelines.clear();
        compute_pipelines.clear();
        std::unordered_set<VkShaderModule> modules;
        for (const auto& [_, program] : program_cache) {
            if (!program) continue;
            for (const auto& permutation : program->modules) {
                if (permutation.module) modules.insert(VkShaderModule(permutation.module));
            }
        }
        for (const auto module : modules) instance.GetDevice().destroyShaderModule(vk::ShaderModule{module});
        program_cache.clear();
        database.Clear();
        database.FinishPreload();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        database.Save(Storage::BlobType::ShaderProfile, "profile", std::move(profile_data));
        std::scoped_lock lock{progress_mutex};
        precache_progress.loaded = 0;
    }
    const auto status = GetPrecacheProgress();
    LOG_INFO(Render, "Preloaded {} of {} pipelines ({} rejected)", status.loaded, status.total,
             status.rejected);
    std::printf("GPU cache: %u of %u cached pipelines built at the start in %.1f s (%u left out; "
                "their state %.1f s, then the driver on %u threads, BB_WARMUP_THREADS)\n",
                status.loaded, status.total,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - warmup_start).count(),
                status.rejected, std::chrono::duration<double>(rebuilt_at - warmup_start).count(),
                threads);
    std::fflush(stdout);
    database.FinishPreload();
    complete();
}

void PipelineCache::Sync() {
    std::scoped_lock lock{native_cache_mutex};
    try {
        SaveNativeCache();
        Storage::DataBase::Instance().Close();
    } catch (const std::exception& error) {
        LOG_WARNING(Render, "Pipeline cache shutdown save failed: {}", error.what());
    }
}

} // namespace Vulkan

namespace Shader {

// "TCX1": the module uses the tone curve extension's push data.
static constexpr u32 ToneCurveTag = 0x31584354;
// "GT71": a composite compiled with the GT7 tone map pass (applied or not).
static constexpr u32 Gt7CheckedTag = 0x31375447;

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
    // bbport: last in the record, so records written before it still load (without a plan).
    table_plan.Serialize(ar);
    if (uses_tone_curve) {
        Serialization::Writer tail{ar};
        tail.Write(ToneCurveTag);
    }
    if (gt7_checked) {
        Serialization::Writer tail{ar};
        tail.Write(Gt7CheckedTag);
    }
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    if (!srt_info.Deserialize(ar)) {
        return false;
    }
    // bbport: a record of an older engine ends here: its shader runs the walker. Then tagged
    // parts: the table plan, the tone curve extension (native/tone_curve.h).
    table_plan = {};
    uses_tone_curve = false;
    gt7_checked = false;
    while (!ar.IsEoS()) {
        // bbport: zero bytes to the end are room the writer left after a vertex shader's fetch
        // shader data (up to 0.9.37 beta 5): not a part. Taken for one, they left every such shader
        // out of the start's warm-up (308 of 387 pipelines on the dev cache compiled in play).
        const u8* rest = ar.CurrPtr();
        if (std::all_of(rest, rest + ar.Remaining(), [](u8 byte) { return byte == 0; })) {
            break;
        }
        if (ar.Remaining() < sizeof(u32)) {
            return false;
        }
        u32 tag{};
        std::memcpy(&tag, ar.CurrPtr(), sizeof(tag));
        if (tag == ToneCurveTag) {
            ar.Advance(sizeof(tag));
            uses_tone_curve = true;
        } else if (tag == Gt7CheckedTag) {
            ar.Advance(sizeof(tag));
            gt7_checked = true;
        } else if (!table_plan.Deserialize(ar)) {
            return false;
        }
    }
    return true;
}

namespace Native {

// "TPL1": a table plan follows (native/table_plan.h).
static constexpr u32 TablePlanMagic = 0x314C5054;

void TablePlan::Serialize(Serialization::Archive& ar) const {
    if (!valid) {
        return;
    }
    Serialization::Writer plan{ar};
    plan.Write(TablePlanMagic);
    plan.Write(flat_dwords);
    plan.Write(entries);
    plan.Write(ops);
    plan.Write(table_dwords);
}

bool TablePlan::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader plan{ar};
    u32 magic{};
    plan.Read(magic);
    if (magic != TablePlanMagic) {
        return false;
    }
    plan.Read(flat_dwords);
    plan.Read(entries);
    plan.Read(ops);
    plan.Read(table_dwords);
    // A plan that would read outside its own arrays is not run.
    valid = flat_dwords >= FirstFlat && entries.size() == flat_dwords - FirstFlat &&
            table_dwords.size() == flat_dwords;
    for (size_t i = 0; valid && i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        valid = entry.parent < FirstFlat + i &&
                (entry.expr_len == 0 || size_t(entry.offset) + entry.expr_len <= ops.size());
        // An expression reads flattened values before its own and keeps its stack in bounds.
        u32 depth = 0;
        for (u32 k = 0; valid && k < entry.expr_len; ++k) {
            const ExprOp& op = ops[entry.offset + k];
            const u32 pops = op.op == Op::Imm || op.op == Op::Flat ? 0
                             : op.op == Op::Not                    ? 1
                             : op.op == Op::Bfe                    ? 3
                                                                   : 2;
            valid = depth >= pops && (op.op != Op::Flat || op.value < FirstFlat + i) &&
                    op.op <= Op::Bfe;
            depth = depth - pops + 1;
            valid = valid && depth <= MaxStack;
        }
        valid = valid && (entry.expr_len == 0 || depth == 1);
    }
    return true;
}

} // namespace Native

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        // bbport: the size is checked before the code is registered (it becomes executable):
        // a cut-short file must not have bytes past its end run as the walker.
        const auto code = ar.CurrPtr();
        ar.Advance(walker_func_size);
        walker_func = RegisterWalkerCode(code, walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        if (fetch_data_size != sizeof(Gcn::FetchShaderData)) {
            return false;
        }
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
