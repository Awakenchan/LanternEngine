// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/gx_whole_hooks.h"
#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace BbEngine {
std::string g_last_log;
void host_log(const char* format, ...) {
    char line[512];
    std::va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    g_last_log = line;
}
}

namespace {
using namespace BbEngine;
using namespace BbEngine::GxWhole::Hooks;
using BbEngine::GxWhole::Mode;
using BbEngine::GxWhole::ModeOf;

constexpr const char* kImageProbe = "F:/bloodbrone/CUSA03173/eboot.elf";
constexpr std::size_t kBodyRoom = 64;
constexpr std::size_t kPendingBytes = 0xc000;
constexpr std::uint64_t kSecondArgument = 0x5150;
constexpr std::uint64_t kStackCheckFail = 0x2bbe4f8;

int failures = 0;
int skipped = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

bool Decoder(ZydisDecoder& decoder) {
    return ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64));
}

bool Relative(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands) {
    if (instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) return true;
    for (std::uint8_t i = 0; i < instruction.operand_count; ++i) {
        const ZydisDecodedOperand& op = operands[i];
        if ((op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP) ||
            (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative))
            return true;
    }
    return false;
}

bool Movable(const Site& site, bool report) {
    const std::span<const std::uint8_t> bytes = Entry(site);
    ZydisDecoder decoder;
    if (!Decoder(decoder)) return false;
    std::size_t at = 0;
    while (at < bytes.size()) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + at, bytes.size() - at, &instruction, operands))) {
            if (report) std::printf("  %s: no whole instruction at byte %zu of %zu\n", site.name, at, bytes.size());
            return false;
        }
        if (Relative(instruction, operands)) {
            if (report) std::printf("  %s: the instruction at byte %zu is relative\n", site.name, at);
            return false;
        }
        at += instruction.length;
    }
    if (bytes.size() < kJumpLength) {
        if (report) std::printf("  %s: %zu bytes, the jump needs %zu\n", site.name, bytes.size(), kJumpLength);
        return false;
    }
    return at == bytes.size();
}

bool TableIsMovable() {
    bool all = true;
    for (const Site& site : kSites) {
        const bool kind = KindOf(site) == SiteKind::Entry && Offset(site) == site.function;
        Check(kind, std::string(site.name) + " is an entry site at its function's first byte");
        const bool movable = Movable(site, true);
        Check(movable, std::string(site.name) + "'s entry bytes are whole instructions with nothing relative, at least the jump long");
        all = all && kind && movable;
    }
    const std::uint8_t split[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83};
    const std::uint8_t rip[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x48, 0x8b, 0x05, 0x10, 0x00, 0x00, 0x00};
    const std::uint8_t branch[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x74, 0x02};
    const std::uint8_t short_entry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53};
    const Site bad[] = {{Flush::OutputMerger, "split", 0, 0, 64, split},
                        {Flush::OutputMerger, "rip", 0, 0, 64, rip},
                        {Flush::OutputMerger, "branch", 0, 0, 64, branch},
                        {Flush::OutputMerger, "short", 0, 0, 64, short_entry}};
    bool refused = true;
    for (const Site& site : bad) refused = refused && !Movable(site, false);
    Check(refused, "the check refuses a split instruction, a rip-relative load, a branch and an entry shorter than the jump");
    return all;
}

void Decisions() {
    Check(ModeOf("1") == Mode::Feed && ModeOf("2") == Mode::Compare && ModeOf("0") == Mode::Off && ModeOf(nullptr) == Mode::Off,
          "the hooks read LANTERN_GX_WHOLE through GX whole's one parser");
    Check(Decide(Mode::Off, Mode::Off) == Verdict::Off, "off installs nothing");
    Check(Decide(Mode::Feed, Mode::Feed) == Verdict::Install, "1 with GX records installs the sites");
    Check(Decide(Mode::Feed, Mode::Off) == Verdict::NeedsRecords, "1 without GX records installs nothing and says it needs them");
    Check(Decide(Mode::Compare, Mode::Compare) == Verdict::Off, "2 installs nothing: the sites are start-only at 1 and 2 keeps GX's packets");
    Check(Decide(Mode::Compare, Mode::Off) == Verdict::Off, "2 without GX records installs nothing");
}

