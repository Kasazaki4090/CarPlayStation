#pragma once

#include "Core/Hardware/IMfiSigner.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

namespace CarPlayStation::Core::Hardware {

// Pure local, offline software MFi 3.0 (v3) authentication signer (DEC-006).
//
// Operates on NIST P-256 (secp256r1) ECDSA cryptographic signing using Windows CNG (bcrypt.dll)
// without requiring physical USB-I2C chips or network signing daemons.
//
// Expected Files:
//   - Certificate: DER / X.509 / PKCS#7 certificate file (e.g. "certs/certificate.p7b" or ".der")
//   - Private Key: PKCS#8 DER private key file (e.g. "certs/identity.pk8" or raw P-256 key)
class FileMfiSigner : public IMfiSigner {
public:
    struct Config {
        std::string certificate_path{"certs/certificate.p7b"};
        std::string private_key_path{"certs/identity.pk8"};
    };

    explicit FileMfiSigner(Config config = {});
    FileMfiSigner(std::string cert_path, std::string key_path);
    ~FileMfiSigner() override;

    // Load and initialize keys from disk, performing startup self-check.
    [[nodiscard]] Common::Status Initialize();

    // Fetch the loaded accessory certificate (~945+ bytes).
    [[nodiscard]] Common::Result<Common::ByteBuffer> FetchCertificate() override;

    // Sign an authentication challenge digest (32-byte SHA-256 digest -> 64-byte raw R||S ECDSA signature).
    [[nodiscard]] Common::Result<Common::ByteBuffer> SignChallenge(Common::ByteSpan challenge) override;

    // Query chip / identity info.
    [[nodiscard]] MFiChipInfo GetChipInfo() const noexcept override;

    // Release cryptographic handles and memory.
    void Shutdown() override;

    // Update file paths (re-initialization required).
    void SetConfig(Config config);
    [[nodiscard]] Config GetConfig() const;

    // In-memory provisioning helper for testing without disk files.
    [[nodiscard]] Common::Status LoadFromMemory(Common::ByteSpan cert_bytes, Common::ByteSpan pkcs8_key_bytes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace CarPlayStation::Core::Hardware
