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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>
#include <winrt/Windows.Devices.WiFiDirect.h>
#include <winrt/Windows.Security.Credentials.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

std::string FormatMacLower(const UCHAR* addr, ULONG len) {
    if (!addr || len != 6) return {};
    char macBuf[32];
    snprintf(macBuf, sizeof(macBuf), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return std::string(macBuf);
}

bool IsValidHotspotClientIp(const std::string& ip) {
    return ip.rfind("192.168.137.", 0) == 0 && ip != "192.168.137.1" && ip != "192.168.137.255";
}

bool IsValidUnicastClientMac(const UCHAR* addr, ULONG len) {
    if (!addr || len != 6) return false;
    // Reject broadcast (ff:ff:...) and multicast (LSB of first byte set) or all-zero MACs.
    if ((addr[0] & 0x01) != 0) return false;
    for (ULONG i = 0; i < 6; ++i) {
        if (addr[i] != 0) return true;
    }
    return false;
}

std::string LookupIpv4ForMac(const std::string& targetMac) {
    if (targetMac.empty()) return "";
    PMIB_IPNET_TABLE2 pTable = nullptr;
    if (GetIpNetTable2(AF_INET, &pTable) == NO_ERROR && pTable) {
        for (ULONG i = 0; i < pTable->NumEntries; ++i) {
            const auto& row = pTable->Table[i];
            std::string mac = FormatMacLower(row.PhysicalAddress, row.PhysicalAddressLength);
            if (!mac.empty() && _stricmp(targetMac.c_str(), mac.c_str()) == 0) {
                char ipBuf[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &row.Address.Ipv4.sin_addr, ipBuf, sizeof(ipBuf));
                std::string res = ipBuf;
                if (IsValidHotspotClientIp(res)) {
                    FreeMibTable(pTable);
                    return res;
                }
            }
        }
        FreeMibTable(pTable);
    }
    return "";
}

std::map<std::string, std::string> EnumerateHotspotArpClients() {
    std::map<std::string, std::string> clients;
    PMIB_IPNET_TABLE2 pTable = nullptr;
    if (GetIpNetTable2(AF_INET, &pTable) == NO_ERROR && pTable) {
        for (ULONG i = 0; i < pTable->NumEntries; ++i) {
            const auto& row = pTable->Table[i];
            if (row.State != NlnsReachable && row.State != NlnsDelay && row.State != NlnsProbe) {
                continue;
            }
            if (!IsValidUnicastClientMac(row.PhysicalAddress, row.PhysicalAddressLength)) {
                continue;
            }
            char ipBuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &row.Address.Ipv4.sin_addr, ipBuf, sizeof(ipBuf));
            std::string ip = ipBuf;
            if (IsValidHotspotClientIp(ip)) {
                std::string mac = FormatMacLower(row.PhysicalAddress, row.PhysicalAddressLength);
                if (!mac.empty()) {
                    clients[mac] = ip;
                }
            }
        }
        FreeMibTable(pTable);
    }
    return clients;
}

template <typename Fn>
void ForEachCandidateConnectionProfile(Fn&& fn) {
    using namespace winrt::Windows::Networking::Connectivity;
    winrt::hstring internetName;
    if (auto inet = NetworkInformation::GetInternetConnectionProfile()) {
        internetName = inet.ProfileName();
        if (fn(inet, true)) {
            return;
        }
    }
    auto allProfiles = NetworkInformation::GetConnectionProfiles();
    // Pass 1: profiles that have at least local/constrained connectivity (e.g. LAN, Loopback).
    for (uint32_t i = 0; i < allProfiles.Size(); ++i) {
        auto p = allProfiles.GetAt(i);
        if (!p) continue;
        if (!internetName.empty() && p.ProfileName() == internetName) continue;
        if (p.GetNetworkConnectivityLevel() == NetworkConnectivityLevel::None) continue;
        if (fn(p, false)) {
            return;
        }
    }
    // Pass 2: any remaining profile (some Windows builds still expose tethering capability on them).
    for (uint32_t i = 0; i < allProfiles.Size(); ++i) {
        auto p = allProfiles.GetAt(i);
        if (!p) continue;
        if (!internetName.empty() && p.ProfileName() == internetName) continue;
        if (p.GetNetworkConnectivityLevel() != NetworkConnectivityLevel::None) continue;
        if (fn(p, false)) {
            return;
        }
    }
}

} // namespace
#endif