std::uint8_t* Executable(std::size_t n) {
#ifdef _WIN32
    return static_cast<std::uint8_t*>(VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(p);
#endif
}

std::size_t ImageSize() {
    std::size_t end = 0;
    for (const Site& site : kSites) end = std::max(end, site.offset + site.entry.size() + kBodyRoom);
    return end;
}

bool Body(const Site& site, std::vector<std::uint8_t>& out) {
    ZydisDecoder decoder;
    if (!Decoder(decoder)) return false;
    std::vector<ZydisRegister> pushed;
    bool framed = false;
    for (std::size_t at = 0; at < site.entry.size();) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, site.entry.data() + at, site.entry.size() - at, &instruction, operands)))
            return false;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV && operands[0].reg.value == ZYDIS_REGISTER_RBP) framed = true;
        else if (framed && instruction.mnemonic == ZYDIS_MNEMONIC_PUSH) pushed.push_back(operands[0].reg.value);
        at += instruction.length;
    }
    if (!framed || pushed.empty() || pushed.size() > 15) return false;
    out = {0x48, 0x8d, 0x65, static_cast<std::uint8_t>(-8 * static_cast<int>(pushed.size()))};
    for (auto it = pushed.rbegin(); it != pushed.rend(); ++it) {
        const int id = ZydisRegisterGetId(*it);
        if (id >= 8) out.push_back(0x41);
        out.push_back(static_cast<std::uint8_t>(0x58 + (id & 7)));
    }
    for (std::uint8_t b : {0x48, 0x8d, 0x04, 0x37, 0x5d, 0xc3}) out.push_back(b);
    return true;
}

bool PlaceSites(std::uint8_t* image) {
    for (const Site& site : kSites) {
        std::vector<std::uint8_t> body;
        if (!Body(site, body)) return false;
        std::memcpy(image + site.offset, site.entry.data(), site.entry.size());
        std::memcpy(image + site.offset + site.entry.size(), body.data(), body.size());
    }
    return true;
}

bool Untouched(const std::uint8_t* image) {
    for (const Site& site : kSites)
        if (std::memcmp(image + site.offset, site.entry.data(), site.entry.size()) != 0) return false;
    return true;
}


void HooksRun(std::uint8_t* image, std::size_t size, bool movable) {
    if (!movable) {
        Check(false, "the hooks were not run: a site is not movable");
        return;
    }
    Check(PlaceSites(image), "the test functions are placed at the sites' offsets");
    std::vector<std::uint8_t> pending(kPendingBytes);
    const auto base = reinterpret_cast<std::uint64_t>(pending.data());
    for (const Site& site : kSites) {
        const std::int64_t plain = Call(reinterpret_cast<std::uint64_t>(image + site.offset), base, kSecondArgument);
        Check(plain == static_cast<std::int64_t>(base + kSecondArgument), std::string(site.name) + ": the test function runs before hooking");
    }
    struct Case {
        const char* name;
        Mode requested, accepted;
        const char* line;
    };
    const Case off[] = {{"unset", Mode::Off, Mode::Off, "GX whole hooks: off"},
                        {"2", Mode::Compare, Mode::Compare, "GX whole hooks: off"},
                        {"1 without GX records", Mode::Feed, Mode::Off, "GX whole hooks off: needs GX records"}};
    for (const Case& c : off) {
        Check(!GxWhole::Hooks::Install(image, size, c.requested, c.accepted), std::string("LANTERN_GX_WHOLE ") + c.name + " installs nothing");
        Check(g_last_log.rfind(c.line, 0) == 0, std::string("LANTERN_GX_WHOLE ") + c.name + " says so: " + g_last_log);
        Check(Untouched(image), std::string("LANTERN_GX_WHOLE ") + c.name + " leaves every site's bytes as they were");
    }
    const bool installed = GxWhole::Hooks::Install(image, size, Mode::Feed, Mode::Feed);
    Check(installed && On(), "LANTERN_GX_WHOLE=1 with GX records installs the sites: " + g_last_log);
    if (!installed) return;
    for (const Site& site : kSites) {
        Check(image[site.offset] == 0xff && image[site.offset + 1] == 0x25, std::string(site.name) + ": the jump is written");
        const std::uint64_t before = ThreadCalls(site.flush);
        const std::int64_t hooked = Call(reinterpret_cast<std::uint64_t>(image + site.offset), base, kSecondArgument);
        Check(hooked == static_cast<std::int64_t>(base + kSecondArgument),
              std::string(site.name) + ": the hooked function still runs with its arguments and its result");
        Check(ThreadCalls(site.flush) == before + 1, std::string(site.name) + ": its own callback ran once");
    }
    Check(!GxWhole::Hooks::InstallSites(image, size), "a second install finds the jumps, not 1.09's bytes, and refuses");
    Check(g_last_log.find("nothing written") != std::string::npos, "the refusal says nothing was written: " + g_last_log);
}

