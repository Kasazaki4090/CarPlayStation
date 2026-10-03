#pragma once
// Crypto primitives for CarPlay pairing: SHA-512, HKDF-SHA512, ChaCha20, Ed25519.
// Uses Windows CNG (bcrypt) for SHA-512/HKDF/Ed25519; pure C++ for ChaCha20.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include "Core/Session/Pairing/Ed25519.h"
#include "Core/Session/Pairing/Curve25519.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <array>
#include <string>
#include <optional>

#pragma comment(lib, "bcrypt.lib")

namespace pairing {

// ─── SHA-512 ─────────────────────────────────────────────────────────────────

inline std::vector<uint8_t> Sha512(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<uint8_t> out(64);
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA512_ALGORITHM, nullptr, 0) != 0)
        return out;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return out;
    }
    BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(data)),
                   static_cast<ULONG>(len), 0);
    BCryptFinishHash(hHash, out.data(), 64, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return out;
}

inline std::vector<uint8_t> Sha512(const std::vector<std::vector<uint8_t>>& parts) {
    std::vector<uint8_t> flat;
    for (auto& p : parts) flat.insert(flat.end(), p.begin(), p.end());
    return Sha512(flat.data(), flat.size());
}

// ─── SHA-1 ───────────────────────────────────────────────────────────────────

inline std::vector<uint8_t> Sha1(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<uint8_t> out(20);
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return out;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return out;
    }
    BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(data)),
                   static_cast<ULONG>(len), 0);
    BCryptFinishHash(hHash, out.data(), 20, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return out;
}

// ─── SHA-256 ─────────────────────────────────────────────────────────────────

inline std::vector<uint8_t> Sha256(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<uint8_t> out(32);
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return out;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return out;
    }
    BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(data)),
                   static_cast<ULONG>(len), 0);
    BCryptFinishHash(hHash, out.data(), 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return out;
}


// ─── HMAC-SHA512 ─────────────────────────────────────────────────────────────

// RFC 2104 HMAC with SHA-512 (block size 128). Handles keys longer than the block
// by hashing them first, per the standard.
inline std::vector<uint8_t> HmacSha512(const uint8_t* key, size_t keyLen,
                                        const uint8_t* msg, size_t msgLen) {
    constexpr size_t BLOCK = 128;

    // Normalize the key to at most BLOCK bytes.
    std::vector<uint8_t> k(key, key + keyLen);
    if (k.size() > BLOCK) k = Sha512(k.data(), k.size());

    // Pad the key to BLOCK bytes.
    std::vector<uint8_t> kpad(BLOCK, 0);
    for (size_t i = 0; i < k.size(); ++i) kpad[i] = k[i];

    std::vector<uint8_t> inner(BLOCK), outer(BLOCK);
    for (size_t i = 0; i < BLOCK; ++i) {
        inner[i] = kpad[i] ^ 0x36;
        outer[i] = kpad[i] ^ 0x5c;
    }

    // innerHash = SHA512((key XOR ipad) || msg)
    std::vector<uint8_t> innerMsg;
    innerMsg.insert(innerMsg.end(), inner.begin(), inner.end());
    innerMsg.insert(innerMsg.end(), msg, msg + msgLen);
    auto innerHash = Sha512(innerMsg.data(), innerMsg.size());

    // outerHash = SHA512((key XOR opad) || innerHash)
    std::vector<uint8_t> outerMsg;
    outerMsg.insert(outerMsg.end(), outer.begin(), outer.end());
    outerMsg.insert(outerMsg.end(), innerHash.begin(), innerHash.end());
    return Sha512(outerMsg.data(), outerMsg.size());
}

// ─── HKDF-SHA512 ─────────────────────────────────────────────────────────────

inline std::vector<uint8_t> HkdfSha512(const uint8_t* ikm, size_t ikmLen,
                                        const uint8_t* salt, size_t saltLen,
                                        const uint8_t* info, size_t infoLen,
                                        size_t outLen) {
    // Extract: PRK = HMAC-SHA512(salt, IKM). A zero-length salt uses the block of
    // zeros as the default salt (RFC 5869).
    const uint8_t zeroSalt[128] = {0};
    const uint8_t* saltPtr = (saltLen > 0) ? salt : zeroSalt;
    size_t saltPtrLen = (saltLen > 0) ? saltLen : 128;
    auto prk = HmacSha512(saltPtr, saltPtrLen, ikm, ikmLen);

    // Expand: T(1) = HMAC(PRK, info || 0x01), T(2) = HMAC(PRK, T(1) || 0x02), ...
    std::vector<uint8_t> result;
    std::vector<uint8_t> prev;
    uint8_t counter = 1;
    while (result.size() < outLen) {
        std::vector<uint8_t> msg;
        if (!prev.empty())
            msg.insert(msg.end(), prev.begin(), prev.end());
        if (info && infoLen)
            msg.insert(msg.end(), info, info + infoLen);
        msg.push_back(counter);
        auto block = HmacSha512(prk.data(), prk.size(), msg.data(), msg.size());
        result.insert(result.end(), block.begin(), block.end());
        prev = block;
        ++counter;
    }
    result.resize(outLen);
    return result;
}