namespace CarPlayStation::Core::Transport {

struct WifiSoftApManager::NativeWifiDirectState {
#ifdef _WIN32
    winrt::Windows::Devices::WiFiDirect::WiFiDirectAdvertisementPublisher publisher{nullptr};
#endif
};

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

    // Safely join any previously running watchdog thread before starting a new one
    m_watchdogRunning = false;
    if (m_watchdogThread.joinable()) {
        if (m_watchdogThread.get_id() != std::this_thread::get_id()) {
            m_watchdogThread.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_credentials = credentials;
    }

    if (m_simulatedMode.load()) {
        m_state = ApState::Running;
        m_watchdogRunning = true;
        try {
            m_watchdogThread = std::thread(&WifiSoftApManager::WatchdogLoop, this);
        } catch (const std::exception& ex) {
            CPS_LOG_ERROR("WifiSoftAp", "Failed to launch simulated WatchdogLoop: ", ex.what());
        }

        ApStateChangedCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cb = m_stateCallback;
        }
        if (cb) cb(ApState::Running, Common::Status::Ok());
        return Common::Status::Ok();
    }

    // Tier 1-3 SoftAP startup on Windows:
    // 1. Mobile Hotspot via Internet connection profile
    // 2. Mobile Hotspot via any non-Internet local/loopback profile
    // 3. Wi-Fi Direct Legacy AP fallback (works 100% offline with zero profiles)
#ifdef _WIN32
    m_state = ApState::Starting;

    WifiApCredentials activeCreds = credentials;
    bool apReady = TryStartWindowsMobileHotspot(activeCreds);
    if (!apReady) {
        CPS_LOG_WARN("WifiSoftAp",
                     "Windows Mobile Hotspot unavailable (likely offline); attempting Wi-Fi Direct Legacy AP fallback...");
        apReady = TryStartWifiDirectLegacyAp(activeCreds);
    }

    if (apReady) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_credentials = activeCreds;
    }

    m_state = ApState::Running;
    m_watchdogRunning = true;
    try {
        m_watchdogThread = std::thread(&WifiSoftApManager::WatchdogLoop, this);
    } catch (const std::exception& ex) {
        CPS_LOG_ERROR("WifiSoftAp", "Failed to launch WatchdogLoop thread: ", ex.what());
    }

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

