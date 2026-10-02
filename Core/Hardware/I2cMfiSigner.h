#pragma once

#include "Core/Hardware/IMfiSigner.h"
#include "Core/Hardware/II2cAdapter.h"
#include <memory>
#include <mutex>
#include <chrono>

namespace CarPlayStation::Core::Hardware {

// Universal MFi hardware signer that drives an Apple authentication coprocessor
// (MFi 2.0C / 3.0 silicon) over an abstract I2C bus adapter (II2cAdapter).
// Decoupled from specific USB bridge hardware (CH341, CH347, FTDI, Mock).
class I2cMfiSigner : public IMfiSigner {
public:
    struct Options {
        std::chrono::milliseconds io_retry_timeout{2000};
        std::chrono::milliseconds io_retry_delay{20};
        std::chrono::milliseconds auth_timeout{3000};
        std::chrono::milliseconds auth_poll_interval{10};
        bool auto_pulse_reset_on_failure{true};
        uint8_t reset_gpio_pin{0};
    };

    explicit I2cMfiSigner(std::shared_ptr<II2cAdapter> adapter,
                         uint8_t slave_address7Bit = 0x11,
                         Options options = {});
    ~I2cMfiSigner() override;

    [[nodiscard]] Common::Result<Common::ByteBuffer> FetchCertificate() override;
    [[nodiscard]] Common::Result<Common::ByteBuffer> SignChallenge(Common::ByteSpan challenge) override;
    [[nodiscard]] MFiChipInfo GetChipInfo() const noexcept override;
    void Shutdown() override;

    [[nodiscard]] std::shared_ptr<II2cAdapter> GetAdapter() const noexcept {
        return adapter_;
    }

    [[nodiscard]] uint8_t GetSlaveAddress() const noexcept {
        return slave_address_;
    }

private:
    [[nodiscard]] Common::Result<uint8_t> ReadByte(uint8_t reg);
    [[nodiscard]] Common::Result<uint16_t> ReadBigEndianShort(uint8_t reg);
    [[nodiscard]] Common::Result<void> WriteBigEndianShort(uint8_t reg, uint16_t value);
    [[nodiscard]] Common::Result<Common::ByteBuffer> ReadRegister(uint8_t reg, size_t length);
    [[nodiscard]] Common::Result<void> WriteRegister(uint8_t reg, Common::ByteSpan data);

    // Lazily open the I2C adapter on first use. Must be called with mutex_ held.
    void EnsureOpen() const;

    std::shared_ptr<II2cAdapter> adapter_;
    uint8_t slave_address_{0x11};
    Options options_{};
    mutable std::mutex mutex_;
    bool is_shutdown_{false};
};

} // namespace CarPlayStation::Core::Hardware

