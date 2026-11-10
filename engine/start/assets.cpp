// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets.h"

#include <windows.h>
#include <zlib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace start {
namespace {

const char* const kLayout = "dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx";
const char* const kInputSha = "8e1de0ee223cd5137a93383cfc7bb1d27964bc480c63e35013674717b403ef8f";
const char* const kOutputSha = "99d5c7b7f52307344c6e8296b499d5e57969f21c443fcc607cb42cb2c38c8afa";
const char* const kEbootSha[] = {"941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7",
                                 "cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86"};
const char* const kImageSha = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";

std::uint32_t BigEndian32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}
void PutBigEndian32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (24 - 8 * i));
}

constexpr std::string_view kDcxHeader = "44435800000100000000001800000024000000440000004c"
                                        "4443530000000000000000004443500044464c5400000020"
                                        "09000000000000000000000000000000000001014443410000000008";
constexpr std::uint32_t kDcxLargest = 32u << 20;

Bytes DcxUnpack(const Bytes& data) {
    if (data.size() < 4 || std::memcmp(data.data(), "DCX\0", 4) != 0) return data;
    if (data.size() < 0x4c || std::memcmp(data.data() + 0x28, "DFLT", 4) != 0) throw Error("Unsupported DCX format");
    static constexpr std::uint8_t kDca[] = {'D', 'C', 'A', 0};
    const auto found = std::search(data.begin() + 0x2c, data.end(), std::begin(kDca), std::end(kDca));
    const std::size_t dca = std::size_t(found - data.begin());
    if (found == data.end() || dca + 8 > data.size()) throw Error("DCX without a valid DCA header");
    const std::size_t begin = dca + BigEndian32(data.data() + dca + 4);
    const std::uint32_t size = BigEndian32(data.data() + 0x1c);
    if (begin < dca + 8 || begin >= data.size() || size == 0 || size > kDcxLargest) throw Error("Invalid DCX header");
    Bytes raw(std::size_t(size) + 1);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) throw Error("zlib unavailable");
    z.next_in = const_cast<Bytef*>(data.data() + begin);
    z.avail_in = uInt(data.size() - begin);
    z.next_out = raw.data();
    z.avail_out = uInt(raw.size());
    const int status = inflate(&z, Z_FINISH);
    const std::size_t made = raw.size() - z.avail_out;
    const bool left_over = z.avail_in != 0;
    inflateEnd(&z);
    if (status != Z_STREAM_END || made != size || left_over) throw Error("Invalid DCX compressed data or size");
    raw.resize(size);
    return raw;
}

Bytes DcxPack(const Bytes& raw) {
    uLongf packed_size = compressBound(uLong(raw.size()));
    Bytes packed(packed_size);
    if (compress2(packed.data(), &packed_size, raw.data(), uLong(raw.size()), 9) != Z_OK) throw Error("compression failed");
    Bytes out = FromHex(kDcxHeader);
    PutBigEndian32(out.data() + 0x1c, std::uint32_t(raw.size()));
    PutBigEndian32(out.data() + 0x20, std::uint32_t(packed_size));
    out.insert(out.end(), packed.begin(), packed.begin() + std::ptrdiff_t(packed_size));
    return out;
}

constexpr int kMirrorPart = 807, kMirrorSlot = 742, kFirstEnemy = 736;
constexpr std::int32_t kEnemy = 2, kDummyEnemy = 10;
constexpr int kEventPartFields[] = {0x48, 0x50, 0x5c};
constexpr int kPartPartFields[] = {0x128, 0x13c, 0x144, 0x178, 0x180, 0x188};

struct MsbList {
    std::size_t table;
    std::vector<std::int64_t> entries;
};

