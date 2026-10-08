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
        s[a] += s[b]; s[d] ^= s[a]; s[d] = rotl(s[d], 16);
        s[c] += s[d]; s[b] ^= s[c]; s[b] = rotl(s[b], 12);
        s[a] += s[b]; s[d] ^= s[a]; s[d] = rotl(s[d], 8);
        s[c] += s[d]; s[b] ^= s[c]; s[b] = rotl(s[b], 7);
    };

    uint32_t init[16];
    init[0] = 0x61707865; init[1] = 0x3320646e; init[2] = 0x79622d32; init[3] = 0x6b206574;
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

inline void Poly1305(const uint8_t key[32], const uint8_t* msg, size_t len, uint8_t tag[16]) {
    auto load32 = [](const uint8_t* p) -> uint32_t {
        return static_cast<uint32_t>(p[0]) |
              (static_cast<uint32_t>(p[1]) << 8) |
              (static_cast<uint32_t>(p[2]) << 16) |
              (static_cast<uint32_t>(p[3]) << 24);
    };

    uint32_t t0 = load32(key + 0);
    uint32_t t1 = load32(key + 4);
    uint32_t t2 = load32(key + 8);
    uint32_t t3 = load32(key + 12);

    // r &= 0x0ffffffc0ffffffc0ffffffc0fffffff (RFC 8439 clamping)
    uint32_t r0 = (t0                    ) & 0x3ffffff;
    uint32_t r1 = ((t0 >> 26) | (t1 <<  6)) & 0x3ffff03;
    uint32_t r2 = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    uint32_t r3 = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    uint32_t r4 = ((t3 >>  8)             ) & 0x00fffff;

    uint32_t s1 = r1 * 5;
    uint32_t s2 = r2 * 5;
    uint32_t s3 = r3 * 5;
    uint32_t s4 = r4 * 5;

    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    auto process_block = [&](const uint8_t* m, uint32_t hibit) {
        uint32_t m0 = load32(m + 0);
        uint32_t m1 = load32(m + 4);
        uint32_t m2 = load32(m + 8);
        uint32_t m3 = load32(m + 12);

        h0 += (m0                    ) & 0x3ffffff;
        h1 += ((m0 >> 26) | (m1 <<  6)) & 0x3ffffff;
        h2 += ((m1 >> 20) | (m2 << 12)) & 0x3ffffff;
        h3 += ((m2 >> 14) | (m3 << 18)) & 0x3ffffff;
        h4 += ((m3 >>  8)             ) | (hibit << 24);

        uint64_t d0 = static_cast<uint64_t>(h0) * r0 +
                      static_cast<uint64_t>(h1) * s4 +
                      static_cast<uint64_t>(h2) * s3 +
                      static_cast<uint64_t>(h3) * s2 +
                      static_cast<uint64_t>(h4) * s1;
        uint64_t d1 = static_cast<uint64_t>(h0) * r1 +
                      static_cast<uint64_t>(h1) * r0 +
                      static_cast<uint64_t>(h2) * s4 +
                      static_cast<uint64_t>(h3) * s3 +
                      static_cast<uint64_t>(h4) * s2;
        uint64_t d2 = static_cast<uint64_t>(h0) * r2 +
                      static_cast<uint64_t>(h1) * r1 +
                      static_cast<uint64_t>(h2) * r0 +
                      static_cast<uint64_t>(h3) * s4 +
                      static_cast<uint64_t>(h4) * s3;
        uint64_t d3 = static_cast<uint64_t>(h0) * r3 +
                      static_cast<uint64_t>(h1) * r2 +
                      static_cast<uint64_t>(h2) * r1 +
                      static_cast<uint64_t>(h3) * r0 +
                      static_cast<uint64_t>(h4) * s4;
        uint64_t d4 = static_cast<uint64_t>(h0) * r4 +
                      static_cast<uint64_t>(h1) * r3 +
                      static_cast<uint64_t>(h2) * r2 +
                      static_cast<uint64_t>(h3) * r1 +
                      static_cast<uint64_t>(h4) * r0;

        uint64_t c;
        c = d0 >> 26; h0 = static_cast<uint32_t>(d0) & 0x3ffffff; d1 += c;
        c = d1 >> 26; h1 = static_cast<uint32_t>(d1) & 0x3ffffff; d2 += c;
        c = d2 >> 26; h2 = static_cast<uint32_t>(d2) & 0x3ffffff; d3 += c;
        c = d3 >> 26; h3 = static_cast<uint32_t>(d3) & 0x3ffffff; d4 += c;
        c = d4 >> 26; h4 = static_cast<uint32_t>(d4) & 0x3ffffff;
        h0 += static_cast<uint32_t>(c * 5);
        c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += static_cast<uint32_t>(c);
    };

    while (len >= 16) {
        process_block(msg, 1);
        msg += 16;
        len -= 16;
    }

    if (len > 0) {
        uint8_t buf[16] = {};
        memcpy(buf, msg, len);
        buf[len] = 1;
        process_block(buf, 0);
    }

    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    uint32_t g0 = h0 + 5;
    c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c;
    c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c;
    c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c;
    c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    uint32_t out0 = (h0      ) | (h1 << 26);
    uint32_t out1 = (h1 >>  6) | (h2 << 20);
    uint32_t out2 = (h2 >> 12) | (h3 << 14);
    uint32_t out3 = (h3 >> 18) | (h4 <<  8);

    uint64_t f0 = static_cast<uint64_t>(out0) + load32(key + 16);
    c = static_cast<uint32_t>(f0 >> 32);
    uint64_t f1 = static_cast<uint64_t>(out1) + load32(key + 20) + c;
    c = static_cast<uint32_t>(f1 >> 32);
    uint64_t f2 = static_cast<uint64_t>(out2) + load32(key + 24) + c;
    c = static_cast<uint32_t>(f2 >> 32);
    uint64_t f3 = static_cast<uint64_t>(out3) + load32(key + 28) + c;

    auto store32 = [](uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    };

    store32(tag + 0, static_cast<uint32_t>(f0));
    store32(tag + 4, static_cast<uint32_t>(f1));
    store32(tag + 8, static_cast<uint32_t>(f2));
    store32(tag + 12, static_cast<uint32_t>(f3));
}

