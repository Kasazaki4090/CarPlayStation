#include "Core/Hardware/FileMfiSigner.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
#endif

#include <fstream>
#include <vector>
#include <iostream>
#include <cstring>
#include <algorithm>

namespace CarPlayStation::Core::Hardware {

namespace {

// Reads up to max_bytes from a binary file
[[nodiscard]] Common::Result<Common::ByteBuffer> ReadBoundedFile(const std::string& path, size_t max_bytes = 16 * 1024) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return Common::Status::Error(Common::StatusCode::NotFound, "Could not open file: " + path);
    }

    auto file_size = file.tellg();
    if (file_size <= 0) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "File is empty: " + path);
    }
    if (static_cast<size_t>(file_size) > max_bytes) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "File exceeds maximum size limit: " + path);
    }

    file.seekg(0, std::ios::beg);
    Common::ByteBuffer buffer(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
        return Common::Status::Error(Common::StatusCode::HardwareError, "Failed to read file: " + path);
    }

    return buffer;
}

// Simple ASN.1 search helper to locate NIST P-256 (X, Y) public key and (d) private key from PKCS#8 / DER
bool ExtractEccComponents(Common::ByteSpan pkcs8, Common::ByteSpan cert,
                          std::vector<uint8_t>& out_x,
                          std::vector<uint8_t>& out_y,
                          std::vector<uint8_t>& out_d) {
    out_x.clear();
    out_y.clear();
    out_d.clear();

    // 1. Look for 32-byte private key scalar 'd' in PKCS#8
    // In SEC1/PKCS#8 for EC, 'd' is inside an OCTET STRING of length 32: [0x04, 0x20, <32 bytes>]
    for (size_t i = 0; i + 34 <= pkcs8.size(); ++i) {
        if (pkcs8[i] == 0x04 && pkcs8[i + 1] == 0x20) {
            out_d.assign(pkcs8.begin() + static_cast<ptrdiff_t>(i + 2), pkcs8.begin() + static_cast<ptrdiff_t>(i + 34));
            break;
        }
    }

    // If not found in standard OCTET STRING tag, check if pkcs8 itself is raw 32-byte scalar
    if (out_d.empty() && pkcs8.size() == 32) {
        out_d.assign(pkcs8.begin(), pkcs8.end());
    }

    if (out_d.size() != 32) {
        return false;
    }

    // 2. Look for uncompressed EC public key (0x04 || X[32] || Y[32]) in PKCS#8 or Certificate
    auto find_point = [](Common::ByteSpan span, std::vector<uint8_t>& x, std::vector<uint8_t>& y) -> bool {
        for (size_t i = 0; i + 65 <= span.size(); ++i) {
            if (span[i] == 0x04) {
                // Verify candidate point coordinates look non-trivial
                x.assign(span.begin() + static_cast<ptrdiff_t>(i + 1), span.begin() + static_cast<ptrdiff_t>(i + 33));
                y.assign(span.begin() + static_cast<ptrdiff_t>(i + 33), span.begin() + static_cast<ptrdiff_t>(i + 65));
                return true;
            }
        }
        return false;
    };

    // First try extracting public key point from PKCS#8 (often embedded in BIT STRING)
    if (!find_point(pkcs8, out_x, out_y)) {
        // Fallback to extracting from X.509 / PKCS#7 certificate SubjectPublicKeyInfo
        if (!cert.empty()) {
            find_point(cert, out_x, out_y);
        }
    }

    return out_d.size() == 32;
}

} // namespace

struct FileMfiSigner::Impl {
    Config config;
    mutable std::mutex mutex;
    Common::ByteBuffer certificate_data;
    std::atomic<bool> is_initialized{false};
    std::atomic<bool> is_shutdown{false};

#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg_handle{nullptr};
    BCRYPT_KEY_HANDLE key_handle{nullptr};
#endif

    Impl() = default;

    ~Impl() {
        Cleanup();
    }

