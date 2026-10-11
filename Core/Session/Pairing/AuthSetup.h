#pragma once
// MFi-SAP (Accessory / Server) /auth-setup handler.
//
// Station-to-Station key exchange supporting both:
//   - MFi 2.0 (2nd Generation, CP 2.0C / CH341 RSA): SHA-1 challenge digest (20 bytes)
//   - MFi 3.0 (3rd Generation, CP 3.0 / ECDSA P-256): SHA-256 challenge digest (32 bytes)
//
// Pipeline:
//   1. Parses M1 (1 byte version 0x01 + 32 bytes peer X25519 public key)
//   2. Generates ephemeral X25519 keypair and calculates shared secret
//   3. Derives AES-128 key & IV via SHA-1("AES-KEY" || shared) and SHA-1("AES-IV" || shared)
//   4. Fetches MFi certificate and detects generation (v2 vs v3) via ChipInfo + X.509 inspection
//   5. Hashes (our_pk || peer_pk) with SHA-1 (v2) or SHA-256 (v3)
//   6. Signs digest via IMfiSigner::SignChallenge
//   7. Encrypts signature with AES-128-CTR
//   8. Constructs standard M2 response:
//      <32:our_pk><4:BE cert_len><cert><4:BE sig_len><enc_sig>

#include "Core/Common/Logging.h"
#include "Core/Hardware/IMfiSigner.h"
#include "Core/Session/Pairing/Crypto.h"
#include <cstdint>
#include <vector>
#include <array>
#include <optional>
#include <memory>
#include <string>
#include <algorithm>
#include <cstdio>

namespace CarPlayStation::Core::Session::Pairing {

class AuthSetupServer {
public:
    explicit AuthSetupServer(std::shared_ptr<Hardware::IMfiSigner> signer)
        : signer_(std::move(signer)) {}