Bytes MirrorLayout(const Bytes& raw) {
    if (Sha256Hex(raw) != kInputSha) throw Error("Dream mirror requires the original 1.09 Dream layout");
    Bytes b = raw;
    const auto i32 = [&](std::int64_t at) { return Load<std::int32_t>(b.data() + at); };
    const auto put32 = [&](std::int64_t at, std::int32_t v) { Store<std::int32_t>(b.data() + at, v); };
    const auto inside = [&](std::int64_t at) { return at > 0 && at < std::int64_t(b.size()); };
    std::vector<MsbList> lists;
    std::int64_t at = 0x10;
    for (int n = 0; n < 4; ++n) {
        if (at < 0 || std::size_t(at) > b.size() - 16) throw Error("Invalid MSB list header");
        const std::int32_t count = i32(at + 4);
        if (count < 1 || std::size_t(count) > (b.size() - std::size_t(at) - 16) / 8) throw Error("Invalid MSB list size");
        MsbList list{std::size_t(at) + 16, {}};
        for (std::int32_t i = 0; i + 1 < count; ++i) {
            const auto entry = Load<std::int64_t>(b.data() + list.table + 8 * std::size_t(i));
            if (!inside(entry)) throw Error("Invalid MSB entry");
            list.entries.push_back(entry);
        }
        at = Load<std::int64_t>(b.data() + list.table + 8 * std::size_t(count - 1));
        if (n < 3 && !inside(at)) throw Error("Invalid MSB entry");
        lists.push_back(std::move(list));
    }
    const auto& events = lists[1].entries;
    const auto& parts = lists[3].entries;
    if (parts.size() <= std::size_t(kMirrorPart + 1) ||
        !(parts[kMirrorSlot] < parts[kMirrorPart] && parts[kMirrorPart] < parts[kMirrorPart + 1]))
        throw Error("Dream layout parts are not as expected");
    if (i32(parts[kMirrorPart] + 0x14) != kDummyEnemy) throw Error("Mirror stand-in is not a DummyEnemy");

    const auto moved = [](std::int32_t index) {
        if (index == kMirrorPart) return kMirrorSlot;
        return kMirrorSlot <= index && index < kMirrorPart ? index + 1 : index;
    };
    std::vector<std::int64_t> boundaries;
    for (const auto& list : lists) boundaries.insert(boundaries.end(), list.entries.begin(), list.entries.end());
    std::sort(boundaries.begin(), boundaries.end());
    const auto renumber = [&](const std::vector<std::int64_t>& entries, std::span<const int> fields) {
        for (const std::int64_t entry : entries) {
            const auto next = std::upper_bound(boundaries.begin(), boundaries.end(), entry);
            const std::int64_t end = next == boundaries.end() ? std::int64_t(b.size()) : *next;
            for (const int field : fields) {
                if (entry + field + 4 > end) continue;
                const std::int32_t index = i32(entry + field);
                if (index >= 0 && moved(index) != index) put32(entry + field, moved(index));
            }
        }
    };
    renumber(events, kEventPartFields);
    renumber(parts, kPartPartFields);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::int32_t kind = i32(parts[i] + 0x14), number = i32(parts[i] + 0x18);
        if (kind == kEnemy && int(i) >= kMirrorSlot) put32(parts[i] + 0x18, number + 1);
        else if (kind == kDummyEnemy && int(i) > kMirrorPart) put32(parts[i] + 0x18, number - 1);
    }
    put32(parts[kMirrorPart] + 0x14, kEnemy);
    put32(parts[kMirrorPart] + 0x18, kMirrorSlot - kFirstEnemy);
    const std::int64_t first = parts[kMirrorSlot], stand_in = parts[kMirrorPart], after = parts[kMirrorPart + 1];
    std::rotate(b.begin() + first, b.begin() + stand_in, b.begin() + after);
    const std::int64_t size = after - stand_in;
    const std::size_t table = lists[3].table;
    Store<std::int64_t>(b.data() + table + 8 * std::size_t(kMirrorSlot), first);
    for (int i = kMirrorSlot; i < kMirrorPart; ++i)
        Store<std::int64_t>(b.data() + table + 8 * std::size_t(i + 1), parts[std::size_t(i)] + size);
    if (Sha256Hex(b) != kOutputSha) throw Error("Mirror layout does not match the verified result");
    return b;
}

