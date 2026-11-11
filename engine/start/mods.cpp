// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods.h"
#include "elf.h"

#include <windows.h>
#include <winioctl.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <random>
#include <set>

namespace start {
namespace {

const std::set<std::wstring> kGameFolders{L"action", L"chr", L"event", L"facegen", L"font", L"map", L"menu",
                                          L"movie", L"msg", L"mtd", L"obj", L"other", L"param", L"paramdef",
                                          L"parts", L"remo", L"script", L"sfx", L"shader", L"sound"};
const wchar_t* const kSerials[] = {L"CUSA00207", L"CUSA00208", L"CUSA00299", L"CUSA00900", L"CUSA01363",
                                   L"CUSA03014", L"CUSA03023", L"CUSA03173", L"CUSA03179"};

std::wstring Fold(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), DWORD(text.size()));
    return text;
}

bool IsDir(const fs::path& p) {
    std::error_code e;
    return fs::is_directory(p, e);
}
bool IsFile(const fs::path& p) {
    std::error_code e;
    return fs::is_regular_file(p, e);
}
bool Exists(const fs::path& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsLink(const fs::path& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

std::vector<fs::path> Entries(const fs::path& folder) {
    std::vector<fs::path> out;
    std::error_code e;
    if (!IsDir(folder)) return out;
    for (const auto& entry : fs::directory_iterator(folder, e)) out.push_back(entry.path());
    return out;
}

fs::path Child(const fs::path& folder, const std::wstring& name) {
    const std::wstring want = Fold(name);
    for (const fs::path& entry : Entries(folder))
        if (Fold(entry.filename().native()) == want) return entry;
    return folder / name;
}

std::optional<std::pair<fs::path, fs::path>> ContentRoot(const fs::path& folder) {
    if (!IsDir(folder)) return std::nullopt;
    std::vector<std::wstring> wrappers{L"", L"app0"};
    for (const wchar_t* s : kSerials) wrappers.push_back(s);
    for (const std::wstring& wrapper : wrappers) {
        const fs::path base = wrapper.empty() ? folder : Child(folder, wrapper);
        if (IsDir(Child(base, L"dvdroot_ps4"))) return std::pair{base, fs::path()};
    }
    std::vector<fs::path> entries, folders;
    for (const fs::path& e : Entries(folder))
        if (!StartsWith(Utf8(e.filename()), ".")) entries.push_back(e);
    for (const fs::path& e : entries)
        if (IsDir(e)) folders.push_back(e);
    if (!folders.empty() && std::all_of(folders.begin(), folders.end(), [](const fs::path& f) {
            return kGameFolders.count(Fold(f.filename().native()));
        }))
        return std::pair{folder, fs::path(L"dvdroot_ps4")};
    if (folders.size() == 1 && std::none_of(entries.begin(), entries.end(), [](const fs::path& e) {
            if (!IsFile(e)) return false;
            const std::wstring ext = Fold(e.extension().native());
            return ext != L".txt" && ext != L".md" && ext != L".jpg" && ext != L".png" && ext != L".ini" && ext != L".toml";
        }))
        return ContentRoot(folders[0]);
    return std::nullopt;
}

std::vector<std::wstring> Discover(const fs::path& root) {
    std::vector<std::wstring> names;
    for (const fs::path& p : Entries(root))
        if (IsDir(p) && ContentRoot(p)) names.push_back(p.filename().native());
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        const std::wstring fa = Fold(a), fb = Fold(b);
        return fa != fb ? fa < fb : a < b;
    });
    return names;
}

