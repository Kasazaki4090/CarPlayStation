#pragma once
// Ed25519 (RFC 8032) signing and verification for Apple CarPlay pairing.
// Backed by proven constant-time ref10/orlp implementation.

#include "Core/Session/Pairing/ed25519/ed25519.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <vector>
#include <cstdint>
#include <cstring>

namespace CarPlayStation::Core::Session::Pairing::ed25519 {

struct KeyPair {
    std::array<uint8_t, 32> pubKey{};
    std::array<uint8_t, 32> secret{}; // 32-byte seed
};

// Generate an Ed25519 keypair using Windows CNG CSPRNG.
inline KeyPair GenerateKeyPair() {
    KeyPair kp;
    BCryptGenRandom(nullptr, kp.secret.data(), 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    uint8_t priv64[64];
    ed25519_create_keypair(kp.pubKey.data(), priv64, kp.secret.data());
    return kp;
}

// Sign a message using 32-byte secret seed. Returns 64-byte signature.
inline std::vector<uint8_t> Sign(const uint8_t secretSeed[32], const uint8_t* message, size_t len) {
    uint8_t pub[32];
    uint8_t priv64[64];
    ed25519_create_keypair(pub, priv64, secretSeed);
    std::vector<uint8_t> sig(64);
    ed25519_sign(sig.data(), message, len, pub, priv64);
    return sig;
}

// Verify a 64-byte signature against a 32-byte public key.
inline bool Verify(const uint8_t publicKey[32], const uint8_t* message, size_t len,
                   const uint8_t signature[64]) {
    return ed25519_verify(signature, message, len, publicKey) == 1;
}

} // namespace CarPlayStation::Core::Session::Pairing::ed25519
