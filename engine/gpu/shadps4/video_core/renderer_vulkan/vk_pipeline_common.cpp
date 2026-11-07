// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "bbport_settings.h"
#include "shader_recompiler/native/gt7_tonemap.h"
#include "shader_recompiler/native/tone_curve.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/pipeline_compile_queue.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() {
    // bbport: a compile thread may still be writing `pipeline` (BB_ASYNC_PIPELINES).
    WaitReady();
}

// bbport: time render threads waited for pipelines created on compile threads; reported with
// the compile time of the GPU thread (BB_FRAME_STATS "shader/pipeline compiles, ... ms").
extern std::atomic<u64> g_bb_compile_ns, g_bb_compile_ns_total;

void Pipeline::WaitReady() const noexcept {
    if (ready.load(std::memory_order_acquire)) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    // bbport: still queued (the compile threads busy with the pipelines before it): compiled
    // here, instead of waiting for them first and then for this one.
    if (auto job = TakePipelineJob(this)) {
        job();
        pipeline_compile_stats.taken.fetch_add(1, std::memory_order_relaxed);
    }
    while (!ready.load(std::memory_order_acquire)) {
        ready.wait(false, std::memory_order_acquire);
    }
    const u64 waited = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
    g_bb_compile_ns += waited;
    g_bb_compile_ns_total += waited;
    pipeline_compile_stats.NoteWait(waited);
}

namespace {
bool EnvOff(const char* name) {
    const char* env = std::getenv(name);
    return env && env[0] == '0';
}
} // namespace

u64 SkipUncompiledBudgetNs() {
    static const u64 budget = [] {
        if (EnvOff("BB_SKIP_UNCOMPILED")) {
            return u64{0};
        }
        const char* env = std::getenv("BB_SKIP_UNCOMPILED_MS");
        const u64 ms = env && env[0] ? std::strtoull(env, nullptr, 10) : 1000;
        return ms * 1'000'000;
    }();
    return budget;
}

bool LatePipelineBindEnabled() {
    static const bool enabled = !EnvOff("BB_LATE_PIPELINE_BIND");
    return enabled;
}

void LogPipelineCompileMode(const Instance& instance, bool late_binds_possible) {
    if (!AsyncPipelinesEnabled()) {
        std::printf("Pipelines: new ones compiled where the draw needs them (BB_ASYNC_PIPELINES=0)\n");
        return;
    }
    // VK_EXT_graphics_pipeline_library is reported, not used: shaders reach the port at the draw
    // that needs them and a pipeline's shaders share one descriptor set layout, so the libraries
    // would be compiled at that draw as well (docs/pipeline_compiles.md).
    const auto extensions = instance.GetAvailableExtensions();
    const bool gpl = std::ranges::find(extensions, std::string{
                                                       VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME}) !=
                     extensions.end();
    bool fast_linking = false;
    if (gpl) {
        const auto chain = instance.GetPhysicalDevice()
                               .getProperties2<vk::PhysicalDeviceProperties2,
                                               vk::PhysicalDeviceGraphicsPipelineLibraryPropertiesEXT>();
        fast_linking = chain.get<vk::PhysicalDeviceGraphicsPipelineLibraryPropertiesEXT>()
                           .graphicsPipelineLibraryFastLinking;
    }
    const u64 budget_ms = SkipUncompiledBudgetNs() / 1'000'000;
    std::printf("Pipelines: new ones compiled on %u threads; draws of one still compiling: scene "
                "geometry %s, other draws %s (BB_SKIP_UNCOMPILED, BB_LATE_PIPELINE_BIND); graphics "
                "pipeline library %s, not used\n",
                PipelineCompileThreads(),
                budget_ms ? fmt::format("left out for up to {} ms", budget_ms).c_str() : "waits",
                late_binds_possible && LatePipelineBindEnabled()
                    ? "bind it on the recording threads (they wait for it)"
                    : "wait for it on the draw recording thread",
                gpl ? (fast_linking ? "offered (fast linking)" : "offered (no fast linking)")
                    : "not offered");
    std::fflush(stdout);
}

void Pipeline::MarkReady() noexcept {
    ready.store(true, std::memory_order_release);
    ready.notify_all();
}


