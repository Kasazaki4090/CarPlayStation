#include "Core/Protocol/Iap2MessageCatalog.h"
#include "Core/Protocol/Iap2LinkEngine.h"
#include <cstring>
#include <functional>

namespace CarPlayStation::Core::Protocol {

// ============================================================================
// TlvBuilder implementation
// ============================================================================

void TlvBuilder::Str(uint16_t pid, std::string_view s) {
    const uint16_t len = static_cast<uint16_t>(4 + s.size() + 1); // header + NUL
    WriteBe16(len);
    WriteBe16(pid);
    out_.insert(out_.end(), s.begin(), s.end());
    out_.push_back(0);
}

void TlvBuilder::Bytes(uint16_t pid, Common::ByteSpan data) {
    const uint16_t len = static_cast<uint16_t>(4 + data.size());
    WriteBe16(len);
    WriteBe16(pid);
    out_.insert(out_.end(), data.begin(), data.end());
}

void TlvBuilder::U8(uint16_t pid, uint8_t v) {
    WriteBe16(5);
    WriteBe16(pid);
    out_.push_back(v);
}

void TlvBuilder::U16(uint16_t pid, uint16_t v) {
    WriteBe16(6);
    WriteBe16(pid);
    WriteBe16(v);
}

void TlvBuilder::U32(uint16_t pid, uint32_t v) {
    WriteBe16(8);
    WriteBe16(pid);
    out_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out_.push_back(static_cast<uint8_t>(v & 0xFF));
}

void TlvBuilder::Bool(uint16_t pid, bool v) {
    WriteBe16(5);
    WriteBe16(pid);
    out_.push_back(v ? 1 : 0);
}

void TlvBuilder::None(uint16_t pid) {
    WriteBe16(4);
    WriteBe16(pid);
}

// ============================================================================
// Iap2MessageCatalog implementation
// ============================================================================

Common::ByteBuffer Iap2MessageCatalog::BuildControlPayloadHeader(uint16_t messageId, size_t bodySize) {
    const uint16_t total = static_cast<uint16_t>(6 + bodySize);
    Common::ByteBuffer hdr;
    hdr.reserve(6);
    hdr.push_back(Iap2Constants::MSG_PREFIX_0); // 0x40
    hdr.push_back(Iap2Constants::MSG_PREFIX_1); // 0x40
    hdr.push_back(static_cast<uint8_t>((total >> 8) & 0xFF));
    hdr.push_back(static_cast<uint8_t>(total & 0xFF));
    hdr.push_back(static_cast<uint8_t>((messageId >> 8) & 0xFF));
    hdr.push_back(static_cast<uint8_t>(messageId & 0xFF));
    return hdr;
}

Common::ByteBuffer Iap2MessageCatalog::GroupOne(uint16_t pid, Common::ByteSpan value) {
    TlvBuilder tlv;
    tlv.Bytes(pid, value);
    return tlv.Data();
}

Common::ByteBuffer Iap2MessageCatalog::BuildIdentifyInfo(
    std::string_view name,
    const Common::MacAddress& btMac,
    bool declareWired)
{
    namespace ip = Iap2IdentParam;
    TlvBuilder b;

    // Param 0: Name (utf8)
    b.Str(ip::Name, name);

    // Param 1: ModelIdentifier
    b.Str(ip::ModelIdentifier, "Magic-Car-Link-1.00");

    // Param 2: Manufacturer
    b.Str(ip::Manufacturer, "Magic Tec.");

    // Param 3: SerialNumber
    b.Str(ip::SerialNumber, "2025.02.25.1521626a");

    // Param 4: FirmwareVersion
    b.Str(ip::FirmwareVersion, "1.0.0");

    // Param 5: HardwareVersion
    b.Str(ip::HardwareVersion, "1.0.0");

    // Param 6: MessagesSentByAccessory (list of uint16 message IDs)
    {
        std::vector<uint16_t> sentIds = {
            0x4154,  // StartCallStateUpdates
            0x5000,  // StartNowPlayingUpdates
            0x5002,  // StopNowPlayingUpdates
            0x5200,  // StartRouteGuidanceUpdates
            0x5203,  // StopRouteGuidanceUpdates
            0x415A,  // InitiateCall
            0x415B,  // AcceptCall
            0x415C,  // EndCall
            0x415D,  // SwapCalls
            0x415E,  // MergeCalls
            0x415F,  // HoldStatusUpdate
            0x4160,  // MuteStatusUpdate
            0x4161,  // SendDTMF
            0x5703   // AccessoryWiFiConfigurationInformation
        };
        if (declareWired) {
            sentIds.push_back(0x4301); // CarPlayStartSession
        }
        Common::ByteBuffer idBytes;
        for (auto id : sentIds) {
            idBytes.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
            idBytes.push_back(static_cast<uint8_t>(id & 0xFF));
        }
        b.Bytes(ip::MessagesSentByAccessory, idBytes);
    }

    // Param 7: MessagesReceivedFromDevice
    {
        std::vector<uint16_t> rcvIds = {
            0x4E0A,  // DeviceLanguageUpdate
            0x4E0B,  // DeviceTimeUpdate
            0x4E0E,  // DeviceTransportIdentifierNotification
            0x4155,  // CallStateUpdate
            0x5001,  // NowPlayingUpdate
            0x5201,  // RouteGuidanceUpdate
            0x5202,  // RouteGuidanceManeuverInformation
            0x5702   // RequestAccessoryWiFiConfigurationInformation
        };
        if (declareWired) {
            rcvIds.push_back(0x4300); // CarPlayAvailability
        }
        Common::ByteBuffer idBytes;
        for (auto id : rcvIds) {
            idBytes.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
            idBytes.push_back(static_cast<uint8_t>(id & 0xFF));
        }
        b.Bytes(ip::MessagesReceivedFromDevice, idBytes);
    }

    // Param 8: PowerProvidingCapability = 2 (Advanced)
    b.U8(ip::PowerProvidingCapability, 2);

    // Param 9: MaximumCurrentDrawnFromDevice = 0
    b.U16(ip::MaxCurrentDrawn, 0);

    // Param 12: CurrentLanguage
    b.Str(ip::CurrentLanguage, "en");

    // Param 13: SupportedLanguage
    b.Str(ip::SupportedLanguage, "en");

    // Param 17: BluetoothTransportComponent (required companion for param 24 in wireless mode)
    {
        TlvBuilder inner;
        inner.U16(0, 1);  // TransportComponentIdentifier = 1
        inner.Str(1, "IAP2-Bluetooth");
        inner.None(2);    // TransportSupportsiAP2Connection (presence flag)
        Common::ByteBuffer macBytes(btMac.bytes.begin(), btMac.bytes.end());
        inner.Bytes(3, macBytes); // BluetoothTransportMACAddress

        b.Bytes(ip::BluetoothTransport, inner.Span());
    }

    // Param 20: VehicleInformationComponent
    {
        TlvBuilder inner;
        inner.U16(0, 2);  // Identifier
        inner.Str(1, name);
        inner.U8(2, 0);   // EngineType = Gasoline (0)
        inner.Str(6, name); // DisplayName

        b.Bytes(ip::VehicleInformation, inner.Span());
    }

    // Param 24: WirelessCarPlayTransportComponent
    {
        TlvBuilder inner;
        inner.U16(0, 2);  // TransportComponentIdentifier = 2 (wireless)
        inner.Str(1, "IAP2-Wireless");
        inner.None(2);    // TransportSupportsiAP2Connection
        inner.None(4);    // TransportSupportsCarPlay

        b.Bytes(ip::WirelessCarPlayTransport, inner.Span());
    }

    // Param 30: RouteGuidanceDisplayComponent
    {
        TlvBuilder inner;
        inner.U16(0, 42); // Identifier
        inner.Str(1, "RouteGuidance");

        b.Bytes(ip::RouteGuidanceDisplay, inner.Span());
    }

    (void)declareWired;
    return b.Data();
}

Common::ByteBuffer Iap2MessageCatalog::BuildAccessoryWiFiConfig(
    std::string_view ssid,
    std::string_view passphrase,
    uint8_t securityType,
    uint8_t channel)
{
    TlvBuilder b;
    // Param 1: WiFiSSID (utf8, required)
    b.Str(1, ssid);
    // Param 2: Passphrase (utf8, required if securityType != 0)
    if (!passphrase.empty()) {
        b.Str(2, passphrase);
    }
    // Param 3: SecurityType (uint8 enum: 0=None, 1=WEP, 2=WPA2/WPA3 Personal)
    b.U8(3, securityType);
    // Param 4: Channel (uint8, e.g. 36 or 6)
    if (channel != 0) {
        b.U8(4, channel);
    }
    return b.Data();
}

std::vector<uint16_t> Iap2MessageCatalog::ParseRejectedParamIds(Common::ByteSpan body) {
    std::vector<uint16_t> ids;
    size_t i = 0;
    while (i + 4 <= body.size()) {
        const uint16_t groupLen = Common::Endian::ReadBe16(body.data() + i);
        const uint16_t pid = Common::Endian::ReadBe16(body.data() + i + 2);
        if (groupLen < 4 || static_cast<size_t>(groupLen) > body.size() - i) {
            break; // malformed or truncated — stop
        }
        ids.push_back(pid);
        i += groupLen;
    }
    return ids;
}

Common::ByteBuffer Iap2MessageCatalog::ExtractChallenge(Common::ByteSpan payload) {
    if (payload.size() < 6 + 4) return {};
    Common::ByteSpan body(payload.data() + 6, payload.size() - 6);
    const uint16_t groupLen = Common::Endian::ReadBe16(body.data());
    const size_t challengeLen = static_cast<size_t>(groupLen) >= 4 ? (static_cast<size_t>(groupLen) - 4) : 0;
    if (body.size() < 4 + challengeLen) return {};
    return Common::ByteBuffer(body.begin() + 4, body.begin() + 4 + challengeLen);
}

std::optional<uint16_t> Iap2MessageCatalog::ParseMessageId(Common::ByteSpan payload) {
    if (payload.size() < 6 || payload[0] != 0x40 || payload[1] != 0x40) {
        return std::nullopt;
    }
    return Common::Endian::ReadBe16(payload.data() + 4);
}

} // namespace CarPlayStation::Core::Protocol
