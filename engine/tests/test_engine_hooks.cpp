// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/engine_hooks.h"
#include "gpu/shim/engine/hook_entries.h"
#include <Zydis/Zydis.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" {
std::uint64_t hook_clobber_align = 0;
std::uint64_t hook_clobber_seen_rsi = 0;
std::uint64_t hook_clobber_seen_r13 = 0;
extern const std::uint8_t hook_guest_begin[];
extern const std::uint8_t hook_guest_site[];
extern const std::uint8_t hook_guest_end[];
std::int64_t __attribute__((sysv_abi)) hook_clobber(std::uint64_t, std::uint64_t*) noexcept;
}

asm(R"(
    .text
    .globl hook_guest_begin
    .globl hook_guest_site
    .globl hook_guest_end
    .globl hook_clobber
hook_guest_begin:
    pushq %rbp
    movq %rsp, %rbp
    pushq %r13
    subq %rsi, %rsp
    movq $0x1313, %r13
    movq $0x1111, -8(%rsp)
    movq $0x2222, -128(%rsp)
    movq $0xa10, %r10
    movq $0xb11, %r11
    movq $0xc1, %rcx
    movq $0xd2, %rdx
    movq $0xe8, %r8
    movq $0xf9, %r9
    movq $0x70, %rax
    movq %rax, %xmm0
    movq $0x71, %rax
    movq %rax, %xmm1
    movq $0x72, %rax
    movq %rax, %xmm2
    movq $0x73, %rax
    movq %rax, %xmm3
    movq $0x74, %rax
    movq %rax, %xmm4
    movq $0x75, %rax
    movq %rax, %xmm5
    movq $0x76, %rax
    movq %rax, %xmm6
    movq $0x77, %rax
    movq %rax, %xmm7
    movq $0x88, %rax
    movq %rax, %xmm8
    movq $0x89, %rax
    movq %rax, %xmm9
    movq $0x8a, %rax
    movq %rax, %xmm10
    movq $0x8b, %rax
    movq %rax, %xmm11
    movq $0x8c, %rax
    movq %rax, %xmm12
    movq $0x8d, %rax
    movq %rax, %xmm13
    movq $0x8e, %rax
    movq %rax, %xmm14
    movq $0x8f, %rax
    movq %rax, %xmm15
    movq $0x5151, %rsi
    movq $0x7777, %rax
    stc
hook_guest_site:
    .byte 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    setc (%rdi)
    movq %r10, 8(%rdi)
    movq %r11, 16(%rdi)
    movq %xmm8, 24(%rdi)
    movq %xmm9, 32(%rdi)
    movq %xmm10, 40(%rdi)
    movq %xmm11, 48(%rdi)
    movq %xmm12, 56(%rdi)
    movq %xmm13, 64(%rdi)
    movq %xmm14, 72(%rdi)
    movq %xmm15, 80(%rdi)
    movq %rsi, 104(%rdi)
    movq %rax, 112(%rdi)
    movq %rcx, 120(%rdi)
    movq %rdx, 128(%rdi)
    movq %r8, 136(%rdi)
    movq %r9, 144(%rdi)
    movq %xmm0, 152(%rdi)
    movq %xmm1, 160(%rdi)
    movq %xmm2, 168(%rdi)
    movq %xmm3, 176(%rdi)
    movq %xmm4, 184(%rdi)
    movq %xmm5, 192(%rdi)
    movq %xmm6, 200(%rdi)
    movq %xmm7, 208(%rdi)
    movq -8(%rsp), %rcx
    movq %rcx, 88(%rdi)
    movq -128(%rsp), %rcx
    movq %rcx, 96(%rdi)
    leaq -8(%rbp), %rsp
    popq %r13
    popq %rbp
    ret
hook_guest_end:
hook_clobber:
    movq %rsp, %rax
    andq $15, %rax
    movq %rax, hook_clobber_align(%rip)
    movq 0x20(%rsi), %rax
    movq %rax, hook_clobber_seen_rsi(%rip)
    movq 0x58(%rsi), %rax
    movq %rax, hook_clobber_seen_r13(%rip)
    movq $0x9999, 0x20(%rsi)
    movq $-1, %rcx
    movq $-1, %rdx
    movq $-1, %r8
    movq $-1, %r9
    movq $-1, %r10
    movq $-1, %r11
    pcmpeqd %xmm0, %xmm0
    pcmpeqd %xmm1, %xmm1
    pcmpeqd %xmm2, %xmm2
    pcmpeqd %xmm3, %xmm3
    pcmpeqd %xmm4, %xmm4
    pcmpeqd %xmm5, %xmm5
    pcmpeqd %xmm6, %xmm6
    pcmpeqd %xmm7, %xmm7
    pcmpeqd %xmm8, %xmm8
    pcmpeqd %xmm9, %xmm9
    pcmpeqd %xmm10, %xmm10
    pcmpeqd %xmm11, %xmm11
    pcmpeqd %xmm12, %xmm12
    pcmpeqd %xmm13, %xmm13
    pcmpeqd %xmm14, %xmm14
    pcmpeqd %xmm15, %xmm15
    subq $256, %rsp
    movq %rcx, 0(%rsp)
    addq $256, %rsp
    xorl %eax, %eax
    movl $1, %eax
    ret
)");

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