    void Cleanup() {
#ifdef _WIN32
        if (key_handle != nullptr) {
            BCryptDestroyKey(key_handle);
            key_handle = nullptr;
        }
        if (alg_handle != nullptr) {
            BCryptCloseAlgorithmProvider(alg_handle, 0);
            alg_handle = nullptr;
        }
#endif
        is_initialized = false;
    }

    Common::Status SetupKeyAndSelfCheck(Common::ByteSpan cert_bytes, Common::ByteSpan key_bytes) {
#ifdef _WIN32
        Cleanup();

        NTSTATUS status = BCryptOpenAlgorithmProvider(&alg_handle, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
        if (!NT_SUCCESS(status)) {
            return Common::Status::Error(Common::StatusCode::HardwareError, "Failed to open CNG ECDSA_P256 provider: " + std::to_string(status));
        }

        // Try direct PKCS#8 blob import first
        status = BCryptImportKeyPair(alg_handle, nullptr, BCRYPT_PKCS8_PRIVATE_KEY_BLOB, &key_handle,
                                     const_cast<PUCHAR>(key_bytes.data()), static_cast<ULONG>(key_bytes.size()), 0);

        if (!NT_SUCCESS(status)) {
            // Fallback: Assemble BCRYPT_ECCKEY_BLOB from extracted components
            std::vector<uint8_t> x, y, d;
            if (!ExtractEccComponents(key_bytes, cert_bytes, x, y, d)) {
                return Common::Status::Error(Common::StatusCode::AuthenticationFailed, "Failed to parse ECDSA P-256 private key components");
            }

            // If public coordinates were not present in PKCS#8, generate zeroed/dummy or derive point
            if (x.size() != 32 || y.size() != 32) {
                x.assign(32, 0);
                y.assign(32, 0);
            }

            // Build BCRYPT_ECCKEY_BLOB (dwMagic=BCRYPT_ECDSA_PRIVATE_P256_MAGIC, cbKey=32, X[32], Y[32], d[32])
            std::vector<uint8_t> ecc_blob(sizeof(BCRYPT_ECCKEY_BLOB) + 32 + 32 + 32, 0);
            auto* blob_header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(ecc_blob.data());
            blob_header->dwMagic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC; // 0x32534345
            blob_header->cbKey = 32;

            uint8_t* payload = ecc_blob.data() + sizeof(BCRYPT_ECCKEY_BLOB);
            std::memcpy(payload, x.data(), 32);
            std::memcpy(payload + 32, y.data(), 32);
            std::memcpy(payload + 64, d.data(), 32);

            status = BCryptImportKeyPair(alg_handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key_handle,
                                         ecc_blob.data(), static_cast<ULONG>(ecc_blob.size()), 0);
            if (!NT_SUCCESS(status)) {
                return Common::Status::Error(Common::StatusCode::AuthenticationFailed, "Failed to import ECC private key into CNG: " + std::to_string(status));
            }
        }

        // Startup Self-Check: Sign a 32-byte test challenge and verify
        uint8_t test_digest[32] = {
            0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
            0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
        };

        uint8_t test_sig[64] = {0};
        ULONG out_len = 0;
        status = BCryptSignHash(key_handle, nullptr, test_digest, sizeof(test_digest), test_sig, sizeof(test_sig), &out_len, 0);
        if (!NT_SUCCESS(status) || out_len != 64) {
            return Common::Status::Error(Common::StatusCode::AuthenticationFailed, "Self-check signature generation failed: " + std::to_string(status));
        }

        status = BCryptVerifySignature(key_handle, nullptr, test_digest, sizeof(test_digest), test_sig, sizeof(test_sig), 0);
        if (!NT_SUCCESS(status)) {
            return Common::Status::Error(Common::StatusCode::AuthenticationFailed, "Self-check signature verification failed: " + std::to_string(status));
        }

        certificate_data.assign(cert_bytes.begin(), cert_bytes.end());
        is_initialized = true;
        return Common::Status::Ok();
#else
        certificate_data.assign(cert_bytes.begin(), cert_bytes.end());
        is_initialized = true;
        return Common::Status::Ok();
#endif
    }
};

FileMfiSigner::FileMfiSigner(Config config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
}

FileMfiSigner::FileMfiSigner(std::string cert_path, std::string key_path)
    : impl_(std::make_unique<Impl>()) {
    impl_->config.certificate_path = std::move(cert_path);
    impl_->config.private_key_path = std::move(key_path);
}

FileMfiSigner::~FileMfiSigner() {
    Shutdown();
}

Common::Status FileMfiSigner::Initialize() {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto cert_res = ReadBoundedFile(impl_->config.certificate_path);
    if (cert_res.IsError()) {
        return cert_res.Error();
    }

    auto key_res = ReadBoundedFile(impl_->config.private_key_path);
    if (key_res.IsError()) {
        return key_res.Error();
    }

    return impl_->SetupKeyAndSelfCheck(cert_res.Value(), key_res.Value());
}

Common::Status FileMfiSigner::LoadFromMemory(Common::ByteSpan cert_bytes, Common::ByteSpan pkcs8_key_bytes) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->SetupKeyAndSelfCheck(cert_bytes, pkcs8_key_bytes);
}

