// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <string_view>

namespace Lantern::Shown {

inline constexpr std::size_t kPlainMax = 16;
inline constexpr std::size_t kNameMax = 96;
inline constexpr std::string_view kSecretWords[] = {"PASSWORD", "TOKEN",      "SECRET", "ACCOUNT", "AUTH",     "NPID",
                                                    "ONLINE_ID", "CREDENTIAL", "_KEY",   "PLAYER",  "USER_NAME", "TITLE",
                                                    "NAME",      "HOST",       "PEER",   "ROOM",    "JOIN",     "INVITE"};

constexpr char Upper(char c) {
    return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
}

constexpr bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Upper(a[i]) != Upper(b[i])) return false;
    }
    return true;
}

constexpr bool HasPrefix(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() && SameName(name.substr(0, prefix.size()), prefix);
}

constexpr bool UpperName(std::string_view name) {
    if (name.empty() || name.size() > kNameMax) return false;
    for (char c : name) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

constexpr bool SecretName(std::string_view name) {
    for (std::string_view word : kSecretWords) {
        for (std::size_t at = 0; at + word.size() <= name.size(); ++at) {
            if (SameName(name.substr(at, word.size()), word)) return true;
        }
    }
    return false;
}

constexpr bool Plain(std::string_view value, bool underscore) {
    if (value.empty() || value.size() > kPlainMax) return false;
    int dots = 0;
    for (char c : value) {
        if (c == '.' && ++dots > 1) return false;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '+' ||
                        c == '-' || (underscore && c == '_');
        if (!ok) return false;
    }
    return true;
}

constexpr char Printable(char c) {
    return (static_cast<unsigned char>(c) < 0x20 || c == ' ' || c == 0x7f) ? '_' : c;
}

}