struct Seen {
    unsigned calls = 0;
    std::uint64_t args[6]{};
    double xmm[2]{};
    bool skip = false;
    std::uint64_t value = 0;
} seen;

std::int64_t __attribute__((sysv_abi)) Watch(std::uint64_t, std::uint64_t* saved) noexcept {
    ++seen.calls;
    for (unsigned i = 0; i < 6; ++i) seen.args[i] = saved[5 - i];
    std::memcpy(&seen.xmm[0], BbEngine::SavedXmm(saved, 0), 8);
    std::memcpy(&seen.xmm[1], BbEngine::SavedXmm(saved, 1), 8);
    __asm__ volatile("xorps %%xmm0, %%xmm0\n\txorps %%xmm1, %%xmm1" ::: "xmm0", "xmm1");
    if (!seen.skip) return 0;
    saved[BbEngine::Saved::kResult] = seen.value;
    return 1;
}

constexpr std::size_t kPage = 4096;

std::uint8_t* Executable(std::size_t n) {
#ifdef _WIN32
    return static_cast<std::uint8_t*>(VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(p);
#endif
}

enum class Access { ReadExecute, ReadWrite };
void SetAccess(std::uint8_t* at, std::size_t n, Access access) {
#ifdef _WIN32
    DWORD before = 0;
    VirtualProtect(at, n, access == Access::ReadExecute ? PAGE_EXECUTE_READ : PAGE_READWRITE, &before);
#else
    mprotect(at, n, access == Access::ReadExecute ? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE);
#endif
}

bool HasAccess(std::uint8_t* at, Access access) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(at, &info, sizeof info)) return false;
    return info.Protect == (access == Access::ReadExecute ? PAGE_EXECUTE_READ : PAGE_READWRITE);
#else
    const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::vector<char> text;
    char chunk[4096];
    for (ssize_t got; (got = read(fd, chunk, sizeof chunk)) > 0;) text.insert(text.end(), chunk, chunk + got);
    close(fd);
    text.push_back('\0');
    const auto page = reinterpret_cast<std::uintptr_t>(at);
    for (const char* line = text.data(); *line;) {
        unsigned long long lo = 0, hi = 0;
        char perms[5] = {};
        if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, perms) == 3 && page >= lo && page < hi)
            return std::strncmp(perms, access == Access::ReadExecute ? "r-x" : "rw-", 3) == 0;
        const char* next = std::strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }
    return false;
#endif
}

std::uint8_t* LockedCopy(std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(kPage), nullptr);
    if (!section) return nullptr;
    void* fill = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, kPage);
    void* view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, kPage);
    CloseHandle(section);
    if (!fill || !view) return nullptr;
    std::memcpy(fill, bytes.data(), bytes.size());
    UnmapViewOfFile(fill);
    return static_cast<std::uint8_t*>(view);