inline std::vector<uint8_t> HkdfSha512(const std::vector<uint8_t>& ikm,
                                        const char* salt, const char* info,
                                        size_t outLen) {
    return HkdfSha512(ikm.data(), ikm.size(),
                      reinterpret_cast<const uint8_t*>(salt), strlen(salt),
                      reinterpret_cast<const uint8_t*>(info), strlen(info),
                      outLen);
}

// ─── ChaCha20 ────────────────────────────────────────────────────────────────

inline void ChaCha20Encrypt(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t* input, size_t len, uint8_t* output) {
    auto rotl = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    auto qr = [&rotl](uint32_t s[16], int a, int b, int c, int d) {
        s[a] += s[b]; s[c] ^= s[a]; s[c] = rotl(s[c], 16);
        s[d] += s[c]; s[a] ^= s[d]; s[a] = rotl(s[a], 12);
        s[b] += s[a]; s[d] ^= s[b]; s[d] = rotl(s[d], 8);
        s[c] += s[d]; s[a] ^= s[c]; s[a] = rotl(s[a], 7);
    };

    uint32_t init[16];
    init[0] = 0x61707865; init[1] = 0x3320646e; init[2] = 0x79622d32; init[3] = 0x73636174;
    for (int i = 0; i < 8; ++i)
        init[4 + i] = key[i * 4] | (key[i * 4 + 1] << 8) | (key[i * 4 + 2] << 16) | (key[i * 4 + 3] << 24);
    init[12] = 0; // counter
    init[13] = nonce[0] | (nonce[1] << 8) | (nonce[2] << 16) | (nonce[3] << 24);
    init[14] = nonce[4] | (nonce[5] << 8) | (nonce[6] << 16) | (nonce[7] << 24);
    init[15] = nonce[8] | (nonce[9] << 8) | (nonce[10] << 16) | (nonce[11] << 24);

    size_t offset = 0;
    while (offset < len) {
        uint32_t state[16];
        memcpy(state, init, sizeof(state));
        for (int round = 0; round < 10; ++round) {
            qr(state, 0, 4, 8, 12);  qr(state, 1, 5, 9, 13);
            qr(state, 2, 6, 10, 14);  qr(state, 3, 7, 11, 15);
            qr(state, 0, 5, 10, 15);  qr(state, 1, 6, 11, 12);
            qr(state, 2, 7, 8, 13);   qr(state, 3, 4, 9, 14);
        }
        for (int i = 0; i < 16; ++i) state[i] += init[i];
        init[12]++;
        size_t chunk = (len - offset < 64) ? (len - offset) : 64;
        for (size_t j = 0; j < chunk; ++j) {
            uint32_t word = state[j / 4];
            uint8_t ks = (word >> (8 * (j % 4))) & 0xFF;
            output[offset + j] = input[offset + j] ^ ks;
        }
        offset += chunk;
    }
}

// ChaCha20 with label-based nonce (CarPlay convention: nonce = label bytes + 0 padding)
inline std::vector<uint8_t> ChaCha20Encrypt(const uint8_t key[32], const char* label,
                                             const uint8_t* data, size_t len) {
    uint8_t nonce[12] = {};
    size_t labelLen = strlen(label);
    if (labelLen > 12) labelLen = 12;
    memcpy(nonce, label, labelLen);
    std::vector<uint8_t> out(len);
    ChaCha20Encrypt(key, nonce, data, len, out.data());
    return out;
}

inline std::vector<uint8_t> ChaCha20Decrypt(const uint8_t key[32], const char* label,
                                             const uint8_t* data, size_t len) {
    // ChaCha20 is symmetric
    return ChaCha20Encrypt(key, label, data, len);
}

// ─── Poly1305 (RFC 8439) ─────────────────────────────────────────────────────

