// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace BbEngine {
using Callback = std::int64_t (__attribute__((sysv_abi)) *)(std::uint64_t, std::uint64_t*) noexcept;

enum class SiteKind : std::uint8_t { Entry, Inside };
enum class [[nodiscard]] WriteResult : std::uint8_t { Applied, NotApplied, Unknown };

namespace Saved {
constexpr std::ptrdiff_t kResult = -1;
constexpr std::ptrdiff_t kR9 = 0, kR8 = 1, kRcx = 2, kRdx = 3, kRsi = 4, kRdi = 5;
constexpr std::ptrdiff_t kEntryReturn = 6, kEntryStackArgs = 7;
constexpr unsigned kXmm = 8;
}
namespace InsideSaved {
constexpr std::ptrdiff_t kR10 = 6, kR11 = 7, kRbx = 8, kRbp = 9, kR12 = 10, kR13 = 11, kR14 = 12, kR15 = 13;
}
constexpr std::ptrdiff_t kSavedXmm = 152;
inline std::uint8_t* SavedXmm(std::uint64_t* saved, unsigned n) {
    return reinterpret_cast<std::uint8_t*>(saved) - kSavedXmm + 16 * n;
}

constexpr std::size_t kMaxMovedBytes = 128;
struct Hook {
    std::uint8_t* site = nullptr;
    std::uint8_t* stub = nullptr;
    std::size_t bytes = 0;
    std::size_t offset = 0;
    SiteKind kind = SiteKind::Entry;
    std::array<std::uint8_t, kMaxMovedBytes> moved{};
    std::array<std::uint8_t, kMaxMovedBytes> jump{};
};

template <typename T, std::size_t N> std::span<const std::uint8_t> AsCode(std::span<T, N> values) {
    return {reinterpret_cast<const std::uint8_t*>(values.data()), values.size_bytes()};
}

struct CodePatch {
    std::uint8_t* image = nullptr;
    std::size_t offset = 0;
    std::span<const std::uint8_t> original;
    std::span<const std::uint8_t> patched;
};

bool Prepare(Hook& hook, std::uint8_t* image, std::size_t size, std::size_t offset, SiteKind kind,
             std::span<const std::uint8_t> expected, Callback callback);
std::uint8_t* OriginalStub(const Hook& hook);
void Discard(Hook& hook);
std::uint8_t* TailStub(Callback callback, std::uint64_t resume);
void FreeStub(std::uint8_t* stub);
WriteResult Commit(std::span<Hook> hooks, std::span<const CodePatch> patches = {});
WriteResult WriteCode(std::span<const CodePatch> patches);
inline WriteResult WriteCode(const CodePatch& patch) { return WriteCode(std::span<const CodePatch>(&patch, 1)); }
WriteResult KeepWritable(std::uint8_t* image, std::span<const std::size_t> offsets, std::size_t width);
void EndInstall();
const char* Describe(WriteResult result);

template <typename T> std::int64_t Word(T value) {
    if constexpr (std::is_pointer_v<T>) return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(value));
    else return static_cast<std::int64_t>(value);
}
template <typename... A> std::int64_t Call(std::uint64_t fn, A... args) {
    static_assert(sizeof...(A) <= 7, "the guest bridge passes at most seven arguments");
    using W = std::int64_t;
    if constexpr (sizeof...(A) == 7) {
        using Fn = W (__attribute__((sysv_abi)) *)(W, W, W, W, W, W, W);
        return reinterpret_cast<Fn>(fn)(Word(args)...);
    } else {
        std::array<W, 6> r{};
        std::size_t i = 0;
        ((r[i++] = Word(args)), ...);
        using Fn = W (__attribute__((sysv_abi)) *)(W, W, W, W, W, W);
        return reinterpret_cast<Fn>(fn)(r[0], r[1], r[2], r[3], r[4], r[5]);
    }
}
void Install(std::uint8_t* image, std::size_t size);
}
