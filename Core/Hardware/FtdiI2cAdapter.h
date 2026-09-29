#pragma once

#include "Core/Hardware/II2cAdapter.h"
#include <mutex>
#include <memory>

namespace CarPlayStation::Core::Hardware {

// Adapter implementation for FTDI FT232H / FT2232H / FT4232H MPSSE USB-to-I2C controller.
// Dynamically loads FTDI D2XX / libMPSSE on Windows.
class FtdiI2cAdapter : public II2cAdapter {
public:
    FtdiI2cAdapter();
    ~FtdiI2cAdapter() override;

    Common::Result<void> Open(uint32_t device_index = 0) override;
    void Close() override;
    [[nodiscard]] bool IsOpen() const noexcept override;

    Common::Result<void> SetSpeed(I2cClockSpeed speed) override;

    Common::Result<void> Write(uint8_t address7Bit, Common::ByteSpan data) override;
    Common::Result<Common::ByteBuffer> Read(uint8_t address7Bit, size_t length) override;
    Common::Result<Common::ByteBuffer> Transaction(uint8_t address7Bit,
                                                 Common::ByteSpan writeData,
                                                 size_t readLength) override;

    [[nodiscard]] std::string GetAdapterName() const override {
        return "FTDI MPSSE USB-to-I2C Bridge";
    }

    [[nodiscard]] bool IsDriverLoaded() const noexcept;

private:
    class DriverBinding;
    std::unique_ptr<DriverBinding> driver_;
    mutable std::mutex mutex_;
    uint32_t device_index_{0};
    bool is_open_{false};
    I2cClockSpeed speed_{I2cClockSpeed::Khz100};
};

} // namespace CarPlayStation::Core::Hardware

