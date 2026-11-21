// SPDX-License-Identifier: GPL-3.0-or-later

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_optiscaler.h"

namespace Vulkan::OptiScaler {

namespace {

/// The switch `name` (BB_OPTISCALER_*): its variable when set (0: off), else lantern.ini's value at
/// start (the in-game menu's optiscaler_*).
bool EnvOn(const char* name) {
    if (const char* env = std::getenv(name)) {
        return env[0] != '0';
    }
    const auto& ini = BbSettings::Get().startup_optiscaler;
    const std::string_view key{name};
    return key == "BB_OPTISCALER_COMPAT"         ? ini.compat
           : key == "BB_OPTISCALER_NGX_LOCK"     ? ini.ngx_lock
           : key == "BB_OPTISCALER_CLEAN_THREAD" ? ini.clean_thread
           : key == "BB_OPTISCALER_SYNC_SUBMIT"  ? ini.sync_submit != BbSettings::OptiSubmitOff
                                                 : true;
}

#ifdef _WIN32
std::string Narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t c : text) {
        out.push_back(c < 128 ? char(c) : '?');
    }
    return out;
}

/// "x.y.z.w" from the file's version resource (version.dll is loaded here, not linked).
std::string FileVersion(const wchar_t* path) {
    const HMODULE dll = LoadLibraryW(L"version.dll");
    if (!dll) {
        return {};
    }
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const auto size_fn = reinterpret_cast<SizeFn>(GetProcAddress(dll, "GetFileVersionInfoSizeW"));
    const auto info_fn = reinterpret_cast<InfoFn>(GetProcAddress(dll, "GetFileVersionInfoW"));
    const auto query_fn = reinterpret_cast<QueryFn>(GetProcAddress(dll, "VerQueryValueW"));
    std::string version;
    DWORD handle = 0;
    const DWORD size = size_fn && info_fn && query_fn ? size_fn(path, &handle) : 0;
    if (size) {
        std::vector<char> data(size);
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT length = 0;
        if (info_fn(path, 0, size, data.data()) &&
            query_fn(data.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &length) && fixed &&
            fixed->dwFileVersionMS) {
            char text[64];
            std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS),
                          LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS),
                          LOWORD(fixed->dwFileVersionLS));
            version = text;
        }
    }
    FreeLibrary(dll);
    return version;
}

std::string Lower(std::string text) {
    for (auto& c : text) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r");
    const auto last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
}

/// Its frame generation from OptiScaler.ini's [FrameGen]: FGOutput (v10: dlssg, fsrfg, xefg, ...)
/// or FGType (0.9: optifg, nukems). Empty when it is off (nofg) or left to its default (auto).
std::string FrameGeneration(const std::filesystem::path& ini) {
    std::ifstream file(ini);
    std::string line, section, output, type;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line[0] == '[') {
            section = Lower(Trim(line.substr(1, line.find(']') - 1)));
            continue;
        }
        const auto eq = line.find('=');
        if (section != "framegen" || eq == std::string::npos) {
            continue;
        }
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string value = Lower(Trim(line.substr(eq + 1)));
        if (key == "fgoutput") {
            output = value;
        } else if (key == "fgtype") {
            type = value;
        }
    }
    const std::string fg = output.empty() ? type : output;
    return fg == "nofg" || fg == "auto" ? std::string{} : fg;
}

/// OptiScaler exports the NGX entry points under a proxy name (winmm.dll, dxgi.dll, version.dll,
/// ...) and keeps OptiScaler.ini beside itself. The driver's own NGX core (_nvngx.dll / nvngx.dll)
/// exports them too and counts only with that ini.
Info Find() {
    Info info;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return info;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot, &entry); more && !info.loaded;
         more = Module32NextW(snapshot, &entry)) {
        std::wstring name = entry.szModule;
        for (auto& c : name) {
            c = wchar_t(std::towlower(c));
        }
        if (!GetProcAddress(entry.hModule, "NVSDK_NGX_VULKAN_Init_Ext2")) {
            continue;
        }
        std::error_code ec;
        const auto ini_path = std::filesystem::path(entry.szExePath).parent_path() / L"OptiScaler.ini";
        const bool ini = std::filesystem::exists(ini_path, ec);
        const bool ngx_core = name == L"_nvngx.dll" || name == L"nvngx.dll";
        if (ngx_core && !ini) {
            continue;
        }
        info.loaded = true;
        info.handle = entry.hModule;
        info.module = Narrow(entry.szModule);
        info.version = FileVersion(entry.szExePath);
        if (ini) {
            info.frame_generation = FrameGeneration(ini_path);
        }
    }
    CloseHandle(snapshot);
    return info;
}
#else
Info Find() {
    return {};
}
#endif

} // namespace

