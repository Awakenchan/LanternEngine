// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

/// bbport: pipelines first created during play are created on compile threads
/// (BB_ASYNC_PIPELINES=0: where they are selected, as before; vk_graphics_pipeline.cpp).
bool AsyncPipelinesEnabled();
/// Runs `job`, which creates `owner`, on a compile thread (BB_PIPELINE_THREADS), first in,
/// first out.
void SubmitPipelineJob(const void* owner, std::function<void()> job);
/// bbport: `owner`'s job when no compile thread has started it yet (the caller runs it), else
/// empty (BB_PIPELINE_TAKE=0: always empty).
std::function<void()> TakePipelineJob(const void* owner);
/// The number of compile threads.
u32 PipelineCompileThreads();
std::uint64_t PipelineJobsUnfinished();

/// bbport: the start's cached pipelines are created on several threads (PipelineCache::WarmUp,
/// BB_WARMUP_THREADS). While this is set on the thread that rebuilds them, a pipeline that would
/// be created at once hands its creation over instead (its handle waits as for a compile
/// thread's); a job returns false when the driver refuses the pipeline (a damaged cache entry).
struct WarmupJobs {
    std::vector<std::function<bool()>> jobs;
};
inline thread_local WarmupJobs* warmup_jobs = nullptr;

/// bbport (docs/pipeline_compiles.md): what a draw does with a pipeline still compiling.
/// Scene geometry is left out (BB_SKIP_UNCOMPILED=0: never) for at most this long
/// (BB_SKIP_UNCOMPILED_MS, default 1000); 0 when draws are never left out.
u64 SkipUncompiledBudgetNs();
/// Other draws and dispatches bind it on the recording threads, which wait for it there
/// (BB_LATE_PIPELINE_BIND=0: the draw recording thread waits, as before).
bool LatePipelineBindEnabled();
/// One line at start saying which of these run, with whether the device offers
/// VK_EXT_graphics_pipeline_library (not used: see the document).
void LogPipelineCompileMode(const Instance& instance, bool late_binds_possible);

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    /// bbport: a graphics pipeline may still be created on a compile thread
    /// (BB_ASYNC_PIPELINES): the first caller that needs the handle waits for it.
    vk::Pipeline Handle() const noexcept {
        if (!ready.load(std::memory_order_acquire)) [[unlikely]] {
            WaitReady();
        }
        return *pipeline;
    }

    /// bbport: false while the pipeline is still being created on a compile thread.
    [[nodiscard]] bool IsReady() const noexcept {
        return ready.load(std::memory_order_acquire);
    }

    /// bbport: the handle once the pipeline is created, else null without waiting (the caller
    /// leaves the draw out or binds the pipeline on a recording thread). The same load and
    /// branch as Handle() for a created pipeline.
    [[nodiscard]] vk::Pipeline HandleIfReady() const noexcept {
        return ready.load(std::memory_order_acquire) ? *pipeline : vk::Pipeline{};
    }

    /// bbport: when a draw of this pipeline was first left out while it compiled (0: never).
    /// Only the thread recording draws uses it (Rasterizer::SkipUncompiledDraw).
    mutable u64 first_skip_ns = 0;

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    /// `image_infos` and `buffer_infos`: the arrays the writes point into, if known (their
    /// descriptors are then copied for the recording thread in one piece each).
    void BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data,
                       std::span<const vk::DescriptorImageInfo> image_infos = {},
                       std::span<const vk::DescriptorBufferInfo> buffer_infos = {}) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;
    /// bbport: blocks until a compile thread has stored `pipeline` (Handle, destructors); runs
    /// the job itself when no compile thread has started it (TakePipelineJob).
    void WaitReady() const noexcept;
    /// bbport: the compile thread stores `pipeline`, then sets this (release) and wakes waiters.
    void MarkReady() noexcept;

    /// bbport: cleared by a graphics pipeline whose creation went to a compile thread.
    std::atomic<bool> ready{true};
    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    /// bbport: the fragment shader reads Shader::Native::ToneCurvePush after the push data.
    bool uses_tone_curve{};
    bool is_compute;
};

} // namespace Vulkan
