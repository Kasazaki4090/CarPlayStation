#pragma once
// SHA-512 via Windows CNG (bcrypt). Kept separate from Crypto.h to avoid a
// circular include with Ed25519.h (which needs SHA-512 but not the rest of Crypto).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace CarPlayStation::Core::Session::Pairing {

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

}  // namespace CarPlayStation::Core::Session::Pairing
