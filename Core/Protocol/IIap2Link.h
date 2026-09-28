#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <functional>

namespace CarPlayStation::Core::Protocol {

// iAP2 Session IDs
enum class Iap2SessionId : uint8_t {
    Control = 1,
    FileTransfer = 2,
    ExternalAccessory = 3
};

// iAP2 Control Flags (Bits in control byte)
namespace Iap2ControlFlags {
    constexpr uint8_t SYN = 0x80;
    constexpr uint8_t ACK = 0x40;
    constexpr uint8_t EAK = 0x20;
    constexpr uint8_t RST = 0x10;
    constexpr uint8_t SLP = 0x08;
}

// iAP2 Packet Header representation
struct Iap2Header {
    uint16_t packet_length{0};
    uint8_t control_byte{0};
    uint8_t seq_num{0};
    uint8_t ack_num{0};
    uint8_t session_id{0};
    uint8_t header_checksum{0};

    [[nodiscard]] bool HasFlag(uint8_t flag) const noexcept {
        return (control_byte & flag) != 0;
    }
};

class IIap2PacketHandler {
public:
    virtual ~IIap2PacketHandler() = default;

    // Called when a complete and checksum-verified iAP2 message payload is assembled for a specific session
    virtual void OnIap2Message(uint8_t sessionId, Common::ByteSpan payload) = 0;

    // Called on protocol level link events (syn handshake completed, reset, etc.)
    virtual void OnLinkEvent(std::string_view eventDescription) = 0;
};

class IIap2Link {
public:
    virtual ~IIap2Link() = default;

    // Feed raw bytes from transport (Bluetooth / USB / Wi-Fi) into the iAP2 framing engine
    [[nodiscard]] virtual Common::Status FeedData(Common::ByteSpan rawBytes) = 0;

    // Send payload over a specific iAP2 session
    [[nodiscard]] virtual Common::Status SendMessage(uint8_t sessionId, Common::ByteSpan payload) = 0;

    // Register listener for demuxed session messages
    virtual void SetPacketHandler(IIap2PacketHandler* handler) = 0;

    // Reset link state machine and sequence numbers
    virtual void Reset() = 0;
};

} // namespace CarPlayStation::Core::Protocol

