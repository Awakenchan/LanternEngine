// SPDX-License-Identifier: GPL-2.0-or-later
#include "assets.h"
#include "common.h"
#include "elf.h"
#include "mods.h"
#include "online.h"
#include "patches.h"
#include "prepare.h"

#include <windows.h>

#include <cpuid.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>


namespace start {
namespace {

fs::path SelfPath() {
    wchar_t buffer[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    return fs::path(std::wstring(buffer, n));
}

struct Folders {
    fs::path engine;
    fs::path package;
    fs::path out;
};

Folders Locate() {
    Folders f;
    f.out = SelfPath().parent_path();
    f.engine = f.out.parent_path();
    std::error_code error;
    f.package = Lower(Utf8(f.engine.filename())) == "engine" && fs::is_regular_file(f.engine.parent_path() / "package.json", error)
                    ? f.engine.parent_path()
                    : f.engine;
    return f;
}

std::optional<std::string> PlateTitle(const std::map<std::string, std::string>& settings, const char* key) {
    const auto set = Setting(settings, key);
    if (!set) return std::optional<std::string>(std::string());
    std::string title;
    for (const char c : *set)
        if (std::uint8_t(c) >= 0x20 && c != 0x7f) title.push_back(c);
    if (title.size() > 48) {
        title.resize(48);
        while (!title.empty() && (std::uint8_t(title.back()) & 0xc0) == 0x80) title.pop_back();
        if (!title.empty() && std::uint8_t(title.back()) >= 0xc0) title.pop_back();
    }
    return title;
}

std::optional<std::string> SettingValue(const fs::path& config, const std::string& key) {
    return Setting(ReadSettings(config), key);
}

bool RecompileGpuCache(const fs::path& cache, const fs::path& recompiler) {
    std::error_code error;
    if (Env("BB_SHADER_STATES", "1") == "0" || !fs::is_regular_file(recompiler, error)) return false;
    bool all = true;
    for (const auto& entry : fs::directory_iterator(cache, error)) {
        std::error_code e;
        if (!entry.is_directory(e) || !fs::is_regular_file(entry.path() / "profile.bin", e)) continue;
        std::string output;
        const auto start = std::chrono::steady_clock::now();
        const int code = RunProgram(recompiler, {L"--recompile-cache", entry.path().native()}, &output);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto line = output.rfind("Shader cache:");
        if (code == 0 && line != std::string::npos) {
            Log("GPU cache %s: %s (%.1f s)", Utf8(entry.path().filename()).c_str(),
                Trim(output.substr(line + 14, output.find('\n', line) - line - 14)).c_str(), seconds);
        } else {
            fs::remove_all(entry.path(), e);
            Log("GPU cache %s: not recompiled (exit %d); its shaders compile again in play",
                Utf8(entry.path().filename()).c_str(), code);
            all = false;
        }
    }
    return all;
}

unsigned CatalogGpuVendor(const fs::path& catalog) {
    std::string text;
    try {
        text = ReadText(catalog / "gpu.txt");
    } catch (const Error&) {
        return 0;
    }
    unsigned vendor = 0;
    return std::sscanf(text.c_str(), "vendor %x", &vendor) == 1 ? vendor : 0;
}

bool HasGpuVendor(unsigned vendor) {
    char wanted[16];
    std::snprintf(wanted, sizeof(wanted), "VEN_%04X", vendor);
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if (Lower(Utf8(device.DeviceID)).find(Lower(wanted)) != std::string::npos) return true;
    }
    return false;
}

unsigned CacheGpuVendor(const fs::path& cache_folder) {
    fs::path driver_cache = cache_folder;
    driver_cache += ".vkcache";
    std::ifstream in(driver_cache, std::ios::binary);
    char header[12]{};
    if (!in.read(header, sizeof header) || std::memcmp(header, "BPBC", 4) != 0) return 0;
    std::uint32_t vendor = 0;
    std::memcpy(&vendor, header + 8, 4);
    return vendor;
}

void SeedGpuCache(const fs::path& cache, const fs::path& catalogs, const fs::path& game, const fs::path& eboot,
                  const fs::path& recompiler) {
    std::error_code error;
    if (Env("BB_SHADER_CATALOG", "1") == "0" || !fs::is_regular_file(recompiler, error)) return;
    std::map<fs::path, std::vector<fs::path>> by_game;
    for (const auto& entry : fs::directory_iterator(catalogs, error)) {
        std::error_code e;
        if (!entry.is_directory(e)) continue;
        if (fs::is_regular_file(entry.path() / "profile.bin", e)) {
            by_game[entry.path().filename()].push_back(entry.path());
            continue;
        }
        for (const auto& sub : fs::directory_iterator(entry.path(), e))
            if (sub.is_directory(e) && fs::is_regular_file(sub.path() / "profile.bin", e))
                by_game[sub.path().filename()].push_back(sub.path());
    }
    for (const auto& [game_name, candidates] : by_game) {
        std::error_code e;
        const fs::path target = cache / game_name;
        unsigned vendor = CacheGpuVendor(target);
        if (!vendor) {
            for (const unsigned preferred : {0x10DEu, 0x1002u, 0x8086u}) {
                if (HasGpuVendor(preferred)) {
                    vendor = preferred;
                    break;
                }
            }
        }
        const auto chosen = std::find_if(candidates.begin(), candidates.end(),
                                         [&](const fs::path& c) { return vendor && CatalogGpuVendor(c) == vendor; });
        if (chosen == candidates.end()) continue;
        const fs::path& catalog = *chosen;
        if (!fs::exists(target, e)) {
            fs::create_directories(target, e);
            fs::copy_file(catalog / "profile.bin", target / "profile.bin", e);
            Log("GPU cache %s: first start; the shader catalog's GPU profile used until the engine checks it",
                Utf8(game_name).c_str());
        }
        if (!fs::is_regular_file(target / "profile.bin", e)) continue;
        std::uint64_t files = 0, bytes = 0;
        for (const auto& file : fs::directory_iterator(catalog, e)) {
            ++files;
            bytes += file.file_size(e);
        }
        const fs::path marker = target / ("catalog-" + std::to_string(files) + "-" + std::to_string(bytes) + ".applied");
        if (fs::exists(marker, e)) continue;
        std::string output;
        const auto start = std::chrono::steady_clock::now();
        const int code = RunProgram(recompiler, {L"--seed-cache", catalog.native(), target.native(),
                                                 game.native(), eboot.native()}, &output);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto first = output.find("Shader catalog:");
        const auto last = output.rfind("Shader cache:");
        if (first != std::string::npos)
            Log("GPU cache %s: %s", Utf8(game_name).c_str(),
                Trim(output.substr(first + 16, output.find('\n', first) - first - 16)).c_str());
        if (code == 0 && last != std::string::npos)
            Log("GPU cache %s: %s (%.1f s)", Utf8(game_name).c_str(),
                Trim(output.substr(last + 14, output.find('\n', last) - last - 14)).c_str(), seconds);
        WriteText(marker, std::to_string(code) + "\n");
    }
}

void ResetStaleGpuCache(const fs::path& cache, const std::string& identity, const std::string& legacy,
                        const fs::path& recompiler) {
    const fs::path stamp = cache / "engine.txt";
    std::error_code error;
    try {
        const std::string stamped = Trim(ReadText(stamp));
        if (identity.empty() || stamped == identity) return;
        if (!legacy.empty() && stamped == legacy) {
            WriteText(stamp, identity + "\n");
            return;
        }
    } catch (const Error&) {
    }
    if (fs::is_directory(cache, error) && RecompileGpuCache(cache, recompiler)) {
        WriteText(stamp, identity + "\n");
        Log("GPU cache: the shader compiler changed (%s); recompiled before the start", identity.c_str());
        return;
    }
    bool removed = false;
    if (fs::is_directory(cache, error)) {
        for (const auto& entry : fs::directory_iterator(cache, error)) {
            if (entry.path() == stamp || Lower(Utf8(entry.path().extension())) == ".vkcache") continue;
            std::error_code e;
            if (entry.is_directory(e) && !entry.is_symlink(e)) fs::remove_all(entry.path(), e);
            else fs::remove(entry.path(), e);
            if (e) {
                Log("GPU cache: cannot remove %s: %s", Utf8(entry.path().filename()).c_str(), e.message().c_str());
                return;
            }
            removed = true;
        }
    }
    fs::create_directories(cache, error);
    WriteText(stamp, identity + "\n");
    if (removed)
        Log("GPU cache: cleared (%s: the shader compiler changed); shaders compile again on this start", identity.c_str());
}

std::string ConfigureFrameRate() {
    std::string fps = Env("BB_FPS", "60");
    if (fps == "120" || fps == "144") {
        SetEnv("BB_FPS", fps);
        SetEnv("BB_FPS_LIMIT", fps);
        SetEnv("BB_VBLANK_HZ", fps);
        return fps;
    }
    const bool digits = !fps.empty() && fps.find_first_not_of("0123456789") == std::string::npos && fps.size() < 6;
    if (fps == "uncap" || (digits && std::stoi(fps) > 90)) {
        std::string limit = digits ? fps : Env("BB_FPS_LIMIT", "240");
        if (limit.empty() || limit.find_first_not_of("0123456789") != std::string::npos) limit = "0";
        SetEnv("BB_FPS", "uncap");
        SetEnv("BB_FPS_LIMIT", limit);
        SetEnv("BB_VBLANK_HZ", "0");
        return "uncap";
    }
    if (fps != "30" && fps != "60" && fps != "90") {
        Log("Unsupported Windows FPS preset '%s'; using 60 FPS.", fps.c_str());
        fps = "60";
    }
    SetEnv("BB_FPS", fps);
    SetEnv("BB_FPS_LIMIT", fps);
    SetEnv("BB_VBLANK_HZ", fps == "90" ? "90" : "60");
    return fps;
}

std::optional<Size> DisplaySize() {
    SetProcessDPIAware();
    const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    if (w < 640 || h < 360) return std::nullopt;
    return Size{w - w % 2, h - h % 2};
}

int RunProbe(const fs::path& probe, const std::vector<std::wstring>& arguments, const fs::path& cwd) {
    HANDLE log = static_cast<HANDLE>(LogHandle());
    STARTUPINFOW startup{sizeof startup};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log ? log : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = log ? log : GetStdHandle(STD_ERROR_HANDLE);
    std::fflush(stdout);
    std::wstring line = CommandLine(probe, arguments);
    std::string shown;
    for (const auto& a : arguments) shown += " '" + Utf8(a) + "'";
    Log("Starting: '%s'%s", Utf8(probe).c_str(), shown.c_str());
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(probe.c_str(), line.data(), nullptr, nullptr, TRUE, 0, nullptr, cwd.c_str(), &startup, &process))
        throw Error("cannot start " + Utf8(probe));
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    Log("bb-probe exited with code %u", unsigned(code));
    return int(code);
}

bool CpuNeedsIsaRewrite() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return true;
    const bool movbe = (c & bit_MOVBE) != 0;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return true;
    const bool bmi1 = (b & bit_BMI) != 0;
    if (!__get_cpuid(0x80000001u, &a, &b, &c, &d)) return true;
    const bool lzcnt = (c & bit_LZCNT) != 0;
    return !(movbe && bmi1 && lzcnt);
}

