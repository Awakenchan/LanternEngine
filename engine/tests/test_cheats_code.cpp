// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gpu/shim/engine/cheats.cpp"
#include <cstdarg>
#include <cstdio>
#include <thread>

using namespace BbEngine::Cheats;

#ifdef _WIN32
namespace BbPlatform {
bool ReadProcessMemory(const void* p, void* out, size_t n) {
    if (!p) return false;
    std::memcpy(out, p, n);
    return true;
}
}
#else
// The runtime's memory map, which BbPlatform::ReadProcessMemory falls back to on Linux: with no
// runtime in the test, every address reads as on Windows above (the test passes its own memory).
extern "C" int runtime_memory_vma_info(uintptr_t, int* prot, int* type, uintptr_t* end) {
    *prot = 1;
    *type = 0;
    *end = UINTPTR_MAX;
    return 1;
}
#endif

namespace BbEngine {
bool host_read_safe(const void* p, void* out, std::size_t n) {
    if (!p) return false;
    std::memcpy(out, p, n);
    return true;
}
void host_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::putchar('\n');
}
bool params_table(const char*, ParamTable*) { return false; }
std::uint64_t params_table_file(std::uint64_t) { return 0; }
bool params_records(const ParamTable&, std::vector<ParamRecord>*) { return false; }
std::uint64_t player_record() { return 0; }
bool player_stat_get(const char*, std::int64_t*) { return false; }
bool player_stat_set(const char*, std::int64_t) { return false; }
bool world_player_block(std::uint32_t*) { return false; }
}

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr std::size_t kImageSize = 0x2000000;
constexpr std::size_t kSite = kBloodMarkSite - BbEngine::kPreferredGuestSlide;
constexpr std::size_t kSignature = 0x17d2b82 - BbEngine::kPreferredGuestSlide;

void Settle() {
    BbEngine::Cheats::Tick();
    std::this_thread::sleep_for(kDebounce + std::chrono::milliseconds(50));
    BbEngine::Cheats::Tick();
}
}

int main() {
    std::vector<std::uint8_t> image(kImageSize);
    static_assert(kSite == 0x13d2bd2);
    std::memcpy(image.data() + kSignature, kSigBloodMark, sizeof kSigBloodMark);
    Check(std::memcmp(image.data() + kSite, kBloodMarkOriginal, sizeof kBloodMarkOriginal) == 0,
          "the game's blood mark code at 0x13d2bd2 is the tail of its signature");
    Check(BbEngine::Cheats::Install(image.data(), image.size()), "the keep-echoes cheat is prepared");
    Check(BbEngine::Cheats::Unavailable(BbEngine::Cheats::KeepEchoes) == nullptr, "keep echoes is available after the start");

    auto& settings = BbSettings::Get();
    settings.cheat_keep_echoes.store(true);
    Settle();
    Check(std::memcmp(image.data() + kSite, kBloodMarkKeep, sizeof kBloodMarkKeep) == 0,
          "turned on in the game, keep echoes writes its code at 0x13d2bd2 through the engine's writer");
    Check(BbEngine::Cheats::Unavailable(BbEngine::Cheats::KeepEchoes) == nullptr, "keep echoes stays available");

    settings.cheat_keep_echoes.store(false);
    Settle();
    Check(std::memcmp(image.data() + kSite, kBloodMarkOriginal, sizeof kBloodMarkOriginal) == 0,
          "turned off, the game's own code is back");

    image[kSite + 3] ^= 0xff;
    settings.cheat_keep_echoes.store(true);
    Settle();
    Check(image[kSite] == kBloodMarkOriginal[0] && image[kSite + 3] == std::uint8_t(kBloodMarkOriginal[3] ^ 0xff),
          "code changed by something else is not written over");
    Check(BbEngine::Cheats::Unavailable(BbEngine::Cheats::KeepEchoes) != nullptr, "and the cheat says so");

    if (failures) return 1;
    std::puts("cheats code: ok");
    return 0;
}
