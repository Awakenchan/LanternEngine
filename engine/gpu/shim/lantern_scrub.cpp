// SPDX-License-Identifier: GPL-3.0-or-later
#include "lantern_scrub.h"

#include <atomic>
#include <cstring>

namespace Lantern::Scrub {

namespace {

constexpr std::size_t kMinSecretBytes = 3;
constexpr int kIpv4Parts = 4;
constexpr int kIpv4PartDigits = 3;
constexpr int kSaveDigits = 4;
constexpr int kMinIpv6Colons = 2;

struct Secret {
    char text[kSecretBytes];
    std::size_t size;
    Kind kind;
};

struct Table {
    Secret secrets[kMaxSecrets];
    std::atomic<int> count{0};
};

Table g_table;

constexpr std::string_view kUrlSchemes[] = {"wss://", "ws://", "https://", "http://"};
constexpr std::string_view kUserRoots[] = {"\\users\\", "/users/", "/home/"};
constexpr std::string_view kKeywords[] = {"password", "token", "invite", "code", "key"};
constexpr std::string_view kOneDrive = "onedrive - ";
constexpr std::string_view kSave = "sprj";

constexpr char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}

constexpr bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

constexpr bool IsAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

constexpr bool IsHex(char c) {
    return IsDigit(c) || (Lower(c) >= 'a' && Lower(c) <= 'f');
}

constexpr bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr bool EndsValue(char c) {
    return IsSpace(c) || c == ';' || c == ',' || c == ')' || c == '"' || c == '\'' || c == '|';
}

bool StartsWithFolded(std::string_view text, std::size_t at, std::string_view prefix) {
    if (at + prefix.size() > text.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (Lower(text[at + i]) != Lower(prefix[i])) return false;
    }
    return true;
}

const char* Placeholder(Kind kind) {
    switch (kind) {
    case Kind::Home: return "<home>";
    case Kind::User: return "<user>";
    case Kind::OneDrive: return "<onedrive>";
    case Kind::Account: return "<account>";
    case Kind::Room: return "<room>";
    case Kind::Server: return "<server>";
    case Kind::Secret: break;
    }
    return "<secret>";
}

struct Writer {
    char* out;
    std::size_t size;
    std::size_t used = 0;
    void Put(std::string_view s) {
        for (char c : s) {
            if (used + 1 >= size) return;
            out[used++] = c;
        }
    }
};

std::size_t SecretAt(std::string_view in, std::size_t at, Kind* kind) {
    std::size_t best = 0;
    const int count = g_table.count.load(std::memory_order_acquire);
    for (int s = 0; s < count; ++s) {
        const Secret& secret = g_table.secrets[s];
        if (secret.size > best && StartsWithFolded(in, at, std::string_view(secret.text, secret.size))) {
            best = secret.size;
            *kind = secret.kind;
        }
    }
    return best;
}

std::size_t SkipToSeparator(std::string_view in, std::size_t at) {
    while (at < in.size() && in[at] != '\\' && in[at] != '/' && in[at] != '"' && in[at] != ';' && in[at] != '\n' &&
           in[at] != '\r') {
        ++at;
    }
    return at;
}

std::size_t Ipv4At(std::string_view in, std::size_t at) {
    if (at > 0 && (IsDigit(in[at - 1]) || in[at - 1] == '.' || IsAlpha(in[at - 1]))) return 0;
    std::size_t p = at;
    for (int part = 0; part < kIpv4Parts; ++part) {
        int digits = 0;
        while (p < in.size() && IsDigit(in[p]) && digits < kIpv4PartDigits + 1) {
            ++p;
            ++digits;
        }
        if (digits == 0 || digits > kIpv4PartDigits) return 0;
        if (part + 1 < kIpv4Parts) {
            if (p >= in.size() || in[p] != '.') return 0;
            ++p;
        }
    }
    if (p < in.size() && (IsDigit(in[p]) || (in[p] == '.' && p + 1 < in.size() && IsDigit(in[p + 1])))) return 0;
    return p - at;
}

std::size_t Ipv6At(std::string_view in, std::size_t at) {
    if (in[at] != '[') return 0;
    std::size_t p = at + 1;
    int colons = 0;
    while (p < in.size() && (IsHex(in[p]) || in[p] == ':' || in[p] == '.')) {
        colons += in[p] == ':';
        ++p;
    }
    if (p >= in.size() || in[p] != ']' || colons < kMinIpv6Colons) return 0;
    return p + 1 - at;
}

std::size_t SaveAt(std::string_view in, std::size_t at) {
    if (!StartsWithFolded(in, at, kSave)) return 0;
    std::size_t p = at + kSave.size();
    for (int d = 0; d < kSaveDigits; ++d, ++p) {
        if (p >= in.size() || !IsDigit(in[p])) return 0;
    }
    return p - at;
}

bool KeywordAt(std::string_view in, std::size_t at, std::size_t* value_start) {
    if (at > 0 && IsAlpha(in[at - 1])) return false;
    for (std::string_view word : kKeywords) {
        if (!StartsWithFolded(in, at, word)) continue;
        std::size_t p = at + word.size();
        if (p < in.size() && IsAlpha(in[p])) continue;
        while (p < in.size() && in[p] == ' ') ++p;
        if (p >= in.size() || (in[p] != '=' && in[p] != ':')) continue;
        ++p;
        while (p < in.size() && in[p] == ' ') ++p;
        *value_start = p;
        return true;
    }
    return false;
}

}