std::vector<std::wstring> Selected(const fs::path& root, const fs::path& config) {
    const std::vector<std::wstring> available = Discover(root);
    if (config.empty() || !IsFile(config)) return available;
    const auto settings = nlohmann::json::parse(ReadText(config));
    std::set<std::wstring> disabled;
    std::vector<std::wstring> order;
    if (settings.contains("disabled")) {
        if (!settings["disabled"].is_array()) throw Error("Disabled mods must be a list of folder names");
        for (const auto& n : settings["disabled"]) {
            if (!n.is_string()) throw Error("Disabled mods must be a list of folder names");
            disabled.insert(Wide(n.get<std::string>()));
        }
    }
    if (settings.contains("order")) {
        if (!settings["order"].is_array()) throw Error("Mod order must be a list of folder names");
        for (const auto& n : settings["order"]) {
            if (!n.is_string()) throw Error("Mod order must be a list of folder names");
            order.push_back(Wide(n.get<std::string>()));
        }
    }
    std::vector<std::wstring> out;
    order.insert(order.end(), available.begin(), available.end());
    for (const std::wstring& n : order)
        if (std::find(available.begin(), available.end(), n) != available.end() && !disabled.count(n) &&
            std::find(out.begin(), out.end(), n) == out.end())
            out.push_back(n);
    return out;
}

void Walk(const fs::path& directory, const fs::path& root, std::vector<std::pair<fs::path, fs::path>>& files,
          bool skip_links) {
    std::vector<fs::path> folders, names;
    for (const fs::path& entry : Entries(directory)) {
        if (IsDir(entry)) {
            if (IsLink(entry)) {
                if (skip_links) Log("Mods: skipped the link %s (links are not followed)", Utf8(entry).c_str());
                continue;
            }
            folders.push_back(entry);
        } else {
            names.push_back(entry);
        }
    }
    std::sort(names.begin(), names.end());
    for (const fs::path& source : names) {
        if (IsLink(source)) {
            if (skip_links) Log("Mods: skipped the link %s (links are not followed)", Utf8(source).c_str());
            continue;
        }
        files.emplace_back(source.lexically_relative(root), source);
    }
    for (const fs::path& folder : folders) Walk(folder, root, files, skip_links);
}

std::vector<std::pair<fs::path, fs::path>> ModFiles(const fs::path& folder) {
    const auto layout = ContentRoot(folder);
    if (!layout) throw Error(Utf8(folder) + ": expected dvdroot_ps4 (or chr/, parts/, ...) inside the mod folder");
    const fs::path root = fs::canonical(layout->first);
    std::vector<std::pair<fs::path, fs::path>> found, out;
    Walk(root, root, found, true);
    for (auto& [relative, source] : found) {
        const fs::path game_path = layout->second.empty() ? relative : layout->second / relative;
        const std::wstring first = Fold(game_path.begin()->native());
        if (first != L"dvdroot_ps4") {
            if (first == L"eboot.bin" || first == L"sce_module" || first == L"sce_sys")
                throw Error("Executable/system replacement is unsupported: " + Utf8(source));
            continue;
        }
        if (!IsFile(source)) throw Error("Not a regular mod file: " + Utf8(source));
        out.emplace_back(game_path, source);
    }
    return out;
}

