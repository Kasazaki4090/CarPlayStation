#include "Core/Transport/MdnsAdvertiser.h"
#include "Core/Common/Logging.h"
#include <chrono>
#include <algorithm>
#include <string_view>

namespace CarPlayStation::Core::Transport {

MdnsAdvertiser::MdnsAdvertiser() = default;

MdnsAdvertiser::~MdnsAdvertiser() {
    Stop();
}

Common::Status MdnsAdvertiser::Start(const MdnsServiceConfig& config) {
    if (m_running.load()) {
        return Common::Status::Ok();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
    }

    if (m_simulatedMode.load()) {
        m_running = true;
        (void)BroadcastAnnouncement();
        m_workerThread = std::thread(&MdnsAdvertiser::BroadcastWorkerLoop, this);
        return Common::Status::Ok();
    }

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return Common::Status::Error(Common::StatusCode::Unavailable, "WSAStartup failed");
    }

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        int err = WSAGetLastError();
        return Common::Status::Error(Common::StatusCode::Unavailable, "Failed to create UDP socket: code " + std::to_string(err));
    }

    BOOL reuse = TRUE;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    BOOL bcast = TRUE;
    setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    // Bind to 0.0.0.0:5353 so all outgoing mDNS packets have source port 5353 per RFC 6762
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(5353);
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_socket, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        if (!config.ipv4Address.empty() && config.ipv4Address != "0.0.0.0") {
            inet_pton(AF_INET, config.ipv4Address.c_str(), &bindAddr.sin_addr);
            bind(m_socket, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr));
        }
    }

    // Join multicast group 224.0.0.251
    ip_mreq mreq{};
    inet_pton(AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
    if (!config.ipv4Address.empty() && config.ipv4Address != "0.0.0.0") {
        inet_pton(AF_INET, config.ipv4Address.c_str(), &mreq.imr_interface);
    } else {
        mreq.imr_interface.s_addr = INADDR_ANY;
    }
    setsockopt(m_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

    // Multicast TTL = 255 per RFC 6762
    unsigned char ttl = 255;
    setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));

    m_running = true;
    (void)BroadcastAnnouncement();
    m_workerThread = std::thread(&MdnsAdvertiser::BroadcastWorkerLoop, this);

    return Common::Status::Ok();
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "mDNS native socket only supported on Windows");
#endif
}

void MdnsAdvertiser::Stop() {
    m_running = false;

#ifdef _WIN32
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
#endif

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

bool MdnsAdvertiser::IsRunning() const noexcept {
    return m_running.load();
}

MdnsServiceConfig MdnsAdvertiser::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

Common::Status MdnsAdvertiser::BroadcastAnnouncement() {
    MdnsServiceConfig cfg;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cfg = m_config;
    }

    auto packet = MdnsPacketBuilder::BuildAirPlayAnnouncement(cfg);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastPacket = packet;
        m_broadcastCount++;
    }

    if (m_simulatedMode.load()) {
        return Common::Status::Ok();
    }

    return SendRawMulticast(Common::ByteSpan(packet.data(), packet.size()));
}

Common::Status MdnsAdvertiser::AnnounceTo(const std::string& targetIp) {
    if (m_simulatedMode.load()) {
        return BroadcastAnnouncement();
    }
#ifdef _WIN32
    if (!targetIp.empty() && m_socket != INVALID_SOCKET) {
        MdnsServiceConfig cfg;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cfg = m_config;
        }
        auto packet = MdnsPacketBuilder::BuildAirPlayAnnouncement(cfg);
        sockaddr_in targetAddr{};
        targetAddr.sin_family = AF_INET;
        targetAddr.sin_port = htons(5353);
        if (inet_pton(AF_INET, targetIp.c_str(), &targetAddr.sin_addr) == 1) {
            sendto(m_socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                   reinterpret_cast<const sockaddr*>(&targetAddr), sizeof(targetAddr));

            // Also query targeted _carplay-ctrl._tcp.local.
            auto query = MdnsPacketBuilder::BuildCarPlayControlQuery();
            sendto(m_socket, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0,
                   reinterpret_cast<const sockaddr*>(&targetAddr), sizeof(targetAddr));
        }
    }
#endif
    return BroadcastAnnouncement();
}

Common::Status MdnsAdvertiser::SendRawMulticast(Common::ByteSpan packet) {
#ifdef _WIN32
    if (m_socket == INVALID_SOCKET) {
        return Common::Status::Error(Common::StatusCode::InvalidState, "UDP socket not open");
    }

    sockaddr_in mcastAddr{};
    mcastAddr.sin_family = AF_INET;
    mcastAddr.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &mcastAddr.sin_addr);

    int sent = sendto(m_socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                      reinterpret_cast<const sockaddr*>(&mcastAddr), sizeof(mcastAddr));
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        return Common::Status::Error(Common::StatusCode::IoError, "Multicast sendto failed: code " + std::to_string(err));
    }

    // Also explicitly target the Hotspot / local interface (e.g. 192.168.137.1)
    if (!m_config.ipv4Address.empty() && m_config.ipv4Address != "0.0.0.0") {
        in_addr ifAddr{};
        if (inet_pton(AF_INET, m_config.ipv4Address.c_str(), &ifAddr) == 1) {
            setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&ifAddr), sizeof(ifAddr));
            sendto(m_socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                   reinterpret_cast<const sockaddr*>(&mcastAddr), sizeof(mcastAddr));
        }

        // Subnet broadcast to 192.168.137.255:5353
        std::string bcastIp = m_config.ipv4Address;
        auto lastDot = bcastIp.rfind('.');
        if (lastDot != std::string::npos) {
            bcastIp = bcastIp.substr(0, lastDot) + ".255";
            sockaddr_in bcastAddr{};
            bcastAddr.sin_family = AF_INET;
            bcastAddr.sin_port = htons(5353);
            if (inet_pton(AF_INET, bcastIp.c_str(), &bcastAddr.sin_addr) == 1) {
                sendto(m_socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                       reinterpret_cast<const sockaddr*>(&bcastAddr), sizeof(bcastAddr));
            }
        }
    }
    return Common::Status::Ok();