struct EffectFix {
    const char* binder;
    const char* packed_sha;
    const char* input_sha;
    const char* output_sha;
};
const char* const kEffectSource = "dvdroot_ps4/sfx/frpg_sfxbnd_m36.ffxbnd.dcx";
const char* const kEffectSourceName = "f001454436.fxr";
const char* const kEffectSourceSha = "990644087f325191d34fd2f4b633a9f7c6a63ce9d71786c4289a7cae5cdfb4e0";
const char* const kEffectName = "f001251720.fxr";
const char* const kEffectSha = "988c55ba0f689bc0b4f8e7a9f9b184ef8cd9bcd4601b2c925387ff5ce3fc9f0b";
const EffectFix kEffectFixes[] = {
    {"dvdroot_ps4/sfx/frpg_sfxbnd_m24_02.ffxbnd.dcx", "6cab7031c5fafba95ed71c4ddb86e590b77b2d669988e8b53bed159e1dbc1015",
     "b0bd083145a753a667c4021a3f817879646b1f8b84201e2774daa17e1c5b0e99",
     "fae6b9639fb02b34188a3dd262b2695f7667812749f984f1eb2fd2ab298a9e9b"},
    {"dvdroot_ps4/sfx/frpg_sfxbnd_m29a.ffxbnd.dcx", "0466ebd7125cc705e341cab18e18c3d1282000a8d54b0628d47da992daed0fd6",
     "085f2d3d60d54e50fa981c7aad89ca42f47b3a733a78b40ee98f572883ce122b",
     "8348b2b7f110feb7ca0a444e8331886b85fe24ba422b8b9909cdc5d551ffcb07"},
};
constexpr std::size_t kBnd4Entry = 0x24;

struct BinderEntry {
    std::size_t header, offset, size;
    std::string name;
};

std::vector<BinderEntry> BinderEntries(const Bytes& b) {
    if (b.size() < 0x40 || std::memcmp(b.data(), "BND4", 4) || Load<std::int64_t>(b.data() + 0x20) != std::int64_t(kBnd4Entry) ||
        b[0x30] != 1 || b[0x31] != 0x74)
        throw Error("Unexpected effect binder layout");
    const std::int32_t count = Load<std::int32_t>(b.data() + 0x0c);
    if (count < 0 || 0x40 + std::size_t(count) * kBnd4Entry > b.size()) throw Error("Unexpected effect binder layout");
    std::vector<BinderEntry> entries;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::size_t at = 0x40 + std::size_t(i) * kBnd4Entry;
        const std::int64_t packed = Load<std::int64_t>(b.data() + at + 8), unpacked = Load<std::int64_t>(b.data() + at + 16);
        const std::uint32_t offset = Load<std::uint32_t>(b.data() + at + 24);
        const std::int32_t name_at = Load<std::int32_t>(b.data() + at + 32);
        if (packed != unpacked || packed < 0 || offset + std::uint64_t(packed) > b.size() || name_at < 0 ||
            std::size_t(name_at) >= b.size())
            throw Error("Unexpected effect binder entry");
        std::string name;
        for (std::size_t p = std::size_t(name_at); p + 1 < b.size() && (b[p] || b[p + 1]); p += 2) {
            const char c = b[p + 1] ? '?' : char(b[p]);
            name = c == '\\' || c == '/' ? std::string() : name + c;
        }
        entries.push_back({at, offset, std::size_t(packed), name});
    }
    return entries;
}

Bytes BinderFile(const Bytes& b, const std::string& name) {
    for (const BinderEntry& e : BinderEntries(b))
        if (e.name == name) return Bytes(b.begin() + std::ptrdiff_t(e.offset), b.begin() + std::ptrdiff_t(e.offset + e.size));
    throw Error(name + " is not in the effect binder");
}

Bytes ReplaceBinderFile(const Bytes& b, const std::string& name, const Bytes& data) {
    const auto entries = BinderEntries(b);
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const BinderEntry& e) { return e.name == name; });
    if (found == entries.end() ||
        std::count_if(entries.begin(), entries.end(), [&](const BinderEntry& e) { return e.name == name; }) != 1)
        throw Error(name + " is not in the effect binder once");
    const std::size_t offset = (b.size() + 15) & ~std::size_t(15);
    if (offset + data.size() > 0xffffffffu) throw Error("effect binder too large");
    Bytes out = b;
    out.resize(offset);
    out.insert(out.end(), data.begin(), data.end());
    Store<std::int64_t>(out.data() + found->header + 8, std::int64_t(data.size()));
    Store<std::int64_t>(out.data() + found->header + 16, std::int64_t(data.size()));
    Store<std::uint32_t>(out.data() + found->header + 24, std::uint32_t(offset));
    return out;
}