void WaitForPreviousStart() {
    const auto previous = Env("BB_START_PID");
    SetEnv("BB_START_PID", std::to_string(GetCurrentProcessId()));
    if (!previous) return;
    const DWORD pid = DWORD(std::strtoul(previous->c_str(), nullptr, 10));
    if (!pid || pid == GetCurrentProcessId()) return;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return;
    wchar_t name[32768];
    DWORD size = DWORD(std::size(name));
    FILETIME theirs{}, ours{}, unused{};
    const bool older = GetProcessTimes(process, &theirs, &unused, &unused, &unused) &&
                       GetProcessTimes(GetCurrentProcess(), &ours, &unused, &unused, &unused) &&
                       CompareFileTime(&theirs, &ours) < 0;
    const bool same = older && QueryFullProcessImageNameW(process, 0, name, &size) &&
                      _wcsicmp(fs::path(std::wstring(name, size)).filename().c_str(), SelfPath().filename().c_str()) == 0;
    if (same) {
        const auto start = std::chrono::steady_clock::now();
        const DWORD result = WaitForSingleObject(process, 60000);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        Log(result == WAIT_OBJECT_0 ? "Restart: the previous start ended (waited %.1f s)"
                                    : "Restart: the previous start is still running after %.1f s; starting anyway",
            seconds);
    }
    CloseHandle(process);
}

