#pragma once

#include "Core/Common/Types.h"
#include "Core/Protocol/Iap2MessageCatalog.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace CarPlayStation::Core::Protocol {

/**
 * @brief The wireless iAP2 control plane that runs after IdentifyAccepted.
 *
 * The class never sends anything itself; it returns the messages the caller must
 * put on the control session, which keeps the identify / Wi-Fi / CarPlay-session
 * exchange testable without a Bluetooth link.
 *
 * Mirrors the reference control loop (Iap2WirelessControlClient): subscriptions
 * are sent once, 0x5702 and 0x4E0E trigger a 0x5703 resend bounded by the
 * pre/post-transport send caps, and 0x4300 is answered with 0x4301.
 */
class Iap2WirelessControlPlane {
public:
    // A message the caller has to emit on the iAP2 control session.
    struct OutgoingMessage {
        uint16_t msgId = 0;
        Common::ByteBuffer body;
        const char* description = "";
    };

    // The hotspot we host, advertised in 0x5703 and repeated in 0x4301.
    struct WiFiConfig {
        std::string ssid;
        std::string passphrase;
        uint8_t securityType = 2;  // WPA/WPA2 personal
        uint8_t channel = 0;       // 0 = unknown, still a legal value
        bool hasBssid = false;
        std::array<uint8_t, 6> bssid{};
    };

    // The AirPlay receiver identity carried by 0x4301.
    struct SessionParams {
        uint32_t airPlayPort = 5000;
        std::string ipAddress;         // our IPv4 address on the hotspot network
        std::string deviceIdentifier;  // our device id (Bluetooth MAC string)
        std::string publicKey;         // accessory Ed25519 LTPK, lowercase hex
        std::string sourceVersion = "320.17";
    };

    // The reference control loop refuses to spam the iPhone with hotspot
    // credentials: five sends before the transport notification, two after.
    static constexpr int kMaxPreTransportWiFiSends = 5;
    static constexpr int kMaxPostTransportWiFiSends = 2;

    void SetWiFiConfig(WiFiConfig cfg);
    void SetSessionParams(SessionParams params);

    // The WinRT hotspot API reports neither channel nor BSSID up front; both may
    // become known later, so they can be patched in without rebuilding the config.
    void UpdateChannel(uint8_t channel);
    void UpdateBssid(Common::ByteSpan bssid);

    // Subscriptions to send immediately after IdentifyAccepted (idempotent).
    [[nodiscard]] std::vector<OutgoingMessage> OnIdentifyAccepted();

    /**
     * @brief Feed one control-session message body (payload header already stripped).
     * @return the messages the caller must send in response.
     */
    [[nodiscard]] std::vector<OutgoingMessage> OnControlMessage(uint16_t msgId, Common::ByteSpan body);

    [[nodiscard]] bool SubscriptionsSent() const noexcept { return subscriptions_sent_; }
    [[nodiscard]] bool TransportNotified() const noexcept { return transport_notified_; }
    [[nodiscard]] bool WirelessCarPlayAvailable() const noexcept { return wireless_carplay_available_; }
    [[nodiscard]] int WiFiConfigSends() const noexcept { return wifi_sends_; }
    [[nodiscard]] int CarPlayStartSessionSends() const noexcept { return start_session_sends_; }

private:
    [[nodiscard]] OutgoingMessage BuildWiFiConfig() const;
    [[nodiscard]] OutgoingMessage BuildStartSession() const;
    [[nodiscard]] std::vector<OutgoingMessage> RequestWiFiConfig(const char* reason);

    WiFiConfig wifi_;
    SessionParams session_;
    bool subscriptions_sent_ = false;
    bool transport_notified_ = false;
    bool wireless_carplay_available_ = false;
    int wifi_sends_ = 0;
    int pre_transport_wifi_sends_ = 0;
    int post_transport_wifi_sends_ = 0;
    int start_session_sends_ = 0;
};

} // namespace CarPlayStation::Core::Protocol
