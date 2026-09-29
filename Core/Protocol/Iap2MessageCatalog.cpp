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
    Common::ByteBuffer out;

    // Helper: build a TLV group and append to out.
    auto pushGroup = [&](uint16_t pid, const std::function<void(TlvBuilder&)>& buildFn) {
        TlvBuilder inner;
        buildFn(inner);
        TlvBuilder outer;
        outer.Bytes(pid, inner.Span());
        out.insert(out.end(), outer.Data().begin(), outer.Data().end());
    };

    // Param 0: Name (utf8)
    pushGroup(ip::Name, [&](TlvBuilder& t) { t.Str(ip::Name, name); });

    // Param 1: ModelIdentifier
    pushGroup(ip::ModelIdentifier, [&](TlvBuilder& t) { t.Str(ip::ModelIdentifier, "CarPlayStation-1.0"); });

    // Param 2: Manufacturer
    pushGroup(ip::Manufacturer, [&](TlvBuilder& t) { t.Str(ip::Manufacturer, "CarPlayStation"); });

    // Param 3: SerialNumber
    pushGroup(ip::SerialNumber, [&](TlvBuilder& t) { t.Str(ip::SerialNumber, "CPS-0001"); });

    // Param 4: FirmwareVersion
    pushGroup(ip::FirmwareVersion, [&](TlvBuilder& t) { t.Str(ip::FirmwareVersion, "1.0.0"); });

    // Param 5: HardwareVersion
    pushGroup(ip::HardwareVersion, [&](TlvBuilder& t) { t.Str(ip::HardwareVersion, "1.0.0"); });

    // Param 6: MessagesSentByAccessory (list of uint16 message IDs)
    {
        std::vector<uint16_t> sentIds = {
            Iap2MsgId::StartNowPlaying,      // 0x5000
            Iap2MsgId::StopNowPlaying,       // 0x5002
            Iap2MsgId::StartRouteGuidance,   // 0x5200
            Iap2MsgId::StopRouteGuidance,    // 0x5203
        };
        Common::ByteBuffer idBytes;
        for (auto id : sentIds) {
            idBytes.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
            idBytes.push_back(static_cast<uint8_t>(id & 0xFF));
        }
        pushGroup(ip::MessagesSentByAccessory, [&](TlvBuilder& t) { t.Bytes(ip::MessagesSentByAccessory, idBytes); });
    }

    // Param 7: MessagesReceivedFromDevice
    {
        std::vector<uint16_t> rcvIds = {
            Iap2MsgId::NowPlayingUpdate,     // 0x5001
            Iap2MsgId::RouteGuidanceUpdate,  // 0x5201
            Iap2MsgId::RouteManeuverInfo,    // 0x5202
        };
        Common::ByteBuffer idBytes;
        for (auto id : rcvIds) {
            idBytes.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
            idBytes.push_back(static_cast<uint8_t>(id & 0xFF));
        }
        pushGroup(ip::MessagesReceivedFromDevice, [&](TlvBuilder& t) { t.Bytes(ip::MessagesReceivedFromDevice, idBytes); });
    }

    // Param 8: PowerProvidingCapability = 2 (Advanced)
    pushGroup(ip::PowerProvidingCapability, [&](TlvBuilder& t) { t.U8(ip::PowerProvidingCapability, 2); });

    // Param 9: MaximumCurrentDrawnFromDevice = 0
    pushGroup(ip::MaxCurrentDrawn, [&](TlvBuilder& t) { t.U16(ip::MaxCurrentDrawn, 0); });

    // Param 12: CurrentLanguage
    pushGroup(ip::CurrentLanguage, [&](TlvBuilder& t) { t.Str(ip::CurrentLanguage, "en"); });

    // Param 13: SupportedLanguage
    pushGroup(ip::SupportedLanguage, [&](TlvBuilder& t) { t.Str(ip::SupportedLanguage, "en"); });

    // Param 17: BluetoothTransportComponent (required companion for param 24 in wireless mode)
    {
        TlvBuilder inner;
        inner.U16(0, 1);  // TransportComponentIdentifier = 1
        inner.Str(1, "IAP2-Bluetooth");
        inner.None(2);    // TransportSupportsiAP2Connection (presence flag)
        Common::ByteBuffer macBytes(btMac.bytes.begin(), btMac.bytes.end());
        inner.Bytes(3, macBytes); // BluetoothTransportMACAddress

        TlvBuilder outer;
        outer.Bytes(ip::BluetoothTransport, inner.Span());
        out.insert(out.end(), outer.Data().begin(), outer.Data().end());
    }

    // Param 20: VehicleInformationComponent
    {
        TlvBuilder inner;
        inner.U16(0, 2);  // Identifier
        inner.Str(1, name);
        inner.U8(2, 0);   // EngineType = Gasoline
        inner.Str(6, name); // DisplayName

        TlvBuilder outer;
        outer.Bytes(ip::VehicleInformation, inner.Span());
        out.insert(out.end(), outer.Data().begin(), outer.Data().end());
    }

    // Param 24: WirelessCarPlayTransportComponent
    {
        TlvBuilder inner;
        inner.U16(0, 2);  // TransportComponentIdentifier = 2 (wireless)
        inner.Str(1, "IAP2-Wireless");
        inner.None(2);    // TransportSupportsiAP2Connection
        inner.None(4);    // TransportSupportsCarPlay

        TlvBuilder outer;
        outer.Bytes(ip::WirelessCarPlayTransport, inner.Span());
        out.insert(out.end(), outer.Data().begin(), outer.Data().end());
    }

    // Param 30: RouteGuidanceDisplayComponent
    {
        TlvBuilder inner;
        inner.U16(0, 42); // Identifier
        inner.Str(1, "RouteGuidance");

        TlvBuilder outer;
        outer.Bytes(ip::RouteGuidanceDisplay, inner.Span());
        out.insert(out.end(), outer.Data().begin(), outer.Data().end());
    }

    (void)declareWired; // Reserved for future wired-mode declaration

    return out;
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
