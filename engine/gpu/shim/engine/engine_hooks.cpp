// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine_hooks.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace BbEngine {
namespace {
constexpr std::size_t kPage = 4096;
constexpr std::size_t kReservation = 64 * 1024;
constexpr std::size_t kMaxPatchPages = 4;
constexpr std::uint8_t kTrap = 0xcc;

constexpr std::uint32_t kFrame = 0x98;
constexpr std::uint32_t kSavedRax = 0x88;
constexpr std::uint32_t kReturnSlot = 0x90;
static_assert(kFrame == kSavedXmm, "SavedXmm() reads the stub's frame");
static_assert(kReturnSlot + 8 == kFrame, "saved[-1] sits just below saved[0]");
static_assert((8 + 6 * 8 + kFrame) % 16 == 0, "the host call needs a 16-byte aligned stack");

constexpr std::uint32_t kRedZone = 128;
constexpr std::uint32_t kInsideSlots = 14;
constexpr std::uint32_t kInsideHighXmm = kFrame + 8 * kInsideSlots + 8;
constexpr std::uint32_t kInsideFrame = kInsideHighXmm + 16 * 8;
static_assert(kInsideFrame % 16 == 0, "the inside stub calls the host with rsp on a 16-byte boundary");
static_assert(InsideSaved::kR15 + 1 == kInsideSlots, "every inside slot has a register");

enum Reg : std::uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3, RBP = 5, RSI = 6, RDI = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14, R15 = 15
};
constexpr Reg kArgs[] = {RDI, RSI, RDX, RCX, R8, R9};
constexpr Reg kInside[kInsideSlots] = {R9, R8, RCX, RDX, RSI, RDI, R10, R11, RBX, RBP, R12, R13, R14, R15};
constexpr std::size_t kInsideRestored = InsideSaved::kR11 + 1;

class Emitter {
public:
    explicit Emitter(std::uint8_t* at) : at_(at) {}
    std::uint8_t* Here() const { return at_; }
    void Put(std::initializer_list<std::uint8_t> bytes) { for (auto b : bytes) *at_++ = b; }
    void Copy(const std::uint8_t* bytes, std::size_t n) { std::memcpy(at_, bytes, n); at_ += n; }
    template <typename T> void Value(T v) { std::memcpy(at_, &v, sizeof v); at_ += sizeof v; }

    void Push(Reg r) { if (r >= 8) Put({0x41}); Put({std::uint8_t(0x50 + (r & 7))}); }
    void Pop(Reg r) { if (r >= 8) Put({0x41}); Put({std::uint8_t(0x58 + (r & 7))}); }
    void SubRsp(std::uint32_t n) { Put({0x48, 0x81, 0xec}); Value(n); }
    void AddRsp(std::uint32_t n) { Put({0x48, 0x81, 0xc4}); Value(n); }
    void Store(std::uint32_t disp, Reg r) { Moved(0x89, r, disp); }
    void Load(Reg r, std::uint32_t disp) { Moved(0x8b, r, disp); }
    void StoreZero(std::uint32_t disp) { Put({0x48, 0xc7, 0x84, 0x24}); Value(disp); Value(std::uint32_t(0)); }
    void StoreXmm(std::uint32_t disp, unsigned n) { Vector(0x11, n, disp); }
    void LoadXmm(unsigned n, std::uint32_t disp) { Vector(0x10, n, disp); }
    void LeaRsi(std::uint32_t disp) { Put({0x48, 0x8d, 0xb4, 0x24}); Value(disp); }
    void StepRsp(std::int32_t by) { Put({0x48, 0x8d, 0xa4, 0x24}); Value(by); }
    void PushFlags() { Put({0x9c}); }
    void PopFlags() { Put({0x9d}); }
    void FrameFromRsp() { Put({0x48, 0x89, 0xe5}); }
    void RspFromFrame() { Put({0x48, 0x89, 0xec}); }
    void AlignRsp() { Put({0x48, 0x83, 0xe4, 0xf0}); }
    void LoadRaxFromFrame() { Put({0x48, 0x8b, 0x45, 0x00}); }
    void ZeroEdi() { Put({0x31, 0xff}); }
    void MovRax(std::uint64_t v) { Put({0x48, 0xb8}); Value(v); }
    void CallRax() { Put({0xff, 0xd0}); }
    void TestRax() { Put({0x48, 0x85, 0xc0}); }
    std::uint8_t* Jz8() { Put({0x74, 0x00}); return at_ - 1; }
    void Land(std::uint8_t* rel8) { *rel8 = std::uint8_t(at_ - (rel8 + 1)); }
    void Ret() { Put({0xc3}); }
    void JumpTo(std::uint64_t target) { Put({0xff, 0x25, 0, 0, 0, 0}); Value(target); }

private:
    void Moved(std::uint8_t op, Reg r, std::uint32_t disp) {
        Put({std::uint8_t(0x48 | ((r & 8) >> 1)), op, std::uint8_t(0x84 | ((r & 7) << 3)), 0x24});
        Value(disp);
    }
    void Vector(std::uint8_t op, unsigned n, std::uint32_t disp) {
        if (n & 8) Put({0x44});
        Put({0x0f, op, std::uint8_t(0x84 | ((n & 7) << 3)), 0x24});
        Value(disp);
    }
    std::uint8_t* at_;
};
constexpr std::size_t kJumpBytes = 14;