#else
    (void)packet;
    return Common::Status::Error(Common::StatusCode::NotImplemented, "SendRawMulticast not implemented on non-Windows");
#endif
}

void MdnsAdvertiser::DialCarPlayControl(const std::string& host, uint16_t port) {
#ifdef _WIN32
    if (host.empty() || port == 0) return;

    std::string key = host + ":" + std::to_string(port);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (std::find(m_dialedTargets.begin(), m_dialedTargets.end(), key) != m_dialedTargets.end()) {
            return;
        }
        m_dialedTargets.push_back(key);
    }

    std::string devId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        devId = m_config.deviceId;
    }
    devId.erase(std::remove(devId.begin(), devId.end(), ':'), devId.end());

    std::thread([host, port, devId]() {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return;

        DWORD timeout = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            closesocket(s);
            return;
        }

        if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
            std::string req = "GET /ctrl-int/1/connect HTTP/1.1\r\n"
                              "Host: " + host + ":" + std::to_string(port) + "\r\n"
                              "User-Agent: AirPlay/320.17\r\n"
                              "AirPlay-Receiver-Device-ID: " + devId + "\r\n"
                              "Connection: close\r\n\r\n";

            send(s, req.data(), static_cast<int>(req.size()), 0);
            CPS_LOG_INFO("MdnsAdvertiser", "Sent /ctrl-int/1/connect to iPhone at ", host, ":", std::to_string(port), " (devId=", devId, ")");

            char buf[256];
            int n = recv(s, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                CPS_LOG_INFO("MdnsAdvertiser", "iPhone /ctrl-int/1/connect response: ", std::string(buf, (std::min)(n, 64)));
            }
        } else {
            CPS_LOG_WARN("MdnsAdvertiser", "Failed to connect to iPhone control port ", host, ":", std::to_string(port));
        }
        closesocket(s);
    }).detach();
#else
    (void)host; (void)port;
#endif
}

void MdnsAdvertiser::BroadcastWorkerLoop() {
    while (m_running.load()) {
        (void)BroadcastAnnouncement();

#ifdef _WIN32
        if (m_socket != INVALID_SOCKET) {
            // Multicast query for _carplay-ctrl._tcp.local.
            auto query = MdnsPacketBuilder::BuildCarPlayControlQuery();
            SendRawMulticast(Common::ByteSpan(query.data(), query.size()));

            // Listen for responses using select() with 1s timeout
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(m_socket, &readfds);

            timeval tv{};
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            int res = select(0, &readfds, nullptr, nullptr, &tv);
            if (res > 0 && FD_ISSET(m_socket, &readfds)) {
                sockaddr_in fromAddr{};
                int fromLen = sizeof(fromAddr);
                uint8_t rxBuf[2048];
                int bytesRead = recvfrom(m_socket, reinterpret_cast<char*>(rxBuf), sizeof(rxBuf), 0,
                                         reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
                if (bytesRead > 12) {
                    char ipStr[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));

                    // Inspect if packet contains _carplay-ctrl
                    std::string_view rxView(reinterpret_cast<const char*>(rxBuf), bytesRead);
                    size_t ctrlPos = rxView.find("_carplay-ctrl");
                    if (ctrlPos != std::string_view::npos) {
                        for (size_t i = ctrlPos; i + 16 < rxView.size(); ++i) {
                            uint16_t type = (static_cast<uint8_t>(rxBuf[i]) << 8) | static_cast<uint8_t>(rxBuf[i + 1]);
                            if (type == MdnsPacketBuilder::kDnsTypeSrv) {
                                uint16_t port = (static_cast<uint8_t>(rxBuf[i + 14]) << 8) | static_cast<uint8_t>(rxBuf[i + 15]);
                                if (port > 0) {
                                    CPS_LOG_INFO("MdnsAdvertiser", "Discovered iPhone _carplay-ctrl service at ", ipStr, ":", std::to_string(port));
                                    DialCarPlayControl(ipStr, port);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
#endif
    }
}

void MdnsAdvertiser::EnableSimulatedMode(bool enable) {
    m_simulatedMode = enable;
}

bool MdnsAdvertiser::IsSimulatedMode() const noexcept {
    return m_simulatedMode.load();
}

size_t MdnsAdvertiser::GetBroadcastCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_broadcastCount;
}

std::optional<Common::ByteBuffer> MdnsAdvertiser::GetLastBroadcastPacket() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lastPacket.empty()) {
        return std::nullopt;
    }
    return m_lastPacket;
}

} // namespace CarPlayStation::Core::Transport
