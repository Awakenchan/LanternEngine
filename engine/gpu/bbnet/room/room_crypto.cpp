// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_crypto.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

namespace BbRoom {
namespace {

constexpr std::string_view kRoomDomain = "lantern-room";
constexpr std::string_view kRoomSalt = "lantern-seamless-coop";
constexpr std::string_view kRoomIdLabel = "room id";
constexpr std::string_view kFrameKeyLabel = "frame key";
constexpr std::size_t kMostPayload = static_cast<std::size_t>(std::numeric_limits<int>::max()) - kSealOverhead;
constexpr int kPbkdf2WithoutSp800Checks = 1;

struct CipherFree {
    void operator()(EVP_CIPHER* cipher) const { EVP_CIPHER_free(cipher); }
};
struct CipherCtxFree {
    void operator()(EVP_CIPHER_CTX* ctx) const { EVP_CIPHER_CTX_free(ctx); }
};
struct KdfFree {
    void operator()(EVP_KDF* kdf) const { EVP_KDF_free(kdf); }
};
struct KdfCtxFree {
    void operator()(EVP_KDF_CTX* ctx) const { EVP_KDF_CTX_free(ctx); }
};

using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxFree>;
using KdfCtx = std::unique_ptr<EVP_KDF_CTX, KdfCtxFree>;

#ifdef LANTERN_ROOM_RANDOM_SEAM
std::atomic<RandomFill> g_random_fill{nullptr};
#endif

const EVP_CIPHER* Aes256Gcm() {
    static const std::unique_ptr<EVP_CIPHER, CipherFree> cipher{EVP_CIPHER_fetch(nullptr, "AES-256-GCM", nullptr)};
    return cipher.get();
}

EVP_KDF* Pbkdf2() {
    static const std::unique_ptr<EVP_KDF, KdfFree> kdf{EVP_KDF_fetch(nullptr, "PBKDF2", nullptr)};
    return kdf.get();
}

std::vector<std::uint8_t> RoomInput(const std::string& name, const std::string& password) {
    std::vector<std::uint8_t> input(kRoomDomain.size() + 1 + name.size() + 1 + password.size());
    std::uint8_t* at = input.data();
    std::memcpy(at, kRoomDomain.data(), kRoomDomain.size());
    at += kRoomDomain.size() + 1;
    if (!name.empty()) std::memcpy(at, name.data(), name.size());
    at += name.size() + 1;
    if (!password.empty()) std::memcpy(at, password.data(), password.size());
    return input;
}

bool Pbkdf2Sha256(std::span<std::uint8_t> password, int iterations, std::span<std::uint8_t> out) {
    if (iterations <= 0) return false;
    EVP_KDF* kdf = Pbkdf2();
    if (!kdf) return false;
    const KdfCtx ctx{EVP_KDF_CTX_new(kdf)};
    if (!ctx) return false;
    char digest[] = "SHA256";
    std::uint64_t iter = static_cast<std::uint64_t>(iterations);
    int pkcs5 = kPbkdf2WithoutSp800Checks;
    const OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, password.data(), password.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<char*>(kRoomSalt.data()), kRoomSalt.size()),
        OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_ITER, &iter),
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_PKCS5, &pkcs5),
        OSSL_PARAM_construct_end(),
    };
    return EVP_KDF_derive(ctx.get(), out.data(), out.size(), params) == 1;
}

bool HmacSha256(std::span<const std::uint8_t> key, std::string_view label, Key& out) {
    std::size_t len = 0;
    const unsigned char* made =
        EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key.data(), key.size(),
                  reinterpret_cast<const unsigned char*>(label.data()), label.size(), out.data(), out.size(), &len);
    return made && len == out.size();
}

std::string Hex(std::span<const std::uint8_t> data) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(data.size() * 2);
    for (const std::uint8_t byte : data) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 15]);
    }
    return text;
}

CipherCtx GcmContext(bool encrypt, const Key& key, const std::uint8_t* nonce) {
    const EVP_CIPHER* cipher = Aes256Gcm();
    CipherCtx ctx{EVP_CIPHER_CTX_new()};
    if (!cipher || !ctx) return nullptr;
    std::size_t iv_len = kNonceLen;
    const OSSL_PARAM params[] = {
        OSSL_PARAM_construct_size_t(OSSL_CIPHER_PARAM_AEAD_IVLEN, &iv_len),
        OSSL_PARAM_construct_end(),
    };
    const auto init = encrypt ? EVP_EncryptInit_ex2 : EVP_DecryptInit_ex2;
    if (init(ctx.get(), cipher, nullptr, nullptr, params) != 1) return nullptr;
    if (init(ctx.get(), nullptr, key.data(), nonce, nullptr) != 1) return nullptr;
    return ctx;
}