inline std::array<uint8_t, 16> Poly1305(const uint8_t key[32], const uint8_t* msg, size_t len) {
    std::array<uint8_t, 16> out;
    Poly1305(key, msg, len, out.data());
    return out;
}

// ─── ChaCha20-Poly1305 AEAD (RFC 8439) ───────────────────────────────────────

inline void ChaCha20Block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                          uint8_t out[64]) {
    auto rotl = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    auto qr = [&rotl](uint32_t s[16], int a, int b, int c, int d) {
        s[a] += s[b]; s[d] ^= s[a]; s[d] = rotl(s[d], 16);
        s[c] += s[d]; s[b] ^= s[c]; s[b] = rotl(s[b], 12);
        s[a] += s[b]; s[d] ^= s[a]; s[d] = rotl(s[d], 8);
        s[c] += s[d]; s[b] ^= s[c]; s[b] = rotl(s[b], 7);
    };
    uint32_t state[16];
    state[0] = 0x61707865; state[1] = 0x3320646e; state[2] = 0x79622d32; state[3] = 0x6b206574;
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
        out[i * 4]     = static_cast<uint8_t>(state[i]);
        out[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 8);
        out[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 16);
        out[i * 4 + 3] = static_cast<uint8_t>(state[i] >> 24);
    }
}

inline void ChaCha20EncryptWithCounter(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                                       const uint8_t* input, size_t len, uint8_t* output) {
    uint8_t block[64];
    size_t offset = 0;
    while (offset < len) {
        ChaCha20Block(key, nonce, counter++, block);
        size_t chunk = (len - offset < 64) ? (len - offset) : 64;
        for (size_t j = 0; j < chunk; ++j) {
            output[offset + j] = (input ? input[offset + j] : 0) ^ block[j];
        }
        offset += chunk;
    }
}

// Cached ChaCha20-Poly1305 algorithm provider (Windows 11+).
inline BCRYPT_ALG_HANDLE ChaCha20Poly1305Alg() {
    static BCRYPT_ALG_HANDLE hAlg = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        BCryptOpenAlgorithmProvider(&h, BCRYPT_CHACHA20_POLY1305_ALGORITHM, nullptr, 0);
        return h;
    }();
    return hAlg;
}

// Forward declarations of AEAD with AAD
inline std::vector<uint8_t> ChaCha20Poly1305SealWithAad(
    const uint8_t key[32], const uint8_t nonce12[12],
    const uint8_t* aad, size_t aadLen,
    const uint8_t* plaintext, size_t len);

inline bool ChaCha20Poly1305OpenWithAad(
    const uint8_t key[32], const uint8_t nonce12[12],
    const uint8_t* aad, size_t aadLen,
    const uint8_t* ciphertext, size_t len,
    std::vector<uint8_t>& outPlaintext);

// AEAD-encrypt `plaintext` with `key` and the pairing 8-byte nonce (left-padded with four zero
// bytes to the 12-byte IETF nonce). Returns ciphertext ‖ 16-byte Poly1305 tag.
inline std::vector<uint8_t> ChaCha20Poly1305Seal(const uint8_t key[32], const uint8_t nonce8[8],
                                                 const uint8_t* plaintext, size_t len) {
    uint8_t nonce12[12] = {};
    memcpy(nonce12 + 4, nonce8, 8);
    return ChaCha20Poly1305SealWithAad(key, nonce12, nullptr, 0, plaintext, len);
}