void CallHost(Emitter& e, Callback callback) {
    e.StoreZero(kReturnSlot);
    e.ZeroEdi();
    e.LeaRsi(kFrame);
    e.MovRax(reinterpret_cast<std::uint64_t>(callback));
    e.CallRax();
}

void WriteEntryStub(std::uint8_t* at, Callback callback, std::uint64_t resume, const std::uint8_t* moved, std::size_t n) {
    Emitter e(at);
    for (Reg r : kArgs) e.Push(r);
    e.SubRsp(kFrame);
    e.Store(kSavedRax, RAX);
    for (unsigned x = 0; x < Saved::kXmm; ++x) e.StoreXmm(16 * x, x);
    CallHost(e, callback);
    e.TestRax();
    std::uint8_t* run = e.Jz8();
    e.Load(RAX, kReturnSlot);
    e.AddRsp(kFrame);
    for (int i = 5; i >= 0; --i) e.Pop(kArgs[i]);
    e.Ret();
    e.Land(run);
    for (unsigned x = 0; x < Saved::kXmm; ++x) e.LoadXmm(x, 16 * x);
    e.Load(RAX, kSavedRax);
    e.AddRsp(kFrame);
    for (int i = 5; i >= 0; --i) e.Pop(kArgs[i]);
    if (n) e.Copy(moved, n);
    e.JumpTo(resume);
}

std::uint32_t XmmSlot(unsigned x) { return x < Saved::kXmm ? 16 * x : kInsideHighXmm + 16 * (x - Saved::kXmm); }

void WriteInsideStub(std::uint8_t* at, Callback callback, std::uint64_t resume, const std::uint8_t* moved, std::size_t n) {
    Emitter e(at);
    e.StepRsp(-std::int32_t(kRedZone));
    e.PushFlags();
    e.Push(RBP);
    e.FrameFromRsp();
    e.AlignRsp();
    e.SubRsp(kInsideFrame);
    e.Store(kSavedRax, RAX);
    e.LoadRaxFromFrame();
    for (std::uint32_t i = 0; i < kInsideSlots; ++i) e.Store(kFrame + 8 * i, kInside[i] == RBP ? RAX : kInside[i]);
    for (unsigned x = 0; x < 16; ++x) e.StoreXmm(XmmSlot(x), x);
    CallHost(e, callback);
    for (unsigned x = 0; x < 16; ++x) e.LoadXmm(x, XmmSlot(x));
    for (std::uint32_t i = 0; i < kInsideRestored; ++i) e.Load(kInside[i], kFrame + 8 * i);
    e.Load(RAX, kSavedRax);
    e.RspFromFrame();
    e.Pop(RBP);
    e.PopFlags();
    e.StepRsp(std::int32_t(kRedZone));
    if (n) e.Copy(moved, n);
    e.JumpTo(resume);
}