const Info& Detect() {
    static const Info info = [] {
        Info found = Find();
        if (found.loaded) {
            std::printf("OptiScaler: %s%s loaded as %s; compatibility mode: %s\n",
                        found.version.empty() ? "" : "v",
                        found.version.empty() ? "(version unknown)" : found.version.c_str(),
                        found.module.c_str(),
                        EnvOn("BB_OPTISCALER_COMPAT")
                            ? "on - submissions from one thread while its frame generation is on "
                              "(BB_OPTISCALER_SYNC_SUBMIT), NGX calls under the queue lock "
                              "(BB_OPTISCALER_NGX_LOCK); "
                              "BB_OPTISCALER_COMPAT=0 turns it off"
                            : "off (BB_OPTISCALER_COMPAT=0 or optiscaler_compat=0)");
            if (!found.frame_generation.empty()) {
                // Its Vulkan frame generation runs on a D3D12 device of its own (Vulkan w/Dx12),
                // created when the first upscaled frame arrives. It needs its vkCreateDevice hook
                // to prepare our device (OnCleanThread), and one submit thread.
                std::printf("OptiScaler: its frame generation is on (FGOutput=%s in OptiScaler.ini): "
                            "the Vulkan device is created where OptiScaler can prepare it "
                            "(BB_OPTISCALER_CLEAN_THREAD), submissions from one thread; if the GPU "
                            "is still lost, set Frame Generation to none in OptiScaler's menu\n",
                            found.frame_generation.c_str());
            }
        }
        return found;
    }();
    return info;
}

bool Compat() {
    static const bool on = Detect().loaded && EnvOn("BB_OPTISCALER_COMPAT");
    return on;
}

bool Part(const char* name) {
    return Compat() && EnvOn(name);
}

bool SyncSubmit() {
    const char* env = std::getenv("BB_OPTISCALER_SYNC_SUBMIT");
    const bool always = env ? env[0] == '1'
                            : BbSettings::Get().startup_optiscaler.sync_submit == BbSettings::OptiSubmitOn;
    return Part("BB_OPTISCALER_SYNC_SUBMIT") && (!Detect().frame_generation.empty() || always);
}

void* NgxProvider() {
    const char* env = std::getenv("BB_OPTISCALER_NGX");
    if (env && env[0] == '0') {
        return nullptr;
    }
    const auto& info = Detect();
    return info.loaded ? info.handle : nullptr;
}

std::filesystem::path PrivateFfxLibrary(const std::filesystem::path& source, const char* name) {
#ifdef _WIN32
    const char* env = std::getenv("BB_OPTISCALER_FFX_PRIVATE");
    if (!(env && env[0] == '1') && !Part("BB_OPTISCALER_FFX_PRIVATE")) {
        return {};
    }
    std::wstring exe(MAX_PATH, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size())));
    const auto dir = std::filesystem::path{exe}.parent_path() / "ffx-private";
    const auto target = dir / name;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto source_size = std::filesystem::file_size(source, ec);
    if (ec) {
        return {};
    }
    const auto source_time = std::filesystem::last_write_time(source, ec);
    const bool same = std::filesystem::file_size(target, ec) == source_size && !ec &&
                      std::filesystem::last_write_time(target, ec) == source_time && !ec;
    if (!same) {
        std::filesystem::remove(target, ec);
        if (!CreateHardLinkW(target.c_str(), source.c_str(), nullptr)) {
            std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
    return std::filesystem::exists(target, ec) ? target : std::filesystem::path{};
#else
    return {};
#endif
}

const char* FfxFailureHint() {
    return Detect().loaded ? "; OptiScaler took over this call: set EnableFfxInputs=false in OptiScaler.ini, "
                             "or take its files out of engine\\out"
                           : "";
}

} // namespace Vulkan::OptiScaler
