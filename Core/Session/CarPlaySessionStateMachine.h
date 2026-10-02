#pragma once

#include "Core/Session/ICarPlaySession.h"
#include "Core/Session/IPairingStore.h"
#include "Core/Session/AirPlayRtspTypes.h"
#include "Core/Session/AirPlayRtspServer.h"
#include "Core/Transport/IWifiSoftApManager.h"
#include "Core/Transport/IMdnsAdvertiser.h"
#include "Core/Common/Types.h"
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>

namespace CarPlayStation::Core::Session {

struct SessionConfig {
    std::string device_id{"00:1A:7D:DA:71:05"};
    std::string ap_ssid{"CarPlayStation"};
    std::string ap_passphrase{"CarPlayStationSecret123"};
    uint8_t ap_channel{36}; // 5GHz
    uint16_t rtsp_port{5000};
    std::chrono::milliseconds bt_auth_timeout{10000};
    std::chrono::milliseconds wifi_handoff_timeout{15000};
    std::chrono::milliseconds rtsp_setup_timeout{15000};
    std::chrono::milliseconds reconnect_interval{2000};
    uint32_t max_reconnect_attempts{3};
    bool auto_reconnect{true};
    bool enable_background_watchdog{false};
};

struct ActiveSessionInfo {
    std::string device_id;
    std::string device_name;
    Common::MacAddress bluetooth_mac{};
    Common::MacAddress wifi_mac{};
    NegotiatedMediaParams media_params{};
    bool is_fast_reconnect{false};
    uint32_t reconnect_attempts{0};
    std::chrono::steady_clock::time_point state_entered_time{};
};

struct SessionDependencies {
    std::shared_ptr<IPairingStore> pairing_store;
    std::shared_ptr<Transport::IWifiSoftApManager> wifi_manager;
    std::shared_ptr<Transport::IMdnsAdvertiser> mdns_advertiser;
    std::shared_ptr<AirPlayRtspServer> rtsp_server;
};

/**
 * @brief Central CarPlay Session State Machine and lifecycle orchestrator.
 *
 * Coordinates Layer 1/2/3/4 components through the wireless CarPlay handshake:
 * Idle -> BtDiscovered -> iAP2Authenticated -> WifiHandoff -> AirPlayStreaming -> Paused -> Teardown -> Reconnecting / Error.
 */
class CarPlaySessionStateMachine : public ICarPlaySession {
public:
    explicit CarPlaySessionStateMachine(SessionConfig config = {}, SessionDependencies deps = {});
    ~CarPlaySessionStateMachine() override;

    // --- ICarPlaySession API ---
    [[nodiscard]] Common::Status Start() override;
    void Stop() override;
    [[nodiscard]] SessionState GetState() const noexcept override;
    void RegisterStateObserver(SessionStateObserver observer) override;

    // --- Session Status & Diagnostics ---
    [[nodiscard]] ActiveSessionInfo GetActiveSessionInfo() const;
    [[nodiscard]] const SessionConfig& GetConfig() const noexcept { return config_; }

    // --- Event Ingestion & State Transitions ---
    [[nodiscard]] Common::Status OnBluetoothDeviceDiscovered(
        std::string_view device_id,
        const Common::MacAddress& mac,
        std::string_view device_name = {});

    [[nodiscard]] Common::Status OnIap2Authenticated(
        bool success,
        std::string_view device_name = {});

    [[nodiscard]] Common::Status OnWifiConnected(
        const Common::MacAddress& client_mac,
        std::string_view client_ip = {});

    [[nodiscard]] Common::Status OnAirPlayStreamingStarted(
        const NegotiatedMediaParams& params);

    [[nodiscard]] Common::Status OnAirPlayStreamingPaused();

    [[nodiscard]] Common::Status OnAirPlayStreamingResumed();

    [[nodiscard]] Common::Status OnAirPlayTeardown();

    [[nodiscard]] Common::Status OnDisconnect(std::string_view reason);

    [[nodiscard]] Common::Status OnError(Common::Status error_status);

    /**
     * @brief Manually tick the watchdog timer for deterministic unit testing.
     */
    void TickWatchdog(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    /**
     * @brief Reset state back to Idle without tearing down dependencies.
     */
    void Reset();

    /**
     * @brief Update or inject session dependencies at runtime.
     */
    void SetDependencies(SessionDependencies deps);

private:
    void SetStateLocked(SessionState new_state, Common::Status status, std::vector<SessionStateObserver>& out_observers);
    void DispatchObservers(const std::vector<SessionStateObserver>& observers, SessionState old_state, SessionState new_state, Common::Status status);
    void WatchdogLoop();

    SessionConfig config_;
    SessionDependencies deps_;

    mutable std::mutex mutex_;
    SessionState state_{SessionState::Idle};
    ActiveSessionInfo active_session_{};
    std::vector<SessionStateObserver> observers_;

    std::atomic<bool> running_{false};
    std::thread watchdog_thread_;
};

} // namespace CarPlayStation::Core::Session
