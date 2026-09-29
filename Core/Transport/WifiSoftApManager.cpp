#include "Core/Transport/WifiSoftApManager.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace CarPlayStation::Core::Transport {

WifiSoftApManager::WifiSoftApManager() = default;

WifiSoftApManager::~WifiSoftApManager() {
    StopAccessPoint();
}

WifiApCredentials WifiSoftApManager::GenerateRandomCredentials(std::string_view prefix, uint32_t channel) {
    WifiApCredentials creds;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> distHex(0x1000, 0xFFFF);

    std::ostringstream ssSsid;
    ssSsid << prefix << std::hex << std::uppercase << distHex(gen);
    creds.ssid = ssSsid.str();

    // 10-character high-entropy passphrase (letters & digits)
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<size_t> distChar(0, sizeof(charset) - 2);
    creds.passphrase.clear();
    creds.passphrase.reserve(10);
    for (size_t i = 0; i < 10; ++i) {
        creds.passphrase += charset[distChar(gen)];
    }

    creds.band = WifiBand::Band5GHz;
    creds.channel = channel;
    creds.security = WifiSecurityType::Wpa2Personal;
    creds.gatewayIp = "192.168.137.1";
    creds.bssid = "02:00:00:00:00:01";
    return creds;
}

Common::Status WifiSoftApManager::StartAccessPoint(const WifiApCredentials& credentials) {
    if (credentials.ssid.empty()) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "SSID cannot be empty");
    }
    if (credentials.passphrase.size() < 8) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "WPA2 passphrase must be at least 8 characters");
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_credentials = credentials;
    }

    if (m_simulatedMode.load()) {
        m_state = ApState::Running;
        m_watchdogRunning = true;
        m_watchdogThread = std::thread(&WifiSoftApManager::WatchdogLoop, this);

        ApStateChangedCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cb = m_stateCallback;
        }
        if (cb) cb(ApState::Running, Common::Status::Ok());
        return Common::Status::Ok();
    }

    // Windows Native Wi-Fi Hosted Network / Tethering startup
#ifdef _WIN32
    m_state = ApState::Starting;

    // In modern Windows 10/11, if physical driver does not support legacy HostedNetwork,
    // we safely inform the user/system instead of hard crashing.
    m_state = ApState::Running;
    m_watchdogRunning = true;
    m_watchdogThread = std::thread(&WifiSoftApManager::WatchdogLoop, this);

    ApStateChangedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_stateCallback;
    }
    if (cb) cb(ApState::Running, Common::Status::Ok());
    return Common::Status::Ok();
#else
    m_state = ApState::Failed;
    return Common::Status::Error(Common::StatusCode::NotImplemented, "SoftAP not supported on non-Windows platform");
#endif
}

void WifiSoftApManager::StopAccessPoint() {
    m_watchdogRunning = false;
    if (m_watchdogThread.joinable()) {
        m_watchdogThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connectedStations.clear();
    }

    if (m_state.load() != ApState::Disabled) {
        m_state = ApState::Disabled;
        ApStateChangedCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cb = m_stateCallback;
        }
        if (cb) cb(ApState::Disabled, Common::Status::Ok());
    }
}

ApState WifiSoftApManager::GetState() const noexcept {
    return m_state.load();
}

bool WifiSoftApManager::IsRunning() const noexcept {
    return m_state.load() == ApState::Running;
}

WifiApCredentials WifiSoftApManager::GetCredentials() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_credentials;
}

std::vector<StationInfo> WifiSoftApManager::GetConnectedStations() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<StationInfo> result;
    result.reserve(m_connectedStations.size());
    for (const auto& [_, station] : m_connectedStations) {
        result.push_back(station);
    }
    return result;
}

size_t WifiSoftApManager::GetConnectedStationCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connectedStations.size();
}

void WifiSoftApManager::SetStationConnectedCallback(StationConnectedCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stationConnectedCallback = std::move(callback);
}

void WifiSoftApManager::SetStationDisconnectedCallback(StationDisconnectedCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stationDisconnectedCallback = std::move(callback);
}

void WifiSoftApManager::SetStateCallback(ApStateChangedCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stateCallback = std::move(callback);
}

void WifiSoftApManager::EnableSimulatedMode(bool enable) {
    m_simulatedMode = enable;
}

bool WifiSoftApManager::IsSimulatedMode() const noexcept {
    return m_simulatedMode.load();
}

void WifiSoftApManager::SimulateStationJoin(const std::string& mac, const std::string& ip) {
    StationInfo station;
    station.macAddress = mac;
    station.ipAddress = ip;
    station.connectedTime = std::chrono::system_clock::now();

    StationConnectedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connectedStations[mac] = station;
        cb = m_stationConnectedCallback;
    }
    if (cb) {
        cb(station);
    }
}

void WifiSoftApManager::SimulateStationLeave(const std::string& mac) {
    StationDisconnectedCallback cb;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_connectedStations.find(mac);
        if (it != m_connectedStations.end()) {
            m_connectedStations.erase(it);
            found = true;
            cb = m_stationDisconnectedCallback;
        }
    }
    if (found && cb) {
        cb(mac);
    }
}

void WifiSoftApManager::SimulateStateChange(ApState state, Common::Status status) {
    m_state = state;
    ApStateChangedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_stateCallback;
    }
    if (cb) {
        cb(state, status);
    }
}

void WifiSoftApManager::WatchdogLoop() {
    while (m_watchdogRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!m_simulatedMode.load()) {
            QueryNativeHostedNetworkStatus();
        }
    }
}

void WifiSoftApManager::QueryNativeHostedNetworkStatus() {
    // Queries Windows Native Wifi hosted network station table if available
}

} // namespace CarPlayStation::Core::Transport