std::wstring Quote(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    for (std::size_t i = 0; i < argument.size(); ++i) {
        std::size_t slashes = 0;
        while (i < argument.size() && argument[i] == L'\\') ++slashes, ++i;
        if (i == argument.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (argument[i] == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        out += argument[i];
    }
    return out + L"\"";
}

}

std::wstring CommandLine(const fs::path& program, const std::vector<std::wstring>& arguments) {
    std::wstring line = Quote(program.native());
    for (const std::wstring& a : arguments) line += L" " + Quote(a);
    return line;
}

int RunProgram(const fs::path& program, const std::vector<std::wstring>& arguments, std::string* output, void* log) {
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (output && !CreatePipe(&read, &write, &inherit, 0)) throw Error("cannot start " + Utf8(program));
    if (read) SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE null = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{sizeof startup};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write ? write : log ? static_cast<HANDLE>(log) : null;
    startup.hStdError = log ? static_cast<HANDLE>(log) : write ? write : null;
    std::wstring line = CommandLine(program, arguments);
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &startup, &process);
    if (write) CloseHandle(write);
    if (!ok) {
        if (read) CloseHandle(read);
        CloseHandle(null);
        throw Error("cannot start " + Utf8(program));
    }
    if (read) {
        char buffer[4096];
        DWORD n = 0;
        while (::ReadFile(read, buffer, sizeof buffer, &n, nullptr) && n) output->append(buffer, n);
        CloseHandle(read);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(null);
    return int(code);
}

std::string VerifyExecutable(const fs::path& elf_path) {
    const Bytes elf = ReadFile(elf_path);
    const std::string sha = Sha256Hex(elf);
    std::string code;
    try {
        code = Trim(ReadText(elf_path.parent_path() / "game-code.txt"));
    } catch (const Error&) {
    }
    if (std::none_of(std::begin(kEbootSha), std::end(kEbootSha), [&](const char* s) { return sha == s; }) &&
        code != "1.09")
        throw Error("Mirror hooks require the verified 1.09 executable");
    if (elf.size() < 64) throw Error("Invalid executable program headers");
    const std::uint64_t phoff = Load<std::uint64_t>(elf.data() + 32);
    const std::uint16_t entsize = Load<std::uint16_t>(elf.data() + 54), count = Load<std::uint16_t>(elf.data() + 56);
    if (entsize != 56 || count == 0 || count >= 256) throw Error("Invalid executable program headers");
    const std::pair<std::uint64_t, const char*> hooks[] = {{0x1c1cce0, "554889e54157415653504d89ce4889fb"},
                                                           {0x2034770, "554889e54157415641554154534883ec38"}};
    for (const auto& [address, hex] : hooks) {
        const Bytes expected = FromHex(hex);
        bool found = false;
        for (int i = 0; i < count; ++i) {
            const std::uint8_t* p = elf.data() + phoff + std::uint64_t(i) * 56;
            if (phoff + std::uint64_t(i) * 56 + 56 > elf.size()) throw Error("Invalid executable program headers");
            const std::uint32_t kind = Load<std::uint32_t>(p), flags = Load<std::uint32_t>(p + 4);
            const std::uint64_t offset = Load<std::uint64_t>(p + 8), va = Load<std::uint64_t>(p + 16),
                                size = Load<std::uint64_t>(p + 32);
            if (kind == 1 && (flags & 1) && va <= address && address + expected.size() <= va + size) {
                const std::uint64_t at = offset + address - va;
                found = at + expected.size() <= elf.size() &&
                        std::equal(expected.begin(), expected.end(), elf.begin() + std::ptrdiff_t(at));
                break;
            }
        }
        if (!found) throw Error("Unexpected 1.09 engine bytes at " + Hex(address));
    }
    return kImageSha;
}

fs::path PrepareMirror(const fs::path& game, const fs::path& elf, const fs::path& cache) {
    VerifyExecutable(elf);
    const Bytes rewritten = MirrorLayout(DcxUnpack(ReadFile(game / Path(kLayout))));
    const fs::path destination = cache / "dream-mirror" / Path(kLayout);
    try {
        if (Sha256Hex(DcxUnpack(ReadFile(destination))) == kOutputSha) return fs::canonical(destination);
    } catch (const Error&) {
    }
    WriteFile(destination, DcxPack(rewritten));
    return fs::canonical(destination);
}

std::vector<std::pair<std::string, fs::path>> PrepareEffectFixes(const fs::path& game, const fs::path& cache,
                                                                 std::vector<std::string>& notes) {
    const fs::path root = cache / "effect-fixes";
    std::optional<Bytes> replacement;
    std::vector<std::pair<std::string, fs::path>> fixed;
    std::error_code error;
    for (const EffectFix& fix : kEffectFixes) {
        if (!fs::is_regular_file(game / Path(fix.binder), error)) continue;
        const Bytes packed = ReadFile(game / Path(fix.binder));
        const std::string file = Utf8(Path(fix.binder).filename());
        std::optional<Bytes> raw;
        if (Sha256Hex(packed) != fix.packed_sha) {
            raw = DcxUnpack(packed);
            if (Sha256Hex(*raw) != fix.input_sha) {
                notes.push_back(file + " is not the 1.09 file (a mod changes it): left as it is");
                continue;
            }
        }
        const fs::path destination = root / Path(fix.binder);
        fs::path stamp = destination;
        stamp += L".sha256";
        try {
            if (Trim(ReadText(stamp)) == Sha256Hex(ReadFile(destination))) {
                fixed.emplace_back(fix.binder, fs::canonical(destination));
                continue;
            }
        } catch (const std::exception&) {
        }
        if (!raw) raw = DcxUnpack(packed);
        if (Sha256Hex(BinderFile(*raw, kEffectName)) != kEffectSha) throw Error(std::string("Unexpected ") + kEffectName + " in " + file);
        if (!replacement) {
            replacement = BinderFile(DcxUnpack(ReadFile(game / Path(kEffectSource))), kEffectSourceName);
            if (Sha256Hex(*replacement) != kEffectSourceSha) throw Error(std::string("Unexpected ") + kEffectSourceName);
        }
        const Bytes result = ReplaceBinderFile(*raw, kEffectName, *replacement);
        if (Sha256Hex(result) != fix.output_sha) throw Error(file + " does not match the verified result");
        const Bytes out = DcxPack(result);
        fs::create_directories(destination.parent_path());
        fs::path temporary = destination;
        temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        WriteFile(temporary, out);
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            fs::remove(temporary, error);
            throw Error("cannot write " + Utf8(destination));
        }
        WriteText(stamp, Sha256Hex(out) + "\n");
        fixed.emplace_back(fix.binder, fs::canonical(destination));
    }
    return fixed;
}