struct Segment {
    std::uint64_t va, offset, filesz;
};

bool Segments(const std::vector<std::uint8_t>& elf, std::vector<Segment>& out) {
    auto read = [&](std::size_t at, auto* value) {
        if (at + sizeof(*value) > elf.size()) return false;
        std::memcpy(value, elf.data() + at, sizeof(*value));
        return true;
    };
    std::uint64_t phoff = 0;
    std::uint16_t entsize = 0, count = 0;
    if (!read(32, &phoff) || !read(54, &entsize) || !read(56, &count) || entsize != 56) return false;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t at = phoff + std::size_t(i) * 56;
        std::uint32_t type = 0;
        Segment s{};
        if (!read(at, &type) || !read(at + 8, &s.offset) || !read(at + 16, &s.va) || !read(at + 32, &s.filesz)) return false;
        if (type == 1) out.push_back(s);
    }
    return !out.empty();
}

const std::uint8_t* At(const std::vector<std::uint8_t>& elf, const std::vector<Segment>& segments, std::uint64_t va, std::size_t n) {
    for (const Segment& s : segments)
        if (va >= s.va && va + n <= s.va + s.filesz && s.offset + (va - s.va) + n <= elf.size()) return elf.data() + s.offset + (va - s.va);
    return nullptr;
}

bool Terminal(const ZydisDecodedInstruction& instruction) {
    return instruction.mnemonic == ZYDIS_MNEMONIC_RET || instruction.mnemonic == ZYDIS_MNEMONIC_INT3 ||
           instruction.mnemonic == ZYDIS_MNEMONIC_UD2 || instruction.mnemonic == ZYDIS_MNEMONIC_JMP;
}

