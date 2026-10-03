#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <string>
#include <string_view>
#include <array>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <bit>

namespace CarPlayStation::Core::Common {

// Basic Byte Buffers and Spans
using Byte = uint8_t;
using ByteBuffer = std::vector<uint8_t>;
using ByteSpan = std::span<const uint8_t>;
using MutableByteSpan = std::span<uint8_t>;

// Time abstractions
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Milliseconds = std::chrono::milliseconds;
using Microseconds = std::chrono::microseconds;
using Nanoseconds = std::chrono::nanoseconds;

// MAC Address (6 bytes)
struct MacAddress {
    std::array<uint8_t, 6> bytes{};

    constexpr MacAddress() = default;
    constexpr MacAddress(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5)
        : bytes{b0, b1, b2, b3, b4, b5} {}

    [[nodiscard]] std::string ToString() const {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (i > 0) oss << ":";
            oss << std::setw(2) << static_cast<int>(bytes[i]);
        }
        return oss.str();
    }

    // True when all six octets are zero (i.e. the address is unset/unknown).
    [[nodiscard]] bool IsZero() const noexcept {
        for (uint8_t b : bytes) {
            if (b != 0) return false;
        }
        return true;
    }

    auto operator<=>(const MacAddress&) const = default;
};

// IP Network Endpoint
struct IpEndpoint {
    std::string address{"127.0.0.1"};
    uint16_t port{0};

    [[nodiscard]] std::string ToString() const {
        return address + ":" + std::to_string(port);
    }

    auto operator<=>(const IpEndpoint&) const = default;
};

// Endianness and Binary Parsing Helpers
namespace Endian {

[[nodiscard]] inline uint16_t ReadBe16(const uint8_t* ptr) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(ptr[0]) << 8) | static_cast<uint16_t>(ptr[1]));
}

[[nodiscard]] inline uint32_t ReadBe32(const uint8_t* ptr) noexcept {
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8)  |
           static_cast<uint32_t>(ptr[3]);
}

[[nodiscard]] inline uint64_t ReadBe64(const uint8_t* ptr) noexcept {
    return (static_cast<uint64_t>(ptr[0]) << 56) |
           (static_cast<uint64_t>(ptr[1]) << 48) |
           (static_cast<uint64_t>(ptr[2]) << 40) |
           (static_cast<uint64_t>(ptr[3]) << 32) |
           (static_cast<uint64_t>(ptr[4]) << 24) |
           (static_cast<uint64_t>(ptr[5]) << 16) |
           (static_cast<uint64_t>(ptr[6]) << 8)  |
           static_cast<uint64_t>(ptr[7]);
}

[[nodiscard]] inline uint16_t ReadLe16(const uint8_t* ptr) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(ptr[1]) << 8) | static_cast<uint16_t>(ptr[0]));
}

[[nodiscard]] inline uint32_t ReadLe32(const uint8_t* ptr) noexcept {
    return (static_cast<uint32_t>(ptr[3]) << 24) |
           (static_cast<uint32_t>(ptr[2]) << 16) |
           (static_cast<uint32_t>(ptr[1]) << 8)  |
           static_cast<uint32_t>(ptr[0]);
}

inline void WriteBe16(uint8_t* ptr, uint16_t val) noexcept {
    ptr[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
    ptr[1] = static_cast<uint8_t>(val & 0xFF);
}

inline void WriteBe32(uint8_t* ptr, uint32_t val) noexcept {
    ptr[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    ptr[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    ptr[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    ptr[3] = static_cast<uint8_t>(val & 0xFF);
}

inline void WriteBe64(uint8_t* ptr, uint64_t val) noexcept {
    ptr[0] = static_cast<uint8_t>((val >> 56) & 0xFF);
    ptr[1] = static_cast<uint8_t>((val >> 48) & 0xFF);
    ptr[2] = static_cast<uint8_t>((val >> 40) & 0xFF);
    ptr[3] = static_cast<uint8_t>((val >> 32) & 0xFF);
    ptr[4] = static_cast<uint8_t>((val >> 24) & 0xFF);
    ptr[5] = static_cast<uint8_t>((val >> 16) & 0xFF);
    ptr[6] = static_cast<uint8_t>((val >> 8) & 0xFF);
    ptr[7] = static_cast<uint8_t>(val & 0xFF);
}

inline void WriteLe16(uint8_t* ptr, uint16_t val) noexcept {
    ptr[0] = static_cast<uint8_t>(val & 0xFF);
    ptr[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

inline void WriteLe32(uint8_t* ptr, uint32_t val) noexcept {
    ptr[0] = static_cast<uint8_t>(val & 0xFF);
    ptr[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    ptr[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    ptr[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

} // namespace Endian

// Hex formatting utility
[[nodiscard]] inline std::string ToHexString(ByteSpan data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < data.size(); ++i) {
        if (i > 0) oss << " ";
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

} // namespace CarPlayStation::Core::Common

