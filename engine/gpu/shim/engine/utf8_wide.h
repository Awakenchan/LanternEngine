// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <string>

namespace BbEngine::Utf8 {
#ifdef LANTERN_UTF8_MUTANTS
enum class Mutant : int { None = 0, NoContinuationCheck, TruncatedKept, HighAscii, kCount };
inline int g_mutant = 0;
inline bool Is(Mutant m) { return g_mutant == int(m); }
#define LANTERN_U8_MUTANT(name) (::BbEngine::Utf8::Is(::BbEngine::Utf8::Mutant::name))
#else
#define LANTERN_U8_MUTANT(name) false
#endif

inline constexpr char32_t kReplacement = 0xFFFD;

inline bool IsAscii(const std::string& text) {
    for (const unsigned char c : text) {
        if (c >= (LANTERN_U8_MUTANT(HighAscii) ? 0xC0 : 0x80)) return false;
    }
    return true;
}

inline std::u32string ToCodePoints(const std::string& text) {
    std::u32string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        unsigned cp = 0;
        std::size_t extra = 0;
        unsigned least = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            extra = 1;
            least = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            extra = 2;
            least = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            extra = 3;
            least = 0x10000;
        } else {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        if (i + extra >= text.size() + (LANTERN_U8_MUTANT(TruncatedKept) ? 1 : 0) && extra) {
            out.push_back(kReplacement);
            break;
        }
        bool valid = true;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(i + k < text.size() ? text[i + k] : 0);
            if ((next & 0xC0) != 0x80 && !LANTERN_U8_MUTANT(NoContinuationCheck)) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (next & 0x3Fu);
        }
        if (!valid || cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        out.push_back(static_cast<char32_t>(cp));
        i += extra + 1;
    }
    return out;
}
}