class StubPages {
public:
    template <typename W> std::uint8_t* Make(W&& write) {
        std::scoped_lock lock(mutex_);
        std::uint8_t* page = Take();
        if (!page) return nullptr;
        if (!Writable(page, true)) { free_.push_back(page); return nullptr; }
        write(page);
        if (!Writable(page, false)) { Give(page); return nullptr; }
        return page;
    }
    void Free(std::uint8_t* page) {
        if (!page) return;
        std::scoped_lock lock(mutex_);
        Give(page);
    }

private:
    std::uint8_t* Take() {
        if (!free_.empty()) { auto* p = free_.back(); free_.pop_back(); return p; }
        if (!next_ || next_ == end_) {
#ifdef _WIN32
            next_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kReservation, MEM_RESERVE, PAGE_NOACCESS));
#else
            void* r = mmap(nullptr, kReservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            next_ = r == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(r);
#endif
            if (!next_) { end_ = nullptr; return nullptr; }
            end_ = next_ + kReservation;
        }
        std::uint8_t* page = next_;
        next_ += kPage;
        return page;
    }
    void Give(std::uint8_t* page) {
#ifdef _WIN32
        VirtualFree(page, kPage, MEM_DECOMMIT);
#else
        mprotect(page, kPage, PROT_NONE);
        madvise(page, kPage, MADV_DONTNEED);
#endif
        free_.push_back(page);
    }
    static bool Writable(std::uint8_t* page, bool writable) {
#ifdef _WIN32
        if (writable) return VirtualAlloc(page, kPage, MEM_COMMIT, PAGE_READWRITE) != nullptr;
        DWORD before = 0;
        return VirtualProtect(page, kPage, PAGE_EXECUTE_READ, &before) &&
               FlushInstructionCache(GetCurrentProcess(), page, kPage);
#else
        if (mprotect(page, kPage, writable ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC)) return false;
        if (!writable) __builtin___clear_cache(reinterpret_cast<char*>(page), reinterpret_cast<char*>(page + kPage));
        return true;
#endif
    }

    std::mutex mutex_;
    std::uint8_t* next_ = nullptr;
    std::uint8_t* end_ = nullptr;
    std::vector<std::uint8_t*> free_;
};
StubPages& Pages() {
    static StubPages pages;
    return pages;
}

void FlushCode(void* at, std::size_t n) {
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), at, n);
#else
    __builtin___clear_cache(static_cast<char*>(at), static_cast<char*>(at) + n);
#endif
}

struct PageState {
    std::uintptr_t page = 0;
    unsigned long found = 0;
    bool changed = false;
};

#ifdef _WIN32
constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kProtectionModifiers = PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;

class Protections {
public:
    void Register(std::uintptr_t, std::size_t) {}
    void EndInstall() {}
    bool Ready() { return true; }
    bool Of(std::uintptr_t page, unsigned long& protect) const {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(page), &info, sizeof info) || info.State != MEM_COMMIT) return false;
        protect = info.Protect;
        return true;
    }
    void Note(std::uintptr_t, unsigned long) {}
};

bool WritableForm(unsigned long found, unsigned long& wanted) {
    if (found & kProtectionModifiers) return false;
    if (found & kWritable) {
        wanted = found;
        return true;
    }
    if (found == PAGE_READONLY) wanted = PAGE_READWRITE;
    else if (found == PAGE_EXECUTE_READ || found == PAGE_EXECUTE) wanted = PAGE_EXECUTE_READWRITE;
    else return false;
    return true;
}

bool SetProtection(std::uintptr_t page, unsigned long protect) {
    DWORD before = 0;
    return VirtualProtect(reinterpret_cast<void*>(page), kPage, DWORD(protect), &before) != 0;
}
#else
constexpr std::size_t PageIndex(std::uintptr_t page, std::uintptr_t first) { return (page - first) / kPage; }

struct MapRange {
    std::uintptr_t lo, hi;
    int prot;
};

bool ReadMaps(std::vector<MapRange>& ranges) {
    constexpr std::size_t kLine = 256, kChunk = 4096;
    ranges.clear();
    const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char line[kLine];
    std::size_t used = 0;
    char chunk[kChunk];
    bool ok = true;
    for (;;) {
        const ssize_t got = read(fd, chunk, sizeof chunk);
        if (got < 0) { ok = false; break; }
        if (got == 0) break;
        for (ssize_t i = 0; i < got; ++i) {
            if (chunk[i] != '\n') {
                if (used + 1 < sizeof line) line[used++] = chunk[i];
                continue;
            }
            line[used] = '\0';
            used = 0;
            unsigned long long lo = 0, hi = 0;
            char perms[5] = {};
            if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, perms) != 3) continue;
            const int prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
            ranges.push_back({std::uintptr_t(lo), std::uintptr_t(hi), prot});
        }
    }
    close(fd);
    return ok && !ranges.empty();
}

int RangeProtection(const std::vector<MapRange>& ranges, std::uintptr_t page) {
    for (const MapRange& r : ranges)
        if (page >= r.lo && page < r.hi) return r.prot;
    return -1;
}