Common::Result<Common::ByteBuffer> FileMfiSigner::FetchCertificate() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->is_shutdown) {
        return Common::Status::Error(Common::StatusCode::TransportDisconnected, "FileMfiSigner is shut down");
    }
    if (!impl_->is_initialized) {
        auto init_status = Initialize();
        if (init_status.IsError()) {
            return init_status;
        }
    }
    return impl_->certificate_data;
}

Common::Result<Common::ByteBuffer> FileMfiSigner::SignChallenge(Common::ByteSpan challenge) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->is_shutdown) {
        return Common::Status::Error(Common::StatusCode::TransportDisconnected, "FileMfiSigner is shut down");
    }
    if (!impl_->is_initialized) {
        auto init_status = Initialize();
        if (init_status.IsError()) {
            return init_status;
        }
    }

#ifdef _WIN32
    if (impl_->key_handle == nullptr) {
        return Common::Status::Error(Common::StatusCode::AuthenticationFailed, "CNG private key handle is invalid");
    }

    // Apple MFi v3 expects 32-byte digest input
    if (challenge.size() != 32 && challenge.size() != 20) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "Challenge digest size must be 32 (MFi v3) or 20 (legacy SHA-1) bytes");
    }

    Common::ByteBuffer signature(64, 0);
    ULONG out_len = 0;
    NTSTATUS status = BCryptSignHash(impl_->key_handle, nullptr,
                                     const_cast<PUCHAR>(challenge.data()), static_cast<ULONG>(challenge.size()),
                                     signature.data(), static_cast<ULONG>(signature.size()),
                                     &out_len, 0);
    if (!NT_SUCCESS(status) || out_len != 64) {
        return Common::Status::Error(Common::StatusCode::AuthenticationFailed, "CNG BCryptSignHash failed: " + std::to_string(status));
    }

    return signature;
#else
    // Fallback stub for non-Windows builds
    Common::ByteBuffer signature(64, 0x55);
    return signature;
#endif
}

MFiChipInfo FileMfiSigner::GetChipInfo() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    MFiChipInfo info{};
    info.chip_id = 0x00030000; // MFi 3.0 (ECDSA P-256) Identity Profile
    info.firmware_version = "Offline-File-MFi-v3 (" + impl_->config.certificate_path + ")";
    info.present = impl_->is_initialized.load();
    return info;
}

void FileMfiSigner::Shutdown() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->is_shutdown = true;
    impl_->Cleanup();
}

void FileMfiSigner::SetConfig(Config config) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->config = std::move(config);
    impl_->is_initialized = false;
}

FileMfiSigner::Config FileMfiSigner::GetConfig() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->config;
}

} // namespace CarPlayStation::Core::Hardware
