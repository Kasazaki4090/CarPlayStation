#pragma once

#include "Core/Protocol/IIap2Link.h"
#include <functional>
#include <mutex>

namespace CarPlayStation::Core::Protocol {

// iAP2 link-layer constants (faithful port of reference implementation)
namespace Iap2Constants {
    constexpr uint8_t SOP1 = 0xFF;
    constexpr uint8_t SOP2 = 0x5A;
    constexpr uint8_t CTL_SYN = 0x80;
    constexpr uint8_t CTL_ACK = 0x40;

    // The fixed 6-byte detect prelude (FF 55 02 00 EE 10) — not a link packet, sent before SYN.
    inline constexpr std::array<uint8_t, 6> DETECT = {0xFF, 0x55, 0x02, 0x00, 0xEE, 0x10};

    // The 16-byte SYN link parameters (max pkt / retransmit / cumulative-ack timings).
    inline constexpr std::array<uint8_t, 16> SYN_PARAMS = {
        1, 32, 0x10, 0x00, 0x07, 0xD0, 0x07, 0xD0, 30, 6, 1, 0, 1, 2, 1, 1
    };

    // Control-session payload magic prefix
    constexpr uint8_t MSG_PREFIX_0 = 0x40;
    constexpr uint8_t MSG_PREFIX_1 = 0x40;
}

// Two's-complement checksum: (0x100 - (sum & 0xFF)) & 0xFF
[[nodiscard]] inline uint8_t Iap2Checksum(Common::ByteSpan bytes) noexcept {
    uint32_t sum = 0;
    for (uint8_t b : bytes) sum += b;
    return static_cast<uint8_t>((0x100 - (sum & 0xFF)) & 0xFF);
}

// A parsed received link-layer packet.
struct Iap2RxPacket {
    uint8_t control{0};
    uint8_t seq{0};
    uint8_t ack{0};
    uint8_t session_id{0};
    Common::ByteBuffer payload{};  // post-header region (includes trailing checksum byte)

    [[nodiscard]] bool IsSynAck() const noexcept {
        return (control & 0xC0) == 0xC0;
    }
};

/**
 * @brief Concrete iAP2 link-layer framing engine.
 *
 * Implements the IIap2Link interface: parses SOP-framed packets from raw transport bytes,
 * validates header/payload checksums, tracks sequence/ack numbers, and builds outgoing
 * SYN / ACK / message packets. Pure byte logic — no I/O; the caller owns the transport.
 */
class Iap2LinkEngine : public IIap2Link {
public:
    Iap2LinkEngine();

    // --- IIap2Link Implementation ---
    [[nodiscard]] Common::Status FeedData(Common::ByteSpan rawBytes) override;
    [[nodiscard]] Common::Status SendMessage(uint8_t sessionId, Common::ByteSpan payload) override;
    void SetPacketHandler(IIap2PacketHandler* handler) override;
    void Reset() override;

    // --- Link Control Frame Builders (exposed for session manager / tests) ---

    // Build a SYN packet carrying the 16-byte link parameters (consumes a seq).
    [[nodiscard]] Common::ByteBuffer BuildSyn();

    // Build a bare ACK — header only, does NOT consume a seq.
    [[nodiscard]] Common::ByteBuffer BuildBareAck() const;

    // Build a control-session message packet: wraps msg_id + body in [40 40][total][msg_id] then frames.
    [[nodiscard]] Common::ByteBuffer BuildMessage(uint8_t sessionId, uint16_t messageId, Common::ByteSpan body);

    // Build a raw-payload packet (no [40 40] wrapper) — used by File Transfer session.
    [[nodiscard]] Common::ByteBuffer BuildRaw(uint8_t sessionId, Common::ByteSpan payload);

    // --- State Accessors ---
    [[nodiscard]] uint8_t GetMySeq() const noexcept { return my_seq_; }
    [[nodiscard]] uint8_t GetPeerSeq() const noexcept { return peer_seq_; }

    // Set the transport send function (the engine calls this to emit built packets).
    using SendFunction = std::function<Common::Status(Common::ByteSpan data)>;
    void SetSendFunction(SendFunction fn) { send_fn_ = std::move(fn); }

private:
    // Parse one link packet from the front of `buf`. Returns bytes consumed (0 on failure).
    [[nodiscard]] size_t TryParsePacket(Common::ByteSpan buf, Iap2RxPacket& out) const;

    uint8_t my_seq_{0};
    uint8_t peer_seq_{0};
    IIap2PacketHandler* handler_{nullptr};
    SendFunction send_fn_;
    mutable std::mutex mutex_;
};

} // namespace CarPlayStation::Core::Protocol