int Main(std::vector<std::wstring> arguments) {
    WaitForPreviousStart();
    const Folders folders = Locate();
    std::optional<std::string> game = Env("BB_GAME_DIR");
    if (arguments.size() > 1 && arguments[0] == L"--game-dir") {
        game = Utf8(arguments[1]);
        arguments.erase(arguments.begin(), arguments.begin() + 2);
    }
    const fs::path data = Path(Env("BB_DATA_DIR", Utf8(folders.package)));
    const fs::path& out = folders.out;
    if (Env("BB_GPU_USER_DIR", "").empty()) SetEnv("BB_GPU_USER_DIR", Utf8(folders.package / "user"));
    {
        std::error_code error;
        fs::create_directories(Path(Env("BB_GPU_USER_DIR", "")), error);
    }
    try {
        const auto package = nlohmann::json::parse(ReadText(folders.package / "package.json"));
        const std::string engine = package.value("engine", "");
        std::string identity = engine, legacy;
        try {
            const std::string id = Trim(ReadText(out / "gpu-cache.id"));
            if (!id.empty()) identity = "gpu " + id;
            const std::string old = Trim(ReadText(out / "gpu-cache.legacy.id"));
            if (!old.empty()) legacy = "gpu " + old;
        } catch (const Error&) {
        }
        ResetStaleGpuCache(Path(Env("BB_GPU_USER_DIR", "")) / "cache", identity, legacy, out / "bb-shader-lab.exe");
    } catch (const std::exception&) {
    }
    fs::create_directories(out);
    if (Lower(Utf8(folders.package)).find("onedrive") != std::string::npos)
        Log("Performance: this Lantern Engine folder is synced by OneDrive (%s); OneDrive uploads its logs and "
            "shader cache while you play. A folder outside OneDrive (C:\\Games, for example) avoids that",
            Utf8(folders.package).c_str());
    {
        std::error_code ec;
        const fs::path ini = data / "lantern.ini", old = data / "bbport.ini";
        const fs::path shipped = folders.engine / "lantern.default.ini";
        if (!fs::exists(ini, ec)) {
            if (fs::exists(old, ec)) fs::copy_file(old, ini, ec);
            else if (fs::exists(shipped, ec)) fs::copy_file(shipped, ini, ec);
        } else if (fs::exists(old, ec) && fs::exists(shipped, ec)) {
            const auto current = ReadSettings(ini);
            if (current == ReadSettings(shipped) && current != ReadSettings(old)) {
                fs::copy_file(old, ini, fs::copy_options::overwrite_existing, ec);
                if (!ec) Log("Settings: lantern.ini restored from bbport.ini (an update had put the shipped settings in it)");
            }
        }
    }
    if (!Env("BB_CONFIG")) SetEnv("BB_CONFIG", Utf8(data / "lantern.ini"));
    const fs::path config = Path(*Env("BB_CONFIG"));
    const auto settings = ReadSettings(config);
    try {
        Log(ConfigureOnline(settings, data) ? "Online: configured server profile and automatic game redirect"
                                            : "Online: offline mode");
    } catch (const Error& error) {
        Log("Online configuration: %s", error.what());
        return 1;
    }
    std::error_code error;
    if (!Env("BB_FSR411_DIR") && !fs::is_directory(folders.engine / "fsr4_411", error) && fs::is_directory(data / "fsr4_411", error))
        SetEnv("BB_FSR411_DIR", Utf8(data / "fsr4_411"));
    if (!Env("BB_FSR4VK_DIR") && fs::is_regular_file(folders.engine / "fsr4vk" / "amd_fidelityfx_upscaler_vk.dll", error))
        SetEnv("BB_FSR4VK_DIR", Utf8(folders.engine / "fsr4vk"));
    if (!Env("BB_FSR_SDK_DIR") && fs::is_regular_file(folders.engine / "fsr_sdk" / "amd_fidelityfx_loader_vk.dll", error))
        SetEnv("BB_FSR_SDK_DIR", Utf8(folders.engine / "fsr_sdk"));
    const fs::path remembered = out / "game_dir.txt";
    if ((!game || game->empty()) && fs::is_regular_file(remembered, error)) game = Trim(ReadText(remembered));
    const fs::path game_dir = game && !game->empty() ? Path(*game) : folders.package.parent_path() / "CUSA03173";
    if (!fs::is_regular_file(game_dir / "eboot.bin", error)) {
        Log("No eboot.bin in %s (pass --game-dir or set BB_GAME_DIR).", Utf8(game_dir).c_str());
        return 1;
    }
    const fs::path original = fs::canonical(game_dir);
    WriteText(remembered, Utf8(original));
    SetEnv("BB_GAME_DIR", Utf8(original));
    {
        std::vector<std::wstring> all(arguments);
        all.insert(all.begin(), {L"--game-dir", original.native()});
        SetEnv("BB_RESTART_COMMAND", Utf8(CommandLine(SelfPath(), all)));
    }

    fs::path merged;
    try {
        const bool own_font = fs::is_regular_file(original / "dvdroot_ps4" / "font" / "DbgFont14h.ccm", error) &&
                              fs::is_regular_file(original / "dvdroot_ps4" / "font" / "DbgFont14h.tpf", error);
        const bool debug_font = Setting(settings, "debug_menu") == "1" && !own_font && fs::is_directory(folders.engine / "debug-font", error);
        std::vector<fs::path> first_layers;
        if (debug_font) first_layers.push_back(folders.engine / "debug-font");
        bool lamp2lamp = false, lamp2lamp_bosses = false;
        try {
            const auto chosen = nlohmann::json::parse(ReadText(Path(Env("BB_PATCHES_CONFIG", Utf8(data / "patches.json")))));
            if (chosen.contains("enabled"))
                for (const auto& key : chosen.at("enabled")) {
                    lamp2lamp = lamp2lamp || key == "Bloodborne.xml/Lamp2Lamp (warp between lamps)";
                    lamp2lamp_bosses = lamp2lamp_bosses || key == "Bloodborne.xml/Lamp2Lamp: warp to bosses";
                }
        } catch (const std::exception&) {
        }
        if (lamp2lamp || lamp2lamp_bosses) {
            try {
                const auto update = GameUpdateFolder(original);
                const fs::path core = PrepareLamp2Lamp(original, update, folders.engine / "lamp2lamp.bin", data / "engine-assets");
                const fs::path bosses = lamp2lamp_bosses ? PrepareLamp2Lamp(original, update, folders.engine / "lamp2lamp-bosses.bin", data / "engine-assets") : fs::path();
                first_layers.push_back(core);
                if (lamp2lamp_bosses) first_layers.push_back(bosses);
                Log("Engine: Lamp2Lamp on%s", lamp2lamp_bosses ? ", with warps to bosses" : "");
            } catch (const std::exception& e) {
                Log("Engine: Lamp2Lamp off: %s", e.what());
            }
        }
        merged = BuildModsOverlay(original, out, Path(Env("BB_MODS_DIR", Utf8(data / "mods"))),
                                  Path(Env("BB_MODS_CONFIG", Utf8(data / "mods.json"))), Env("BB_MODS_ENABLED", "1") == "1",
                                  first_layers);
    } catch (const std::exception& e) {
        Log("Mods: %s", e.what());
        return 1;
    }
    std::optional<fs::path> overlay;
    if (fs::canonical(merged) != original) overlay = merged;
    struct Cleanup {
        std::optional<fs::path>& overlay;
        ~Cleanup() {
            if (overlay) try {
                    RemoveOverlay(*overlay);
                } catch (...) {
                }
        }
    } cleanup{overlay};
    const char* const core[] = {"dvdroot_ps4/shader/gxshader.shaderbnd.dcx", "dvdroot_ps4/other/systex.tpf.dcx"};
    std::string missing;
    for (const char* name : core)
        if (!fs::is_regular_file(merged / Path(name), error)) missing += (missing.empty() ? "" : ", ") + std::string(name);
    if (!missing.empty() && overlay &&
        std::all_of(std::begin(core), std::end(core), [&](const char* n) { return fs::is_regular_file(original / Path(n), error); })) {
        Log("Mods: the merged game lost its own files; starting without mods");
        RemoveOverlay(*overlay);
        overlay.reset();
        merged = original;
        missing.clear();
    }
    if (!missing.empty()) {
        Log("The game folder %s is incomplete: %s missing. Dump or extract the game again (base game plus the 1.09 update).",
            Utf8(original).c_str(), missing.c_str());
        return 1;
    }
    SetEnv("BB_GAME_MERGED", Utf8(merged));

    try {
        PrepareImage(merged, out, folders.engine);
        if (CpuNeedsIsaRewrite()) {
            std::string isa_scan_output;
            const int isa_scan_status = RunProgram(out / "bb-isa-scan.exe",
                                                   {(out / "boot-linked.bin").native(), out.native()},
                                                   &isa_scan_output);
            if (isa_scan_status != 0)
                throw Error("ISA scan failed (exit " + std::to_string(isa_scan_status) + "): " + Trim(isa_scan_output));
            if (!Trim(isa_scan_output).empty()) Log("%s", Trim(isa_scan_output).c_str());
        }
    } catch (const GameCheckError& e) {
        Log("\nUnsupported game files: %s\nSet BB_SKIP_GAME_CHECK=1 to start anyway.", e.what());
        return 2;
    } catch (const std::exception& e) {
        Log("prepare failed: %s", e.what());
        return 1;
    }
    for (const char* key : {"BB_DREAM_MIRROR_ASSET", "BB_ENGINE_IMAGE_SHA256", "BB_PC_MENU_ASSETS", "BB_REBIRTH_ASSET"})
        UnsetEnv(key);
    bool other_edition = false;
    try {
        (void)VerifyExecutable(out / "eboot.elf");
    } catch (const std::exception&) {
        std::string code;
        try {
            code = Trim(ReadText(out / "game-code.txt"));
        } catch (const std::exception&) {
        }
        std::string title;
        try {
            title = SfoText(ParseSfo(ReadFile(merged / "sce_sys" / "param.sfo")), "TITLE_ID");
        } catch (const std::exception&) {
        }
        other_edition = code == "unchecked" ? title != "CUSA03173" : code != "1.09";
    }
    if (other_edition) SetEnv("BB_OTHER_EDITION", "1");
    else UnsetEnv("BB_OTHER_EDITION");
    const auto is_on = [&](const char* key) { return Setting(settings, key) == "1"; };
    try {
        SetEnv("BB_ENGINE_IMAGE_SHA256", VerifyExecutable(out / "eboot.elf"));
        if (is_on("change_appearance")) {
            try {
                const fs::path asset = PrepareMirror(merged, out / "eboot.elf", data / "engine-assets");
                SetEnv("BB_DREAM_MIRROR_ASSET", Utf8(asset));
                Log("Engine: Dream mirror prepared locally: %s", Utf8(asset).c_str());
            } catch (const std::exception& e) {
                Log("Engine: Dream mirror skipped: %s", e.what());
            }
        }
        if (Setting(settings, "ui_4k") != "0") {
            std::error_code ui_error;
            const fs::path ui = data / "engine-assets" / "ui-4k";
            if (fs::is_regular_file(ui / "complete", ui_error)) {
                SetEnv("BB_UI_4K", Utf8(ui));
                Log("Engine: 4K menu textures from %s", Utf8(ui).c_str());
            }
        }
        if (is_on("change_appearance") || is_on("camera_controls") || is_on("rebirth") || is_on("graphics_controls") ||
            Setting(settings, "pc_controls") != "0" || Setting(settings, "widescreen") != "0") {
            try {
                const fs::path menus = PrepareMenuAssets(merged, out / "eboot.elf", data / "engine-assets",
                                                         out / "bb-engine-assets.exe", is_on("rebirth"),
                                                         PlateTitle(settings, "plate_host_title"),
                                                         PlateTitle(settings, "plate_guest_title"));
                SetEnv("BB_PC_MENU_ASSETS", Utf8(menus));
                Log("Engine: native PC menus prepared locally: %s", Utf8(menus).c_str());
                if (is_on("rebirth"))
                    SetEnv("BB_REBIRTH_ASSET", Utf8(menus / Path("dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx")));
            } catch (const std::exception& e) {
                Log("Engine: native PC menus disabled: %s", e.what());
            }
        }
    } catch (const std::exception& e) {
        Log("Engine: enhancement preparation skipped: %s", e.what());
    }
    UnsetEnv("BB_FILE_FIXES");
    if (Env("BB_EFFECT_FIXES", "") != "0") {
        try {
            std::vector<std::string> notes;
            const auto fixed = PrepareEffectFixes(merged, data / "engine-assets", notes);
            for (const std::string& note : notes) Log("Engine: Ebrietas effect fix: %s", note.c_str());
            if (!fixed.empty()) {
                std::string list;
                for (const auto& [relative, file] : fixed) list += relative + "\t" + Utf8(file) + "\n";
                WriteText(out / "file-fixes.txt", list);
                SetEnv("BB_FILE_FIXES", Utf8(out / "file-fixes.txt"));
                Log("Engine: Ebrietas' trail effect replaced as Nexus mod 41 does, in %zu effect files "
                    "(BB_EFFECT_FIXES=0 keeps the game's)", fixed.size());
            }
        } catch (const std::exception& e) {
            Log("Engine: Ebrietas effect fix skipped: %s", e.what());
        }
    }
    if (!Env("BB_DISPLAY_SIZE") || Env("BB_DISPLAY_SIZE", "").empty()) {
        if (const auto size = DisplaySize()) {
            SetEnv("BB_DISPLAY_SIZE", std::to_string(size->w) + "x" + std::to_string(size->h));
            if (Lower(Setting(settings, "output_res").value_or("")) == "auto")
                Log("Output resolution: the display's own, %dx%d", size->w, size->h);
        }
    }
    if (Env("BB_AUTO_RENDER_RES", "") == "1")
        for (const char* key : {"BB_RENDER_RES", "BB_OUTPUT_RES", "BB_AUTO_RENDER_RES"}) UnsetEnv(key);
    const std::string fps = ConfigureFrameRate();
    std::optional<std::pair<Size, Size>> scaled;
    if (Env("BB_RENDER_RES", "").empty() && !other_edition) {
        try {
            scaled = ScaledSizes(ReadSettings(config));
        } catch (const Error&) {
        }
    }
    std::string live = "0";
    if (scaled) {
        live = Env("BB_LIVE_RES", "");
        if (live.empty()) live = SettingValue(config, "live_resolution").value_or("0");
        if (live.empty()) live = "0";
        if (live == "auto") {
            std::string answer;
            try {
                RunProgram(out / "bb-gpu-capabilities.exe", {L"--live-resolution"}, &answer);
            } catch (const Error&) {
            }
            live = Trim(answer).empty() ? "0" : Trim(answer);
        }
        live = live == "1" ? "1" : "0";
    }
    const auto size_text = [](Size s) { return std::to_string(s.w) + "x" + std::to_string(s.h); };
    if (live == "1") {
        Log("Output %s: live resolution changes (live_resolution=0: startup patch)", size_text(scaled->second).c_str());
    } else if (scaled) {
        std::string early = Env("BB_UPSCALE_EARLY", "");
        if (early.empty()) early = SettingValue(config, "upscale_before_post").value_or("0");
        if (early == "1" && scaled->second.w % 4 == 0 && scaled->second.h % 4 == 0) {
            scaled->first = scaled->second;
            SetEnv("BB_UPSCALE_EARLY", "1");
        } else {
            UnsetEnv("BB_UPSCALE_EARLY");
        }
        SetEnv("BB_RENDER_RES", size_text(scaled->first));
        SetEnv("BB_OUTPUT_RES", size_text(scaled->second));
        SetEnv("BB_AUTO_RENDER_RES", "1");
        if (std::int64_t(scaled->first.w) * scaled->first.h > 1920ll * 1080 && !Env("BB_DMEM_MB")) {
            if (const unsigned long error = PoolRefused(9152))
                Log("Memory: Windows refused the 9152 MiB pool a scene above 1080p uses (error %lu); 5056 MiB, "
                    "some models may not show at this resolution", error);
            else
                SetEnv("BB_DMEM_MB", "9152");
        }
        Log("Output %s: scene %s, direct memory %s MiB (live_resolution=1: live changes)", size_text(scaled->second).c_str(),
            size_text(scaled->first).c_str(), Env("BB_DMEM_MB", "5056").c_str());
    }
    PatchResult patches;
    try {
        patches = CompilePatches(PatchRequest{
            .out = out,
            .xml = folders.engine / "patches" / "Bloodborne.xml",
            .settings = config,
            .game = merged,
            .patches_dir = Path(Env("BB_PATCHES_DIR", Utf8(data / "patches"))),
            .patches_config = Path(Env("BB_PATCHES_CONFIG", Utf8(data / "patches.json"))),
            .fps = fps,
            .extra = Env("BB_PATCHES", ""),
            .render_res = Env("BB_RENDER_RES", ""),
            .output_res = Env("BB_OUTPUT_RES", ""),
        });
    } catch (const std::exception& e) {
        Log("Patches: %s", e.what());
        return 1;
    }
    if (!(patches.ui == Size{1920, 1080})) {
        SetEnv("BB_UI_SIZE", size_text(patches.ui));
        Log("Widescreen: UI viewport %s (the 16:9 stage centred in it)", size_text(patches.ui).c_str());
    } else {
        UnsetEnv("BB_UI_SIZE");
    }
    UnsetEnv("BB_WIDE_UI_MOVIES");
    UnsetEnv("BB_WIDE_OPTION_MOVIE");
    // Widescreen on a screen of another shape: the menus' full-screen dims, fades, pictures (but the
    // title screen's) and bands (subtitles, area names) are drawn for the 16:9 stage and stopped at
    // its edges; widened copies of their movies are mounted over the game's (src/sys/file.c,
    // BB_WIDE_UI_MOVIES). The PC menus' optionsetting.gfx is widened too, and mounted by the System
    // pages themselves (BB_WIDE_OPTION_MOVIE, engine/option_menu.cpp). A 16:9 viewport (Widescreen
    // off, or a 16:9 screen) changes nothing.
    const bool wide_ui = std::int64_t(patches.ui.w) * 1080 != std::int64_t(patches.ui.h) * 1920;
    if (wide_ui) {
        try {
            std::optional<fs::path> option_movie;
            if (const auto menus = Env("BB_PC_MENU_ASSETS"); menus && !menus->empty()) {
                const fs::path prepared = Path(*menus) / "dvdroot_ps4" / "menu" / "optionsetting.gfx";
                if (fs::is_regular_file(prepared)) option_movie = prepared;
            }
            const auto movies = PrepareWideMovies(merged, option_movie, data / "engine-assets",
                                                  out / "bb-engine-assets.exe", patches.ui.w, patches.ui.h);
            std::string list, names;
            for (const auto& m : movies) {
                if (option_movie && m.relative == "dvdroot_ps4/menu/optionsetting.gfx")
                    SetEnv("BB_WIDE_OPTION_MOVIE", Utf8(m.file));
                else
                    list += m.relative + "\t" + Utf8(m.file) + "\n";
                names += (names.empty() ? "" : ", ") + Utf8(Path(m.relative).stem());
                if (Env("BB_UI_VIEWPORT_LOG", "") == "1")
                    Log("Widescreen: %s: %s", m.relative.c_str(), m.note.c_str());
            }
            if (!list.empty()) {
                WriteText(out / "wide-ui-movies.txt", list);
                SetEnv("BB_WIDE_UI_MOVIES", Utf8(out / "wide-ui-movies.txt"));
            }
            Log("Widescreen: %zu menu movies' full-screen dims, fades and bands widened for %s: %s",
                movies.size(), size_text(patches.ui).c_str(), names.c_str());
        } catch (const std::exception& e) {
            Log("Widescreen: the menus' full-screen dims and fades stay 16:9: %s", e.what());
        }
    }
    if (patches.heap_patch && !Env("BB_DMEM_MB")) {
        SetEnv("BB_DMEM_MB", "9152");
        Log("Patches: the game's heaps are enlarged; direct memory 9152 MiB");
    }
    if (const auto dmem = Env("BB_DMEM_MB"); dmem && *dmem == "9152") {
        if (const unsigned long error = PoolRefused(9152))
            Log("Memory: Windows refused the 9152 MiB pool before the start (error %lu)", error);
        PoolHandOff(9152);
    } else if (!dmem) {
        if (const unsigned long error = PoolRefused(5056))
            Log("Memory: Windows refused the 5056 MiB pool before the start (error %lu)", error);
        PoolHandOff(5056);
    } else {
        PoolHandOff(0);
    }
    try {
        SeedGpuCache(Path(Env("BB_GPU_USER_DIR", "")) / "cache", folders.engine / "shader-catalog", merged,
                     out / "eboot.elf", out / "bb-shader-lab.exe");
    } catch (const std::exception&) {
    }
    const fs::path probe = out / "bb-probe.exe";
    std::vector<std::wstring> command{(out / "boot-linked.bin").native(), L"--content-profile", (out / "content.bin").native(),
                                      L"--patches", (out / "patches.bin").native(), L"--app0", merged.native(), L"--user",
                                      Path(Env("BB_USER_DIR", Utf8(data / "user"))).native(), L"--timeout",
                                      Wide(Env("BB_TIMEOUT", "0"))};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return RunProbe(probe, command, folders.engine);
}

}
}

