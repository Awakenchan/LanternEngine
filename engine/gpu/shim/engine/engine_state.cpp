// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine_state.h"
#include "trophy_rules.h"
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace BbEngine {
namespace {
constexpr std::size_t kGameDataMan = 0x553b130;
constexpr std::size_t kEventFlagMan = 0x553b100;
constexpr std::size_t kWorldChrMan = 0x553e878;
constexpr std::size_t kParamRepository = 0x5540340;

std::uint64_t g_image = 0;

template <typename T> bool Peek(std::uint64_t at, T* out) {
    return at && host_read_safe(reinterpret_cast<const void*>(at), out, sizeof(T));
}
std::uint64_t Follow(std::uint64_t at) {
    std::uint64_t v = 0;
    return Peek(at, &v) ? v : 0;
}
std::uint64_t Singleton(std::size_t slot) { return g_image ? Follow(g_image + slot) : 0; }

struct Stat { const char* name; std::uint32_t at; };
constexpr Stat kStats[] = {{"hp", 0x14},       {"vitality", 0x40},   {"endurance", 0x48}, {"strength", 0x58},
                           {"skill", 0x60},    {"bloodtinge", 0x68}, {"arcane", 0x70},    {"insight", 0x84},
                           {"level", 0x90},    {"echoes", 0x94}};
constexpr std::size_t kOrigin = 0xce;
const Stat* StatNamed(const char* name) {
    if (!name) return nullptr;
    for (const auto& s : kStats) if (!std::strcmp(s.name, name)) return &s;
    return nullptr;
}
std::atomic<std::uint64_t> g_said_player{0};

constexpr std::size_t kPlayerChr = 0x60, kPlayerBlock = 0x3f8;

constexpr std::size_t kFlagsPerBlock = 0x1c, kPoolStride = 0x20, kPool = 0x28, kBlocks = 0x38, kFlagMode = 0x80;

std::uint64_t FlagBlock(std::uint64_t man, std::uint32_t block, std::uint32_t* kind_out = nullptr) {
    const std::uint64_t head = Follow(man + kBlocks);
    std::uint64_t node = head ? Follow(head + 8) : 0, best = head;
    for (int depth = 0; node && depth < 64; ++depth) {
        std::uint8_t nil = 1;
        std::uint32_t key = 0;
        if (!Peek(node + 0x19, &nil) || nil || !Peek(node + 0x20, &key)) break;
        if (key < block) {
            node = Follow(node + 0x10);
        } else {
            best = node;
            node = Follow(node);
        }
    }
    std::uint32_t key = 0, kind = 0;
    if (!head || best == head || !Peek(best + 0x20, &key) || key != block || !Peek(best + 0x28, &kind)) return 0;
    if (kind_out) *kind_out = kind;
    if (kind == 2) return Follow(best + 0x30);
    if (kind != 1) return 0;
    std::uint32_t stride = 0, index = 0;
    const std::uint64_t pool = Follow(man + kPool);
    if (!pool || !Peek(man + kPoolStride, &stride) || !Peek(best + 0x30, &index)) return 0;
    return pool + std::uint32_t(stride * index);
}
struct LiveFlags {
    std::uint64_t man;
    bool Per(std::uint32_t* per) const { return Peek(man + kFlagsPerBlock, per); }
    bool Mode(std::int32_t* mode) const { return Peek(man + kFlagMode, mode); }
    std::uint64_t Block(std::uint32_t block, std::uint32_t* kind) const { return FlagBlock(man, block, kind); }
    bool Byte(std::uint64_t data, std::uint32_t at, std::uint8_t* out) const { return Peek(data + at, out); }
};
bool FlagByte(std::uint32_t id, std::uint64_t* byte, std::uint8_t* mask) {
    const std::uint64_t man = Singleton(kEventFlagMan);
    std::uint32_t per = 0;
    if (!man || !Peek(man + kFlagsPerBlock, &per) || !per) return false;
    const std::uint64_t data = FlagBlock(man, id / per);
    if (!data) return false;
    const std::uint32_t bit = id % per;
    *byte = data + bit / 8;
    *mask = std::uint8_t(0x80u >> (bit % 8));
    return true;
}

constexpr unsigned kSlots = 63;
constexpr std::size_t kSlotBase = 0x70, kSlotSize = 0x48;

bool NameIs(std::uint64_t cap, const char* wanted) {
    std::uint64_t capacity = 0;
    if (!Peek(cap + 0x30, &capacity)) return false;
    const std::uint64_t text = capacity < 8 ? cap + 0x18 : Follow(cap + 0x18);
    for (unsigned i = 0; i < 64; ++i) {
        char16_t c = 0;
        if (!Peek(text + 2 * i, &c) || c != char16_t(static_cast<unsigned char>(wanted[i]))) return false;
        if (!c) return true;
    }
    return false;
}
std::uint64_t FindCap(const char* wanted) {
    const std::uint64_t repository = Singleton(kParamRepository);
    if (!repository || !wanted) return 0;
    for (unsigned s = 0; s < kSlots; ++s) {
        const std::uint64_t slot = repository + kSlotBase + kSlotSize * s;
        std::int32_t count = 0;
        if (!Peek(slot, &count) || count <= 0) continue;
        for (int k = 0; k < count && k < 8; ++k) {
            const std::uint64_t cap = Follow(slot + 8 + 8 * k);
            if (cap && NameIs(cap, wanted)) return cap;
        }
    }
    return 0;
}
bool Describe(std::uint64_t cap, ParamTable* t) {
    t->cap = cap;
    t->file = params_table_file(cap);
    return t->file && Peek(t->file + 0xa, &t->rows) && t->rows && Peek(t->file + 0x2d, &t->format) &&
           Peek(t->file + 0x2e, &t->flags);
}
bool Wide(const ParamTable& t) { return t.format > 3 && (t.flags & 2); }
std::size_t Stride(const ParamTable& t) { return Wide(t) ? 0x18 : 12; }
std::uint64_t Records(const ParamTable& t) { return t.file + (t.format == 2 ? 0x34 : 0x40); }
bool RecordAt(const ParamTable& t, unsigned i, ParamRecord* r) {
    const std::uint64_t at = Records(t) + Stride(t) * i;
    std::uint64_t offset = 0;
    if (!Peek(at, &r->id)) return false;
    if (Wide(t)) {
        if (!Peek(at + 8, &offset)) return false;
    } else {
        std::uint32_t o = 0;
        if (!Peek(at + 4, &o)) return false;
        offset = o;
    }
    if (offset < 0x34 || offset > 64u << 20) return false;
    r->data = t.file + offset;
    return true;
}

struct Watch {
    std::uint64_t type = 0;
    const char16_t* name = nullptr;
    std::atomic<std::uint64_t> made{0};
};
std::array<Watch, 8> g_watches;
std::atomic<int> g_watch_count{0};
}

bool host_read_safe(const void* p, void* out, std::size_t n) { return p && BbPlatform::ReadProcessMemory(p, out, n); }

void host_log(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::fputs("Engine: ", stdout);
    std::vprintf(format, args);
    std::putchar('\n');
    va_end(args);
}

void StateInitialize(std::uint64_t image) { g_image = image; }

std::uint64_t player_record() {
    const std::uint64_t man = Singleton(kGameDataMan);
    const std::uint64_t record = man ? Follow(man + 8) : 0;
    if (record && g_said_player.load(std::memory_order_relaxed) != record) {
        std::int32_t level = 0, echoes = 0, insight = 0;
        if (Peek(record + 0x90, &level) && level > 0 && Peek(record + 0x94, &echoes) && Peek(record + 0x84, &insight) &&
            g_said_player.exchange(record) != record)
            host_log("player: level %d, %d blood echoes, %d insight", level, echoes, insight);
    }
    return record;
}

bool player_stat_get(const char* name, std::int64_t* value) {
    const Stat* s = StatNamed(name);
    const std::uint64_t record = s ? player_record() : 0;
    std::int32_t v = 0;
    if (!record || !Peek(record + s->at, &v)) return false;
    *value = v;
    return true;
}

bool player_stat_set(const char* name, std::int64_t value) {
    const Stat* s = StatNamed(name);
    const std::uint64_t record = s ? player_record() : 0;
    std::int32_t now = 0;
    if (!record || !Peek(record + s->at, &now)) return false;
    const auto v = static_cast<std::int32_t>(value);
    std::memcpy(reinterpret_cast<void*>(record + s->at), &v, sizeof v);
    return true;
}

bool player_origin(int* origin) {
    const std::uint64_t record = player_record();
    std::uint8_t v = 0;
    if (!record || !Peek(record + kOrigin, &v) || v > 8) return false;
    *origin = v;
    return true;
}

bool world_player_block(std::uint32_t* block) {
    const std::uint64_t man = Singleton(kWorldChrMan), chr = man ? Follow(man + kPlayerChr) : 0;
    std::uint32_t v = 0xffffffffu;
    if (!chr || !Peek(chr + kPlayerBlock, &v) || v == 0xffffffffu) return false;
    if (block) *block = v;
    return true;
}

bool event_flag_get(std::uint32_t id, bool* value) {
    std::uint64_t byte = 0;
    std::uint8_t mask = 0, v = 0;
    if (!FlagByte(id, &byte, &mask) || !Peek(byte, &v)) return false;
    if (value) *value = (v & mask) != 0;
    return true;
}

bool event_flag_get_own(std::uint32_t id, bool* value) {
    const std::uint64_t man = Singleton(kEventFlagMan);
    return man && TrophyRules::OwnFlag(LiveFlags{man}, id, value);
}

bool event_flag_mode(std::int32_t* mode) {
    const std::uint64_t man = Singleton(kEventFlagMan);
    return man && Peek(man + kFlagMode, mode);
}

bool event_flag_set(std::uint32_t id, bool value) {
    std::uint64_t byte = 0;
    std::uint8_t mask = 0, v = 0;
    if (!FlagByte(id, &byte, &mask) || !Peek(byte, &v)) return false;
    v = value ? std::uint8_t(v | mask) : std::uint8_t(v & ~mask);
    std::memcpy(reinterpret_cast<void*>(byte), &v, 1);
    return true;
}

void* params_row(const char* table, std::uint32_t id, std::size_t* size) {
    ParamTable t;
    const std::uint64_t cap = FindCap(table);
    if (!cap || !Describe(cap, &t)) return nullptr;
    unsigned lo = 0, hi = t.rows;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        ParamRecord r{};
        if (!RecordAt(t, mid, &r)) return nullptr;
        if (r.id == id) {
            if (size) {
                ParamRecord a{}, b{};
                *size = t.rows > 1 && RecordAt(t, 0, &a) && RecordAt(t, 1, &b) && b.data > a.data && b.data - a.data < 65536
                            ? std::size_t(b.data - a.data)
                            : 0;
            }
            return reinterpret_cast<void*>(r.data);
        }
        if (r.id < id) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

bool params_table(const char* table, ParamTable* out) {
    if (!out) return false;
    const std::uint64_t cap = FindCap(table);
    ParamTable t;
    if (!cap || !Describe(cap, &t)) return false;
    *out = t;
    return true;
}

std::uint64_t params_table_file(std::uint64_t cap) {
    const std::uint64_t holder = cap ? Follow(cap + 0x70) : 0;
    return holder ? Follow(holder + 0x70) : 0;
}

bool params_records(const ParamTable& t, std::vector<ParamRecord>* out) {
    if (!t.file || !t.rows || !out) return false;
    const std::size_t stride = Stride(t);
    std::vector<std::uint8_t> bytes(stride * t.rows);
    if (!host_read_safe(reinterpret_cast<const void*>(Records(t)), bytes.data(), bytes.size())) return false;
    out->clear();
    out->reserve(t.rows);
    for (std::size_t i = 0; i < t.rows; ++i) {
        const std::uint8_t* r = bytes.data() + stride * i;
        ParamRecord rec{};
        std::uint64_t offset = 0;
        std::memcpy(&rec.id, r, 4);
        if (Wide(t)) {
            std::memcpy(&offset, r + 8, 8);
        } else {
            std::uint32_t o = 0;
            std::memcpy(&o, r + 4, 4);
            offset = o;
        }
        if (offset < 0x34 || offset > 64u << 20) return false;
        rec.data = t.file + offset;
        out->push_back(rec);
    }
    return true;
}

int menu_steps_watch(std::uint64_t type, const char16_t* name) {
    if (!name) return -1;
    const int w = g_watch_count.load();
    if (w >= int(g_watches.size())) return -1;
    g_watches[w].type = type;
    g_watches[w].name = name;
    g_watch_count = w + 1;
    return w;
}

void menu_steps_notify(const std::uint64_t* saved) {
    const std::uint64_t name = saved[2];
    if (!name) return;
    const int n = g_watch_count.load();
    for (int w = 0; w < n; ++w) {
        Watch& x = g_watches[w];
        if (saved[3] != x.type) continue;
        for (unsigned i = 0; i < 64; ++i) {
            char16_t c = 0;
            if (!Peek(name + 2 * i, &c) || c != x.name[i]) break;
            if (!c) {
                x.made.store(saved[5], std::memory_order_release);
                break;
            }
        }
    }
}

std::uint64_t menu_steps_take(int w) {
    return w >= 0 && w < g_watch_count.load() ? g_watches[w].made.exchange(0, std::memory_order_acq_rel) : 0;
}

bool menu_step_is(std::uint64_t step, std::uint64_t type, const char16_t* name) {
    std::int32_t count = 0;
    std::uint64_t kind = 0, text = 0;
    if (!step || !name || !Peek(step + 8, &count) || count < 1 || !Peek(step + 0x28, &kind) || kind != type ||
        !Peek(step + 0x30, &text) || !text)
        return false;
    for (unsigned i = 0; i < 64; ++i) {
        char16_t c = 0;
        if (!Peek(text + 2 * i, &c) || c != name[i]) return false;
        if (!c) return true;
    }
    return false;
}

void menu_step_release(std::uint64_t step) {
    if (__atomic_sub_fetch(menu_step_count(step), 1, __ATOMIC_ACQ_REL) != 0) return;
    Call(Follow(Follow(step)), step);
}
}