bool Encrypt(EVP_CIPHER_CTX* ctx, const std::uint8_t* in, std::size_t size, std::uint8_t* out, std::size_t& written) {
    if (!size) return true;
    int len = 0;
    if (EVP_EncryptUpdate(ctx, out + written, &len, in, static_cast<int>(size)) != 1) return false;
    written += static_cast<std::size_t>(len);
    return true;
}

bool GetTag(EVP_CIPHER_CTX* ctx, std::uint8_t* tag) {
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, tag, kTagLen),
        OSSL_PARAM_construct_end(),
    };
    return EVP_CIPHER_CTX_get_params(ctx, params) == 1;
}

bool SetTag(EVP_CIPHER_CTX* ctx, std::uint8_t* tag) {
    const OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, tag, kTagLen),
        OSSL_PARAM_construct_end(),
    };
    return EVP_CIPHER_CTX_set_params(ctx, params) == 1;
}

} // namespace

bool RandomBytes(std::span<std::uint8_t> out) {
    if (out.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    const int size = static_cast<int>(out.size());
#ifdef LANTERN_ROOM_RANDOM_SEAM
    if (const RandomFill fill = g_random_fill.load(std::memory_order_relaxed)) return fill(out.data(), size) == 1;
#endif
    return RAND_bytes(out.data(), size) == 1;
}

void WipeKey(Key& key) { OPENSSL_cleanse(key.data(), key.size()); }

#ifdef LANTERN_ROOM_RANDOM_SEAM
void SetRandomFill(RandomFill fill) { g_random_fill.store(fill, std::memory_order_relaxed); }
#endif

std::optional<Secret> Derive(const std::string& name, const std::string& password, int iterations) {
    std::vector<std::uint8_t> input = RoomInput(name, password);
    Key master{};
    Key id{};
    std::optional<Secret> secret{std::in_place};
    const bool ok = Pbkdf2Sha256(input, iterations, master) && HmacSha256(master, kRoomIdLabel, id) &&
                    HmacSha256(master, kFrameKeyLabel, secret->key);
    OPENSSL_cleanse(input.data(), input.size());
    OPENSSL_cleanse(master.data(), master.size());
    if (!ok) {
        WipeKey(secret->key);
        return std::nullopt;
    }
    secret->room_id = Hex(id);
    return secret;
}

std::vector<std::uint8_t> Seal(const Key& key, std::uint8_t sender, const std::uint8_t* data, std::size_t size) {
    if (size > kMostPayload || (size && !data)) return {};
    std::vector<std::uint8_t> out(kSealOverhead + size);
    if (!RandomBytes(std::span(out).first(kNonceLen))) return {};
    const CipherCtx ctx = GcmContext(true, key, out.data());
    if (!ctx) return {};
    std::uint8_t* const cipher = out.data() + kNonceLen;
    std::size_t written = 0;
    int tail = 0;
    const bool ok = Encrypt(ctx.get(), &sender, 1, cipher, written) && Encrypt(ctx.get(), data, size, cipher, written) &&
                    EVP_EncryptFinal_ex(ctx.get(), cipher + written, &tail) == 1 &&
                    written + static_cast<std::size_t>(tail) == 1 + size && GetTag(ctx.get(), cipher + 1 + size);
    if (!ok) return {};
    return out;
}

std::optional<std::vector<std::uint8_t>> Open(const Key& key, std::uint8_t sender, const std::uint8_t* data,
                                              std::size_t size) {
    if (!data || size < kSealOverhead || size - kSealOverhead > kMostPayload) return std::nullopt;
    const std::size_t cipher_len = size - kNonceLen - kTagLen;
    std::array<std::uint8_t, kTagLen> tag{};
    std::memcpy(tag.data(), data + size - kTagLen, kTagLen);
    const CipherCtx ctx = GcmContext(false, key, data);
    if (!ctx) return std::nullopt;
    std::vector<std::uint8_t> plain(cipher_len);
    int len = 0;
    int tail = 0;
    const bool ok =
        EVP_DecryptUpdate(ctx.get(), plain.data(), &len, data + kNonceLen, static_cast<int>(cipher_len)) == 1 &&
        SetTag(ctx.get(), tag.data()) && EVP_DecryptFinal_ex(ctx.get(), plain.data() + len, &tail) == 1 &&
        static_cast<std::size_t>(len) + static_cast<std::size_t>(tail) == cipher_len;
    if (!ok || plain[0] != sender) {
        OPENSSL_cleanse(plain.data(), plain.size());
        return std::nullopt;
    }
    plain.erase(plain.begin());
    return plain;
}

} // namespace BbRoom
