// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#ifdef LANTERN_CACHE_SETTINGS_MUTANTS
namespace BbEngine::CacheSettings {
inline int g_mutant = 0;
}
#define LANTERN_CACHE_SETTINGS_PICK(n, wrong, right) (::BbEngine::CacheSettings::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_CACHE_SETTINGS_PICK(n, wrong, right) (right)
#endif

namespace BbEngine::CacheSettings {
using u8 = std::uint8_t;
using u32 = std::uint32_t;

inline constexpr int kMutants = 15;
inline constexpr u32 kMagic = 0x3153434c;
inline constexpr u32 kVersion = 1;
inline constexpr std::size_t kFields = 4;
inline constexpr std::size_t kRecordBytes = 4 + 4 + kFields + 4;

enum Field : u8 { Lifter, DirectMemory, ModernSsr, PomCavity };
inline constexpr std::array<const char*, kFields> kFieldNames = {"LANTERN_LIFTER", "BB_DIRECT_MEMORY_ACCESS",
                                                                 "Modern SSR", "POM cavity"};
inline constexpr std::array<u8, kFields> kMaxValue = {2, 1, 1, 1};

struct Values {
    std::array<u8, kFields> field{};
    bool operator==(const Values&) const = default;
};

inline void PutWord(std::vector<u8>& out, u32 value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(value >> (8 * i)));
}

inline u32 GetWord(std::span<const u8> in, std::size_t at) {
    u32 value = 0;
    for (int i = 0; i < 4; ++i) value |= u32(in[at + i]) << (8 * i);
    return value;
}

inline void Append(std::vector<u8>& blob, const Values& values) {
    PutWord(blob, kMagic);
    PutWord(blob, kVersion);
    for (std::size_t i = 0; i < kFields; ++i) blob.push_back(values.field[i]);
    PutWord(blob, 0);
}

enum class Read : u8 { Empty, Absent, Valid, Unreadable, ForeignSize };

inline Read Split(std::span<const u8> blob, std::size_t profile_bytes, Values& out) {
    if (blob.empty()) return Read::Empty;
    if (blob.size() == LANTERN_CACHE_SETTINGS_PICK(1, profile_bytes + kRecordBytes, profile_bytes)) return Read::Absent;
    if (blob.size() != profile_bytes + kRecordBytes) return Read::ForeignSize;
    const std::size_t at = profile_bytes;
    if (LANTERN_CACHE_SETTINGS_PICK(2, false, GetWord(blob, at) != kMagic) || GetWord(blob, at + 4) != kVersion ||
        GetWord(blob, at + 8 + kFields) != 0)
        return Read::Unreadable;
    for (std::size_t i = 0; i < kFields; ++i) {
        const u8 value = blob[at + 8 + i];
        if (value > LANTERN_CACHE_SETTINGS_PICK(3, u8(2), LANTERN_CACHE_SETTINGS_PICK(9, u8(1), kMaxValue[i])))
            return Read::Unreadable;
        out.field[i] = value;
    }
    return Read::Valid;
}

inline bool Lifts(const Values& v) {
    return LANTERN_CACHE_SETTINGS_PICK(10, v.field[Lifter] == 1, v.field[Lifter] != 0);
}

enum class Verdict : u8 { Fresh, Adopt, Keep, Drop, Rebuild };

inline Verdict Decide(Read read, const Values& stored, const Values& now) {
    if (read == Read::Empty) return Verdict::Fresh;
    if (read == Read::Absent) return LANTERN_CACHE_SETTINGS_PICK(4, Verdict::Rebuild, Verdict::Adopt);
    if (read != Read::Valid) return LANTERN_CACHE_SETTINGS_PICK(5, Verdict::Keep, Verdict::Rebuild);
    if (stored.field[DirectMemory] != now.field[DirectMemory]) return Verdict::Rebuild;
    if (LANTERN_CACHE_SETTINGS_PICK(6, false, Lifts(stored) != Lifts(now))) return Verdict::Rebuild;
    if (Lifts(now) && (stored.field[ModernSsr] != now.field[ModernSsr] || stored.field[PomCavity] != now.field[PomCavity]))
        return LANTERN_CACHE_SETTINGS_PICK(11, Verdict::Rebuild, Verdict::Drop);
    return stored == now ? Verdict::Keep : LANTERN_CACHE_SETTINGS_PICK(12, Verdict::Keep, Verdict::Adopt);
}

inline int FirstDifference(const Values& stored, const Values& now) {
    for (std::size_t i = 0; i < kFields; ++i) {
        if (stored.field[i] != now.field[i]) return static_cast<int>(i);
    }
    return -1;
}

inline u8 LifterValue(bool on, bool compare) {
    return LANTERN_CACHE_SETTINGS_PICK(7, u8(on || compare), compare ? u8(2) : on ? u8(1) : u8(0));
}

inline Values Make(u8 lifter, bool direct_memory, bool modern_ssr, bool pom_cavity) {
    const bool effects = LANTERN_CACHE_SETTINGS_PICK(13, true, lifter != 0);
    Values v;
    v.field[Lifter] = lifter;
    v.field[DirectMemory] = direct_memory;
    v.field[ModernSsr] = LANTERN_CACHE_SETTINGS_PICK(8, u8(0), u8(effects && modern_ssr));
    v.field[PomCavity] = effects && pom_cavity;
    return v;
}

struct Opened {
    Read read = Read::Empty;
    Verdict verdict = Verdict::Fresh;
    Values stored;
    std::span<const u8> profile;
};

inline Opened Open(std::span<const u8> blob, std::size_t profile_bytes, const Values& now) {
    Opened opened;
    opened.read = Split(blob, profile_bytes, opened.stored);
    opened.verdict = Decide(opened.read, opened.stored, now);
    if (opened.verdict != Verdict::Fresh && opened.verdict != Verdict::Rebuild) {
        opened.profile = LANTERN_CACHE_SETTINGS_PICK(14, blob, blob.first(profile_bytes));
    }
    return opened;
}

inline std::optional<std::span<const u8>> ProfileOf(std::span<const u8> blob, std::size_t profile_bytes) {
    Values ignored;
    const Read read = Split(blob, profile_bytes, ignored);
    if (read != Read::Absent && read != Read::Valid) return std::nullopt;
    return blob.first(profile_bytes);
}

struct LabRecord {
    bool recorded = false;
    Values stored;
    Values compile;
    std::span<const u8> profile;
};

inline std::optional<LabRecord> OpenForLab(std::span<const u8> blob, std::size_t profile_bytes, bool direct_memory,
                                           u8 lifter) {
    LabRecord lab;
    const Read read = Split(blob, profile_bytes, lab.stored);
    if (read != Read::Absent && read != Read::Valid) return std::nullopt;
    lab.recorded = read == Read::Valid;
    if (lab.recorded && lab.stored.field[DirectMemory] != u8(direct_memory)) return std::nullopt;
    lab.compile = lab.recorded ? lab.stored
                               : Make(LANTERN_CACHE_SETTINGS_PICK(15, u8(0), lifter), direct_memory, false, false);
    lab.profile = blob.first(profile_bytes);
    return lab;
}

inline std::vector<u8> LabStamped(std::span<const u8> profile, const Values& compiled) {
    std::vector<u8> blob(profile.begin(), profile.end());
    Append(blob, compiled);
    return blob;
}
}
