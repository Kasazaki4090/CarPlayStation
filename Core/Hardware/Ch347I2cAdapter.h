#pragma once

#include "Core/Hardware/II2cAdapter.h"
#include <mutex>
#include <memory>

namespace CarPlayStation::Core::Hardware {

// Adapter implementation for WCH CH347 High-Speed USB-to-I2C bridge controller.
// Dynamically loads CH347DLL.DLL on Windows to communicate with official vendor drivers.
class Ch347I2cAdapter : public II2cAdapter {
public:
    Ch347I2cAdapter();
    ~Ch347I2cAdapter() override;

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
        return "WCH CH347 High-Speed USB-to-I2C Bridge";
    }

    [[nodiscard]] bool IsDriverLoaded() const noexcept;

private:
    class DriverBinding;
    // Applies speed_ to the hardware. Callers must already hold mutex_.
    void ApplySpeedLocked();

    std::unique_ptr<DriverBinding> driver_;
    mutable std::mutex mutex_;
    uint32_t device_index_{0};
    bool is_open_{false};
    I2cClockSpeed speed_{I2cClockSpeed::Khz100};
};

} // namespace CarPlayStation::Core::Hardware