// AEAD-decrypt `ciphertext` (ciphertext ‖ 16-byte tag) with `key` and the pairing 8-byte nonce.
// Returns true on success (tag verified), false on authentication failure.
inline bool ChaCha20Poly1305Open(const uint8_t key[32], const uint8_t nonce8[8],
                                 const uint8_t* ciphertext, size_t len,
                                 std::vector<uint8_t>& outPlaintext) {
    uint8_t nonce12[12] = {};
    memcpy(nonce12 + 4, nonce8, 8);
    return ChaCha20Poly1305OpenWithAad(key, nonce12, nullptr, 0, ciphertext, len, outPlaintext);
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
    BCRYPT_ALG_HANDLE hAlg = ChaCha20Poly1305Alg();
    if (hAlg) {
        BCRYPT_KEY_HANDLE hKey = nullptr;
        if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key), 32, 0) == 0) {
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

            if (status == 0) {
                ct.insert(ct.end(), tag, tag + 16);
                return ct;
            }
        }
    }

    // RFC 8439 pure C++ fallback (e.g. Windows 10 where CNG lacks BCRYPT_CHACHA20_POLY1305_ALGORITHM)
    uint8_t block0[64];
    ChaCha20Block(key, nonce12, 0, block0);
    uint8_t otk[32];
    memcpy(otk, block0, 32);

    std::vector<uint8_t> ct(len);
    if (len > 0) {
        ChaCha20EncryptWithCounter(key, nonce12, 1, plaintext, len, ct.data());
    }

    std::vector<uint8_t> mac_data;
    mac_data.reserve(aadLen + 16 + len + 16 + 16);
    if (aad && aadLen > 0) {
        mac_data.insert(mac_data.end(), aad, aad + aadLen);
        if (aadLen % 16 != 0) {
            mac_data.insert(mac_data.end(), 16 - (aadLen % 16), 0);
        }
    }
    if (len > 0) {
        mac_data.insert(mac_data.end(), ct.begin(), ct.end());
        if (len % 16 != 0) {
            mac_data.insert(mac_data.end(), 16 - (len % 16), 0);
        }
    }
    uint64_t aad_len_u64 = static_cast<uint64_t>(aadLen);
    for (int i = 0; i < 8; ++i) mac_data.push_back(static_cast<uint8_t>(aad_len_u64 >> (8 * i)));
    uint64_t ct_len_u64 = static_cast<uint64_t>(len);
    for (int i = 0; i < 8; ++i) mac_data.push_back(static_cast<uint8_t>(ct_len_u64 >> (8 * i)));

    uint8_t tag[16] = {};
    Poly1305(otk, mac_data.data(), mac_data.size(), tag);

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

    BCRYPT_ALG_HANDLE hAlg = ChaCha20Poly1305Alg();
    if (hAlg) {
        BCRYPT_KEY_HANDLE hKey = nullptr;
        if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key), 32, 0) == 0) {
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

            if (status == 0) {
                return true;
            }
        }
    }

    // RFC 8439 pure C++ fallback
    uint8_t block0[64];
    ChaCha20Block(key, nonce12, 0, block0);
    uint8_t otk[32];
    memcpy(otk, block0, 32);

    std::vector<uint8_t> mac_data;
    mac_data.reserve(aadLen + 16 + ctLen + 16 + 16);
    if (aad && aadLen > 0) {
        mac_data.insert(mac_data.end(), aad, aad + aadLen);
        if (aadLen % 16 != 0) {
            mac_data.insert(mac_data.end(), 16 - (aadLen % 16), 0);
        }
    }
    if (ctLen > 0) {
        mac_data.insert(mac_data.end(), ciphertext, ciphertext + ctLen);
        if (ctLen % 16 != 0) {
            mac_data.insert(mac_data.end(), 16 - (ctLen % 16), 0);
        }
    }
    uint64_t aad_len_u64 = static_cast<uint64_t>(aadLen);
    for (int i = 0; i < 8; ++i) mac_data.push_back(static_cast<uint8_t>(aad_len_u64 >> (8 * i)));
    uint64_t ct_len_u64 = static_cast<uint64_t>(ctLen);
    for (int i = 0; i < 8; ++i) mac_data.push_back(static_cast<uint8_t>(ct_len_u64 >> (8 * i)));

    uint8_t expected_tag[16] = {};
    Poly1305(otk, mac_data.data(), mac_data.size(), expected_tag);

    uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i) {
        diff |= (tag[i] ^ expected_tag[i]);
    }
    if (diff != 0) {
        return false;
    }

    outPlaintext.resize(ctLen);
    if (ctLen > 0) {
        ChaCha20EncryptWithCounter(key, nonce12, 1, ciphertext, ctLen, outPlaintext.data());
    }
    return true;
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