// 128-bit little-endian integer (lo, hi) with carry-aware add/sub.
struct Poly1305Acc {
    uint64_t lo = 0, hi = 0;
};

// r = a + b (mod 2^128), with the Poly1305 "mod 2^130 - 5" reduction applied.
inline Poly1305Acc PolyAdd(Poly1305Acc a, Poly1305Acc b) {
    Poly1305Acc r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1 : 0);
    return r;
}

// Portable 128-bit unsigned integer (lo, hi) for the Poly1305 130-bit arithmetic.
// MSVC x64 has no __uint128_t, so the needed operations are implemented by hand.
struct U128 {
    uint64_t lo = 0, hi = 0;
    U128() = default;
    U128(uint64_t v) : lo(v), hi(0) {}
    U128(uint64_t l, uint64_t h) : lo(l), hi(h) {}

    static U128 Mul64(uint64_t a, uint64_t b) {
#if defined(_MSC_VER)
        U128 r;
        r.lo = _umul128(a, b, &r.hi);
        return r;
#else
        __uint128_t p = static_cast<__uint128_t>(a) * b;
        return U128(static_cast<uint64_t>(p), static_cast<uint64_t>(p >> 64));
#endif
    }
    U128 operator+(const U128& o) const {
        U128 r;
        r.lo = lo + o.lo;
        uint64_t carry = (r.lo < lo) ? 1 : 0;
        r.hi = hi + o.hi + carry;
        return r;
    }
    U128 operator&(const U128& o) const { return U128(lo & o.lo, hi & o.hi); }
    U128 operator<<(int n) const {
        if (n >= 128) return U128(0, 0);
        if (n == 0) return *this;
        if (n < 64) return U128(lo << n, (hi << n) | (lo >> (64 - n)));
        return U128(0, lo << (n - 64));
    }
    U128 operator>>(int n) const {
        if (n >= 128) return U128(0, 0);
        if (n == 0) return *this;
        if (n < 64) return U128((lo >> n) | (hi << (64 - n)), hi >> n);
        return U128(hi >> (n - 64), 0);
    }
    U128 operator*(int m) const {
        U128 a = Mul64(lo, static_cast<uint64_t>(m));
        U128 b = Mul64(hi, static_cast<uint64_t>(m));
        return U128(a.lo, a.hi + b.lo);
    }
    U128 operator*(const U128& o) const {
        U128 a = Mul64(lo, o.lo);
        U128 b = Mul64(lo, o.hi);
        U128 c = Mul64(hi, o.lo);
        U128 d = Mul64(hi, o.hi);
        // Sum the four 128-bit partial products into a 256-bit result, keep low 128 bits.
        U128 s = a + (b << 64) + (c << 64) + (d << 128);
        return s;
    }
    explicit operator uint64_t() const { return lo; }
};

// r = a * b (mod 2^130 - 5) — the Poly1305 130-bit multiply-reduce.
inline Poly1305Acc PolyMul(Poly1305Acc a, Poly1305Acc b) {
    // 128-bit * 128-bit -> 256-bit product, then reduce mod (2^130 - 5).
    U128 t0 = U128::Mul64(a.lo, b.lo);
    U128 t1 = U128::Mul64(a.lo, b.hi);
    U128 t2 = U128::Mul64(a.hi, b.lo);
    U128 t3 = U128::Mul64(a.hi, b.hi);

    // Sum the partial products into four 64-bit limbs (c0..c3) of the 256-bit product.
    uint64_t c0 = static_cast<uint64_t>(t0);
    U128 s1 = (t0 >> 64) + (t1 & U128(0xFFFFFFFFFFFFFFFF)) + (t2 & U128(0xFFFFFFFFFFFFFFFF));
    uint64_t c1 = static_cast<uint64_t>(s1);
    U128 s2 = (s1 >> 64) + (t1 >> 64) + (t2 >> 64) + (t3 & U128(0xFFFFFFFFFFFFFFFF));
    uint64_t c2 = static_cast<uint64_t>(s2);
    U128 s3 = (s2 >> 64) + (t3 >> 64);
    uint64_t c3 = static_cast<uint64_t>(s3);

    // Split the 256-bit value into the low 130 bits and the high 126 bits.
    // low  = c0 + c1*2^64 + (c2 & 0x3F)*2^128
    // high = (c2 >> 6) + c3*2^62
    U128 low  = U128(c0) + (U128(c1) << 64) + (U128(c2 & 0x3F) << 128);
    U128 high = U128(c2 >> 6) + (U128(c3) << 62);
    // Since 2^130 ≡ 5 (mod 2^130 - 5), the value ≡ low + 5*high (mod 2^130 - 5).
    U128 r = low + (U128(5) * high);
    // r < 2^130 + 5*2^126, so one more reduction pass: (r & (2^130-1)) + (r >> 130)*5.
    // The 128-bit mask (2^130-1) overflows to all-ones, so this is r + 5*carry.
    U128 carry = r >> 130;
    r = r + (U128(5) * carry);
    Poly1305Acc out;
    out.lo = static_cast<uint64_t>(r);
    out.hi = static_cast<uint64_t>(r >> 64);
    return out;
}

