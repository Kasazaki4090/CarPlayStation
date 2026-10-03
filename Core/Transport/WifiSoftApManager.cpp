#include "Core/Transport/WifiSoftApManager.h"
#include "Core/Common/Logging.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>
#endif

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

    // Windows Native Mobile Hotspot (WinRT NetworkOperatorTetheringManager) startup
#ifdef _WIN32
    m_state = ApState::Starting;

    try {
        using namespace winrt::Windows::Networking::Connectivity;
        using namespace winrt::Windows::Networking::NetworkOperators;

        auto profile = NetworkInformation::GetInternetConnectionProfile();
        if (profile) {
            auto mgr = NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
            if (mgr) {
                if (mgr.TetheringOperationalState() == TetheringOperationalState::Off) {
                    CPS_LOG_INFO("WifiSoftAp", "Windows Mobile Hotspot is Off; starting tethering...");
                    auto op = mgr.StartTetheringAsync().get();
                    if (op.Status() == TetheringOperationStatus::Success) {
                        CPS_LOG_INFO("WifiSoftAp", "Windows Mobile Hotspot started successfully");
                    } else {
                        CPS_LOG_WARN("WifiSoftAp", "StartTetheringAsync status: ", std::to_string(static_cast<int>(op.Status())));
                    }
                } else {
                    CPS_LOG_INFO("WifiSoftAp", "Windows Mobile Hotspot is already active (On)");
                }

                auto ap = mgr.GetCurrentAccessPointConfiguration();
                if (ap) {
                    std::string activeSsid = winrt::to_string(ap.Ssid());
                    std::string activePass = winrt::to_string(ap.Passphrase());
                    if (!activeSsid.empty()) {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_credentials.ssid = activeSsid;
                        m_credentials.passphrase = activePass;
                        m_credentials.band = WifiBand::Band5GHz;
                        // The WinRT API does not expose the real channel of the
                        // Mobile Hotspot, so we cannot know it. Keep channel=0
                        // (omit) so the iPhone scans for the SSID on its actual
                        // channel instead of being told a possibly-wrong one.
                        m_credentials.channel = 0;
                        m_credentials.security = WifiSecurityType::Wpa2Personal;
                        m_credentials.gatewayIp = "192.168.137.1";
                        CPS_LOG_INFO("WifiSoftAp", "Discovered Active Hotspot: SSID='", activeSsid, "', Passphrase='", activePass, "' (channel omitted; iPhone will scan)");
                    }
                }
            }
        }
    } catch (const std::exception& ex) {
        CPS_LOG_WARN("WifiSoftAp", "WinRT tethering check exception: ", ex.what());
    } catch (...) {
        CPS_LOG_WARN("WifiSoftAp", "WinRT tethering check unknown exception");
    }

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
#ifdef _WIN32
    try {
        using namespace winrt::Windows::Networking::Connectivity;
        using namespace winrt::Windows::Networking::NetworkOperators;

        auto profile = NetworkInformation::GetInternetConnectionProfile();
        if (!profile) return;
        auto mgr = NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
        if (!mgr || mgr.TetheringOperationalState() != TetheringOperationalState::On) return;

        auto clients = mgr.GetTetheringClients();
        std::map<std::string, StationInfo> currentClients;
        for (uint32_t i = 0; i < clients.Size(); ++i) {
            auto client = clients.GetAt(i);
            std::string mac = winrt::to_string(client.MacAddress());
            std::transform(mac.begin(), mac.end(), mac.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            std::string ip;
            auto hostNames = client.HostNames();
            for (uint32_t hIdx = 0; hIdx < hostNames.Size(); ++hIdx) {
                auto h = hostNames.GetAt(hIdx);
                if (h.Type() == winrt::Windows::Networking::HostNameType::Ipv4) {
                    ip = winrt::to_string(h.CanonicalName());
                    break;
                }
            }
            if (ip.empty() && hostNames.Size() > 0) {
                std::string raw = winrt::to_string(hostNames.GetAt(0).CanonicalName());
                if (raw.find('.') != std::string::npos && raw != "iPhone") {
                    ip = raw;
                }
            }
            if (ip.empty()) {
                ip = "192.168.137.100";
            }

            StationInfo st;
            st.macAddress = mac;
            st.ipAddress = ip;
            st.connectedTime = std::chrono::system_clock::now();
            currentClients[mac] = st;
        }

        std::vector<StationInfo> newlyConnected;
        std::vector<std::string> disconnected;
        StationConnectedCallback connCb;
        StationDisconnectedCallback discCb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& [mac, info] : currentClients) {
                if (m_connectedStations.find(mac) == m_connectedStations.end()) {
                    newlyConnected.push_back(info);
                }
            }
            for (const auto& [mac, info] : m_connectedStations) {
                if (currentClients.find(mac) == currentClients.end()) {
                    disconnected.push_back(mac);
                }
            }
            m_connectedStations = currentClients;
            connCb = m_stationConnectedCallback;
            discCb = m_stationDisconnectedCallback;
        }

        for (const auto& st : newlyConnected) {
            CPS_LOG_INFO("WifiSoftAp", "Station joined Mobile Hotspot: MAC=", st.macAddress, " IP=", st.ipAddress);
            if (connCb) connCb(st);
        }
        for (const auto& mac : disconnected) {
            CPS_LOG_INFO("WifiSoftAp", "Station left Mobile Hotspot: MAC=", mac);
            if (discCb) discCb(mac);
        }
    } catch (...) {
        // Query failure is ignored
    }
#endif
}

} // namespace CarPlayStation::Core::Transport