std::optional<std::string> AppVersion(const fs::path& folder) {
    try {
        return SfoText(ParseSfo(ReadFile(folder / "sce_sys" / "param.sfo")), "APP_VER", "");
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<fs::path> UpdateFolder(const fs::path& game) {
    const std::wstring update = Fold(game.filename().native() + L"-update"),
                       patch = Fold(game.filename().native() + L"-patch");
    for (const fs::path& entry : Entries(game.parent_path())) {
        const std::wstring name = Fold(entry.filename().native());
        if (!IsDir(entry) || (name != update && name != patch) || !IsFile(Child(entry, L"eboot.bin"))) continue;
        const auto version = AppVersion(entry);
        if (version && !version->empty() && AppVersion(game) == version) {
            Log("Game update: %s is already in the game (%s)", Utf8(entry.filename()).c_str(), version->c_str());
            return std::nullopt;
        }
        return entry;
    }
    return std::nullopt;
}

void CreateJunction(const fs::path& target, const fs::path& link) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) throw Error("cannot create " + Utf8(link));
    const std::wstring substitute = L"\\??\\" + fs::absolute(target).native();
    const std::wstring print = fs::absolute(target).native();
    const std::size_t path_bytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    std::vector<std::uint8_t> buffer(8 + 8 + path_bytes, 0);
    Store<std::uint32_t>(buffer.data(), IO_REPARSE_TAG_MOUNT_POINT);
    Store<std::uint16_t>(buffer.data() + 4, std::uint16_t(8 + path_bytes));
    Store<std::uint16_t>(buffer.data() + 8, 0);
    Store<std::uint16_t>(buffer.data() + 10, std::uint16_t(substitute.size() * sizeof(wchar_t)));
    Store<std::uint16_t>(buffer.data() + 12, std::uint16_t((substitute.size() + 1) * sizeof(wchar_t)));
    Store<std::uint16_t>(buffer.data() + 14, std::uint16_t(print.size() * sizeof(wchar_t)));
    std::memcpy(buffer.data() + 16, substitute.c_str(), substitute.size() * sizeof(wchar_t));
    std::memcpy(buffer.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(), print.size() * sizeof(wchar_t));
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    DWORD returned = 0;
    const bool ok = handle != INVALID_HANDLE_VALUE &&
                    DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr, 0,
                                    &returned, nullptr);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (!ok) {
        RemoveDirectoryW(link.c_str());
        throw Error("cannot link " + Utf8(link) + " to " + Utf8(target));
    }
}

bool JunctionsWork(const fs::path& folder) {
    const fs::path probe = folder / L"mod-junction-test";
    RemoveDirectoryW(probe.c_str());
    try {
        CreateJunction(folder, probe);
    } catch (const Error&) {
        return false;
    }
    RemoveDirectoryW(probe.c_str());
    return true;
}

void Link(const fs::path& path, const fs::path& target) {
    if (IsDir(target)) {
        CreateJunction(target, path);
    } else if (!CreateHardLinkW(path.c_str(), target.c_str(), nullptr)) {
        if (!CopyFileW(target.c_str(), path.c_str(), TRUE)) throw Error("cannot link " + Utf8(path));
    }
}

void Unlink(const fs::path& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (!RemoveDirectoryW(path.c_str())) throw Error("cannot remove " + Utf8(path));
        return;
    }
    if (!DeleteFileW(path.c_str())) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(path.c_str())) throw Error("cannot remove " + Utf8(path));
    }
}

fs::path Resolved(const fs::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw Error("cannot resolve " + Utf8(path));
    wchar_t buffer[32768];
    const DWORD n = GetFinalPathNameByHandleW(handle, buffer, DWORD(std::size(buffer)), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    std::wstring text(buffer, n);
    if (text.rfind(L"\\\\?\\UNC\\", 0) == 0) text = L"\\\\" + text.substr(8);
    else if (text.rfind(L"\\\\?\\", 0) == 0) text = text.substr(4);
    return fs::path(text);
}

void Expand(const fs::path& directory) {
    if (IsLink(directory)) {
        const fs::path target = Resolved(directory);
        if (!IsDir(target)) throw Error("File/directory conflict at " + Utf8(directory.filename()));
        Unlink(directory);
        fs::create_directory(directory);
        for (const fs::path& entry : Entries(target)) Link(directory / entry.filename(), entry);
    } else if (Exists(directory) && !IsDir(directory)) {
        throw Error("File/directory conflict at " + Utf8(directory.filename()));
    } else {
        std::error_code e;
        fs::create_directory(directory, e);
    }
}

fs::path OverlayBase(const fs::path& game, const fs::path& out) {
    const std::wstring out_drive = Fold(fs::absolute(out).root_name().native());
    const std::wstring game_drive = Fold(game.root_name().native());
    if (out_drive != game_drive) return game.parent_path() / L".bbport-overlays";
    return out;
}

void RemoveStaleOverlays(const fs::path& out) {
    for (const fs::path& entry : Entries(out)) {
        if (!StartsWith(Utf8(entry.filename()), "mod-game-") || !IsDir(entry) || IsLink(entry)) continue;
        try {
            RemoveOverlay(entry);
        } catch (const Error&) {
        }
    }
}

fs::path MakeTemporary(const fs::path& out) {
    std::random_device random;
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string name = "mod-game-";
        for (int i = 0; i < 8; ++i) name += letters[random() % (sizeof letters - 1)];
        const fs::path path = out / Path(name);
        if (CreateDirectoryW(path.c_str(), nullptr)) return path;
    }
    throw Error("cannot create a mod overlay in " + Utf8(out));
}

