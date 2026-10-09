#include "Core/Protocol/Iap2ControlPlane.h"

#include "Core/Common/Logging.h"

#include <cstdio>

namespace CarPlayStation::Core::Protocol {

namespace {

constexpr const char* kLogTag = "IAP2-Wireless";

} // namespace

void Iap2WirelessControlPlane::SetWiFiConfig(WiFiConfig cfg) {
    wifi_ = std::move(cfg);
    const Common::ByteBuffer bssid(wifi_.bssid.begin(), wifi_.bssid.end());
    CPS_LOG_INFO(kLogTag, "Wi-Fi config set: ssid='", wifi_.ssid, "' security=",
                 static_cast<int>(wifi_.securityType), " channel=", static_cast<int>(wifi_.channel),
                 " bssid=", wifi_.hasBssid ? Common::ToHexCompact(bssid) : "unknown");
}

void Iap2WirelessControlPlane::SetSessionParams(SessionParams params) {
    session_ = std::move(params);
    CPS_LOG_INFO(kLogTag, "Session params set: port=", session_.airPlayPort,
                 " ip=", session_.ipAddress.empty() ? "unknown" : session_.ipAddress,
                 " id=", session_.deviceIdentifier, " publicKeyLen=", session_.publicKey.size());
}

void Iap2WirelessControlPlane::UpdateChannel(uint8_t channel) {
    if (wifi_.channel == channel) {
        return;
    }
    wifi_.channel = channel;
    CPS_LOG_INFO(kLogTag, "Wi-Fi channel updated to ", static_cast<int>(channel));
}

void Iap2WirelessControlPlane::UpdateBssid(Common::ByteSpan bssid) {
    if (bssid.size() != 6) {
        return;
    }
    for (size_t i = 0; i < 6; ++i) {
        wifi_.bssid[i] = bssid[i];
    }
    wifi_.hasBssid = true;
    CPS_LOG_INFO(kLogTag, "Wi-Fi BSSID updated to ", Common::ToHexCompact(bssid));
}

std::vector<Iap2WirelessControlPlane::OutgoingMessage>
Iap2WirelessControlPlane::OnIdentifyAccepted() {
    std::vector<OutgoingMessage> out;
    if (subscriptions_sent_) {
        return out;
    }
    subscriptions_sent_ = true;

    // These five subscriptions correspond to the message ids declared in our
    // identify record. Without them the iPhone has no reason to start the
    // CarPlay session, so they go out before the first 0x5703.
    struct Subscription {
        uint16_t msgId;
        const char* name;
        Common::ByteBuffer body;
    };
    const Subscription subscriptions[] = {
        {Iap2MsgId::StartNowPlaying, "NowPlaying", Iap2MessageCatalog::BuildStartNowPlaying()},
        {Iap2MsgId::StartRouteGuidance, "RouteGuidance", Iap2MessageCatalog::BuildStartRouteGuidance()},
        {Iap2MsgId::StartPowerUpdates, "PowerUpdate", Iap2MessageCatalog::BuildStartPowerUpdates()},
        {Iap2MsgId::StartCommunications, "Telephony", Iap2MessageCatalog::BuildStartCommunicationsUpdates()},
        {Iap2MsgId::StartCallState, "CallState", Iap2MessageCatalog::BuildStartCallState()},
    };
    for (const auto& sub : subscriptions) {
        out.push_back({sub.msgId, sub.body, sub.name});
    }
    CPS_LOG_INFO(kLogTag,
                 "Identify accepted: queueing NowPlaying, RouteGuidance, PowerUpdate, Telephony and CallState subscriptions");
    return out;
}

std::vector<Iap2WirelessControlPlane::OutgoingMessage>
Iap2WirelessControlPlane::OnControlMessage(uint16_t msgId, Common::ByteSpan body) {
    std::vector<OutgoingMessage> out;
    char id[8];
    switch (msgId) {
        case Iap2MsgId::RequestWiFiInfo: {
            // 0x5700: the iPhone wants our hotspot before it will join it.
            CPS_LOG_INFO(kLogTag, "0x5700 RequestWiFiInformation received");
            auto req = RequestWiFiConfig("0x5700 request");
            out.insert(out.end(), req.begin(), req.end());
            break;
        }
        case Iap2MsgId::WiFiInfo: {
            // 0x5701: the iPhone describes the network it wants us to join. We
            // host the hotspot, so there is nothing to act on; log it for parity.
            CPS_LOG_INFO(kLogTag, "0x5701 WiFiInformation received (ignored, we host the AP)");
            break;
        }
        case Iap2MsgId::RequestAccessoryWiFi: {
            // 0x5702: the iPhone is still waiting for our hotspot credentials.
            CPS_LOG_INFO(kLogTag, "0x5702 RequestAccessoryWiFiConfiguration received");
            auto req = RequestWiFiConfig("0x5702 request");
            out.insert(out.end(), req.begin(), req.end());
            break;
        }
        case Iap2MsgId::DeviceLanguageUpdate:
        case Iap2MsgId::DeviceTimeUpdate: {
            // 0x4E0A / 0x4E0B: device language and time. Not needed to bring up
            // CarPlay, but they show the iPhone is still driving the control
            // plane, so log them instead of dropping them silently.
            std::snprintf(id, sizeof(id), "0x%04X", msgId);
            CPS_LOG_DEBUG(kLogTag, id, " device information received (", body.size(), " bytes)");
            break;
        }
        case Iap2MsgId::WirelessCarPlayAvailability: {
            const auto available = Iap2MessageCatalog::ParseWirelessCarPlayAvailability(body);
            if (!available.has_value()) {
                CPS_LOG_WARN(kLogTag, "0x4E0D WirelessCarPlayAvailability could not be parsed");
                break;
            }
            wireless_carplay_available_ = *available;
            CPS_LOG_INFO(kLogTag, "0x4E0D WirelessCarPlayAvailability = ",
                         *available ? "available" : "unavailable");
            break;
        }
        case Iap2MsgId::DeviceTransportId: {
            const auto ids = Iap2MessageCatalog::ParseDeviceTransportIdentifier(body);
            transport_notified_ = true;
            CPS_LOG_INFO(kLogTag, "0x4E0E DeviceTransportIdentifier: BT=",
                         (ids && !ids->bluetoothMac.empty()) ? ids->bluetoothMac : "unknown",
                         " USB=", (ids && !ids->usbTransportIdentifier.empty()) ? ids->usbTransportIdentifier
                                                                               : "unknown");
            // The reference treats this as the iPhone being ready for the hotspot
            // credentials, then repeats them under the post-transport cap.
            auto req = RequestWiFiConfig("post-transport retry");
            out.insert(out.end(), req.begin(), req.end());
            break;
        }
        case Iap2MsgId::CarPlayAvailability: {
            const auto info = Iap2MessageCatalog::ParseCarPlayAvailability(body);
            if (!info.has_value()) {
                CPS_LOG_WARN(kLogTag, "0x4300 CarPlayAvailability could not be parsed");
                break;
            }
            const std::string identifier =
                info->wirelessIdentifier.empty() ? info->wiredIdentifier : info->wirelessIdentifier;
            CPS_LOG_INFO(kLogTag, "0x4300 CarPlayAvailability: wired=", info->wiredAvailable ? "yes" : "no",
                         " wireless=", info->wirelessAvailable ? "yes" : "no",
                         " identifier=", identifier.empty() ? "none" : identifier);
            if (!session_.publicKey.empty() && session_.airPlayPort > 0) {
                out.push_back(BuildStartSession());
            } else {
                CPS_LOG_ERROR(kLogTag,
                              "0x4301 CarPlayStartSession cannot be built: publicKey or AirPlay port missing");
            }
            break;
        }
        case Iap2MsgId::CarPlayStartSession:
        case Iap2MsgId::WiredCarPlayAvailability: {
            // Our own 0x4301 echoed back, or a wired-availability notification.
            std::snprintf(id, sizeof(id), "0x%04X", msgId);
            CPS_LOG_DEBUG(kLogTag, id, " CarPlay session message received");
            break;
        }
        case Iap2MsgId::NowPlayingUpdate:
        case Iap2MsgId::CallStateUpdate: {
            // Handled by CoreService packet handler
            break;
        }
        default:
            break;
    }
    return out;
}

Iap2WirelessControlPlane::OutgoingMessage Iap2WirelessControlPlane::BuildWiFiConfig() const {
    Common::ByteSpan bssid;
    if (wifi_.hasBssid) {
        bssid = Common::ByteSpan(wifi_.bssid.data(), wifi_.bssid.size());
    }
    return OutgoingMessage{Iap2MsgId::AccessoryWiFiConfig,
                           Iap2MessageCatalog::BuildAccessoryWiFiConfig(wifi_.ssid, wifi_.passphrase,
                                                                        wifi_.securityType, wifi_.channel, bssid),
                           "AccessoryWiFiConfiguration"};
}

Iap2WirelessControlPlane::OutgoingMessage Iap2WirelessControlPlane::BuildStartSession() const {
    Iap2MessageCatalog::CarPlaySessionConfig cfg;
    cfg.ssid = wifi_.ssid;
    cfg.passphrase = wifi_.passphrase;
    cfg.channel = wifi_.channel;
    cfg.securityType = wifi_.securityType;
    cfg.ipAddress = session_.ipAddress;
    cfg.airPlayPort = session_.airPlayPort;
    cfg.deviceIdentifier = session_.deviceIdentifier;
    cfg.publicKey = session_.publicKey;
    cfg.sourceVersion = session_.sourceVersion;

    ++const_cast<Iap2WirelessControlPlane*>(this)->start_session_sends_;
    CPS_LOG_INFO(kLogTag, "Sending 0x4301 CarPlayStartSession: port=", cfg.airPlayPort,
                 " ip=", cfg.ipAddress.empty() ? "unknown" : cfg.ipAddress,
                 " id=", cfg.deviceIdentifier, " ssid='", cfg.ssid, "'");
    return OutgoingMessage{Iap2MsgId::CarPlayStartSession,
                           Iap2MessageCatalog::BuildCarPlayStartSession(cfg),
                           "CarPlayStartSession"};
}

std::vector<Iap2WirelessControlPlane::OutgoingMessage>
Iap2WirelessControlPlane::RequestWiFiConfig(const char* reason) {
    int& budget = transport_notified_ ? post_transport_wifi_sends_ : pre_transport_wifi_sends_;
    const int limit = transport_notified_ ? kMaxPostTransportWiFiSends : kMaxPreTransportWiFiSends;
    if (budget >= limit) {
        CPS_LOG_WARN(kLogTag, "0x5703 not resent (", reason, "): ",
                     transport_notified_ ? "post" : "pre", "-transport send limit of ", limit, " reached");
        return {};
    }
    ++budget;
    ++wifi_sends_;
    CPS_LOG_INFO(kLogTag, "Sending 0x5703 AccessoryWiFiConfiguration (", reason, ", attempt ", budget, "/", limit,
                 ", ssid='", wifi_.ssid, "' channel=", static_cast<int>(wifi_.channel), ")");
    return {BuildWiFiConfig()};
}

} // namespace CarPlayStation::Core::Protocol