bool WifiSoftApManager::TryStartWindowsMobileHotspot(WifiApCredentials& creds) {
#ifdef _WIN32
    try {
        using namespace winrt::Windows::Networking::Connectivity;
        using namespace winrt::Windows::Networking::NetworkOperators;

        bool started = false;
        ForEachCandidateConnectionProfile([&](const ConnectionProfile& profile, bool isInternet) -> bool {
            try {
                auto mgr = NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
                if (!mgr) {
                    return false;
                }
                const std::string profName = winrt::to_string(profile.ProfileName());
                if (mgr.TetheringOperationalState() == TetheringOperationalState::Off) {
                    CPS_LOG_INFO("WifiSoftAp", "Starting Windows Mobile Hotspot via profile '",
                                 profName, "' (internet=", isInternet ? "yes" : "no", ")...");
                    auto op = mgr.StartTetheringAsync().get();
                    if (op.Status() == TetheringOperationStatus::Success) {
                        CPS_LOG_INFO("WifiSoftAp", "Windows Mobile Hotspot started successfully via '", profName, "'");
                    } else {
                        CPS_LOG_WARN("WifiSoftAp", "StartTetheringAsync failed on '", profName,
                                     "', status=", std::to_string(static_cast<int>(op.Status())));
                        return false;
                    }
                } else {
                    CPS_LOG_INFO("WifiSoftAp", "Windows Mobile Hotspot is already active (On) via '", profName, "'");
                }

                auto ap = mgr.GetCurrentAccessPointConfiguration();
                if (ap) {
                    std::string activeSsid = winrt::to_string(ap.Ssid());
                    std::string activePass = winrt::to_string(ap.Passphrase());
                    if (!activeSsid.empty()) {
                        creds.ssid = activeSsid;
                        creds.passphrase = activePass;
                        creds.band = WifiBand::Band5GHz;
                        // WinRT does not expose the real channel; omit channel=0 so the iPhone scans.
                        creds.channel = 0;
                        creds.security = WifiSecurityType::Wpa2Personal;
                        creds.gatewayIp = "192.168.137.1";
                        CPS_LOG_INFO("WifiSoftAp", "Discovered Active Hotspot: SSID='", activeSsid,
                                     "', Passphrase='", activePass, "' (channel omitted; iPhone will scan)");
                    }
                }
                m_wifiDirectActive = false;
                started = true;
                return true;
            } catch (const winrt::hresult_error& ex) {
                CPS_LOG_WARN("WifiSoftAp", "Tethering attempt failed on profile: 0x",
                             std::hex, static_cast<uint32_t>(ex.code()), std::dec,
                             " ", winrt::to_string(ex.message()));
                return false;
            }
        });
        return started;
    } catch (const winrt::hresult_error& ex) {
        CPS_LOG_WARN("WifiSoftAp", "WinRT tethering exception: 0x",
                     std::hex, static_cast<uint32_t>(ex.code()), std::dec,
                     " ", winrt::to_string(ex.message()));
    } catch (const std::exception& ex) {
        CPS_LOG_WARN("WifiSoftAp", "WinRT tethering check exception: ", ex.what());
    } catch (...) {
        CPS_LOG_WARN("WifiSoftAp", "WinRT tethering check unknown exception");
    }
#else
    (void)creds;
#endif
    return false;
}

bool WifiSoftApManager::TryStartWifiDirectLegacyAp(WifiApCredentials& creds) {
#ifdef _WIN32
    try {
        using namespace winrt::Windows::Devices::WiFiDirect;
        using namespace winrt::Windows::Security::Credentials;

        StopWifiDirectLegacyAp();

        auto state = std::make_unique<NativeWifiDirectState>();
        state->publisher = WiFiDirectAdvertisementPublisher();

        auto advert = state->publisher.Advertisement();
        advert.ListenStateDiscoverability(WiFiDirectAdvertisementListenStateDiscoverability::Normal);
        advert.IsAutonomousGroupOwnerEnabled(true);

        auto legacy = advert.LegacySettings();
        legacy.IsEnabled(true);
        if (!creds.ssid.empty()) {
            legacy.Ssid(winrt::to_hstring(creds.ssid));
        }
        PasswordCredential pass;
        pass.Password(winrt::to_hstring(creds.passphrase));
        legacy.Passphrase(pass);

        state->publisher.Start();
        for (int i = 0; i < 30; ++i) {
            auto st = state->publisher.Status();
            if (st == WiFiDirectAdvertisementPublisherStatus::Started) {
                creds.band = WifiBand::Band5GHz;
                creds.channel = 0;
                creds.security = WifiSecurityType::Wpa2Personal;
                creds.gatewayIp = "192.168.137.1";
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_wifiDirect = std::move(state);
                }
                m_wifiDirectActive = true;
                CPS_LOG_INFO("WifiSoftAp", "Offline Wi-Fi Direct Legacy AP started: SSID='",
                             creds.ssid, "', Passphrase='", creds.passphrase, "'");
                return true;
            }
            if (st == WiFiDirectAdvertisementPublisherStatus::Aborted) {
                CPS_LOG_WARN("WifiSoftAp", "Wi-Fi Direct Legacy AP aborted on Start()");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        state->publisher.Stop();
    } catch (const winrt::hresult_error& ex) {
        CPS_LOG_WARN("WifiSoftAp", "Wi-Fi Direct Legacy AP failed: 0x",
                     std::hex, static_cast<uint32_t>(ex.code()), std::dec,
                     " ", winrt::to_string(ex.message()));
    } catch (const std::exception& ex) {
        CPS_LOG_WARN("WifiSoftAp", "Wi-Fi Direct Legacy AP exception: ", ex.what());
    } catch (...) {
        CPS_LOG_WARN("WifiSoftAp", "Wi-Fi Direct Legacy AP unknown exception");
    }
#else
    (void)creds;
#endif
    return false;
}

void WifiSoftApManager::StopWifiDirectLegacyAp() {
#ifdef _WIN32
    std::unique_ptr<NativeWifiDirectState> oldState;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        oldState = std::move(m_wifiDirect);
    }
    m_wifiDirectActive = false;
    if (oldState && oldState->publisher) {
        try {
            oldState->publisher.Stop();
        } catch (...) {
        }
    }
#endif
}