void RealSite(const std::vector<std::uint8_t>& elf, const std::vector<Segment>& segments, const Site& site) {
    const std::span<const std::uint8_t> entry = Entry(site);
    const std::size_t offset = Offset(site);
    const std::uint8_t* code = At(elf, segments, site.function, site.function_size);
    if (!code) {
        Check(false, std::string(site.name) + ": its function is not in a load segment of the 1.09 image");
        return;
    }
    Check(offset >= site.function && offset + entry.size() <= site.function + site.function_size &&
              !std::memcmp(code + (offset - site.function), entry.data(), entry.size()),
          std::string(site.name) + ": the 1.09 image holds the entry bytes at " + std::to_string(offset));
    ZydisDecoder decoder;
    if (!Decoder(decoder)) return;
    std::set<std::uint64_t> starts, done;
    std::vector<std::uint64_t> todo = {site.function};
    std::uint64_t into = 0;
    bool undecodable = false;
    while (!todo.empty()) {
        std::uint64_t va = todo.back();
        todo.pop_back();
        while (va >= site.function && va < site.function + site.function_size && done.insert(va).second) {
            ZydisDecodedInstruction instruction;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            const std::size_t left = site.function + site.function_size - va;
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + (va - site.function), left, &instruction, operands))) {
                undecodable = true;
                break;
            }
            starts.insert(va);
            std::uint64_t target = 0;
            const bool branch = (instruction.meta.category == ZYDIS_CATEGORY_COND_BR ||
                                 instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR || instruction.meta.category == ZYDIS_CATEGORY_CALL) &&
                                operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                                ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[0], va, &target));
            if (branch && target > offset && target < offset + entry.size()) into = va;
            if (branch && instruction.meta.category != ZYDIS_CATEGORY_CALL) todo.push_back(target);
            if (Terminal(instruction) || (branch && target == kStackCheckFail)) break;
            va += instruction.length;
        }
    }
    Check(!undecodable, std::string(site.name) + ": every path through the function decodes");
    Check(!into, std::string(site.name) + ": no branch lands inside the moved bytes (one at " + std::to_string(into) + ")");
    std::size_t prologue = 0;
    for (std::uint64_t va : starts)
        if (va >= offset && va < offset + entry.size()) ++prologue;
    Check(starts.count(offset + entry.size()) == 1, std::string(site.name) + ": the moved bytes end on an instruction the function runs next");
    ZydisDecodedInstruction first;
    std::size_t moved = 0;
    for (std::size_t at = 0; at < entry.size() && ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, entry.data() + at, entry.size() - at, &first)); at += first.length)
        ++moved;
    Check(prologue == moved, std::string(site.name) + ": the function's own decode has the moved instructions and no others there");
}

void RealImage() {
    const char* env = std::getenv("BB_BOOT_INSTALL_IMAGE");
    const char* path = env && *env ? env : kImageProbe;
    static const std::vector<std::uint8_t> elf = [&] {
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();
    if (elf.empty()) {
        std::printf("gx-whole-hooks-test: SKIPPED: the 1.09 image checks need %s (BB_BOOT_INSTALL_IMAGE names another)\n", path);
        ++skipped;
        return;
    }
    std::vector<Segment> segments;
    if (!Segments(elf, segments)) {
        Check(false, std::string(path) + " is not an ELF with load segments");
        return;
    }
    for (const Site& site : kSites) RealSite(elf, segments, site);
}

int RunSuite(std::uint8_t* image, std::size_t size) {
    const int before = failures;
    const bool movable = TableIsMovable();
    Decisions();
    HooksRun(image, size, movable);
    RealImage();
    return failures - before;
}
}

int main() {
    const std::size_t size = ImageSize();
    std::uint8_t* image = Executable(size);
    if (!image) {
        std::printf("FAIL: gx-whole-hooks-test: no executable memory for the test image\n");
        return 1;
    }
#ifdef LANTERN_GX_WHOLE_HOOKS_MUTANTS
    int survivors = 0;
    for (int m = 0; m < static_cast<int>(Mutant::kCount); ++m) {
        selected_mutant = static_cast<Mutant>(m);
        std::memset(image, 0xcc, size);
        const int found = RunSuite(image, size);
        if (m == static_cast<int>(Mutant::None)) {
            if (found) {
                std::printf("gx-whole-hooks mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("gx-whole-hooks mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("gx-whole-hooks mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("gx-whole-hooks mutants: every mutant caught%s\n", skipped ? " (the 1.09 image checks SKIPPED)" : "");
    return 0;
#else
    std::memset(image, 0xcc, size);
    if (RunSuite(image, size)) {
        std::printf("gx-whole-hooks-test: %d failures\n", failures);
        return 1;
    }
    std::printf("PASS: gx-whole-hooks-test: %zu GX flush sites are entry sites with movable bytes, the hooks install only at 1 with GX "
                "records, each hooked function still runs with its own callback%s\n",
                kFlushes, skipped ? "; the 1.09 image checks SKIPPED" : ", and on the 1.09 image no branch lands in the moved bytes");
    return 0;
#endif
}