#else
    const int fd = memfd_create("locked", MFD_CLOEXEC);
    if (fd < 0) return nullptr;
    const bool filled = ftruncate(fd, kPage) == 0 && write(fd, bytes.data(), bytes.size()) == ssize_t(bytes.size());
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    const int read_only = filled ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    close(fd);
    if (read_only < 0) return nullptr;
    void* view = mmap(nullptr, kPage, PROT_READ, MAP_SHARED, read_only, 0);
    close(read_only);
    return view == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(view);
#endif
}

__attribute__((noinline)) std::uint64_t CallWithRax(const void* fn, std::uint64_t rax) {
    std::uint64_t out = rax;
    __asm__ volatile("sub $128, %%rsp\n\t"
                     "call *%[fn]\n\t"
                     "add $128, %%rsp"
                     : "+a"(out)
                     : [fn] "r"(fn)
                     : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc", "xmm0", "xmm1",
                       "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
                       "xmm12", "xmm13", "xmm14", "xmm15");
    return out;
}

template <std::size_t N> void Place(std::uint8_t* at, const std::uint8_t (&code)[N]) {
    std::memcpy(at, code, N);
}

bool MovableEntry(const char* name, std::span<const std::uint8_t> bytes, bool report = true) {
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) return false;
    std::size_t at = 0;
    while (at < bytes.size()) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + at, bytes.size() - at, &instruction, operands))) {
            if (report) std::printf("FAIL: %s: no whole instruction at byte %zu\n", name, at);
            return false;
        }
        if (instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
            if (report) std::printf("FAIL: %s: the instruction at byte %zu is relative to the instruction pointer\n", name, at);
            return false;
        }
        for (std::uint8_t i = 0; i < instruction.operand_count; ++i) {
            const ZydisDecodedOperand& op = operands[i];
            if ((op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP) ||
                (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative)) {
                if (report) std::printf("FAIL: %s: operand %u at byte %zu is relative\n", name, unsigned(i), at);
                return false;
            }
        }
        at += instruction.length;
    }
    return at == bytes.size();
}

void EntryBytesAreMovable() {
    std::size_t movable = 0;
    for (const auto& entry : BbEngine::HookEntries::kAll) movable += MovableEntry(entry.name, entry.bytes);
    Check(movable == std::size(BbEngine::HookEntries::kAll), "every shared entry-byte constant is whole instructions with nothing relative");
    const std::uint8_t split[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec, 0x20};
    const std::uint8_t rip[] = {0x48, 0x8b, 0x05, 0x10, 0x00, 0x00, 0x00};
    const std::uint8_t call[] = {0xe8, 0x00, 0x00, 0x00, 0x00};
    const std::uint8_t branch[] = {0x74, 0x02};
    Check(!MovableEntry("split", split, false) && !MovableEntry("rip", rip, false) && !MovableEntry("call", call, false) &&
              !MovableEntry("branch", branch, false),
          "the check refuses a split instruction, a rip-relative operand and relative branches");
}

struct GuestOut {
    std::uint64_t carry;
    std::uint64_t r10, r11;
    std::uint64_t xmm_high[8];
    std::uint64_t red_near, red_far;
    std::uint64_t rsi, rax, rcx, rdx, r8, r9;
    std::uint64_t xmm_low[8];
};
static_assert(sizeof(GuestOut) == 216, "the guest writes 27 words");

bool GuestKept(const GuestOut& out) {
    bool xmm = true;
    for (unsigned i = 0; i < 8; ++i) xmm = xmm && out.xmm_high[i] == 0x88 + i && out.xmm_low[i] == 0x70 + i;
    return out.carry == 1 && out.r10 == 0xa10 && out.r11 == 0xb11 && out.rcx == 0xc1 && out.rdx == 0xd2 &&
           out.r8 == 0xe8 && out.r9 == 0xf9 && out.rax == 0x7777 && out.red_near == 0x1111 && out.red_far == 0x2222 && xmm;
}