std::optional<std::pair<std::string, fs::path>> PrepareJumpKey(const fs::path& game, const fs::path& cache,
                                                              std::string& note) {
    static const char* const kScript = "dvdroot_ps4/action/script/c0000.hks";
    static const char* const kInputSha = "335a1af801d4f2108fc77d347593a2ca65a43f5dd6e23348f47d953606d34c59";
    static const char* const kOutputSha = "026275a6d3fef4830ee765698abb1658a747a0f5c9a0246ffe9ec8b28cef9bdf";
    constexpr std::size_t kSite = 0x1e450;
    static constexpr std::uint8_t kOld[8] = {0xb6, 0x00, 0x4d, 0x02, 0x98, 0x00, 0x4e, 0x14};
    static constexpr std::uint8_t kNew[8] = {0x32, 0x00, 0x1a, 0x02, 0x50, 0x05, 0x26, 0x02};
    constexpr std::size_t kSprintSite = 0x1e48c;
    static constexpr std::uint8_t kSprintOld[4] = {0x39, 0x00, 0x0b, 0x00};
    static constexpr std::uint8_t kSprintNew[4] = {0x38, 0xff, 0xff, 0x00};
    const fs::path destination = cache / "jump-key" / "c0000.hks";
    Bytes script = ReadFile(game / Path(kScript));
    if (Sha256Hex(script) != kInputSha) {
        note = "the action script is not the 1.09 file (a mod changes it): jump stays on the dodge button";
        return std::nullopt;
    }
    try {
        if (Sha256Hex(ReadFile(destination)) == kOutputSha) return std::pair{std::string(kScript), fs::canonical(destination)};
    } catch (const std::exception&) {
    }
    if (std::memcmp(script.data() + kSite, kOld, sizeof kOld) != 0 ||
        std::memcmp(script.data() + kSprintSite, kSprintOld, sizeof kSprintOld) != 0)
        throw Error("Unexpected action script");
    std::memcpy(script.data() + kSite, kNew, sizeof kNew);
    std::memcpy(script.data() + kSprintSite, kSprintNew, sizeof kSprintNew);
    if (Sha256Hex(script) != kOutputSha) throw Error("The action script does not match the verified result");
    fs::create_directories(destination.parent_path());
    WriteFile(destination, script);
    return std::pair{std::string(kScript), fs::canonical(destination)};
}