bool ChangesGameplay(const std::wstring& key) {
    for (const wchar_t* folder : {L"dvdroot_ps4/param/", L"dvdroot_ps4/event/", L"dvdroot_ps4/script/",
                                  L"dvdroot_ps4/map/mapstudio/", L"dvdroot_ps4/action/"})
        if (key.rfind(folder, 0) == 0) return true;
    return key.rfind(L"dvdroot_ps4/chr/", 0) == 0 &&
           (key.find(L".anibnd") != std::wstring::npos || key.find(L".behbnd") != std::wstring::npos);
}

bool ChangesModels(const std::wstring& key) {
    bool model_folder = false;
    for (const wchar_t* folder : {L"dvdroot_ps4/map/", L"dvdroot_ps4/chr/", L"dvdroot_ps4/parts/", L"dvdroot_ps4/obj/"})
        model_folder |= key.rfind(folder, 0) == 0;
    if (!model_folder || key.rfind(L"dvdroot_ps4/map/mapstudio/", 0) == 0) return false;
    for (const wchar_t* type : {L".flver", L".partsbnd", L".chrbnd", L".objbnd", L".tpf", L".tpfbhd", L".tpfbdt", L".mapbnd"})
        if (key.find(type) != std::wstring::npos) return true;
    return false;
}