// Compute the Poly1305 tag over `msg` with the 32-byte `key` (r = key[0..16] & 0x0FFFFFFC0F...,
// s = key[16..32]).
inline std::array<uint8_t, 16> Poly1305(const uint8_t key[32], const uint8_t* msg, size_t len) {
    // r = key[0..16] with the low 2 bits of each 32-bit word cleared.
    Poly1305Acc r;
    r.lo = (static_cast<uint64_t>(key[0]) | (static_cast<uint64_t>(key[1]) << 8) |
           (static_cast<uint64_t>(key[2]) << 16) | (static_cast<uint64_t>(key[3]) << 24) |
           (static_cast<uint64_t>(key[4]) << 32) | (static_cast<uint64_t>(key[5]) << 40) |
           (static_cast<uint64_t>(key[6]) << 48) | (static_cast<uint64_t>(key[7]) << 56)) & 0x0FFFFFFC0FFFFFFCu;
    r.hi = (static_cast<uint64_t>(key[8]) | (static_cast<uint64_t>(key[9]) << 8) |
           (static_cast<uint64_t>(key[10]) << 16) | (static_cast<uint64_t>(key[11]) << 24) |
           (static_cast<uint64_t>(key[12]) << 32) | (static_cast<uint64_t>(key[13]) << 40) |
           (static_cast<uint64_t>(key[14]) << 48) | (static_cast<uint64_t>(key[15]) << 56)) & 0x0FFFFFFC0FFFFFFCu;
    Poly1305Acc h{0, 0};
    size_t off = 0;
    while (off < len) {
        uint64_t block = 0;
        size_t n = std::min<size_t>(8, len - off);
        for (size_t i = 0; i < n; ++i) block |= static_cast<uint64_t>(msg[off + i]) << (8 * i);
        block |= (n < 8) ? (1ull << (8 * n)) : 0;  // pad with 0x01
        h = PolyMul(h, r);
        h = PolyAdd(h, {block, 0});
        off += n;
    }
    // h + r (mod 2^130 - 5)
    h = PolyAdd(h, r);
    // s = key[16..32]
    Poly1305Acc s;
    s.lo = static_cast<uint64_t>(key[16]) | (static_cast<uint64_t>(key[17]) << 8) |
           (static_cast<uint64_t>(key[18]) << 16) | (static_cast<uint64_t>(key[19]) << 24) |
           (static_cast<uint64_t>(key[20]) << 32) | (static_cast<uint64_t>(key[21]) << 40) |
           (static_cast<uint64_t>(key[22]) << 48) | (static_cast<uint64_t>(key[23]) << 56);
    s.hi = static_cast<uint64_t>(key[24]) | (static_cast<uint64_t>(key[25]) << 8) |
           (static_cast<uint64_t>(key[26]) << 16) | (static_cast<uint64_t>(key[27]) << 24) |
           (static_cast<uint64_t>(key[28]) << 32) | (static_cast<uint64_t>(key[29]) << 40) |
           (static_cast<uint64_t>(key[30]) << 48) | (static_cast<uint64_t>(key[31]) << 56);
    Poly1305Acc mac = PolyAdd(h, s);
    std::array<uint8_t, 16> out;
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>(mac.lo >> (8 * i));
        out[i + 8] = static_cast<uint8_t>(mac.hi >> (8 * i));
    }
    return out;
}

// ─── ChaCha20-Poly1305 AEAD (RFC 8439) ───────────────────────────────────────