int wmain(int argc, wchar_t** argv) {
    start::InitLog();
    std::vector<std::wstring> arguments(argv + 1, argv + argc);
    try {
        if (arguments.size() == 4 && arguments[0] == L"--test-prepare") {
            start::PrepareImage(arguments[1], arguments[2], arguments[3]);
            return 0;
        }
        if (arguments.size() == 5 && arguments[0] == L"--test-mods") {
            const fs::path overlay = start::BuildModsOverlay(arguments[1], arguments[2], arguments[3], arguments[4], true);
            std::printf("%s\n", start::Utf8(overlay).c_str());
            return 0;
        }
        if (arguments.size() == 10 && arguments[0] == L"--test-patches") {
            const auto result = start::CompilePatches(start::PatchRequest{
                .out = arguments[1],
                .xml = fs::path(arguments[2]) / "patches" / "Bloodborne.xml",
                .settings = arguments[3],
                .game = arguments[4],
                .patches_dir = arguments[8],
                .patches_config = arguments[9],
                .fps = start::Utf8(arguments[5]),
                .extra = start::Env("BB_PATCHES", ""),
                .render_res = start::Utf8(arguments[6]),
                .output_res = start::Utf8(arguments[7]),
            });
            start::Log("heap_patch=%d ui=%dx%d", int(result.heap_patch), result.ui.w, result.ui.h);
            return 0;
        }
        return start::Main(std::move(arguments));
    } catch (const std::exception& e) {
        start::Log("bb-start: %s", e.what());
        return 1;
    }
}