class Protections {
public:
    void Register(std::uintptr_t first, std::size_t size) {
        if (image_pages_.size() || !first || !size) return;
        first_ = first & ~std::uintptr_t(kPage - 1);
        image_pages_.assign((first + size - first_ + kPage - 1) / kPage, kUnknown);
    }
    void EndInstall() {
        sealed_ = true;
        filled_ = false;
    }
    bool Ready() {
        if (!sealed_) return ReadMaps(ranges_);
        if (filled_) return true;
        if (!ReadMaps(ranges_)) return false;
        for (std::size_t i = 0; i < image_pages_.size(); ++i)
            image_pages_[i] = std::int8_t(RangeProtection(ranges_, first_ + i * kPage));
        filled_ = true;
        return true;
    }
    bool Of(std::uintptr_t page, unsigned long& prot) const {
        int found = kUnknown;
        if (sealed_ && InImage(page)) found = image_pages_[PageIndex(page, first_)];
        else found = RangeProtection(ranges_, page);
        if (found < 0) return false;
        prot = static_cast<unsigned long>(found);
        return true;
    }
    void Note(std::uintptr_t page, unsigned long prot) {
        if (sealed_ && InImage(page)) image_pages_[PageIndex(page, first_)] = std::int8_t(prot);
    }

private:
    static constexpr std::int8_t kUnknown = -1;
    bool InImage(std::uintptr_t page) const { return page >= first_ && PageIndex(page, first_) < image_pages_.size(); }
    std::uintptr_t first_ = 0;
    std::vector<std::int8_t> image_pages_;
    std::vector<MapRange> ranges_;
    bool sealed_ = false, filled_ = false;
};

bool WritableForm(unsigned long found, unsigned long& wanted) {
    wanted = found | PROT_WRITE;
    return true;
}

bool SetProtection(std::uintptr_t page, unsigned long protect) {
    return mprotect(reinterpret_cast<void*>(page), kPage, int(protect)) == 0;
}
#endif

bool Open(PageState& state, const Protections& protections) {
    unsigned long found = 0, wanted = 0;
    if (!protections.Of(state.page, found) || !WritableForm(found, wanted)) return false;
    if (wanted == found) return true;
    if (!SetProtection(state.page, wanted)) return false;
    state.found = found;
    state.changed = true;
    return true;
}

bool Close(const PageState& state) {
    return !state.changed || SetProtection(state.page, state.found);
}

struct PageSpan {
    std::uintptr_t first = 0;
    std::size_t pages = 0;
};
PageSpan PagesOf(const std::uint8_t* at, std::size_t n) {
    const auto start = reinterpret_cast<std::uintptr_t>(at);
    const std::uintptr_t first = start & ~std::uintptr_t(kPage - 1);
    return {first, (((start + n + kPage - 1) & ~std::uintptr_t(kPage - 1)) - first) / kPage};
}

struct Written {
    bool bytes = false;
    bool protection = true;
};

Written WriteBytes(std::uint8_t* at, std::span<const std::uint8_t> bytes, const Protections& protections) {
    const PageSpan span = PagesOf(at, bytes.size());
    if (bytes.empty() || span.pages > kMaxPatchPages) return {false, true};
    std::array<PageState, kMaxPatchPages> states{};
    std::size_t opened = 0;
    bool open = true;
    while (opened < span.pages && open) {
        states[opened].page = span.first + opened * kPage;
        open = Open(states[opened], protections);
        if (open) ++opened;
    }
    Written result;
    if (open) {
        std::memcpy(at, bytes.data(), bytes.size());
        FlushCode(at, bytes.size());
        result.bytes = true;
    }
    for (std::size_t i = 0; i < opened; ++i) result.protection = Close(states[i]) && result.protection;
    return result;
}

struct Item {
    std::uint8_t* at = nullptr;
    std::size_t offset = 0;
    std::span<const std::uint8_t> original;
    std::span<const std::uint8_t> wanted;
    bool written = false;
    bool open = false;
    bool no_image = false;
};

enum class Fault : std::uint8_t { NotPrepared, NoImage, NotExpected, NotWritten, LeftOpen, NotPutBack, NoProtections };
constexpr const char* kFaultText[] = {
    "is a hook that was not prepared; nothing written",
    "is a patch whose image pointer is missing; nothing written",
    "is not what the engine expects; nothing written",
    "could not be written",
    "was written, but its page could not get its protection back",
    "could not be put back as it was",
    "was not written: the page protections could not be read",
};
static_assert(std::size(kFaultText) == std::size_t(Fault::NoProtections) + 1, "one text per fault");