// Derive the Poly1305 subkey from the first 32 bytes of the ChaCha20 keystream.
inline void ChaCha20Block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                          uint8_t out[64]) {
    auto rotl = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    auto qr = [&rotl](uint32_t s[16], int a, int b, int c, int d) {
        s[a] += s[b]; s[c] ^= s[a]; s[c] = rotl(s[c], 16);
        s[d] += s[c]; s[a] ^= s[d]; s[a] = rotl(s[a], 12);
        s[b] += s[a]; s[d] ^= s[b]; s[d] = rotl(s[d], 8);
        s[c] += s[d]; s[a] ^= s[c]; s[a] = rotl(s[a], 7);
    };
    uint32_t state[16];
    state[0] = 0x61707865; state[1] = 0x3320646e; state[2] = 0x79622d32; state[3] = 0x73636174;
    for (int i = 0; i < 8; ++i)
        state[4 + i] = key[i * 4] | (key[i * 4 + 1] << 8) | (key[i * 4 + 2] << 16) | (key[i * 4 + 3] << 24);
    state[12] = counter;
    state[13] = nonce[0] | (nonce[1] << 8) | (nonce[2] << 16) | (nonce[3] << 24);
    state[14] = nonce[4] | (nonce[5] << 8) | (nonce[6] << 16) | (nonce[7] << 24);
    state[15] = nonce[8] | (nonce[9] << 8) | (nonce[10] << 16) | (nonce[11] << 24);
    uint32_t init[16];
    memcpy(init, state, sizeof(state));
    for (int round = 0; round < 10; ++round) {
        qr(state, 0, 4, 8, 12);  qr(state, 1, 5, 9, 13);
        qr(state, 2, 6, 10, 14);  qr(state, 3, 7, 11, 15);
        qr(state, 0, 5, 10, 15);  qr(state, 1, 6, 11, 12);
        qr(state, 2, 7, 8, 13);   qr(state, 3, 4, 9, 14);
    }
    for (int i = 0; i < 16; ++i) state[i] += init[i];
    for (int i = 0; i < 16; ++i) {
        out[i] = static_cast<uint8_t>(state[i]);
        out[i + 1] = static_cast<uint8_t>(state[i] >> 8);
        out[i + 2] = static_cast<uint8_t>(state[i] >> 16);
        out[i + 3] = static_cast<uint8_t>(state[i] >> 24);
    }
}

// AEAD-encrypt `plaintext` with `key` and the pairing 8-byte nonce (left-padded with four zero
// bytes to the 12-byte IETF nonce). Returns ciphertext ‖ 16-byte Poly1305 tag.
inline std::vector<uint8_t> ChaCha20Poly1305Seal(const uint8_t key[32], const uint8_t nonce8[8],
                                                  const uint8_t* plaintext, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_CHACHA20_POLY1305_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key), 32, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    uint8_t nonce[12] = {};
    memcpy(nonce + 4, nonce8, 8);
    uint8_t tag[16] = {};

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = sizeof(nonce);
    authInfo.pbTag = tag;
    authInfo.cbTag = sizeof(tag);

    std::vector<uint8_t> ct(len);
    ULONG cbResult = 0;
    NTSTATUS status = BCryptEncrypt(hKey, const_cast<PUCHAR>(plaintext), static_cast<ULONG>(len),
                                    &authInfo, nullptr, 0, ct.data(), static_cast<ULONG>(ct.size()), &cbResult, 0);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (status != 0) {
        return {};
    }
    ct.insert(ct.end(), tag, tag + 16);
    return ct;
}

// AEAD-decrypt `ciphertext` (ciphertext ‖ 16-byte tag) with `key` and the pairing 8-byte nonce.
// Returns true on success (tag verified), false on authentication failure.
inline bool ChaCha20Poly1305Open(const uint8_t key[32], const uint8_t nonce8[8],
                                  const uint8_t* ciphertext, size_t len,
                                  std::vector<uint8_t>& outPlaintext) {
    if (len < 16) return false;
    size_t ctLen = len - 16;
    const uint8_t* tag = ciphertext + ctLen;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_CHACHA20_POLY1305_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key), 32, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    uint8_t nonce[12] = {};
    memcpy(nonce + 4, nonce8, 8);
    uint8_t tagCopy[16];
    memcpy(tagCopy, tag, 16);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = sizeof(nonce);
    authInfo.pbTag = tagCopy;
    authInfo.cbTag = sizeof(tagCopy);

    outPlaintext.resize(ctLen);
    ULONG cbResult = 0;
    NTSTATUS status = BCryptDecrypt(hKey, const_cast<PUCHAR>(ciphertext), static_cast<ULONG>(ctLen),
                                    &authInfo, nullptr, 0, outPlaintext.data(), static_cast<ULONG>(ctLen), &cbResult, 0);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return (status == 0);
}

