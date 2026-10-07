#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <cstdint>
#include <chrono>
#include <string>

namespace CarPlayStation::Core::Hardware {

// I2C Clock speed presets
enum class I2cClockSpeed : uint32_t {
    Khz20  = 20,
    Khz100 = 100,  // Standard Mode
    Khz400 = 400,  // Fast Mode
    Khz750 = 750,
    Khz1000 = 1000 // Fast Mode Plus
};

// Abstract hardware adapter interface for USB-to-I2C bridge controllers.
// Decouples MFi coprocessor protocol logic from specific physical silicon
// (WCH CH341, WCH CH347, FTDI MPSSE, Linux /dev/i2c, or mock/test buses).
class II2cAdapter {
public:
    virtual ~II2cAdapter() = default;

    // Open connection to the USB-I2C bridge device.
    virtual Common::Result<void> Open(uint32_t device_index = 0) = 0;

    // Close the connection and release USB / driver handles.
    virtual void Close() = 0;

    // Check if the adapter is currently open and communicating.
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;

    // Set I2C bus clock speed.
    virtual Common::Result<void> SetSpeed(I2cClockSpeed speed) = 0;

    // Execute a standard I2C write transaction (START -> Address(W) -> Data -> STOP).
    virtual Common::Result<void> Write(uint8_t address7Bit, Common::ByteSpan data) = 0;

    // Execute a standard I2C read transaction (START -> Address(R) -> Read -> STOP).
    virtual Common::Result<Common::ByteBuffer> Read(uint8_t address7Bit, size_t length) = 0;

    // Execute a combined I2C write-then-read transaction with Repeated Start:
    // START -> Address(W) -> WriteData -> REPEATED START -> Address(R) -> ReadData -> STOP.
    // If writeData is empty, acts as Read. If readLength is 0, acts as Write.
    virtual Common::Result<Common::ByteBuffer> Transaction(uint8_t address7Bit,
                                                         Common::ByteSpan writeData,
                                                         size_t readLength) = 0;

    // Optional hardware GPIO reset pulse for MFi coprocessor reset line (RST pin).
    // pin_index: GPIO index (e.g. D0..D5 on CH341).
    // low_duration: duration to hold pin LOW before releasing / driving HIGH.
    // drive_high: if true, drive HIGH actively; if false, release to input with pull-up.
    virtual Common::Result<void> PulseReset(uint8_t pin_index = 0,
                                           std::chrono::milliseconds low_duration = std::chrono::milliseconds(20),
                                           bool drive_high = true) {
        (void)pin_index;
        (void)low_duration;
        (void)drive_high;
        return Common::Result<void>::Ok();
    }

    // Probe if an I2C device acknowledges its 7-bit address.
    //
    // A zero-length write cannot answer this question: the USB bridge drivers
    // (CH341StreamI2C and friends) return success even when the slave NACKed
    // the address, so every address on the bus would look present. Ask for a
    // couple of bytes instead — an addressed slave drives the data lines, and a
    // slave that never answered leaves the bridge's read buffer at the idle
    // level (all 0xFF).
    virtual bool ProbeDevice(uint8_t address7Bit) {
        if (!IsOpen()) return false;
        auto res = Read(address7Bit, 2);
        if (res.IsError()) return false;
        const auto& data = res.Value();
        if (data.empty()) return false;
        for (uint8_t byte : data) {
            if (byte != 0xFF) return true;
        }
        return false;
    }

    // Get adapter name / driver description for diagnostics.
    [[nodiscard]] virtual std::string GetAdapterName() const = 0;
};

} // namespace CarPlayStation::Core::Hardware