constexpr std::size_t kReportedFaults = 8;
class Faults {
public:
    void Add(std::size_t offset, Fault fault) {
        if (count_ < kReportedFaults) faults_[count_] = {offset, fault};
        ++count_;
    }
    void Print(WriteResult result) const {
        for (std::size_t i = 0; i < std::min(count_, kReportedFaults); ++i)
            std::printf("Engine: code at image offset %#zx %s\n", faults_[i].offset, kFaultText[std::size_t(faults_[i].fault)]);
        if (count_ > kReportedFaults) std::printf("Engine: %zu more code sites failed\n", count_ - kReportedFaults);
        if (result == WriteResult::Unknown)
            std::printf("Engine: game code writes could not all be undone; the code's state is unknown\n");
    }

private:
    struct Entry {
        std::size_t offset = 0;
        Fault fault = Fault::NotWritten;
    };
    std::array<Entry, kReportedFaults> faults_{};
    std::size_t count_ = 0;
};

struct Writer {
    std::mutex lock;
    Protections protections;
};
Writer& TheWriter() {
    static Writer writer;
    return writer;
}

WriteResult WriteLocked(std::span<Item> items, Protections& protections, Faults& faults) {
    bool same = true;
    for (const Item& item : items) {
        if (!item.at) {
            faults.Add(item.offset, item.no_image ? Fault::NoImage : Fault::NotPrepared);
            same = false;
        } else if (item.original.size() != item.wanted.size() || item.original.empty() ||
                   std::memcmp(item.at, item.original.data(), item.original.size()) != 0) {
            faults.Add(item.offset, Fault::NotExpected);
            same = false;
        }
    }
    if (!same) return WriteResult::NotApplied;
    if (!protections.Ready()) {
        faults.Add(items.front().offset, Fault::NoProtections);
        return WriteResult::NotApplied;
    }
    bool failed = false;
    for (Item& item : items) {
        const Written w = WriteBytes(item.at, item.wanted, protections);
        item.written = w.bytes;
        item.open = !w.protection;
        if (!w.bytes) faults.Add(item.offset, Fault::NotWritten);
        else if (!w.protection) faults.Add(item.offset, Fault::LeftOpen);
        failed = failed || !w.bytes || !w.protection;
    }
    if (!failed) return WriteResult::Applied;
    bool known = true;
    for (Item& item : items) {
        if (!item.written && !item.open) continue;
        const Written w = item.written ? WriteBytes(item.at, item.original, protections) : Written{true, false};
        if (!w.bytes || !w.protection || item.open) {
            faults.Add(item.offset, Fault::NotPutBack);
            known = false;
        }
    }
    return known ? WriteResult::NotApplied : WriteResult::Unknown;
}

WriteResult WriteAll(std::span<Item> items) {
    if (items.empty()) return WriteResult::Applied;
    Faults faults;
    WriteResult result;
    {
        Writer& writer = TheWriter();
        const std::scoped_lock lock(writer.lock);
        result = WriteLocked(items, writer.protections, faults);
    }
    faults.Print(result);
    return result;
}

constexpr std::size_t kFixedItems = 128;

template <typename F> WriteResult WithItems(std::size_t count, F&& fill) {
    if (count <= kFixedItems) {
        std::array<Item, kFixedItems> fixed;
        fill(std::span<Item>(fixed).first(count));
        return WriteAll(std::span<Item>(fixed).first(count));
    }
    std::vector<Item> grown(count);
    fill(std::span<Item>(grown));
    return WriteAll(grown);
}

void JumpBytes(Hook& hook) {
    hook.jump.fill(kTrap);
    Emitter e(hook.jump.data());
    e.JumpTo(reinterpret_cast<std::uint64_t>(hook.stub));
}
}