void WifiSoftApManager::StopAccessPoint() {
    m_watchdogRunning = false;
    if (m_watchdogThread.joinable()) {
        if (m_watchdogThread.get_id() != std::this_thread::get_id()) {
            m_watchdogThread.join();
        }
    }

    StopWifiDirectLegacyAp();

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
#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    uint64_t watchdogTick = 0;
    while (m_watchdogRunning.load()) {
        for (int i = 0; i < 10 && m_watchdogRunning.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!m_watchdogRunning.load()) {
            break;
        }
        if (!m_simulatedMode.load()) {
            QueryNativeHostedNetworkStatus();

            watchdogTick++;
            // Every ~3 seconds (6 ticks of 500ms), re-notify connected stations
            // to keep mDNS discovery burst and AirPlay polling fresh while awaiting session
            if (watchdogTick % 6 == 0) {
                std::vector<StationInfo> activeStations;
                StationConnectedCallback connCb;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (const auto& [mac, info] : m_connectedStations) {
                        if (!info.ipAddress.empty()) {
                            activeStations.push_back(info);
                        }
                    }
                    connCb = m_stationConnectedCallback;
                }
                if (connCb) {
                    for (const auto& st : activeStations) {
                        connCb(st);
                    }
                }
            }
        }
    }
#ifdef _WIN32
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
#endif
}

void WifiSoftApManager::QueryNativeHostedNetworkStatus() {
#ifdef _WIN32
    try {
        using namespace winrt::Windows::Networking::Connectivity;
        using namespace winrt::Windows::Networking::NetworkOperators;

        std::map<std::string, StationInfo> currentClients;
        bool queriedOk = false;

        if (!m_wifiDirectActive.load()) {
            ForEachCandidateConnectionProfile([&](const ConnectionProfile& profile, bool /*isInternet*/) -> bool {
                try {
                    auto mgr = NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
                    if (!mgr || mgr.TetheringOperationalState() != TetheringOperationalState::On) {
                        return false;
                    }
                    auto clients = mgr.GetTetheringClients();
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
                            ip = LookupIpv4ForMac(mac);
                        }

                        StationInfo st;
                        st.macAddress = mac;
                        st.ipAddress = ip;
                        st.connectedTime = std::chrono::system_clock::now();
                        currentClients[mac] = st;
                    }
                    queriedOk = true;
                    return true;
                } catch (...) {
                    return false;
                }
            });
        } else {
            for (const auto& [mac, ip] : EnumerateHotspotArpClients()) {
                StationInfo st;
                st.macAddress = mac;
                st.ipAddress = ip;
                st.connectedTime = std::chrono::system_clock::now();
                currentClients[mac] = st;
            }
            queriedOk = true;
        }

        if (!queriedOk) {
            return;
        }

        std::vector<StationInfo> newlyConnected;
        std::vector<std::string> disconnected;
        StationConnectedCallback connCb;
        StationDisconnectedCallback discCb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& [mac, info] : currentClients) {
                auto it = m_connectedStations.find(mac);
                if (it == m_connectedStations.end()) {
                    newlyConnected.push_back(info);
                } else if (!info.ipAddress.empty() && (it->second.ipAddress.empty() || it->second.ipAddress != info.ipAddress)) {
                    CPS_LOG_INFO("WifiSoftAp", "Station IP resolved/updated for MAC=", mac,
                                 " from '", it->second.ipAddress, "' to '", info.ipAddress, "'");
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
            CPS_LOG_INFO("WifiSoftAp", "Station joined/active on Mobile Hotspot: MAC=", st.macAddress, " IP=", st.ipAddress);
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
