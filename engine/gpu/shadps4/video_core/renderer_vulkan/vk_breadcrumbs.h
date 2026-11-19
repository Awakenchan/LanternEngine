// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: GPU breadcrumbs, to name the command a GPU hang (ring timeout, then device lost) is
// stuck in. Each scheduler is a stream: its draws, dispatches and our own passes are numbered in
// recording order, and the GPU writes 2n before command n and 2n+1 after it into a host-visible
// buffer (VK_AMD_buffer_marker at the bottom of the pipe: written once all earlier work of the
// queue has finished). On device lost the last value says which command never finished, and the
// notes kept here say what it was. Off by default; BB_BREADCRUMBS=1 forces them on and =0 off,
// and a device loss turns them on for the next three starts (vk_breadcrumbs_arming.h).
#pragma once

#include <atomic>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace VideoCore {
struct Image;
}

namespace Vulkan::Breadcrumbs {

struct DrawResources;

enum class Kind : u8 { Pass, Draw, DrawIndexed, DrawIndirect, Dispatch, DispatchIndirect };

struct Crumb {
    Kind kind = Kind::Pass;
    const char* name = nullptr; ///< our passes: a string literal
    u64 hash[2]{};              ///< guest shaders: vertex and pixel, or compute
    u64 program[2]{};           ///< their code in guest memory
    u64 address = 0;            ///< indirect arguments
    u32 count[3]{};             ///< indices or vertices and instances, or groups
    u32 args_slot = ~0u;        ///< indirect dispatches: where the GPU copies its arguments
};

struct Switch {
    bool markers = false;
};
extern Switch marker_switch;

/// A stream for a new scheduler (`name` a string literal); the first call creates the buffer.
u32 NewStream(const Instance& instance, const char* name);
[[nodiscard]] inline bool Enabled() noexcept {
    return marker_switch.markers;
}
/// Recording side, in command order: numbers the next command of `stream`.
u32 Note(u32 stream, const Crumb& crumb);
/// Recorded right before (`after` false) and right after the command numbered `id`.
void Mark(vk::CommandBuffer cmdbuf, u32 stream, u32 id, bool after);
/// Indirect dispatches: a slot of a host-visible buffer for the arguments the GPU reads (the
/// report and the statistics show them), ~0u when off. CopyArgs is recorded before the dispatch.
u32 ArgsSlot();
void CopyArgs(vk::CommandBuffer cmdbuf, vk::Buffer args, u64 offset, u32 slot);
/// Device lost: prints, per stream, the command the GPU did not finish and what it was.
void ReportDeviceLost(const char* where);
bool DeviceLost() noexcept;
void ReportDeviceFault();
vk::UniqueDebugUtilsMessengerEXT CreateAddressReport(vk::Instance instance);
/// The same report, once, while the GPU has not finished submitted work for a long time (no
/// device lost yet: a reset may never come, or the waiting thread would not see it).
void ReportStuck(const char* where);
/// Frame statistics: per stream, the commands noted and how many the GPU has yet to finish.
void PrintProgress();
void WatchStageB(const std::atomic<u64>* busy_cycles) noexcept;

[[nodiscard]] DrawResources* BeginDraw(u32 stream, u32 image_descriptors, u32 buffer_descriptors);
void AddImage(DrawResources& draw, const VideoCore::Image& image);
void AddBuffer(DrawResources& draw, vk::Buffer buffer, u64 guest, u64 offset, u64 size);
void EndDraw(DrawResources& draw);
void NameBound(u64 handle, const char* name);

/// Breadcrumbs around what is recorded on `cmdbuf` right here (not deferred) in the scope.
class Scope {
public:
    Scope(vk::CommandBuffer cmdbuf_, u32 stream_, const char* name)
        : cmdbuf{cmdbuf_}, stream{stream_} {
        if (Enabled()) {
            id = Note(stream, {.name = name});
            Mark(cmdbuf, stream, id, false);
        }
    }
    ~Scope() {
        if (id) {
            Mark(cmdbuf, stream, id, true);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    vk::CommandBuffer cmdbuf;
    u32 stream;
    u32 id = 0;
};

} // namespace Vulkan::Breadcrumbs