// AEAD-decrypt `ctAndTag` (ciphertext ‖ 16-byte tag) with `key` and the pairing 8-byte nonce.
// Returns the plaintext, or nullopt if the tag does not authenticate.
inline std::optional<std::vector<uint8_t>> ChaCha20Poly1305Open(const uint8_t key[32],
                                                                 const uint8_t nonce8[8],
                                                                 const uint8_t* ctAndTag, size_t len) {
    std::vector<uint8_t> pt;
    if (!ChaCha20Poly1305Open(key, nonce8, ctAndTag, len, pt)) {
        return std::nullopt;
    }
    return pt;
}

// Convenience: seal with an 8-byte label (e.g. "PS-Msg05").
inline std::vector<uint8_t> ChaCha20Poly1305Seal(const uint8_t key[32], const char* label,
                                                 const uint8_t* plaintext, size_t len) {
    uint8_t nonce8[8] = {};
    size_t n = strlen(label);
    if (n > 8) n = 8;
    memcpy(nonce8, label, n);
    return ChaCha20Poly1305Seal(key, nonce8, plaintext, len);
}

inline std::optional<std::vector<uint8_t>> ChaCha20Poly1305Open(const uint8_t key[32],
                                                                const char* label,
                                                                const uint8_t* ctAndTag, size_t len) {
    uint8_t nonce8[8] = {};
    size_t n = strlen(label);
    if (n > 8) n = 8;
    memcpy(nonce8, label, n);
    return ChaCha20Poly1305Open(key, nonce8, ctAndTag, len);
}

// ─── AES-128-CTR (for MFi-SAP /auth-setup) ───────────────────────────────────

inline std::vector<uint8_t> Aes128Ctr(const uint8_t key[16], const uint8_t iv[16],
                                      const uint8_t* input, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    const wchar_t* mode = BCRYPT_CHAIN_MODE_ECB;
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
                      static_cast<ULONG>((wcslen(mode) + 1) * sizeof(wchar_t)), 0);

    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key), 16, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    uint8_t counter[16];
    memcpy(counter, iv, 16);
    std::vector<uint8_t> output(len);
    uint8_t keystream[16];
    ULONG result = 0;

    for (size_t offset = 0; offset < len; offset += 16) {
        if (BCryptEncrypt(hKey, counter, 16, nullptr, nullptr, 0, keystream, 16, &result, 0) != 0) {
            BCryptDestroyKey(hKey);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return {};
        }
        size_t block_bytes = (std::min<size_t>)(16, len - offset);
        for (size_t i = 0; i < block_bytes; ++i) {
            output[offset + i] = input[offset + i] ^ keystream[i];
        }
        // Big-endian 128-bit counter increment
        for (int i = 15; i >= 0; --i) {
            if (++counter[i] != 0) break;
        }
    }

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return output;
}

// ─── ChaCha20-Poly1305 with AAD & 12-byte Nonce (for ControlChannel) ──────────

inline std::vector<uint8_t> ChaCha20Poly1305SealWithAad(
    const uint8_t key[32], const uint8_t nonce12[12],
    const uint8_t* aad, size_t aadLen,
    const uint8_t* plaintext, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_CHACHA20_POLY1305_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key), 32, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    uint8_t nonce[12];
    memcpy(nonce, nonce12, 12);
    uint8_t tag[16] = {};

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = sizeof(nonce);
    if (aad && aadLen > 0) {
        authInfo.pbAuthData = const_cast<PUCHAR>(aad);
        authInfo.cbAuthData = static_cast<ULONG>(aadLen);
    }
    authInfo.pbTag = tag;
    authInfo.cbTag = sizeof(tag);

    std::vector<uint8_t> ct(len);
    ULONG cbResult = 0;
    NTSTATUS status = BCryptEncrypt(hKey, const_cast<PUCHAR>(plaintext), static_cast<ULONG>(len),
                                    &authInfo, nullptr, 0, ct.data(), static_cast<ULONG>(ct.size()), &cbResult, 0);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (status != 0) {
        return {};
    }
    ct.insert(ct.end(), tag, tag + 16);
    return ct;
}