fs::path PrepareMenuAssets(const fs::path& game, const fs::path& elf, const fs::path& cache, const fs::path& tool,
                           bool rebirth, const std::optional<std::string>& host_title,
                           const std::optional<std::string>& guest_title) {
    VerifyExecutable(elf);
    if (host_title) SetEnv("BB_PLATE_HOST_TITLE", "=" + *host_title);
    else UnsetEnv("BB_PLATE_HOST_TITLE");
    if (guest_title) SetEnv("BB_PLATE_GUEST_TITLE", "=" + *guest_title);
    else UnsetEnv("BB_PLATE_GUEST_TITLE");
    const std::string plates = (host_title ? "=" + *host_title : std::string("-")) + "|" +
                               (guest_title ? "=" + *guest_title : std::string("-"));
    const fs::path root = cache / "pc-menus";
    std::vector<std::pair<std::string, std::string>> sources{{"dvdroot_ps4/menu/optionsetting.gfx", "movie"}};
    std::vector<std::string> messages;
    std::error_code error;
    const fs::path message_dir = game / "dvdroot_ps4" / "msg";
    if (fs::is_directory(message_dir, error))
        for (const auto& language : fs::directory_iterator(message_dir, error))
            if (fs::is_regular_file(language.path() / "menu.msgbnd.dcx", error))
                messages.push_back("dvdroot_ps4/msg/" + Utf8(language.path().filename()) + "/menu.msgbnd.dcx");
    std::sort(messages.begin(), messages.end());
    for (const std::string& m : messages) sources.emplace_back(m, "messages");
    if (sources.size() < 2 || sources.size() > 24)
        throw Error("Native options require valid menu bundles for every installed language");
    if (rebirth) sources.emplace_back("dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx", "rebirth");
    const std::string generator = Sha256Hex(ReadFile(tool));
    const fs::path manifest_path = root / "manifest.json";
    nlohmann::json previous = nlohmann::json::object();
    try {
        previous = nlohmann::json::parse(ReadText(manifest_path));
    } catch (const std::exception&) {
    }
    nlohmann::ordered_json rows = nlohmann::ordered_json::object();
    for (const auto& [relative, kind] : sources) {
        const fs::path source = game / Path(relative), destination = root / Path(relative);
        if (fs::file_size(source) > 64ull * 1024 * 1024) throw Error("Native menu source is oversized: " + relative);
        const std::string digest = Sha256Hex(ReadFile(source));
        std::optional<std::string> current;
        const auto prior = previous.contains("files") && previous["files"].contains(relative) ? previous["files"][relative]
                                                                                             : nlohmann::json::object();
        if (previous.value("generator", "") == generator && prior.value("input", "") == digest &&
            prior.value("kind", "") == kind && (kind != "messages" || prior.value("plates", "") == plates)) {
            try {
                const std::string now = Sha256Hex(ReadFile(destination));
                if (now == prior.value("output", "")) current = now;
            } catch (const Error&) {
            }
        }
        if (!current) {
            fs::create_directories(destination.parent_path());
            fs::path temporary = destination;
            temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            std::string errors;
            const int code = RunProgram(tool, {Wide(kind), source.native(), temporary.native()}, &errors);
            if (code) {
                fs::remove(temporary, error);
                const std::string message = Trim(errors);
                throw Error(message.empty() ? "Menu generator failed: " + relative : message);
            }
            current = Sha256Hex(ReadFile(temporary));
            if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                fs::remove(temporary, error);
                throw Error("cannot write " + Utf8(destination));
            }
        }
        rows[relative] = {{"input", digest}, {"output", *current}, {"kind", kind}, {"plates", plates}};
    }
    fs::create_directories(root);
    WriteText(manifest_path, nlohmann::ordered_json{{"generator", generator}, {"files", rows}}.dump(2));
    return fs::canonical(root);
}

