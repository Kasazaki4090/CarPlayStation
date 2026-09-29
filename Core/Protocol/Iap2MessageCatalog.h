#pragma once

#include "Core/Common/Types.h"
#include <string>
#include <vector>
#include <optional>
#include <span>

namespace CarPlayStation::Core::Protocol {

// ============================================================================
// iAP2 Message ID Constants (Apple's authoritative catalog)
// ============================================================================
namespace Iap2MsgId {
    // Authentication plane (0xAA0x)
    inline constexpr uint16_t RequestAuthCertificate = 0xAA00;
    inline constexpr uint16_t AuthCertificate        = 0xAA01;
    inline constexpr uint16_t RequestAuthChallenge   = 0xAA02;
    inline constexpr uint16_t AuthResponse           = 0xAA03;
    inline constexpr uint16_t AuthFailed             = 0xAA04;
    inline constexpr uint16_t AuthSucceeded          = 0xAA05;

    // Identification (0x1D0x)
    inline constexpr uint16_t StartIdentification    = 0x1D00;
    inline constexpr uint16_t IdentifyInformation    = 0x1D01;
    inline constexpr uint16_t IdentifyAccepted       = 0x1D02;
    inline constexpr uint16_t IdentifyRejected       = 0x1D03;

    // CarPlay session (0x430x)
    inline constexpr uint16_t CarPlayAvailability    = 0x4300;
    inline constexpr uint16_t CarPlayStartSession    = 0x4301;

    // WiFi handoff (0x570x)
    inline constexpr uint16_t RequestWiFiInfo        = 0x5700;
    inline constexpr uint16_t WiFiInfo               = 0x5701;
    inline constexpr uint16_t RequestAccessoryWiFi   = 0x5702;
    inline constexpr uint16_t AccessoryWiFiConfig    = 0x5703;

    // Now Playing (0x500x)
    inline constexpr uint16_t StartNowPlaying        = 0x5000;
    inline constexpr uint16_t NowPlayingUpdate       = 0x5001;
    inline constexpr uint16_t StopNowPlaying         = 0x5002;

    // Route Guidance (0x520x)
    inline constexpr uint16_t StartRouteGuidance     = 0x5200;
    inline constexpr uint16_t RouteGuidanceUpdate    = 0x5201;
    inline constexpr uint16_t RouteManeuverInfo      = 0x5202;
    inline constexpr uint16_t StopRouteGuidance      = 0x5203;

    // Call control (0x415x)
    inline constexpr uint16_t StartCallState         = 0x4154;
    inline constexpr uint16_t CallStateUpdate        = 0x4155;
    inline constexpr uint16_t AcceptCall             = 0x415B;
    inline constexpr uint16_t EndCall                = 0x415C;

    // Device info (0x4E0x)
    inline constexpr uint16_t DeviceLanguageUpdate   = 0x4E0A;
    inline constexpr uint16_t DeviceTimeUpdate       = 0x4E0B;
    inline constexpr uint16_t DeviceTransportId      = 0x4E0E;

    // Vehicle status (0xA10x)
    inline constexpr uint16_t StartVehicleStatus     = 0xA100;
    inline constexpr uint16_t VehicleStatusUpdate    = 0xA101;
}

// ============================================================================
// IdentificationInformation parameter IDs (0x1D01 sub-params)
// ============================================================================
namespace Iap2IdentParam {
    inline constexpr uint16_t Name                          = 0;
    inline constexpr uint16_t ModelIdentifier               = 1;
    inline constexpr uint16_t Manufacturer                  = 2;
    inline constexpr uint16_t SerialNumber                  = 3;
    inline constexpr uint16_t FirmwareVersion               = 4;
    inline constexpr uint16_t HardwareVersion               = 5;
    inline constexpr uint16_t MessagesSentByAccessory       = 6;
    inline constexpr uint16_t MessagesReceivedFromDevice    = 7;
    inline constexpr uint16_t PowerProvidingCapability      = 8;
    inline constexpr uint16_t MaxCurrentDrawn               = 9;
    inline constexpr uint16_t CurrentLanguage               = 12;
    inline constexpr uint16_t SupportedLanguage             = 13;
    inline constexpr uint16_t USBHostTransport              = 16;
    inline constexpr uint16_t BluetoothTransport            = 17;
    inline constexpr uint16_t VehicleInformation            = 20;
    inline constexpr uint16_t VehicleStatus                 = 21;
    inline constexpr uint16_t LocationInformation           = 22;
    inline constexpr uint16_t WirelessCarPlayTransport      = 24;
    inline constexpr uint16_t RouteGuidanceDisplay          = 30;
}

// ============================================================================
// TLV Group Builder — emits [group_len BE16][param_id BE16][value...]
// ============================================================================
class TlvBuilder {
public:
    // NUL-terminated string parameter.
    void Str(uint16_t pid, std::string_view s);

    // Raw bytes parameter.
    void Bytes(uint16_t pid, Common::ByteSpan data);

    // uint8 scalar.
    void U8(uint16_t pid, uint8_t v);

    // uint16 scalar (big-endian).
    void U16(uint16_t pid, uint16_t v);

    // uint32 scalar (big-endian).
    void U32(uint16_t pid, uint32_t v);

    // bool (single 0/1 byte).
    void Bool(uint16_t pid, bool v);

    // Presence-only (empty value) parameter.
    void None(uint16_t pid);

    [[nodiscard]] const Common::ByteBuffer& Data() const noexcept { return out_; }
    [[nodiscard]] Common::ByteSpan Span() const noexcept { return out_; }
    void Clear() { out_.clear(); }

private:
    void WriteBe16(uint16_t v) {
        out_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out_.push_back(static_cast<uint8_t>(v & 0xFF));
    }

    Common::ByteBuffer out_;
};

// ============================================================================
// Iap2MessageCatalog — static helpers for building/parsing iAP2 message bodies
// ============================================================================
class Iap2MessageCatalog {
public:
    // Build the [40 40][total BE16][msg_id BE16] control-session payload header.
    [[nodiscard]] static Common::ByteBuffer BuildControlPayloadHeader(uint16_t messageId, size_t bodySize);

    // Wrap a value in a single TLV group: [len BE16][pid BE16][value...]
    [[nodiscard]] static Common::ByteBuffer GroupOne(uint16_t pid, Common::ByteSpan value);

    // Build the IdentifyInformation (0x1D01) body for wireless CarPlay.
    [[nodiscard]] static Common::ByteBuffer BuildIdentifyInfo(
        std::string_view name,
        const Common::MacAddress& btMac,
        bool declareWired = false);

    // Parse rejected top-level param IDs from a 0x1D03 body (after stripping the 6-byte header).
    [[nodiscard]] static std::vector<uint16_t> ParseRejectedParamIds(Common::ByteSpan body);

    // Extract the challenge bytes from a 0xAA02 payload.
    // Payload layout: [40 40][total BE16][msg_id BE16][group_len BE16][pid BE16][challenge...]
    [[nodiscard]] static Common::ByteBuffer ExtractChallenge(Common::ByteSpan payload);

    // Parse the message ID from a control-session payload ([40 40] prefix, >= 6 bytes).
    [[nodiscard]] static std::optional<uint16_t> ParseMessageId(Common::ByteSpan payload);
};

} // namespace CarPlayStation::Core::Protocol