inline bool ChaCha20Poly1305OpenWithAad(
    const uint8_t key[32], const uint8_t nonce12[12],
    const uint8_t* aad, size_t aadLen,
    const uint8_t* ciphertext, size_t len,
    std::vector<uint8_t>& outPlaintext) {
    if (len < 16) return false;
    size_t ctLen = len - 16;
    const uint8_t* tag = ciphertext + ctLen;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_CHACHA20_POLY1305_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key), 32, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    uint8_t nonce[12];
    memcpy(nonce, nonce12, 12);
    uint8_t tagCopy[16];
    memcpy(tagCopy, tag, 16);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = sizeof(nonce);
    if (aad && aadLen > 0) {
        authInfo.pbAuthData = const_cast<PUCHAR>(aad);
        authInfo.cbAuthData = static_cast<ULONG>(aadLen);
    }
    authInfo.pbTag = tagCopy;
    authInfo.cbTag = sizeof(tagCopy);

    outPlaintext.resize(ctLen);
    ULONG cbResult = 0;
    NTSTATUS status = BCryptDecrypt(hKey, const_cast<PUCHAR>(ciphertext), static_cast<ULONG>(ctLen),
                                    &authInfo, nullptr, 0, outPlaintext.data(), static_cast<ULONG>(ctLen), &cbResult, 0);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return (status == 0);
}

// ─── Control Channel Keys & Framer ───────────────────────────────────────────

struct ControlKeys {
    std::array<uint8_t, 32> read_key;
    std::array<uint8_t, 32> write_key;
};

inline ControlKeys DeriveControlKeys(const std::array<uint8_t, 32>& shared_secret) {
    ControlKeys keys;
    const char* salt = "Control-Salt";
    const char* write_info = "Control-Write-Encryption-Key";
    const char* read_info = "Control-Read-Encryption-Key";

    auto r = HkdfSha512(shared_secret.data(), 32,
                        reinterpret_cast<const uint8_t*>(salt), strlen(salt),
                        reinterpret_cast<const uint8_t*>(write_info), strlen(write_info),
                        32);
    memcpy(keys.read_key.data(), r.data(), 32);

    auto w = HkdfSha512(shared_secret.data(), 32,
                        reinterpret_cast<const uint8_t*>(salt), strlen(salt),
                        reinterpret_cast<const uint8_t*>(read_info), strlen(read_info),
                        32);
    memcpy(keys.write_key.data(), w.data(), 32);

    return keys;
}

inline ControlKeys DeriveEventKeys(const std::array<uint8_t, 32>& shared_secret) {
    ControlKeys keys;
    const char* salt = "Events-Salt";
    const char* read_info = "Events-Read-Encryption-Key";
    const char* write_info = "Events-Write-Encryption-Key";

    auto r = HkdfSha512(shared_secret.data(), 32,
                        reinterpret_cast<const uint8_t*>(salt), strlen(salt),
                        reinterpret_cast<const uint8_t*>(read_info), strlen(read_info),
                        32);
    memcpy(keys.read_key.data(), r.data(), 32);

    auto w = HkdfSha512(shared_secret.data(), 32,
                        reinterpret_cast<const uint8_t*>(salt), strlen(salt),
                        reinterpret_cast<const uint8_t*>(write_info), strlen(write_info),
                        32);
    memcpy(keys.write_key.data(), w.data(), 32);

    return keys;
}

struct StreamKeys {
    std::array<uint8_t, 32> output{0};
    std::array<uint8_t, 32> input{0};
};

inline StreamKeys DeriveStreamKeys(const std::array<uint8_t, 32>& shared_secret, uint64_t stream_connection_id) {
    std::string salt = "DataStream-Salt" + std::to_string(stream_connection_id);
    const char* out_info = "DataStream-Output-Encryption-Key";
    const char* in_info = "DataStream-Input-Encryption-Key";

    StreamKeys keys;
    auto o = HkdfSha512(shared_secret.data(), 32,
                        reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
                        reinterpret_cast<const uint8_t*>(out_info), strlen(out_info),
                        32);
    memcpy(keys.output.data(), o.data(), 32);

    auto i = HkdfSha512(shared_secret.data(), 32,
                        reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
                        reinterpret_cast<const uint8_t*>(in_info), strlen(in_info),
                        32);
    memcpy(keys.input.data(), i.data(), 32);

    return keys;
}

class ControlChannel {
public:
    explicit ControlChannel(ControlKeys keys)
        : keys_(keys), read_counter_(0), write_counter_(0) {}

