#pragma once

#include "Core/Hardware/II2cAdapter.h"
#include "Core/Hardware/Ch341Protocol.h"
#include <mutex>
#include <memory>

namespace CarPlayStation::Core::Hardware {

// Adapter implementation for WCH CH341 USB-to-I2C controller.
// Dynamically loads CH341DLL.DLL on Windows to communicate with official vendor drivers,
// with safe fallback and protocol frame generation.
class Ch341I2cAdapter : public II2cAdapter {
public:
    Ch341I2cAdapter();
    ~Ch341I2cAdapter() override;

    Common::Result<void> Open(uint32_t device_index = 0) override;
    void Close() override;
    [[nodiscard]] bool IsOpen() const noexcept override;

    Common::Result<void> SetSpeed(I2cClockSpeed speed) override;

    Common::Result<void> Write(uint8_t address7Bit, Common::ByteSpan data) override;
    Common::Result<Common::ByteBuffer> Read(uint8_t address7Bit, size_t length) override;
    Common::Result<Common::ByteBuffer> Transaction(uint8_t address7Bit,
                                                 Common::ByteSpan writeData,
                                                 size_t readLength) override;

    Common::Result<void> PulseReset(uint8_t pin_index = 0,
                                   std::chrono::milliseconds low_duration = std::chrono::milliseconds(20),
                                   bool drive_high = true) override;

    [[nodiscard]] std::string GetAdapterName() const override {
        return "WCH CH341 USB-to-I2C Bridge";
    }

    // Diagnostics / driver availability query
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

