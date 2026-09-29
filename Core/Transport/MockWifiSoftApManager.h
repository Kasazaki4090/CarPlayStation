#pragma once

#include "Core/Transport/IWifiSoftApManager.h"
#include <mutex>
#include <map>

namespace CarPlayStation::Core::Transport {

/**
 * @brief In-memory mock Wi-Fi SoftAP manager for upper-layer protocol testing
 * and determinism in CI.
 */
class MockWifiSoftApManager : public IWifiSoftApManager {
public:
    explicit MockWifiSoftApManager(ApState initialState = ApState::Disabled)
        : m_state(initialState) {}

    ~MockWifiSoftApManager() override = default;

    Common::Status StartAccessPoint(const WifiApCredentials& credentials) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_failStart) {
            return Common::Status::Error(Common::StatusCode::Unavailable, "Injected SoftAP start failure");
        }
        m_credentials = credentials;
        m_state = ApState::Running;
        if (m_stateCallback) {
            m_stateCallback(ApState::Running, Common::Status::Ok());
        }
        return Common::Status::Ok();
    }

    void StopAccessPoint() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = ApState::Disabled;
        m_connectedStations.clear();
        if (m_stateCallback) {
            m_stateCallback(ApState::Disabled, Common::Status::Ok());
        }
    }

    [[nodiscard]] ApState GetState() const noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

    [[nodiscard]] bool IsRunning() const noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state == ApState::Running;
    }

    [[nodiscard]] WifiApCredentials GetCredentials() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_credentials;
    }

    [[nodiscard]] std::vector<StationInfo> GetConnectedStations() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<StationInfo> res;
        for (const auto& [_, st] : m_connectedStations) {
            res.push_back(st);
        }
        return res;
    }

    [[nodiscard]] size_t GetConnectedStationCount() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_connectedStations.size();
    }

    void SetStationConnectedCallback(StationConnectedCallback callback) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stationConnectedCallback = std::move(callback);
    }

    void SetStationDisconnectedCallback(StationDisconnectedCallback callback) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stationDisconnectedCallback = std::move(callback);
    }

    void SetStateCallback(ApStateChangedCallback callback) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stateCallback = std::move(callback);
    }

    // Mock control helpers
    void SetFailStart(bool fail) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failStart = fail;
    }

    void SimulateStationJoin(const std::string& mac, const std::string& ip = "192.168.137.100") {
        StationInfo info{mac, ip, std::chrono::system_clock::now()};
        StationConnectedCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_connectedStations[mac] = info;
            cb = m_stationConnectedCallback;
        }
        if (cb) cb(info);
    }

    void SimulateStationLeave(const std::string& mac) {
        StationDisconnectedCallback cb;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_connectedStations.erase(mac) > 0) {
                found = true;
                cb = m_stationDisconnectedCallback;
            }
        }
        if (found && cb) cb(mac);
    }

private:
    mutable std::mutex m_mutex;
    ApState m_state{ApState::Disabled};
    bool m_failStart{false};
    WifiApCredentials m_credentials{};
    std::map<std::string, StationInfo> m_connectedStations;

    StationConnectedCallback m_stationConnectedCallback;
    StationDisconnectedCallback m_stationDisconnectedCallback;
    ApStateChangedCallback m_stateCallback;
};

} // namespace CarPlayStation::Core::Transport
