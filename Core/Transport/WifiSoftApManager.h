#pragma once

#include "Core/Transport/IWifiSoftApManager.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <map>

namespace CarPlayStation::Core::Transport {

/**
 * @brief Manages Windows Wi-Fi SoftAP / Mobile Hotspot lifecycle,
 * credential provisioning, and client station association watchdogs.
 */
class WifiSoftApManager : public IWifiSoftApManager {
public:
    WifiSoftApManager();
    ~WifiSoftApManager() override;

    // --- IWifiSoftApManager Implementation ---
    [[nodiscard]] Common::Status StartAccessPoint(const WifiApCredentials& credentials) override;
    void StopAccessPoint() override;
    [[nodiscard]] ApState GetState() const noexcept override;
    [[nodiscard]] bool IsRunning() const noexcept override;
    [[nodiscard]] WifiApCredentials GetCredentials() const override;
    [[nodiscard]] std::vector<StationInfo> GetConnectedStations() const override;
    [[nodiscard]] size_t GetConnectedStationCount() const override;
    void SetStationConnectedCallback(StationConnectedCallback callback) override;
    void SetStationDisconnectedCallback(StationDisconnectedCallback callback) override;
    void SetStateCallback(ApStateChangedCallback callback) override;

    // --- Dynamic Credential Helper ---
    static WifiApCredentials GenerateRandomCredentials(std::string_view prefix = "CarPlayStation-", uint32_t channel = 36);

    // --- Simulation & Test Mode ---
    void EnableSimulatedMode(bool enable);
    [[nodiscard]] bool IsSimulatedMode() const noexcept;
    void SimulateStationJoin(const std::string& mac, const std::string& ip = "192.168.137.100");
    void SimulateStationLeave(const std::string& mac);
    void SimulateStateChange(ApState state, Common::Status status = Common::Status::Ok());

private:
    void WatchdogLoop();
    void QueryNativeHostedNetworkStatus();

    mutable std::mutex m_mutex;
    std::atomic<ApState> m_state{ApState::Disabled};
    std::atomic<bool> m_simulatedMode{false};
    std::atomic<bool> m_watchdogRunning{false};

    WifiApCredentials m_credentials{};
    std::map<std::string, StationInfo> m_connectedStations;

    StationConnectedCallback m_stationConnectedCallback;
    StationDisconnectedCallback m_stationDisconnectedCallback;
    ApStateChangedCallback m_stateCallback;

    std::thread m_watchdogThread;
};

} // namespace CarPlayStation::Core::Transport
