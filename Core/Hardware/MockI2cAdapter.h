#pragma once

#include "Core/Hardware/II2cAdapter.h"
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstring>

namespace CarPlayStation::Core::Hardware {

// In-memory virtual I2C adapter simulating an Apple MFi authentication coprocessor (CP 2.0C/3.0).
// Enables 100% deterministic unit testing without physical USB hardware or drivers.
class MockI2cAdapter : public II2cAdapter {
public:
    explicit MockI2cAdapter(uint8_t coprocessor_address = 0x11)
        : coprocessor_address_(coprocessor_address) {
        ResetRegisters();
    }

    Common::Result<void> Open(uint32_t device_index = 0) override {
        (void)device_index;
        is_open_ = true;
        return Common::Result<void>::Ok();
    }

    void Close() override {
        is_open_ = false;
    }

    [[nodiscard]] bool IsOpen() const noexcept override {
        return is_open_;
    }

    Common::Result<void> SetSpeed(I2cClockSpeed speed) override {
        speed_ = speed;
        return Common::Result<void>::Ok();
    }

    Common::Result<void> Write(uint8_t address7Bit, Common::ByteSpan data) override {
        return Transaction(address7Bit, data, 0).IsOk() 
            ? Common::Result<void>::Ok() 
            : Common::Status::Error(Common::StatusCode::HardwareError, "Mock I2C Write failed");
    }

    Common::Result<Common::ByteBuffer> Read(uint8_t address7Bit, size_t length) override {
        return Transaction(address7Bit, {}, length);
    }

    Common::Result<Common::ByteBuffer> Transaction(uint8_t address7Bit,
                                                 Common::ByteSpan writeData,
                                                 size_t readLength) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++transaction_count_;

        if (!is_open_) {
            return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "Mock I2C adapter is closed");
        }

        if (inject_nack_) {
            return Common::Status::Error(Common::StatusCode::HardwareError, "Simulated I2C NACK");
        }

        if (address7Bit != coprocessor_address_) {
            return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "Address NACK");
        }

        // Process write phase
        if (!writeData.empty()) {
            uint8_t reg = writeData[0];
            selected_register_ = reg;
            if (writeData.size() > 1) {
                // Register write
                Common::ByteBuffer payload(writeData.begin() + 1, writeData.end());
                HandleRegisterWrite(reg, payload);
            }
        }

        // Process read phase
        if (readLength > 0) {
            return HandleRegisterRead(selected_register_, readLength);
        }

        return Common::ByteBuffer{};
    }

    Common::Result<void> PulseReset(uint8_t pin_index = 0,
                                   std::chrono::milliseconds low_duration = std::chrono::milliseconds(20),
                                   bool drive_high = true) override {
        (void)pin_index;
        (void)low_duration;
        (void)drive_high;
        std::lock_guard<std::mutex> lock(mutex_);
        ++reset_count_;
        ResetRegisters();
        return Common::Result<void>::Ok();
    }

    [[nodiscard]] std::string GetAdapterName() const override {
        return "In-Memory Virtual MFi Coprocessor I2C Adapter";
    }

    // --- Mock Control & Inspection APIs ---

    void SetInjectNack(bool nack) {
        std::lock_guard<std::mutex> lock(mutex_);
        inject_nack_ = nack;
    }

    void SetCertificate(Common::ByteBuffer cert) {
        std::lock_guard<std::mutex> lock(mutex_);
        certificate_ = std::move(cert);
    }

    [[nodiscard]] size_t GetTransactionCount() const noexcept {
        return transaction_count_.load();
    }

    [[nodiscard]] size_t GetResetCount() const noexcept {
        return reset_count_.load();
    }

    [[nodiscard]] Common::ByteBuffer GetLastChallenge() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_challenge_;
    }

private:
    void ResetRegisters() {
        selected_register_ = 0x00;
        protocol_major_ = 0x02; // MFi 2.0C / 3.0
        auth_status_ = 0x00;
        error_code_ = 0x00;
        challenge_length_ = 0;
        last_challenge_.clear();

        // 945-byte default mock accessory certificate
        certificate_.resize(945);
        for (size_t i = 0; i < certificate_.size(); ++i) {
            certificate_[i] = static_cast<uint8_t>((i * 17 + 33) & 0xFF);
        }
    }

    void HandleRegisterWrite(uint8_t reg, const Common::ByteBuffer& data) {
        if (reg == 0x10 && !data.empty()) {
            if (data[0] == 0x01) { // AUTH_START_COMMAND
                // Compute simulated signature (128 bytes RSA)
                signature_.resize(128);
                for (size_t i = 0; i < signature_.size(); ++i) {
                    uint8_t seed = last_challenge_.empty() ? 0xAA : last_challenge_[i % last_challenge_.size()];
                    signature_[i] = static_cast<uint8_t>((seed ^ (i * 7 + 1)) & 0xFF);
                }
                auth_status_ = 0x10; // AUTH_SUCCESS_STATUS
            }
        } else if (reg == 0x20 && data.size() >= 2) {
            challenge_length_ = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        } else if (reg == 0x21) {
            last_challenge_ = data;
        }
    }

    Common::Result<Common::ByteBuffer> HandleRegisterRead(uint8_t reg, size_t length) {
        Common::ByteBuffer buf(length, 0);

        if (reg == 0x02) { // PROTOCOL_MAJOR
            if (length >= 1) buf[0] = protocol_major_;
        } else if (reg == 0x05) { // ERROR_CODE
            if (length >= 1) buf[0] = error_code_;
        } else if (reg == 0x10) { // AUTH_CONTROL_STATUS
            if (length >= 1) buf[0] = auth_status_;
        } else if (reg == 0x11) { // RESPONSE_LENGTH
            if (length >= 2) {
                uint16_t sig_len = static_cast<uint16_t>(signature_.size());
                buf[0] = static_cast<uint8_t>((sig_len >> 8) & 0xFF);
                buf[1] = static_cast<uint8_t>(sig_len & 0xFF);
            }
        } else if (reg == 0x12) { // RESPONSE_DATA
            size_t copy_len = std::min(length, signature_.size());
            std::memcpy(buf.data(), signature_.data(), copy_len);
        } else if (reg == 0x30) { // CERTIFICATE_LENGTH
            if (length >= 2) {
                uint16_t cert_len = static_cast<uint16_t>(certificate_.size());
                buf[0] = static_cast<uint8_t>((cert_len >> 8) & 0xFF);
                buf[1] = static_cast<uint8_t>(cert_len & 0xFF);
            }
        } else if (reg == 0x31) { // CERTIFICATE_DATA stream
            size_t copy_len = std::min(length, certificate_.size());
            std::memcpy(buf.data(), certificate_.data(), copy_len);
        }

        return buf;
    }

    uint8_t coprocessor_address_{0x11};
    bool is_open_{true};
    bool inject_nack_{false};
    I2cClockSpeed speed_{I2cClockSpeed::Khz100};

    uint8_t selected_register_{0x00};
    uint8_t protocol_major_{0x02};
    uint8_t auth_status_{0x00};
    uint8_t error_code_{0x00};
    uint16_t challenge_length_{0};

    Common::ByteBuffer certificate_;
    Common::ByteBuffer last_challenge_;
    Common::ByteBuffer signature_;

    std::atomic<size_t> transaction_count_{0};
    std::atomic<size_t> reset_count_{0};
    mutable std::mutex mutex_;
};

} // namespace CarPlayStation::Core::Hardware