bool Prepare(Hook& hook, std::uint8_t* image, std::size_t size, std::size_t offset, SiteKind kind,
             std::span<const std::uint8_t> expected, Callback callback) {
    hook = {};
    const std::size_t n = expected.size();
    if (!image || !callback || n < kJumpBytes || n > kMaxMovedBytes || offset > size || n > size - offset) return false;
    std::uint8_t* site = image + offset;
    if (std::memcmp(site, expected.data(), n) != 0) return false;
    {
        Writer& writer = TheWriter();
        const std::scoped_lock lock(writer.lock);
        writer.protections.Register(reinterpret_cast<std::uintptr_t>(image), size);
    }
    const auto resume = reinterpret_cast<std::uint64_t>(site + n);
    std::uint8_t* stub = Pages().Make([&](std::uint8_t* page) {
        if (kind == SiteKind::Inside) WriteInsideStub(page, callback, resume, expected.data(), n);
        else WriteEntryStub(page, callback, resume, expected.data(), n);
    });
    if (!stub) return false;
    hook.site = site;
    hook.stub = stub;
    hook.bytes = n;
    hook.offset = offset;
    hook.kind = kind;
    std::memcpy(hook.moved.data(), expected.data(), n);
    JumpBytes(hook);
    return true;
}

std::uint8_t* OriginalStub(const Hook& hook) {
    if (!hook.site || !hook.stub) return nullptr;
    return Pages().Make([&](std::uint8_t* page) {
        Emitter e(page);
        e.Copy(hook.moved.data(), hook.bytes);
        e.JumpTo(reinterpret_cast<std::uint64_t>(hook.site + hook.bytes));
    });
}

void Discard(Hook& hook) {
    Pages().Free(hook.stub);
    hook = {};
}

std::uint8_t* TailStub(Callback callback, std::uint64_t resume) {
    if (!callback || !resume) return nullptr;
    return Pages().Make([&](std::uint8_t* page) { WriteEntryStub(page, callback, resume, nullptr, 0); });
}

void FreeStub(std::uint8_t* stub) {
    Pages().Free(stub);
}

WriteResult Commit(std::span<Hook> hooks, std::span<const CodePatch> patches) {
    const WriteResult result = WithItems(hooks.size() + patches.size(), [&](std::span<Item> items) {
        std::size_t i = 0;
        for (const Hook& hook : hooks) {
            const bool ready = hook.site && hook.stub;
            items[i++] = {ready ? hook.site : nullptr, hook.offset, std::span(hook.moved).first(hook.bytes),
                          std::span(hook.jump).first(hook.bytes)};
        }
        for (const CodePatch& patch : patches) {
            items[i] = {patch.image ? patch.image + patch.offset : nullptr, patch.offset, patch.original, patch.patched};
            items[i++].no_image = !patch.image;
        }
    });
    if (result == WriteResult::NotApplied) {
        for (Hook& hook : hooks) Discard(hook);
    }
    return result;
}

WriteResult WriteCode(std::span<const CodePatch> patches) {
    return Commit({}, patches);
}

void EndInstall() {
    Writer& writer = TheWriter();
    const std::scoped_lock lock(writer.lock);
    writer.protections.EndInstall();
}

WriteResult KeepWritable(std::uint8_t* image, std::span<const std::size_t> offsets, std::size_t width) {
    Faults faults;
    WriteResult result = WriteResult::Applied;
    {
        Writer& writer = TheWriter();
        const std::scoped_lock lock(writer.lock);
        if (!writer.protections.Ready()) {
            faults.Add(offsets.empty() ? 0 : offsets.front(), Fault::NoProtections);
            result = WriteResult::NotApplied;
        }
        for (std::size_t i = 0; i < offsets.size() && result == WriteResult::Applied; ++i) {
            const PageSpan span = PagesOf(image + offsets[i], width);
            for (std::size_t p = 0; p < span.pages; ++p) {
                PageState state;
                state.page = span.first + p * kPage;
                unsigned long found = 0, wanted = 0;
                if (!writer.protections.Of(state.page, found) || !WritableForm(found, wanted)) {
                    faults.Add(offsets[i], Fault::NotWritten);
                    result = WriteResult::NotApplied;
                    break;
                }
                if (wanted == found) continue;
                if (!SetProtection(state.page, wanted)) {
                    faults.Add(offsets[i], Fault::NotWritten);
                    result = WriteResult::NotApplied;
                    break;
                }
                writer.protections.Note(state.page, wanted);
            }
        }
    }
    faults.Print(result);
    return result;
}

const char* Describe(WriteResult result) {
    switch (result) {
    case WriteResult::Applied: return "applied";
    case WriteResult::NotApplied: return "not applied";
    case WriteResult::Unknown: return "state unknown";
    }
    return "state unknown";
}
}