void InsideStubKeepsState() {
    const std::size_t length = std::size_t(hook_guest_end - hook_guest_begin);
    const std::size_t site = std::size_t(hook_guest_site - hook_guest_begin);
    std::uint8_t* image = Executable(kPage);
    if (!image || length > kPage) { Check(false, "room for the guest"); return; }
    std::memcpy(image, hook_guest_begin, length);
    using Guest = void (__attribute__((sysv_abi)) *)(GuestOut*, std::uint64_t);
    const auto guest = reinterpret_cast<Guest>(image);
    constexpr std::uint64_t kSiteRspOdd = 0, kSiteRspEven = 8;
    GuestOut before{};
    guest(&before, kSiteRspOdd);
    Check(GuestKept(before) && before.rsi == 0x5151, "the guest runs unhooked");
    BbEngine::Hook hook;
    const std::vector<std::uint8_t> moved(image + site, image + site + 16);
    Check(BbEngine::Prepare(hook, image, kPage, site, BbEngine::SiteKind::Inside, moved, hook_clobber), "an inside site is prepared");
    Check(BbEngine::Commit(std::span(&hook, 1)) == BbEngine::WriteResult::Applied, "the inside hook is applied");
    for (const std::uint64_t parity : {kSiteRspOdd, kSiteRspEven}) {
        GuestOut out{};
        hook_clobber_align = 0xff;
        guest(&out, parity);
        Check(hook_clobber_align == 8, "the callback starts as after an aligned call, at either rsp parity of the site");
        Check(hook_clobber_seen_rsi == 0x5151, "saved[4] holds rsi at the site");
        Check(hook_clobber_seen_r13 == 0x1313, "the inside frame holds r13");
        Check(GuestKept(out), "flags, rax, rcx, rdx, r8-r11, xmm0-15 (low 128 bits) and the red zone survive a callback that clobbers them");
        Check(out.rsi == 0x9999, "a callback changes rsi through its frame");
    }
    BbEngine::Discard(hook);
}

void CommitIsAllOrNothing() {
    std::uint8_t* image = Executable(3 * kPage);
    if (!image) { Check(false, "room for the all-or-nothing test"); return; }
    std::array<std::uint8_t, 16> entry;
    entry.fill(0x90);
    std::memcpy(image, entry.data(), entry.size());
    std::memcpy(image + 2 * kPage, entry.data(), entry.size());
    const std::uint8_t game[] = {1, 2, 3, 4}, ours[] = {5, 6, 7, 8}, other[] = {9, 9, 9, 9};
    std::memcpy(image + 0x100, game, sizeof game);
    std::array<BbEngine::Hook, 2> hooks;
    Check(BbEngine::Prepare(hooks[0], image, 3 * kPage, 0, BbEngine::SiteKind::Entry, entry, Watch) &&
              BbEngine::Prepare(hooks[1], image, 3 * kPage, 2 * kPage, BbEngine::SiteKind::Entry, entry, Watch),
          "two hooks are prepared");
    const BbEngine::CodePatch wrong{image, 0x100, other, ours};
    Check(BbEngine::Commit(hooks, std::span(&wrong, 1)) == BbEngine::WriteResult::NotApplied,
          "a patch whose original differs stops the whole feature");
    Check(!std::memcmp(image, entry.data(), entry.size()) && !std::memcmp(image + 0x100, game, sizeof game),
          "nothing is written when an original differs");
    Check(!hooks[0].stub && !hooks[1].stub, "a feature that is not applied frees its stubs");

    Check(BbEngine::Prepare(hooks[0], image, 3 * kPage, 0, BbEngine::SiteKind::Entry, entry, Watch) &&
              BbEngine::Prepare(hooks[1], image, 3 * kPage, 2 * kPage, BbEngine::SiteKind::Entry, entry, Watch),
          "the hooks are prepared again");
    std::uint8_t* locked = LockedCopy(entry);
    if (!locked) { Check(false, "a page that cannot be made writable"); return; }
    BbEngine::Discard(hooks[1]);
    Check(BbEngine::Prepare(hooks[1], locked, kPage, 0, BbEngine::SiteKind::Entry, entry, Watch), "a hook on a locked page");
    const BbEngine::CodePatch right{image, 0x100, game, ours};
    Check(BbEngine::Commit(hooks, std::span(&right, 1)) == BbEngine::WriteResult::NotApplied,
          "a site that cannot be written stops the whole feature");
    Check(!std::memcmp(image, entry.data(), entry.size()) && !std::memcmp(image + 0x100, game, sizeof game),
          "what was written before the failure is put back");
}

