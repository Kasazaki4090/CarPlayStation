#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <chrono>

namespace CarPlayStation::Core::Transport {

enum class WifiBand {
    Band2_4GHz,
    Band5GHz,
    BandDual
};

[[nodiscard]] constexpr std::string_view WifiBandToString(WifiBand band) noexcept {
    switch (band) {
        case WifiBand::Band2_4GHz: return "2.4GHz";
        case WifiBand::Band5GHz:   return "5GHz";
        case WifiBand::BandDual:   return "DualBand";
    }
    return "Unknown";
}

enum class WifiSecurityType {
    Open,
    Wpa2Personal,
    Wpa3Personal
};

[[nodiscard]] constexpr std::string_view WifiSecurityTypeToString(WifiSecurityType sec) noexcept {
    switch (sec) {
        case WifiSecurityType::Open:         return "Open";
        case WifiSecurityType::Wpa2Personal: return "WPA2-Personal";
        case WifiSecurityType::Wpa3Personal: return "WPA3-Personal";
    }
    return "Unknown";
}

enum class ApState {
    Disabled,
    Starting,
    Running,
    Stopping,
    Failed
};

[[nodiscard]] constexpr std::string_view ApStateToString(ApState state) noexcept {
    switch (state) {
        case ApState::Disabled: return "Disabled";
        case ApState::Starting: return "Starting";
        case ApState::Running:  return "Running";
        case ApState::Stopping: return "Stopping";
        case ApState::Failed:   return "Failed";
    }
    return "Unknown";
}

struct WifiApCredentials {
    std::string ssid{"CarPlayStation"};
    std::string passphrase{"CarPlay12345"};
    WifiSecurityType security{WifiSecurityType::Wpa2Personal};
    WifiBand band{WifiBand::Band5GHz};
    uint32_t channel{36};
    std::string gatewayIp{"192.168.137.1"};
    std::string bssid{"00:00:00:00:00:00"};
};

struct StationInfo {
    std::string macAddress;
    std::string ipAddress;
    std::chrono::system_clock::time_point connectedTime;
};

using StationConnectedCallback = std::function<void(const StationInfo& station)>;
using StationDisconnectedCallback = std::function<void(const std::string& macAddress)>;
using ApStateChangedCallback = std::function<void(ApState newState, Common::Status status)>;

/**
 * @brief Abstract interface for Wi-Fi Access Point (SoftAP / Mobile Hotspot)
 * lifecycle management and iPhone station association monitoring.
 */
class IWifiSoftApManager {
public:
    virtual ~IWifiSoftApManager() = default;

    // Start hosting the Wi-Fi AP with given credentials
    [[nodiscard]] virtual Common::Status StartAccessPoint(const WifiApCredentials& credentials) = 0;

    // Stop hosting the Wi-Fi AP
    virtual void StopAccessPoint() = 0;

    // Current AP operational state
    [[nodiscard]] virtual ApState GetState() const noexcept = 0;

    // Check if AP is currently active and broadcasting
    [[nodiscard]] virtual bool IsRunning() const noexcept = 0;

    // Active Wi-Fi AP network credentials
    [[nodiscard]] virtual WifiApCredentials GetCredentials() const = 0;

    // Alias matching IWifiHandoffController convention
    [[nodiscard]] virtual WifiApCredentials GetNetworkCredentials() const {
        return GetCredentials();
    }

    // List of currently associated client stations (iPhones)
    [[nodiscard]] virtual std::vector<StationInfo> GetConnectedStations() const = 0;

    // Number of connected stations
    [[nodiscard]] virtual size_t GetConnectedStationCount() const = 0;

    // Callback registrations
    virtual void SetStationConnectedCallback(StationConnectedCallback callback) = 0;
    virtual void SetStationDisconnectedCallback(StationDisconnectedCallback callback) = 0;
    virtual void SetStateCallback(ApStateChangedCallback callback) = 0;
};

} // namespace CarPlayStation::Core::Transport