fs::path BuildOverlay(fs::path game, const fs::path& out_base, const std::vector<std::pair<std::wstring, fs::path>>& mods,
                      const std::optional<fs::path>& update) {
    game = fs::canonical(game);
    fs::path out = OverlayBase(game, out_base);
    if (IsDir(out)) RemoveStaleOverlays(out);
    std::vector<std::pair<fs::path, fs::path>> replacements;
    std::map<std::wstring, std::wstring> owners;
    if (update) {
        std::vector<std::pair<fs::path, fs::path>> files;
        const fs::path root = fs::canonical(*update);
        Walk(root, root, files, false);
        for (const auto& [relative, source] : files) {
            owners[Fold(relative.generic_wstring())] = update->filename().native();
            replacements.emplace_back(relative, source);
        }
        Log("Game update: %s: %zu files over the game", Utf8(update->filename()).c_str(), files.size());
    }
    std::size_t gameplay = 0, models = 0;
    UnsetEnv("BB_MOD_MODELS");
    for (const auto& [name, root] : mods) {
        std::size_t count = 0;
        for (const auto& [relative, source] : ModFiles(root)) {
            const std::wstring key = Fold(relative.generic_wstring());
            gameplay += ChangesGameplay(key);
            models += ChangesModels(key);
            if (const auto it = owners.find(key); it != owners.end())
                Log("Mods: %s: %s -> %s", Utf8(relative).c_str(), Utf8(it->second).c_str(), Utf8(name).c_str());
            owners[key] = name;
            replacements.emplace_back(relative, source);
            ++count;
        }
        Log("Mods: %s: %zu files", Utf8(name).c_str(), count);
    }
    if (gameplay)
        Log("Mods: %zu of these files change gameplay (parameters, map layouts, events, AI, animations): "
            "co-op partners need the same files, or enemies and items desync between you",
            gameplay);
    if (models) {
        SetEnv("BB_MOD_MODELS", std::to_string(models));
        Log("Mods: %zu of these files are models or textures (maps, characters, parts, objects): the game's graphics "
            "heaps are enlarged for them",
            models);
    }
    UnsetEnv("BB_MOD_FILES");
    if (replacements.empty()) return game;
    fs::create_directories(out);
    out = fs::canonical(out);
    if (!JunctionsWork(out)) {
        const fs::path list = out / L"mod-files.txt";
        std::string text;
        std::set<std::wstring> seen;
        for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
            if (!seen.insert(Fold(it->first.generic_wstring())).second) continue;
            text += Utf8(it->first.generic_wstring()) + "\t" + Utf8(fs::absolute(it->second)) + "\n";
        }
        WriteText(list, text);
        SetEnv("BB_MOD_FILES", Utf8(list));
        Log("Mods: %s cannot hold links (an exFAT or FAT32 drive?): the engine reads the %zu mod files in place",
            Utf8(out.root_path()).c_str(), seen.size());
        return game;
    }
    const fs::path overlay = MakeTemporary(out);
    try {
        for (const fs::path& entry : Entries(game)) Link(overlay / entry.filename(), entry);
        std::size_t replaced = 0, added = 0;
        std::vector<std::string> added_names;
        for (const auto& [relative, source] : replacements) {
            fs::path parent = overlay;
            std::vector<fs::path> parts(relative.begin(), relative.end());
            for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
                parent = Child(parent, parts[i].native());
                Expand(parent);
            }
            const fs::path destination = Child(parent, parts.back().native());
            if (IsFile(destination) && !IsLink(destination)) {
                ++replaced;
                Unlink(destination);
            } else if (IsLink(destination) && !IsDir(destination)) {
                ++replaced;
                Unlink(destination);
            } else if (Exists(destination)) {
                throw Error("File/directory conflict: " + Utf8(relative));
            } else {
                ++added;
                if (added_names.size() < 10) added_names.push_back(Utf8(relative.generic_wstring()));
            }
            Link(destination, source);
        }
        Log("Mods: %zu game files replaced, %zu added", replaced, added);
        for (const std::string& name : added_names) Log("Mods: added %s", name.c_str());
        if (added > added_names.size()) Log("Mods: and %zu more added files", added - added_names.size());
        return overlay;
    } catch (...) {
        try {
            RemoveOverlay(overlay);
        } catch (...) {
        }
        throw;
    }
}

}

void RemoveOverlay(const fs::path& path) {
    if (IsLink(path) || !IsDir(path)) {
        if (IsLink(path) || Exists(path)) Unlink(path);
        return;
    }
    for (const fs::path& entry : Entries(path)) RemoveOverlay(entry);
    if (!RemoveDirectoryW(path.c_str())) throw Error("cannot remove " + Utf8(path));
}

fs::path BuildModsOverlay(const fs::path& game, const fs::path& out, const fs::path& mods_dir, const fs::path& config,
                          bool enabled, const std::vector<fs::path>& first_layers) {
    std::vector<std::pair<std::wstring, fs::path>> layers;
    for (const fs::path& layer : first_layers) layers.emplace_back(layer.filename().native(), layer);
    if (enabled) {
        fs::path legacy = fs::canonical(game);
        legacy += L"-mods";
        if (IsDir(legacy)) layers.emplace_back(legacy.filename().native(), legacy);
        if (IsDir(Child(mods_dir, L"dvdroot_ps4"))) {
            layers.emplace_back(mods_dir.filename().native(), mods_dir);
        } else {
            for (const std::wstring& name : Selected(mods_dir, config)) layers.emplace_back(name, mods_dir / name);
        }
    }
    return BuildOverlay(game, out, layers, UpdateFolder(fs::canonical(game)));
}

std::optional<fs::path> GameUpdateFolder(const fs::path& game) { return UpdateFolder(fs::canonical(game)); }

}