fs::path PrepareLamp2Lamp(const fs::path& game, const std::optional<fs::path>& update, const fs::path& patch,
                          const fs::path& cache) {
    const Bytes file = ReadFile(patch);
    if (file.size() < 8 || std::memcmp(file.data(), "L2LP", 4)) throw Error("not a Lamp2Lamp patch file");
    uLongf size = Load<std::uint32_t>(file.data() + 4);
    Bytes body(size);
    if (uncompress(body.data(), &size, file.data() + 8, uLong(file.size() - 8)) != Z_OK || size != body.size())
        throw Error("damaged Lamp2Lamp patch file");
    std::size_t at = 0;
    const auto take = [&](std::size_t count) {
        if (count > body.size() - at) throw Error("damaged Lamp2Lamp patch file");
        const std::uint8_t* here = body.data() + at;
        at += count;
        return here;
    };
    const auto u32 = [&] { return Load<std::uint32_t>(take(4)); };
    const auto hex = [&] {
        static constexpr char kDigits[] = "0123456789abcdef";
        const std::uint8_t* raw = take(32);
        std::string text;
        for (int i = 0; i < 32; ++i) {
            text += kDigits[raw[i] >> 4];
            text += kDigits[raw[i] & 15];
        }
        return text;
    };
    struct Row {
        std::string name, input, output;
        std::uint32_t length;
        std::size_t operations, count;
        Bytes packed;
    };
    std::vector<Row> rows(u32());
    std::string state = Sha256Hex(file);
    std::error_code error;
    for (Row& row : rows) {
        const std::size_t name = Load<std::uint16_t>(take(2));
        const std::uint8_t* text = take(name);
        row.name.assign(reinterpret_cast<const char*>(text), name);
        row.input = hex();
        row.output = hex();
        row.length = u32();
        row.count = u32();
        row.operations = at;
        for (std::size_t i = 0; i < row.count; ++i) {
            const std::uint32_t where = u32(), length = u32();
            if (where == 0xFFFFFFFFu) take(length);
        }
        const fs::path source = update && fs::is_regular_file(*update / Path(row.name), error) ? *update / Path(row.name) : game / Path(row.name);
        if (!fs::is_regular_file(source, error)) throw Error(row.name + " is missing from the game");
        row.packed = ReadFile(source);
        state += Sha256Hex(row.packed);
    }
    if (at != body.size()) throw Error("damaged Lamp2Lamp patch file");
    const fs::path root = cache / patch.stem(), stamp = root / "complete";
    const std::string wanted = Sha256Hex(state.data(), state.size());
    try {
        if (Trim(ReadText(stamp)) == wanted) return fs::canonical(root);
    } catch (const std::exception&) {
    }
    fs::remove_all(root, error);
    for (const Row& row : rows) {
        const Bytes raw = DcxUnpack(row.packed);
        if (Sha256Hex(raw) != row.input) throw Error(row.name + " is not the 1.09 file");
        Bytes result;
        result.reserve(row.length);
        at = row.operations;
        for (std::size_t i = 0; i < row.count; ++i) {
            const std::uint32_t where = u32(), length = u32();
            if (where == 0xFFFFFFFFu) {
                const std::uint8_t* data = take(length);
                result.insert(result.end(), data, data + length);
            } else {
                if (where > raw.size() || length > raw.size() - where) throw Error("damaged Lamp2Lamp patch file");
                result.insert(result.end(), raw.begin() + std::ptrdiff_t(where), raw.begin() + std::ptrdiff_t(where) + length);
            }
        }
        if (Sha256Hex(result) != row.output) throw Error(row.name + " does not match the verified result");
        const fs::path destination = root / Path(row.name);
        fs::create_directories(destination.parent_path());
        WriteFile(destination, DcxPack(result));
    }
    WriteText(stamp, wanted + "\n");
    return fs::canonical(root);
}

