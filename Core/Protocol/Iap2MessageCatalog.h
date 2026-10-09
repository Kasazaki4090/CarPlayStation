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
    inline constexpr uint16_t WiredCarPlayAvailability = 0x4302;

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
    inline constexpr uint16_t StopCallState          = 0x4156;
    inline constexpr uint16_t StartCommunications    = 0x4157;
    inline constexpr uint16_t StopCommunications     = 0x4159;
    inline constexpr uint16_t AcceptCall             = 0x415B;
    inline constexpr uint16_t EndCall                = 0x415C;

    // Power (0xAE0x)
    inline constexpr uint16_t StartPowerUpdates      = 0xAE00;
    inline constexpr uint16_t PowerUpdate            = 0xAE01;
    inline constexpr uint16_t StopPowerUpdates       = 0xAE02;
    inline constexpr uint16_t PowerSourceUpdate      = 0xAE03;

    // Device info (0x4E0x)
    inline constexpr uint16_t WirelessCarPlayAvailability = 0x4E0D;
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
    inline constexpr uint16_t ExternalAccessoryProtocol     = 10;
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
    // The record always declares the CarPlay session messages (0x4301 out,
    // 0x4300/0x4E0D in); without them the iPhone never starts the CarPlay
    // session handshake and the wireless control plane stalls at 0x5703.
    [[nodiscard]] static Common::ByteBuffer BuildIdentifyInfo(
        std::string_view name,
        const Common::MacAddress& btMac);

    // Build the AccessoryWiFiConfigurationInformation (0x5703) body.
    // `bssid` may be empty; `channel` 0 means "unknown" and is still a legal value.
    [[nodiscard]] static Common::ByteBuffer BuildAccessoryWiFiConfig(
        std::string_view ssid,
        std::string_view passphrase,
        uint8_t securityType = 2,
        uint8_t channel = 0,
        Common::ByteSpan bssid = {});

    // Parse rejected top-level param IDs from a 0x1D03 body (after stripping the 6-byte header).
    [[nodiscard]] static std::vector<uint16_t> ParseRejectedParamIds(Common::ByteSpan body);

    // --- Subscriptions the accessory sends right after IdentifyAccepted ---------
    // Each returns the message body for the matching Start*Updates message.
    [[nodiscard]] static Common::ByteBuffer BuildStartNowPlaying();
    [[nodiscard]] static Common::ByteBuffer BuildStartRouteGuidance();
    [[nodiscard]] static Common::ByteBuffer BuildStartPowerUpdates();
    [[nodiscard]] static Common::ByteBuffer BuildStartCommunicationsUpdates();
    [[nodiscard]] static Common::ByteBuffer BuildStartCallState();

    // Build AcceptCall (0x415B) body. AcceptAction: 0=Accept/HoldAndAccept, 1=EndAndAccept.
    [[nodiscard]] static Common::ByteBuffer BuildAcceptCall(uint8_t acceptAction = 0, std::string_view callUuid = {});

    // Build EndCall (0x415C) body. EndAction: 0=End/Decline, 1=EndAll.
    [[nodiscard]] static Common::ByteBuffer BuildEndCall(uint8_t endAction = 0, std::string_view callUuid = {});

    // NowPlaying delta update parsed from 0x5001.
    struct NowPlayingDelta {
        std::optional<std::string> title;
        std::optional<std::string> artist;
        std::optional<std::string> album;
        std::optional<std::string> genre;
        std::optional<std::string> composer;
        std::optional<uint32_t> duration_ms;
        std::optional<uint16_t> track_number;
        std::optional<uint16_t> track_count;
        std::optional<uint32_t> elapsed_ms;
        std::optional<uint8_t> playback_status; // 0=Stopped, 1=Playing, 2=Paused, 3=SeekForward, 4=SeekBackward
        std::optional<std::string> app_name;
        std::optional<std::string> app_bundle_id;
    };
    [[nodiscard]] static NowPlayingDelta ParseNowPlayingUpdate(Common::ByteSpan body);

    // Call state update parsed from 0x4155.
    struct CallStateInfo {
        std::string remote_id;       // pid 0, phone number
        std::string display_name;    // pid 1, contact name
        uint8_t status = 0;          // pid 2: 0=Disconnected, 1=Active/Sending, 2=Incoming/Ringing...
        uint8_t direction = 0;       // pid 3: 1=Incoming, 2=Outgoing
        std::string call_uuid;       // pid 4
        std::string address_book_id; // pid 6
        std::string label;           // pid 7
        uint8_t service = 0;         // pid 8
        bool is_conferenced = false; // pid 9
        uint8_t conference_group = 0;// pid 10
        uint8_t disconnect_reason = 0;// pid 11
        uint64_t start_timestamp = 0;// pid 12
    };
    [[nodiscard]] static std::optional<CallStateInfo> ParseCallStateUpdate(Common::ByteSpan body);

    // --- CarPlay session handshake (0x4300 -> 0x4301) -------------------------
    struct CarPlayAvailabilityInfo {
        bool hasWired = false;
        bool wiredAvailable = false;
        std::string wiredIdentifier;
        bool hasWireless = false;
        bool wirelessAvailable = false;
        std::string wirelessIdentifier;
    };

    struct DeviceTransportIdentifier {
        std::string bluetoothMac;
        std::string usbTransportIdentifier;
    };

    struct CarPlaySessionConfig {
        uint32_t airPlayPort = 5000;
        // Wireless transport parameters (the hotspot we host).
        std::string ssid;
        std::string passphrase;
        std::string ipAddress;  // our IPv4 on the hotspot network
        uint8_t channel = 0;
        uint8_t securityType = 2;
        // Identity the iPhone pairs with over AirPlay.
        std::string deviceIdentifier;
        std::string publicKey;      // accessory Ed25519 LTPK, lowercase hex
        std::string sourceVersion = "320.17";
    };

    // Build the CarPlayStartSession (0x4301) body.
    [[nodiscard]] static Common::ByteBuffer BuildCarPlayStartSession(const CarPlaySessionConfig& cfg);

    // Parse a 0x4300 body. Returns nullopt when the body is malformed.
    [[nodiscard]] static std::optional<CarPlayAvailabilityInfo> ParseCarPlayAvailability(Common::ByteSpan body);

    // Parse the availability byte of a 0x4E0D body.
    [[nodiscard]] static std::optional<bool> ParseWirelessCarPlayAvailability(Common::ByteSpan body);

    // Parse a 0x4E0E body.
    [[nodiscard]] static std::optional<DeviceTransportIdentifier> ParseDeviceTransportIdentifier(Common::ByteSpan body);

    // Extract the challenge bytes from a 0xAA02 payload.
    // Payload layout: [40 40][total BE16][msg_id BE16][group_len BE16][pid BE16][challenge...]
    [[nodiscard]] static Common::ByteBuffer ExtractChallenge(Common::ByteSpan payload);

    // Parse the message ID from a control-session payload ([40 40] prefix, >= 6 bytes).
    [[nodiscard]] static std::optional<uint16_t> ParseMessageId(Common::ByteSpan payload);
};

} // namespace CarPlayStation::Core::Protocol
