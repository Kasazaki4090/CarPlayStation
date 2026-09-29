#pragma once

#include "Core/Hardware/IMfiSigner.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <vector>
#include <string>
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>

namespace CarPlayStation::Core::Hardware {

// Mock implementation of IMfiSigner for offline unit testing and protocol verification.
class MockMfiSigner : public IMfiSigner {
public:
    explicit MockMfiSigner(bool present = true) {
        chip_info_.chip_id = 0x000200C0; // Standard MFi 2.0C Chip ID representation
        chip_info_.firmware_version = "Mock-MFi-2.0C-v1.0";
        chip_info_.present = present;

        // Default mock certificate (945 bytes realistic length for MFi 2.0C cert)
        mock_certificate_.resize(945);
        for (size_t i = 0; i < mock_certificate_.size(); ++i) {
            mock_certificate_[i] = static_cast<uint8_t>((i * 31 + 17) & 0xFF);
        }
    }

    [[nodiscard]] Common::Result<Common::ByteBuffer> FetchCertificate() override {
        ++fetch_cert_calls_;

        if (simulated_latency_.count() > 0) {
            std::this_thread::sleep_for(simulated_latency_);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_fetch_cert_) {
            return Common::Status::Error(fail_fetch_code_, fail_fetch_msg_.empty() ? "Mock fetch cert failure" : fail_fetch_msg_);
        }

        return mock_certificate_;
    }

    [[nodiscard]] Common::Result<Common::ByteBuffer> SignChallenge(Common::ByteSpan challenge) override {
        ++sign_challenge_calls_;

        if (simulated_latency_.count() > 0) {
            std::this_thread::sleep_for(simulated_latency_);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        last_challenge_.assign(challenge.begin(), challenge.end());

        if (fail_sign_challenge_) {
            return Common::Status::Error(fail_sign_code_, fail_sign_msg_.empty() ? "Mock sign challenge failure" : fail_sign_msg_);
        }

        if (custom_signer_) {
            return custom_signer_(challenge);
        }

        // Default: generate standard 128-byte deterministic RSA-like signature derived from challenge
        Common::ByteBuffer signature(128, 0);
        for (size_t i = 0; i < signature.size(); ++i) {
            uint8_t seed = challenge.empty() ? 0x5A : challenge[i % challenge.size()];
            signature[i] = static_cast<uint8_t>((seed ^ (i * 13 + 7)) & 0xFF);
        }
        return signature;
    }

    [[nodiscard]] MFiChipInfo GetChipInfo() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return chip_info_;
    }

    void Shutdown() override {
        is_shutdown_ = true;
    }

    // --- Configuration helpers for test harnesses ---

    void SetCertificate(Common::ByteBuffer cert) {
        std::lock_guard<std::mutex> lock(mutex_);
        mock_certificate_ = std::move(cert);
    }

    void SetCustomSigner(std::function<Common::Result<Common::ByteBuffer>(Common::ByteSpan)> signer) {
        std::lock_guard<std::mutex> lock(mutex_);
        custom_signer_ = std::move(signer);
    }

    void SetFailFetchCertificate(bool fail, Common::StatusCode code = Common::StatusCode::AuthenticationFailed, std::string msg = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_fetch_cert_ = fail;
        fail_fetch_code_ = code;
        fail_fetch_msg_ = std::move(msg);
    }

    void SetFailSignChallenge(bool fail, Common::StatusCode code = Common::StatusCode::AuthenticationFailed, std::string msg = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_sign_challenge_ = fail;
        fail_sign_code_ = code;
        fail_sign_msg_ = std::move(msg);
    }

    void SetSimulatedLatency(std::chrono::milliseconds latency) noexcept {
        simulated_latency_ = latency;
    }

    void SetChipInfo(MFiChipInfo info) {
        std::lock_guard<std::mutex> lock(mutex_);
        chip_info_ = std::move(info);
    }

    [[nodiscard]] size_t GetFetchCertCalls() const noexcept { return fetch_cert_calls_.load(); }
    [[nodiscard]] size_t GetSignChallengeCalls() const noexcept { return sign_challenge_calls_.load(); }
    [[nodiscard]] bool IsShutdown() const noexcept { return is_shutdown_.load(); }

    [[nodiscard]] Common::ByteBuffer GetLastChallenge() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_challenge_;
    }

    void ResetMetrics() noexcept {
        fetch_cert_calls_ = 0;
        sign_challenge_calls_ = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        last_challenge_.clear();
    }

private:
    mutable std::mutex mutex_;
    MFiChipInfo chip_info_{};
    Common::ByteBuffer mock_certificate_{};
    Common::ByteBuffer last_challenge_{};
    std::function<Common::Result<Common::ByteBuffer>(Common::ByteSpan)> custom_signer_{};

    std::atomic<size_t> fetch_cert_calls_{0};
    std::atomic<size_t> sign_challenge_calls_{0};
    std::atomic<bool> is_shutdown_{false};
    std::chrono::milliseconds simulated_latency_{0};

    bool fail_fetch_cert_{false};
    Common::StatusCode fail_fetch_code_{Common::StatusCode::AuthenticationFailed};
    std::string fail_fetch_msg_{};

    bool fail_sign_challenge_{false};
    Common::StatusCode fail_sign_code_{Common::StatusCode::AuthenticationFailed};
    std::string fail_sign_msg_{};
};

} // namespace CarPlayStation::Core::Hardware