bool AddSecret(std::string_view value, Kind kind) noexcept {
    if (value.size() < kMinSecretBytes) return false;
    const int at = g_table.count.load(std::memory_order_relaxed);
    if (at >= kMaxSecrets || value.size() >= kSecretBytes) return false;
    Secret& s = g_table.secrets[at];
    std::memcpy(s.text, value.data(), value.size());
    s.size = value.size();
    s.kind = kind;
    g_table.count.store(at + 1, std::memory_order_release);
    return true;
}

void ClearSecretsForTest() noexcept {
    g_table.count.store(0, std::memory_order_release);
}

int SecretCount() noexcept {
    return g_table.count.load(std::memory_order_acquire);
}

std::size_t Text(std::string_view in, char* out, std::size_t size) noexcept {
    if (size == 0) return 0;
    Writer w{out, size};
    std::size_t i = 0;
    while (i < in.size()) {
        Kind kind = Kind::Secret;
        if (const std::size_t n = SecretAt(in, i, &kind)) {
            w.Put(Placeholder(kind));
            i += n;
            continue;
        }
        bool done = false;
        for (std::string_view scheme : kUrlSchemes) {
            if (!StartsWithFolded(in, i, scheme)) continue;
            std::size_t p = i + scheme.size();
            while (p < in.size() && !EndsValue(in[p])) ++p;
            w.Put("<server>");
            i = p;
            done = true;
            break;
        }
        if (done) continue;
        for (std::string_view root : kUserRoots) {
            if (!StartsWithFolded(in, i, root)) continue;
            w.Put(in.substr(i, root.size()));
            w.Put("<user>");
            i = SkipToSeparator(in, i + root.size());
            done = true;
            break;
        }
        if (done) continue;
        if (StartsWithFolded(in, i, kOneDrive)) {
            w.Put("<onedrive>");
            i = SkipToSeparator(in, i + kOneDrive.size());
            continue;
        }
        std::size_t value_start = 0;
        if (KeywordAt(in, i, &value_start)) {
            w.Put(in.substr(i, value_start - i));
            std::size_t p = value_start;
            while (p < in.size() && !EndsValue(in[p])) ++p;
            if (p > value_start) w.Put("<secret>");
            i = p;
            continue;
        }
        if (const std::size_t n = Ipv4At(in, i)) {
            w.Put("<address>");
            i += n;
            continue;
        }
        if (const std::size_t n = Ipv6At(in, i)) {
            w.Put("<address>");
            i += n;
            continue;
        }
        if (const std::size_t n = SaveAt(in, i)) {
            w.Put("<save>");
            i += n;
            continue;
        }
        w.Put(in.substr(i, 1));
        ++i;
    }
    w.out[w.used] = 0;
    return w.used;
}

}