void WriterKeepsProtection() {
    std::uint8_t* image = Executable(2 * kPage);
    if (!image) { Check(false, "room for the protection test"); return; }
    const std::uint8_t five[] = {0xb8, 5, 0, 0, 0, 0xc3};
    Place(image, five);
    SetAccess(image, kPage, Access::ReadExecute);
    SetAccess(image + kPage, kPage, Access::ReadWrite);
    const std::uint8_t game = 5, seven = 7;
    Check(BbEngine::WriteCode({image, 1, std::span(&game, 1), std::span(&seven, 1)}) == BbEngine::WriteResult::Applied,
          "WriteCode writes into read-execute code");
    Check(BbEngine::Call(reinterpret_cast<std::uint64_t>(image)) == 7, "the written code runs");
    Check(HasAccess(image, Access::ReadExecute), "a read-execute page is read-execute again");
    const std::uint8_t zero = 0;
    Check(BbEngine::WriteCode({image, kPage, std::span(&zero, 1), std::span(&seven, 1)}) == BbEngine::WriteResult::Applied &&
              image[kPage] == 7,
          "WriteCode writes into a read-write page");
    Check(HasAccess(image + kPage, Access::ReadWrite), "a read-write page stays read-write, not executable");
    Check(BbEngine::WriteCode({image, 1, std::span(&game, 1), std::span(&seven, 1)}) == BbEngine::WriteResult::NotApplied,
          "WriteCode refuses bytes that are not the expected original");
    Check(BbEngine::WriteCode({nullptr, 1, std::span(&game, 1), std::span(&seven, 1)}) == BbEngine::WriteResult::NotApplied,
          "WriteCode refuses a patch with no image pointer");
}
}