    std::optional<std::vector<uint8_t>> Handle(const std::vector<uint8_t>& requestBody) {
        if (requestBody.size() != 33) {
            CPS_LOG_ERROR("AuthSetup", "Invalid M1 length: ", std::to_string(requestBody.size()), " (expected 33)");
            return std::nullopt;
        }
        if (requestBody[0] != 0x01) {
            CPS_LOG_ERROR("AuthSetup", "Unsupported MFi-SAP version: ", std::to_string(requestBody[0]));
            return std::nullopt;
        }
        if (!signer_) {
            CPS_LOG_ERROR("AuthSetup", "No MFi signer configured for /auth-setup");
            return std::nullopt;
        }

        const uint8_t* peer_pk = requestBody.data() + 1;

        // Generate ephemeral X25519 keypair
        uint8_t rnd[32];
        if (BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            CPS_LOG_ERROR("AuthSetup", "BCryptGenRandom failed");
            return std::nullopt;
        }
        std::array<uint8_t, 32> our_pk{};
        pairing::Curve25519BaseMult(our_pk.data(), rnd);

        std::array<uint8_t, 32> shared{};
        pairing::Curve25519Mult(shared.data(), rnd, peer_pk);

        // Derive AES key and IV:
        // aes_key = SHA1("AES-KEY" || shared)[0..16]
        // aes_iv  = SHA1("AES-IV"  || shared)[0..16]
        std::vector<uint8_t> key_mat;
        key_mat.insert(key_mat.end(), {'A', 'E', 'S', '-', 'K', 'E', 'Y'});
        key_mat.insert(key_mat.end(), shared.begin(), shared.end());
        auto hash_key = pairing::Sha1(key_mat.data(), key_mat.size());

        std::vector<uint8_t> iv_mat;
        iv_mat.insert(iv_mat.end(), {'A', 'E', 'S', '-', 'I', 'V'});
        iv_mat.insert(iv_mat.end(), shared.begin(), shared.end());
        auto hash_iv = pairing::Sha1(iv_mat.data(), iv_mat.size());

        uint8_t aes_key[16];
        uint8_t aes_iv[16];
        memcpy(aes_key, hash_key.data(), 16);
        memcpy(aes_iv, hash_iv.data(), 16);

        // Fetch certificate to determine hardware generation & certificate format
        CPS_LOG_INFO("AuthSetup", "Calling MFi signer FetchCertificate for /auth-setup...");
        auto cert_res = signer_->FetchCertificate();
        if (cert_res.IsError()) {
            CPS_LOG_ERROR("AuthSetup", "MFi FetchCertificate failed: ", cert_res.Error().Message());
            return std::nullopt;
        }
        auto cert = cert_res.Value();

        // Detect whether the backend is MFi 2.0 (2代 / RSA / SHA-1) or MFi 3.0 (3代 / ECDSA / SHA-256)
        bool is_v2 = false;
        auto chip_info = signer_->GetChipInfo();
        if ((chip_info.chip_id & 0x00FF0000) == 0x00030000 ||
            (chip_info.chip_id & 0x000000FF) == 0x03 ||
            chip_info.firmware_version.find("v3") != std::string::npos ||
            chip_info.firmware_version.find("3.0") != std::string::npos) {
            is_v2 = false;
        } else if ((chip_info.chip_id & 0x00FF0000) == 0x00020000 ||
                   chip_info.firmware_version.find("v2") != std::string::npos ||
                   chip_info.firmware_version.find("2.0") != std::string::npos) {
            is_v2 = true;
        } else {
            // Fallback heuristics based on certificate inspection:
            // MFi 3.0 P-256 certificates are ~600 bytes and contain ECDSA OID 1.2.840.10045.2.1
            // MFi 2.0 RSA certificates are larger (800 - 1200+ bytes)
            const uint8_t ecdsa_oid[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};
            bool has_ecdsa = std::search(cert.begin(), cert.end(),
                                         std::begin(ecdsa_oid), std::end(ecdsa_oid)) != cert.end();
            if (has_ecdsa || cert.size() < 750) {
                is_v2 = false;
            } else {
                is_v2 = true;
            }
        }

        char chip_id_hex[12];
        std::snprintf(chip_id_hex, sizeof(chip_id_hex), "0x%08X",
                      static_cast<unsigned>(chip_info.chip_id));
        CPS_LOG_INFO("AuthSetup", "MFi hardware generation detected: ",
                     is_v2 ? "2nd Gen (MFi 2.0 / SHA-1)" : "3rd Gen (MFi 3.0 / SHA-256)",
                     " (cert size=", std::to_string(cert.size()), ", chip_id=", chip_id_hex, ")");

        // Digest over our_pk || peer_pk:
        // MFi 2.0 uses SHA-1 (20 bytes)
        // MFi 3.0 uses SHA-256 (32 bytes)
        std::vector<uint8_t> to_digest;
        to_digest.reserve(64);
        to_digest.insert(to_digest.end(), our_pk.begin(), our_pk.end());
        to_digest.insert(to_digest.end(), peer_pk, peer_pk + 32);

        std::vector<uint8_t> digest;
        if (is_v2) {
            digest = pairing::Sha1(to_digest.data(), to_digest.size());
        } else {
            digest = pairing::Sha256(to_digest.data(), to_digest.size());
        }

        // Sign challenge using MFi hardware / file signer
        CPS_LOG_INFO("AuthSetup", "Calling MFi signer SignChallenge (digest len=",
                     std::to_string(digest.size()), ")...");
        auto sig_res = signer_->SignChallenge(Common::ByteSpan(digest.data(), digest.size()));
        if (sig_res.IsError()) {
            CPS_LOG_ERROR("AuthSetup", "MFi SignChallenge failed: ", sig_res.Error().Message());
            return std::nullopt;
        }
        auto sig = sig_res.Value();

        // Encrypt signature with AES-128-CTR
        auto enc_sig = pairing::Aes128Ctr(aes_key, aes_iv, sig.data(), sig.size());

        // Build M2 response:
        // <32:our_pk><4:BE cert_len><cert><4:BE sig_len><enc_sig>
        std::vector<uint8_t> out;
        out.reserve(32 + 4 + cert.size() + 4 + enc_sig.size());
        out.insert(out.end(), our_pk.begin(), our_pk.end());

        uint32_t cert_len = static_cast<uint32_t>(cert.size());
        out.push_back(static_cast<uint8_t>((cert_len >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((cert_len >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((cert_len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(cert_len & 0xFF));
        out.insert(out.end(), cert.begin(), cert.end());

        uint32_t sig_len = static_cast<uint32_t>(enc_sig.size());
        out.push_back(static_cast<uint8_t>((sig_len >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((sig_len >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(sig_len & 0xFF));
        out.insert(out.end(), enc_sig.begin(), enc_sig.end());

        CPS_LOG_INFO("AuthSetup", "MFi-SAP exchange succeeded: M2 size=", std::to_string(out.size()),
                     " bytes (cert=", std::to_string(cert.size()), ", sig=", std::to_string(sig.size()),
                     ", gen=", is_v2 ? "2nd" : "3rd", ")");
        return out;
    }

private:
    std::shared_ptr<Hardware::IMfiSigner> signer_;
};

} // namespace CarPlayStation::Core::Session::Pairing
