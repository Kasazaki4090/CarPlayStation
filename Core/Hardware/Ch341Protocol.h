#pragma once

#include "Core/Common/Types.h"
#include <cstdint>
#include <vector>
#include <algorithm>

namespace CarPlayStation::Core::Hardware {

// CH341 USB-to-I2C Stream Protocol Definitions & Frame Encoder
// Implemented to match the reference xcertplay implementation (Ch341I2cStreamEncoder.kt).
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

// Clock speed flags for I2C_STM_SET
inline constexpr uint8_t SPEED_20KHZ       = 0x60;
inline constexpr uint8_t SPEED_100KHZ      = 0x61;
inline constexpr uint8_t SPEED_400KHZ      = 0x62;
inline constexpr uint8_t SPEED_750KHZ      = 0x63;

// UIO constants
inline constexpr uint8_t UIO_OUT           = 0x80;
inline constexpr uint8_t UIO_DIR           = 0x40;
inline constexpr uint8_t UIO_END           = 0x20;

inline constexpr size_t MAX_STREAM_PACKET_BYTES   = 32;
inline constexpr size_t MAX_WRITE_COMMAND_BYTES   = 0x3F; // 63
inline constexpr size_t MAX_READ_BLOCK_BYTES      = 32;
inline constexpr size_t MAX_TRANSACTION_BYTES      = 0xFFFF;

// Helper to convert 7-bit I2C address to 8-bit wire address
[[nodiscard]] constexpr uint8_t MakeAddressByte(uint8_t address7Bit, bool isRead) noexcept {
    return static_cast<uint8_t>((static_cast<uint8_t>(address7Bit & 0x7F) << 1) | (isRead ? 1 : 0));
}

// Encode clock speed configuration packet: [0xAA, speed_cmd, 0x00]
[[nodiscard]] inline Common::ByteBuffer EncodeSpeed(uint8_t speed_cmd = SPEED_100KHZ) {
    return Common::ByteBuffer{
        CMD_I2C_STREAM,
        speed_cmd,
        CMD_STREAM_END
    };
}

struct InstructionLengths {
    size_t written_bytes = 0;
    size_t data_bytes = 0;
};

// Calculate how many status bytes (written_bytes) and data bytes (data_bytes)
// this CH341 stream segment produces.
[[nodiscard]] inline InstructionLengths GetInstructionLengths(Common::ByteSpan segment) {
    InstructionLengths lengths{};
    if (segment.empty() || segment[0] != CMD_I2C_STREAM) {
        return lengths;
    }
    size_t index = 1;
    while (index < segment.size()) {
        uint8_t cmd = segment[index++];
        if (cmd == CMD_STREAM_END) {
            return lengths;
        }
        if (cmd == I2C_STM_STA || cmd == I2C_STM_STO) {
            continue;
        }
        if (cmd >= 0x80 && cmd <= 0xBF) {
            uint8_t enc = cmd & 0x3F;
            size_t written = (enc == 0) ? 1 : enc;
            if (enc == 0) {
                // Bare 0x80 returns 1 ACK/NACK status byte per written byte
                lengths.written_bytes += written;
            }
            index += written;
        } else if (cmd >= 0xC0) {
            uint8_t enc = cmd & 0x3F;
            size_t read_cnt = (enc == 0) ? 1 : enc;
            lengths.data_bytes += read_cnt;
        } else if (cmd >= 0x60 && cmd <= 0x63) {
            // Speed set command
            continue;
        }
        if (index > segment.size()) {
            break;
        }
    }
    return lengths;
}

// Internal helper for legacy (single packet <= 32 bytes) transactions
[[nodiscard]] inline Common::ByteBuffer LegacyTransaction(uint8_t address7Bit,
                                                         Common::ByteSpan writeData,
                                                         size_t readLength) {
    if (readLength > MAX_READ_BLOCK_BYTES || (writeData.size() + 1) > MAX_WRITE_COMMAND_BYTES) {
        return {};
    }

    Common::ByteBuffer bytes;
    bytes.reserve(MAX_STREAM_PACKET_BYTES);

    auto add = [&](uint8_t val) { bytes.push_back(val); };
    auto write = [&](uint8_t addr, Common::ByteSpan data) {
        add(I2C_STM_OUT);
        add(addr);
        for (uint8_t b : data) {
            add(I2C_STM_OUT);
            add(b);
        }
    };

    add(CMD_I2C_STREAM);
    if (!writeData.empty()) {
        add(I2C_STM_STA);
        write(MakeAddressByte(address7Bit, false), writeData);
    }
    if (readLength > 0) {
        add(I2C_STM_STA);
        write(MakeAddressByte(address7Bit, true), {});
        if (readLength > 1) {
            add(static_cast<uint8_t>(I2C_STM_IN + (readLength - 1)));
        }
        add(I2C_STM_IN);
    }
    add(I2C_STM_STO);
    add(CMD_STREAM_END);

    if (bytes.size() <= MAX_STREAM_PACKET_BYTES) {
        return bytes;
    }
    return {};
}

// Encode a complete I2C transaction matching Ch341I2cStreamEncoder.kt
[[nodiscard]] inline Common::ByteBuffer EncodeTransaction(uint8_t address7Bit,
                                                         Common::ByteSpan writeData,
                                                         size_t readLength) {
    // Try legacy single-packet format first
    auto legacy = LegacyTransaction(address7Bit, writeData, readLength);
    if (!legacy.empty()) {
        return legacy;
    }

    // SegmentedStream format for larger payloads
    Common::ByteBuffer result;
    Common::ByteBuffer segment;
    segment.reserve(MAX_STREAM_PACKET_BYTES);
    segment.push_back(CMD_I2C_STREAM);

    auto finish_intermediate = [&]() {
        segment.push_back(CMD_STREAM_END);
        while (segment.size() < MAX_STREAM_PACKET_BYTES) {
            segment.push_back(0x00);
        }
        result.insert(result.end(), segment.begin(), segment.end());
        segment.clear();
        segment.push_back(CMD_I2C_STREAM);
    };

    auto command = [&](Common::ByteSpan cmd_bytes) {
        if (segment.size() + cmd_bytes.size() + 1 > MAX_STREAM_PACKET_BYTES) {
            finish_intermediate();
        }
        segment.insert(segment.end(), cmd_bytes.begin(), cmd_bytes.end());
    };

    auto write = [&](uint8_t addr, Common::ByteSpan data) {
        if (data.empty()) {
            // Batched 0x81 suppresses status byte for segmented read setup
            uint8_t cmd[2] = { static_cast<uint8_t>(I2C_STM_OUT + 1), addr };
            command(Common::ByteSpan(cmd, 2));
            return;
        }
        uint8_t cmd[2] = { I2C_STM_OUT, addr };
        command(Common::ByteSpan(cmd, 2));
        for (uint8_t b : data) {
            uint8_t b_cmd[2] = { I2C_STM_OUT, b };
            command(Common::ByteSpan(b_cmd, 2));
        }
    };

    auto start_and_write = [&](uint8_t addr, Common::ByteSpan data) {
        // START(1) + ADDR(2) + END(1) = 4
        if (segment.size() + 4 > MAX_STREAM_PACKET_BYTES) {
            finish_intermediate();
        }
        segment.push_back(I2C_STM_STA);
        write(addr, data);
    };

    auto read = [&](size_t length) {
        size_t remaining = length;
        while (remaining > MAX_READ_BLOCK_BYTES) {
            uint8_t c = static_cast<uint8_t>(I2C_STM_IN + MAX_READ_BLOCK_BYTES);
            command(Common::ByteSpan(&c, 1));
            finish_intermediate();
            remaining -= MAX_READ_BLOCK_BYTES;
        }
        if (remaining > 1) {
            uint8_t c = static_cast<uint8_t>(I2C_STM_IN + (remaining - 1));
            command(Common::ByteSpan(&c, 1));
        }
        uint8_t c = I2C_STM_IN;
        command(Common::ByteSpan(&c, 1));
    };

    if (!writeData.empty()) {
        start_and_write(MakeAddressByte(address7Bit, false), writeData);
    }
    if (readLength > 0) {
        start_and_write(MakeAddressByte(address7Bit, true), {});
        read(readLength);
    }

    if (segment.size() + 2 > MAX_STREAM_PACKET_BYTES) {
        finish_intermediate();
    }
    segment.push_back(I2C_STM_STO);
    segment.push_back(CMD_STREAM_END);
    result.insert(result.end(), segment.begin(), segment.end());

    return result;
}

// Encode GPIO reset pulse streams matching xcertplay pulseActiveLowReset:
// low_stream: [0xAB, 0x80, 0x40 | pin_mask, 0x20]
// release_stream: [0xAB, drive_high ? (0x80 | pin_mask) : 0x80, drive_high ? (0x40 | pin_mask) : 0x40, 0x20]
[[nodiscard]] inline Common::ByteBuffer EncodeResetLow(uint8_t pin_index) {
    uint8_t pin_mask = static_cast<uint8_t>(1 << (pin_index & 0x07));
    return Common::ByteBuffer{
        CMD_UIO_STREAM, // 0xAB
        UIO_OUT,        // 0x80
        static_cast<uint8_t>(UIO_DIR | pin_mask), // 0x40 | pin_mask
        UIO_END         // 0x20
    };
}

[[nodiscard]] inline Common::ByteBuffer EncodeResetRelease(uint8_t pin_index, bool drive_high = true) {
    uint8_t pin_mask = static_cast<uint8_t>(1 << (pin_index & 0x07));
    return Common::ByteBuffer{
        CMD_UIO_STREAM, // 0xAB
        static_cast<uint8_t>(drive_high ? (UIO_OUT | pin_mask) : UIO_OUT),
        static_cast<uint8_t>(drive_high ? (UIO_DIR | pin_mask) : UIO_DIR),
        UIO_END         // 0x20
    };
}

// Legacy single-buffer EncodeResetPulse for backward compatibility with existing tests
[[nodiscard]] inline Common::ByteBuffer EncodeResetPulse(uint8_t pin_index, bool drive_high = true) {
    return EncodeResetRelease(pin_index, drive_high);
}

} // namespace Ch341Protocol
} // namespace CarPlayStation::Core::Hardware

