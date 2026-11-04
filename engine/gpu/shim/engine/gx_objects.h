// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

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
struct HeldImage {
    std::uint64_t object = 0, uid = 0;
    std::uint32_t index = 0;
};
HeldImage HeldAt(std::uint64_t address);
void HoldImage(std::uint64_t address, std::uint32_t index, std::uint64_t uid);
enum ObjectPath { PathServed, PathNoObject, PathNothingHeld, PathHeldGone, PathHeldRefused, PathMadeDirectly, PathOverlapped, kObjectPaths };
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
void Report();
}