int main() {
    std::uint8_t* image = Executable(8192);
    if (!image) { std::puts("FAIL: no executable memory"); return 1; }
    std::array<std::uint8_t, 16> entry;
    entry.fill(0x90);
    const std::uint8_t sum[] = {0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x01, 0xd0, 0x48, 0x01,
                                0xc8, 0x4c, 0x01, 0xc0, 0x4c, 0x01, 0xc8, 0xc3};
    const auto fn = reinterpret_cast<std::uint64_t>(image);
    std::memcpy(image, entry.data(), entry.size());
    Place(image + 16, sum);
    using BbEngine::SiteKind;

    BbEngine::Hook hook;
    auto other = entry;
    other[3] = 0xcc;
    Check(!BbEngine::Prepare(hook, image, 8192, 0, SiteKind::Entry, other, Watch), "a site with other bytes is refused");
    Check(!BbEngine::Prepare(hook, image, 10, 0, SiteKind::Entry, entry, Watch), "a site past the image's end is refused");
    Check(!BbEngine::Prepare(hook, image, 8192, 0, SiteKind::Entry, std::span(entry).first(13), Watch),
          "fewer bytes than the jump are refused");
    Check(!hook.stub && !std::memcmp(image, entry.data(), entry.size()), "a refused hook changes nothing");

    Check(BbEngine::Call(fn, 1, 2, 3, 4, 5, 6) == 21, "the bridge passes six arguments");
    Check(BbEngine::Call(fn, 100) == 100, "registers a call does not fill are zero");
    Check(BbEngine::Prepare(hook, image, 8192, 0, SiteKind::Entry, entry, Watch), "a matching site is prepared");
    Check(!std::memcmp(image, entry.data(), entry.size()), "Prepare leaves the image alone");
    Check(BbEngine::Commit(std::span(&hook, 1)) == BbEngine::WriteResult::Applied, "the hook is applied");
    Check(image[0] == 0xff && image[1] == 0x25 && image[14] == 0xcc && image[15] == 0xcc,
          "Commit writes the jump and fills the rest with int3");
    Check(BbEngine::Call(fn, 1, 2, 3, 4, 5, 6) == 21, "the hooked function runs with its arguments");
    Check(seen.calls == 1, "the callback runs once per call");
    Check(seen.args[0] == 1 && seen.args[2] == 3 && seen.args[5] == 6, "saved[] holds r9 down to rdi");

    seen.skip = true;
    seen.value = 0x1234;
    Check(BbEngine::Call(fn, 1, 2, 3, 4, 5, 6) == 0x1234, "a skipped call returns saved[-1]");
    seen.skip = false;

    const std::uint8_t just_return[] = {0xc3};
    Place(image + 16, just_return);
    Check(CallWithRax(image, 0x5a5a) == 0x5a5a, "rax survives the stub");
    BbEngine::Discard(hook);
    Check(!hook.stub, "Discard clears the hook");

    std::memcpy(image, entry.data(), entry.size());
    const std::uint8_t add_doubles[] = {0xf2, 0x0f, 0x58, 0xc1, 0xc3};
    Place(image + 16, add_doubles);
    Check(BbEngine::Prepare(hook, image, 8192, 0, SiteKind::Entry, entry, Watch), "a second hook on the same site");
    Check(BbEngine::Commit(std::span(&hook, 1)) == BbEngine::WriteResult::Applied, "the second hook is applied");
    using Doubles = double (__attribute__((sysv_abi)) *)(double, double);
    Check(reinterpret_cast<Doubles>(image)(1.25, 3.5) == 4.75, "xmm0-1 reach the function");
    Check(seen.xmm[0] == 1.25 && seen.xmm[1] == 3.5, "SavedXmm reads xmm0-1");
    BbEngine::Discard(hook);

    const std::uint8_t seventh[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x45, 0x10, 0x4c, 0x01, 0xc8, 0x5d, 0xc3};
    Place(image + 0x100, seventh);
    Check(BbEngine::Call(fn + 0x100, 1, 2, 3, 4, 5, 6, 70) == 76, "the seventh argument is on the stack");

    seen = {};
    Place(image + 0x300, sum);
    std::uint8_t* tail = BbEngine::TailStub(Watch, fn + 0x300);
    Check(tail != nullptr, "a tail stub is made");
    if (tail) {
        std::uint8_t* p = image + 0x210;
        *p++ = 0x49;
        *p++ = 0xbb;
        const auto to = reinterpret_cast<std::uint64_t>(tail);
        std::memcpy(p, &to, 8);
        p += 8;
        *p++ = 0x41;
        *p++ = 0xff;
        *p = 0xe3;
        image[0x200] = 0xe9;
        const std::int32_t rel = 0x210 - (0x200 + 5);
        std::memcpy(image + 0x201, &rel, 4);
        Check(BbEngine::Call(fn + 0x200, 1, 2, 3, 4, 5, 6) == 21, "the tail call's target gets the arguments");
        Check(seen.calls == 1 && seen.args[4] == 5, "the tail stub calls the host first");
        BbEngine::FreeStub(tail);
    }

    std::vector<std::uint8_t*> many;
    for (int i = 0; i < 40; ++i) many.push_back(BbEngine::TailStub(Watch, fn + 0x300));
    bool all = true;
    for (auto* s : many) all = all && s;
    Check(all, "forty stubs are made");
    for (auto* s : many) BbEngine::FreeStub(s);
    std::uint8_t* again = BbEngine::TailStub(Watch, fn + 0x300);
    Check(again != nullptr, "a freed page is used again");
    BbEngine::FreeStub(again);

    WriterKeepsProtection();
    CommitIsAllOrNothing();
    InsideStubKeepsState();
    EntryBytesAreMovable();

    if (failures) return 1;
    std::printf("PASS: hook guards, applied or not as one unit, protection as found, saved registers and XMM, skip with "
                "saved[-1], rax kept, inside stub keeps flags, r10, r11, xmm8-15 and the red zone on any rsp, "
                "%zu shared entry-byte constants decode as movable, seven-argument bridge, tail stubs, stub pages reused\n",
                std::size(BbEngine::HookEntries::kAll));
    return 0;
}