std::vector<WideMovie> PrepareWideMovies(const fs::path& game, const std::optional<fs::path>& option_movie,
                                         const fs::path& cache, const fs::path& tool, int width, int height) {
    const std::string size = std::to_string(width) + "x" + std::to_string(height);
    const fs::path root = cache / "wide-ui" / size;
    const std::string option = "dvdroot_ps4/menu/optionsetting.gfx";
    std::vector<std::pair<std::string, fs::path>> sources;
    std::error_code error;
    for (const auto& item : fs::directory_iterator(game / "dvdroot_ps4" / "menu", error)) {
        if (!item.is_regular_file(error) || Lower(Utf8(item.path().extension())) != ".gfx") continue;
        const std::string relative = "dvdroot_ps4/menu/" + Utf8(item.path().filename());
        sources.emplace_back(relative, relative == option && option_movie ? *option_movie : item.path());
    }
    if (sources.empty()) throw Error("no menu movies in " + Utf8(game / "dvdroot_ps4" / "menu"));
    std::sort(sources.begin(), sources.end());
    // Kept while the movies, the tool and the size are the same, told by each file's size and time
    // (reading and hashing some 70 movies at every start took longer than the check is worth).
    const auto stamp = [](const fs::path& path) {
        std::error_code e;
        const auto bytes = fs::file_size(path, e);
        const long long time = e ? 0 : static_cast<long long>(fs::last_write_time(path, e).time_since_epoch().count());
        return e ? std::string("missing") : std::to_string(bytes) + ":" + std::to_string(time);
    };
    std::string key = stamp(tool) + " " + size;
    for (const auto& [relative, source] : sources) key += "\n" + relative + " " + stamp(source);
    key = Sha256Hex(key.data(), key.size());
    const fs::path manifest_path = root / "manifest.json";
    // The movies the last run made, if its manifest reads as one (anything else: made again).
    std::string previous_key;
    std::vector<std::pair<std::string, std::string>> previous_files;  // relative path, output stamp
    std::vector<std::string> previous_notes;
    try {
        const auto previous = nlohmann::json::parse(ReadText(manifest_path));
        previous_key = previous.at("key").get<std::string>();
        for (const auto& f : previous.at("files")) {
            previous_files.emplace_back(f.at("relative").get<std::string>(), f.at("output").get<std::string>());
            previous_notes.push_back(f.value("note", ""));
        }
    } catch (const std::exception&) {
        previous_key.clear();
    }
    std::vector<WideMovie> made;
    if (!previous_key.empty() && previous_key == key) {
        bool whole = true;
        for (std::size_t i = 0; i < previous_files.size(); ++i) {
            const auto& [relative, output] = previous_files[i];
            WideMovie m{relative, root / Path(relative), previous_notes[i]};
            whole = whole && !relative.empty() && stamp(m.file) == output;
            made.push_back(std::move(m));
        }
        if (whole) return made;
        made.clear();
    }
    // Only the movies this made before are removed: a movie that needs nothing now must not stay.
    for (const auto& [relative, output] : previous_files)
        if (!relative.empty()) fs::remove(root / Path(relative), error);
    fs::create_directories(root);
    std::string list;
    for (const auto& [relative, source] : sources) list += relative + "\t" + Utf8(source) + "\n";
    WriteText(root / "sources.txt", list);
    std::string output;
    if (RunProgram(tool, {L"wide-movies", Wide(size), (root / "sources.txt").native(), root.native()}, &output))
        throw Error("the menu movie tool failed: " + Trim(output));
    nlohmann::ordered_json rows = nlohmann::ordered_json::array();
    std::size_t at = 0;
    while (at < output.size()) {
        std::size_t end = output.find('\n', at);
        if (end == std::string::npos) end = output.size();
        std::string line = output.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || !tab) continue;
        if (line[0] == '!') {
            Log("Widescreen: %s left as it is: %s", line.substr(1, tab - 1).c_str(), line.substr(tab + 1).c_str());
            continue;
        }
        WideMovie m{line.substr(0, tab), root / Path(line.substr(0, tab)), line.substr(tab + 1)};
        rows.push_back({{"relative", m.relative}, {"output", stamp(m.file)}, {"note", m.note}});
        made.push_back(std::move(m));
    }
    WriteText(manifest_path, nlohmann::ordered_json{{"key", key}, {"files", rows}}.dump(2));
    return made;
}

}