    std::vector<uint8_t> EncryptMessage(const uint8_t* data, size_t len) {
        constexpr size_t MAX_WRITE = 1024;
        std::vector<uint8_t> out;
        size_t offset = 0;
        while (offset < len) {
            size_t chunk_len = (std::min)(MAX_WRITE, len - offset);
            auto frame = SealOneChunk(data + offset, chunk_len);
            out.insert(out.end(), frame.begin(), frame.end());
            offset += chunk_len;
        }
        return out;
    }

    std::optional<std::vector<uint8_t>> DecryptFrame(
        const uint8_t* buf, size_t len, size_t& bytes_consumed, bool& auth_failed) {
        bytes_consumed = 0;
        auth_failed = false;
        if (len < 2) return std::nullopt;

        uint16_t chunk_len = static_cast<uint16_t>(buf[0] | (static_cast<uint16_t>(buf[1]) << 8));
        constexpr size_t MAX_READ = 16 * 1024;
        if (chunk_len > MAX_READ) {
            auth_failed = true;
            return std::nullopt;
        }

        size_t total_frame_len = 2 + chunk_len + 16;
        if (len < total_frame_len) {
            return std::nullopt;
        }

        uint8_t nonce[12] = {0};
        uint64_t cnt = read_counter_;
        memcpy(nonce + 4, &cnt, 8);

        std::vector<uint8_t> pt;
        if (!ChaCha20Poly1305OpenWithAad(keys_.read_key.data(), nonce,
                                        buf, 2,
                                        buf + 2, chunk_len + 16, pt)) {
            auth_failed = true;
            return std::nullopt;
        }

        read_counter_++;
        bytes_consumed = total_frame_len;
        return pt;
    }

private:
    std::vector<uint8_t> SealOneChunk(const uint8_t* chunk, size_t chunk_len) {
        uint8_t len_le[2] = {
            static_cast<uint8_t>(chunk_len & 0xFF),
            static_cast<uint8_t>((chunk_len >> 8) & 0xFF)
        };

        uint8_t nonce[12] = {0};
        uint64_t cnt = write_counter_;
        memcpy(nonce + 4, &cnt, 8);

        auto ct_and_tag = ChaCha20Poly1305SealWithAad(
            keys_.write_key.data(), nonce, len_le, 2, chunk, chunk_len);

        write_counter_++;

        std::vector<uint8_t> frame;
        frame.reserve(2 + ct_and_tag.size());
        frame.push_back(len_le[0]);
        frame.push_back(len_le[1]);
        frame.insert(frame.end(), ct_and_tag.begin(), ct_and_tag.end());
        return frame;
    }

    ControlKeys keys_;
    uint64_t read_counter_{0};
    uint64_t write_counter_{0};
};

// ─── Ed25519 (pure C++) ──────────────────────────────────────────────────────

struct Ed25519KeyPair {
    std::vector<uint8_t> publicKey;  // 32 bytes
    std::vector<uint8_t> privateKey; // 32 bytes (seed)
};

// Pure-C++ Ed25519 (RFC 8032) via the ed25519 namespace; CNG does not expose
// Ed25519 key generation/signing in the installed SDK.
inline std::optional<Ed25519KeyPair> Ed25519Generate() {
    auto kp = CarPlayStation::Core::Session::Pairing::ed25519::GenerateKeyPair();
    Ed25519KeyPair out;
    out.publicKey.assign(kp.pubKey.begin(), kp.pubKey.end());
    out.privateKey.assign(kp.secret.begin(), kp.secret.end());
    return out;
}

inline bool Ed25519Verify(const uint8_t pubKey[32], const uint8_t* message, size_t msgLen,
                          const uint8_t signature[64]) {
    return CarPlayStation::Core::Session::Pairing::ed25519::Verify(pubKey, message, msgLen, signature);
}

inline std::optional<std::array<uint8_t, 64>> Ed25519Sign(const uint8_t privKey[32],
                                                             const uint8_t* message, size_t msgLen) {
    auto sig = CarPlayStation::Core::Session::Pairing::ed25519::Sign(privKey, message, msgLen);
    if (sig.size() != 64) return std::nullopt;
    std::array<uint8_t, 64> out = {};
    for (int i = 0; i < 64; ++i) out[i] = sig[i];
    return out;
}

// ─── Curve25519 / X25519 ─────────────────────────────────────────────────────

inline void Curve25519Mult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    CarPlayStation::Core::Session::Pairing::curve25519::Mult(out, scalar, point);
}

inline void Curve25519BaseMult(uint8_t out[32], const uint8_t scalar[32]) {
    CarPlayStation::Core::Session::Pairing::curve25519::BaseMult(out, scalar);
}

} // namespace pairing

