// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace BbEngine::GxObjects {
enum Kind : std::uint8_t {
    Buffer,
    Texture2D,
    Texture3D,
    DepthView,
    TargetView,
    ResourceView,
    AccessView,
    Shader,
    BlendState,
    DepthState,
    RasterState,
    SamplerState,
    kKinds
};

struct Object {
    Kind kind = Buffer;
    std::uint8_t creator = 0;
    bool live = false;
    std::uint32_t generation = 0;
    std::uint64_t source = 0;
    std::uint32_t description[16]{};
};

bool Install(std::uint8_t* image, std::size_t size);
bool On();
bool Find(std::uint64_t address, Object* out);
void CheckDraw(std::uint64_t pending);
void CheckDispatch(std::uint64_t pending);
struct TextureExtent {
    std::uint32_t width = 0, height = 0, levels = 0, layers = 0;
    bool volume = false;
    std::uint64_t room = 0;
};
bool TexturesOwned();
bool TextureAt(std::uint64_t address, TextureExtent* out);
enum WidenResult { Widened, WidenTooLarge, WidenOtherLayout, WidenOtherSize, kWidenResults };
void NoteWiden(WidenResult result);
bool TexturesChecked();
struct HeldLink {
    std::uint64_t uid = 0, generation = 0;
    std::uint32_t index = 0;
};
enum HeldUse { HeldNothing, HeldServe, HeldSearch };
inline HeldUse UseHeld(const HeldLink& link, std::uint64_t registry_generation) {
    if (!link.uid) return HeldNothing;
    return link.generation == registry_generation ? HeldServe : HeldSearch;
}
class HeldLinks {
public:
    void Hold(std::uint64_t base, std::uint32_t index, std::uint64_t uid, std::uint64_t generation) {
        if (!uid) return;
        Forget(base);
        if (const auto old = bases_.find(uid); old != bases_.end()) links_.erase(old->second);
        links_[base] = {uid, generation, index};
        bases_[uid] = base;
    }
    HeldLink At(std::uint64_t base) const {
        const auto at = links_.find(base);
        return at == links_.end() ? HeldLink{} : at->second;
    }
    void Forget(std::uint64_t base) {
        const auto at = links_.find(base);
        if (at == links_.end()) return;
        bases_.erase(at->second.uid);
        links_.erase(at);
    }
    bool Drop(std::uint64_t uid) {
        const auto at = bases_.find(uid);
        if (at == bases_.end()) return false;
        links_.erase(at->second);
        bases_.erase(at);
        return true;
    }
    std::size_t Size() const { return links_.size(); }

private:
    std::unordered_map<std::uint64_t, HeldLink> links_;
    std::unordered_map<std::uint64_t, std::uint64_t> bases_;
};
struct HeldImage {
    std::uint64_t object = 0, uid = 0, generation = 0;
    std::uint32_t index = 0;
};
HeldImage HeldAt(std::uint64_t address);
void HoldImage(std::uint64_t address, std::uint32_t index, std::uint64_t uid, std::uint64_t generation);
void DropHeldImage(std::uint64_t uid);
enum ObjectPath { PathServed, PathNoObject, PathNothingHeld, PathHeldGone, PathHeldRefused, PathMadeDirectly, PathOverlapped, PathHeldStale, kObjectPaths };
void NoteObjectPath(ObjectPath path, const char* refused = nullptr, std::size_t length = 0);
enum HeldCheck { CheckSame, CheckResolver, CheckOther, kHeldChecks };
void NoteHeldCheck(HeldCheck result, std::uint32_t others);
void NoteOverlap(const char* action, std::size_t length);
void NoteImage(std::uint64_t address, std::uint32_t bytes, std::uint64_t uid, std::uint32_t width, std::uint32_t height,
               std::uint32_t format);
std::uint64_t Epoch();
std::uint32_t BufferVersion(std::uint64_t object);
void TouchBuffer(std::uint64_t object);
bool BuffersOwned();
bool TakeUploadArena(std::uint64_t* base, std::uint64_t* bytes);
bool UploadArena(std::uint64_t* base, std::uint64_t* bytes);
bool BufferAt(std::uint64_t address);
bool SealingOn();
void SealForWhole(int whole);
struct SealLevels {
    std::uint32_t live = 0;
    std::uint64_t sealed = 0, notes = 0;
};
bool SealedLevels(SealLevels* out);
bool SealedFind(std::uint64_t address, std::uint64_t size, std::uint32_t* id, std::uint32_t* generation, std::uint64_t* base, std::uint64_t* bytes);
struct SealedExtent {
    bool found;
    std::uint32_t id, generation;
    std::uint64_t base, bytes;
};
void SealedFindAll(const std::uint64_t* addresses, const std::uint64_t* sizes, std::uint32_t count, SealedExtent* out);
bool SealedValid(std::uint32_t id, std::uint32_t generation);
void NoteCpuWrite(std::uint64_t address, std::uint64_t size, std::uint8_t retired_by);
void NoteGpuWrite(std::uint64_t address, std::uint64_t size);
bool SealedAt(std::uint64_t address);
void NoteSealedFault(bool still_sealed);
void PrintSeals(const char* when);
void Report();
}
