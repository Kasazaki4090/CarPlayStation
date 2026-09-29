#pragma once

#include "Core/Common/Types.h"
#include <cstdint>
#include <vector>
#include <algorithm>

namespace CarPlayStation::Core::Hardware {

// CH341 USB-to-I2C Stream Protocol Definitions & Frame Encoder
// Based on WCH CH341 technical specification and stream packet format.
namespace Ch341Protocol {

// Stream command constants
inline constexpr uint8_t CMD_I2C_STREAM    = 0xAA; // Start I2C stream command
inline constexpr uint8_t CMD_UIO_STREAM    = 0xAB; // Start UIO (GPIO) stream command
inline constexpr uint8_t CMD_STREAM_END    = 0x00; // Terminate stream

inline constexpr uint8_t I2C_STM_STA       = 0x74; // Generate I2C START condition
inline constexpr uint8_t I2C_STM_STO       = 0x75; // Generate I2C STOP condition
inline constexpr uint8_t I2C_STM_OUT       = 0x80; // Write data bytes (0x80 | length), bare 0x80 = 1 byte with ACK status
inline constexpr uint8_t I2C_STM_IN        = 0xC0; // Read data bytes (0xC0 | length)
inline constexpr uint8_t I2C_STM_SET       = 0x60; // Set clock frequency
inline constexpr uint8_t I2C_STM_US        = 0x40; // Delay microseconds (0x40 | us)
inline constexpr uint8_t I2C_STM_MS        = 0x50; // Delay milliseconds (0x50 | ms)

// Clock speed flags for I2C_STM_SET
inline constexpr uint8_t SPEED_20KHZ       = 0x60;
inline constexpr uint8_t SPEED_100KHZ      = 0x61;
inline constexpr uint8_t SPEED_400KHZ      = 0x62;
inline constexpr uint8_t SPEED_750KHZ      = 0x63;

inline constexpr size_t MAX_STREAM_PACKET_BYTES = 32;
inline constexpr size_t MAX_TRANSACTION_BYTES   = 0xFFFF;

// Helper to convert 7-bit I2C address to 8-bit wire address
[[nodiscard]] constexpr uint8_t MakeAddressByte(uint8_t address7Bit, bool isRead) noexcept {
    return static_cast<uint8_t>((static_cast<uint8_t>(address7Bit & 0x7F) << 1) | (isRead ? 1 : 0));
}

// Encode clock speed configuration packet
[[nodiscard]] inline Common::ByteBuffer EncodeSpeed(uint8_t speed_cmd = SPEED_100KHZ) {
    return Common::ByteBuffer{
        CMD_I2C_STREAM,
        speed_cmd,
        CMD_STREAM_END
    };
}

// Encode a complete I2C transaction into segmented CH341 stream commands.
// Uses bare 0x80 commands for each written byte so CH341 returns ACK/NACK status bytes.
[[nodiscard]] inline Common::ByteBuffer EncodeTransaction(uint8_t address7Bit,
                                                         Common::ByteSpan writeData,
                                                         size_t readLength) {
    Common::ByteBuffer stream;
    stream.reserve(MAX_STREAM_PACKET_BYTES * 2);

    auto flush_segment_if_needed = [&](size_t needed_space) {
        if (stream.size() % MAX_STREAM_PACKET_BYTES + needed_space > MAX_STREAM_PACKET_BYTES) {
            // Pad current packet to 32-byte boundary with CMD_STREAM_END
            size_t remainder = stream.size() % MAX_STREAM_PACKET_BYTES;
            if (remainder != 0) {
                stream.resize(stream.size() + (MAX_STREAM_PACKET_BYTES - remainder), CMD_STREAM_END);
            }
            stream.push_back(CMD_I2C_STREAM);
        }
    };

    stream.push_back(CMD_I2C_STREAM);

    // 1. Write Phase (if writeData is provided)
    if (!writeData.empty() || readLength == 0) {
        stream.push_back(I2C_STM_STA); // START
        
        // Write Address(W) with bare 0x80
        stream.push_back(I2C_STM_OUT);
        stream.push_back(MakeAddressByte(address7Bit, false));

        // Write each payload byte with bare 0x80
        for (uint8_t b : writeData) {
            flush_segment_if_needed(2);
            stream.push_back(I2C_STM_OUT);
            stream.push_back(b);
        }
    }

    // 2. Read Phase (if readLength > 0)
    if (readLength > 0) {
        flush_segment_if_needed(2);
        stream.push_back(I2C_STM_STA); // REPEATED START or initial START
        stream.push_back(I2C_STM_OUT);
        stream.push_back(MakeAddressByte(address7Bit, true));

        size_t remaining_reads = readLength;
        while (remaining_reads > 0) {
            // Read chunks (up to 32 or remaining segment space)
            size_t chunk = std::min(remaining_reads, static_cast<size_t>(32));
            flush_segment_if_needed(1);
            stream.push_back(static_cast<uint8_t>(I2C_STM_IN | (chunk == 32 ? 0 : chunk)));
            remaining_reads -= chunk;
        }
    }

    stream.push_back(I2C_STM_STO); // STOP
    stream.push_back(CMD_STREAM_END);

    return stream;
}

// Encode GPIO reset pulse (e.g. driving D0..D5 low then high)
[[nodiscard]] inline Common::ByteBuffer EncodeResetPulse(uint8_t pin_index, bool drive_high = true) {
    uint8_t pin_mask = static_cast<uint8_t>(1 << (pin_index & 0x07));
    Common::ByteBuffer stream;
    stream.reserve(16);

    stream.push_back(CMD_UIO_STREAM);
    // 0x80: set outputs. byte 1: enable/dir, byte 2: data
    stream.push_back(0x80);
    stream.push_back(pin_mask); // Direction: output
    stream.push_back(0x00);     // Data: LOW

    if (drive_high) {
        stream.push_back(0x80);
        stream.push_back(pin_mask); // Direction: output
        stream.push_back(pin_mask); // Data: HIGH
    }

    stream.push_back(CMD_STREAM_END);
    return stream;
}

} // namespace Ch341Protocol
} // namespace CarPlayStation::Core::Hardware

