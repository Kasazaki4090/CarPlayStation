#include "Core/Transport/MdnsAdvertiser.h"
#include <chrono>

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
    return Common::Status::Ok();
#else
    (void)packet;
    return Common::Status::Error(Common::StatusCode::NotImplemented, "SendRawMulticast not implemented on non-Windows");
#endif
}

void MdnsAdvertiser::BroadcastWorkerLoop() {
    // Initial burst: 3 packets with 1 second delay
    for (int i = 0; i < 3 && m_running.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (m_running.load()) {
            (void)BroadcastAnnouncement();
        }
    }

    // Refresh announcement every 60 seconds
    while (m_running.load()) {
        for (int s = 0; s < 60 && m_running.load(); ++s) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (m_running.load()) {
            (void)BroadcastAnnouncement();
        }
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