namespace {
/// bbport: the tone curve extension's parameters for this draw (native/tone_curve.h), from the
/// HDR settings: on only with an HDR output. BB_HDR_SCENE_KNEE (0.75): the curve's output where
/// the extension starts.
Shader::Native::ToneCurvePush ToneCurveParams() {
    static const float knee = [] {
        const char* env = std::getenv("BB_HDR_SCENE_KNEE");
        const float value = env ? float(std::atof(env)) : 0.75f;
        return value > 0.0f && value < 1.0f ? value : 0.75f;
    }();
    const auto& settings = BbSettings::Get();
    const float paper = std::clamp(settings.hdr_paper_white.load(std::memory_order_relaxed),
                                   BbSettings::HdrPaperWhiteMin, BbSettings::HdrPaperWhiteMax);
    const bool hdr = settings.hdr_output.load(std::memory_order_relaxed) != BbSettings::HdrOutputOff;
    return {
        .knee = knee,
        .peak = BbSettings::HdrPeakNits(settings) / paper,
        // As native mode: boost is the share of the scene's range above white, sqrt in log2.
        .strength = std::sqrt(std::clamp(settings.hdr_boost.load(std::memory_order_relaxed), 0.0f, 1.0f)),
        .enabled = hdr ? 1.0f : 0.0f,
    };
}
} // namespace

void Pipeline::BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data,
                             std::span<const vk::DescriptorImageInfo> image_infos,
                             std::span<const vk::DescriptorBufferInfo> buffer_infos) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });
    if (uses_tone_curve && !Shader::Native::ToneCurveEnabled()) {
        const auto& settings = BbSettings::Get();
        const Shader::Native::Gt7Push gt7{
            .enabled = settings.graphics_tonemapper.load(std::memory_order_relaxed) ==
                               BbSettings::TonemapperGt7
                           ? 1.0f
                           : 0.0f,
            .exposure = settings.graphics_gt7_exposure.load(std::memory_order_relaxed),
            .unused = {},
        };
        scheduler.Record([layout, stage_flags, gt7](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushConstants(layout, stage_flags, Shader::Native::ToneCurvePushOffset,
                                 sizeof(gt7), &gt7);
        });
    } else if (uses_tone_curve) {
        const Shader::Native::ToneCurvePush tone = ToneCurveParams();
        scheduler.Record([layout, stage_flags, tone](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushConstants(layout, stage_flags, Shader::Native::ToneCurvePushOffset,
                                 sizeof(tone), &tone);
        });
    }

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        // bbport: when every write points into the caller's info arrays (draws and
        // dispatches), each array is copied once and the pointers are rebased: a copy per
        // descriptor (two per write) cost the draw recording thread ~3% of its time.
        const auto within = [](const auto* p, u32 count, const auto& array) {
            return p >= array.data() && p + count <= array.data() + array.size();
        };
        bool bulk = !image_infos.empty() || !buffer_infos.empty();
        for (const auto& write : set_writes) {
            if (!bulk) {
                break;
            }
            bulk = !write.pTexelBufferView &&
                   (!write.pImageInfo || within(write.pImageInfo, write.descriptorCount,
                                                image_infos)) &&
                   (!write.pBufferInfo || within(write.pBufferInfo, write.descriptorCount,
                                                 buffer_infos));
        }
        if (bulk) {
            scheduler.ReserveRecordData(set_writes.size() * sizeof(vk::WriteDescriptorSet) +
                                        image_infos.size_bytes() + buffer_infos.size_bytes() +
                                        256);
            const auto writes =
                scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
            const auto images = scheduler.RecordData(image_infos);
            const auto buffers = scheduler.RecordData(buffer_infos);
            auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
            for (size_t i = 0; i < writes.size(); ++i) {
                auto& write = patched[i];
                if (write.pImageInfo) {
                    write.pImageInfo = images.data() + (write.pImageInfo - image_infos.data());
                }
                if (write.pBufferInfo) {
                    write.pBufferInfo = buffers.data() + (write.pBufferInfo - buffer_infos.data());
                }
            }
            scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
            });
            return;
        }
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
