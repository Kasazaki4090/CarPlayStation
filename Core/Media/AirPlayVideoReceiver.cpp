#include "Core/Media/AirPlayVideoReceiver.h"
#include "Core/Common/Logging.h"
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace CarPlayStation::Core::Media {

AirPlayVideoReceiver::AirPlayVideoReceiver(std::shared_ptr<IVideoDecoder> decoder, uint16_t port)
    : decoder_(std::move(decoder)), port_(port) {
    demuxer_.SetFrameCallback([this](const DemuxedFrame& frame) {
        if (decoder_ && !frame.annexBData.empty()) {
            decoder_->DecodeAccessUnit(Common::ByteSpan(frame.annexBData.data(), frame.annexBData.size()), frame.ptsUs);
        }
    });
}

AirPlayVideoReceiver::~AirPlayVideoReceiver() {
    Stop();
}

Common::Status AirPlayVideoReceiver::Start() {
    if (running_.load()) {
        return Common::Status::Ok();
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return Common::MakeError(Common::StatusCode::Unavailable, "WSAStartup failed");
    }

    // 1. Create TCP listener socket for AirPlay screen mirroring
    SOCKET tcpSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcpSock == INVALID_SOCKET) {
        return Common::MakeError(Common::StatusCode::IoError, "Failed to create TCP video socket");
    }

    int opt = 1;
    setsockopt(tcpSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in tcpAddr{};
    tcpAddr.sin_family = AF_INET;
    tcpAddr.sin_port = htons(port_);
    tcpAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(tcpSock, reinterpret_cast<sockaddr*>(&tcpAddr), sizeof(tcpAddr)) == SOCKET_ERROR) {
        closesocket(tcpSock);
        CPS_LOG_WARN("VideoReceiver", "Failed to bind TCP port ", std::to_string(port_));
    } else if (listen(tcpSock, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(tcpSock);
        CPS_LOG_WARN("VideoReceiver", "Failed to listen on TCP port ", std::to_string(port_));
    } else {
        tcp_listen_sock_ = static_cast<uintptr_t>(tcpSock);
        CPS_LOG_INFO("VideoReceiver", "TCP Video listener active on port ", std::to_string(port_));
    }

    // 2. Create UDP socket for RTP video packets
    SOCKET udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSock != INVALID_SOCKET) {
        setsockopt(udpSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        int rcvBuf = 2 * 1024 * 1024; // 2MB buffer
        setsockopt(udpSock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

        sockaddr_in udpAddr{};
        udpAddr.sin_family = AF_INET;
        udpAddr.sin_port = htons(port_);
        udpAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(udpSock, reinterpret_cast<sockaddr*>(&udpAddr), sizeof(udpAddr)) == SOCKET_ERROR) {
            closesocket(udpSock);
            CPS_LOG_WARN("VideoReceiver", "Failed to bind UDP port ", std::to_string(port_));
        } else {
            udp_sock_ = static_cast<uintptr_t>(udpSock);
            CPS_LOG_INFO("VideoReceiver", "UDP Video listener active on port ", std::to_string(port_));
        }
    }

    running_.store(true);

    if (tcp_listen_sock_ != ~0ULL) {
        tcp_thread_ = std::thread(&AirPlayVideoReceiver::TcpListenLoop, this);
    }
    if (udp_sock_ != ~0ULL) {
        udp_thread_ = std::thread(&AirPlayVideoReceiver::UdpListenLoop, this);
    }

    return Common::Status::Ok();
#else
    return Common::MakeError(Common::StatusCode::NotImplemented, "Only supported on Windows");
#endif
}

void AirPlayVideoReceiver::Stop() {
    if (!running_.load()) return;
    running_.store(false);

#ifdef _WIN32
    if (tcp_listen_sock_ != ~0ULL) {
        closesocket(static_cast<SOCKET>(tcp_listen_sock_));
        tcp_listen_sock_ = ~0ULL;
    }
    if (udp_sock_ != ~0ULL) {
        closesocket(static_cast<SOCKET>(udp_sock_));
        udp_sock_ = ~0ULL;
    }
#endif

    if (tcp_thread_.joinable()) {
        tcp_thread_.join();
    }
    if (udp_thread_.joinable()) {
        udp_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        for (auto& th : client_threads_) {
            if (th.joinable()) {
                th.join();
            }
        }
        client_threads_.clear();
    }
}

bool AirPlayVideoReceiver::IsRunning() const noexcept {
    return running_.load();
}

uint64_t AirPlayVideoReceiver::GetReceivedFrames() const noexcept {
    return received_frames_.load(std::memory_order_relaxed);
}

void AirPlayVideoReceiver::TcpListenLoop() {
#ifdef _WIN32
    SOCKET listenSock = static_cast<SOCKET>(tcp_listen_sock_);
    while (running_.load()) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientSock == INVALID_SOCKET) {
            if (!running_.load()) break;
            continue;
        }

        char ipStr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        CPS_LOG_INFO("VideoReceiver", "Incoming TCP video stream connected from ", ipStr, ":", std::to_string(ntohs(clientAddr.sin_port)));

        std::lock_guard<std::mutex> lock(client_mutex_);
        client_threads_.emplace_back(&AirPlayVideoReceiver::TcpClientWorker, this, static_cast<uintptr_t>(clientSock));
    }
#endif
}

void AirPlayVideoReceiver::TcpClientWorker(uintptr_t clientSockRaw) {
#ifdef _WIN32
    SOCKET sock = static_cast<SOCKET>(clientSockRaw);
    uint8_t hdr[128];
    std::vector<uint8_t> body;

    while (running_.load()) {
        size_t hdrRead = 0;
        while (hdrRead < sizeof(hdr) && running_.load()) {
            int n = recv(sock, reinterpret_cast<char*>(hdr + hdrRead), static_cast<int>(sizeof(hdr) - hdrRead), 0);
            if (n <= 0) {
                closesocket(sock);
                CPS_LOG_INFO("VideoReceiver", "TCP video stream disconnected");
                return;
            }
            hdrRead += static_cast<size_t>(n);
        }

        uint32_t bodySize = Common::Endian::ReadLe32(hdr);
        uint8_t opcode = hdr[4];

        if (bodySize > 10 * 1024 * 1024) {
            CPS_LOG_WARN("VideoReceiver", "Abnormally large video payload: ", std::to_string(bodySize));
            break;
        }

        if (bodySize == 0) continue;

        body.resize(bodySize);
        size_t bodyRead = 0;
        while (bodyRead < bodySize && running_.load()) {
            int n = recv(sock, reinterpret_cast<char*>(body.data() + bodyRead), static_cast<int>(bodySize - bodyRead), 0);
            if (n <= 0) {
                closesocket(sock);
                return;
            }
            bodyRead += static_cast<size_t>(n);
        }

        uint64_t ptsUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        auto status = demuxer_.FeedPayload(Common::ByteSpan(body.data(), body.size()), ptsUs);
        if (status.IsOk()) {
            received_frames_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    closesocket(sock);
#else
    (void)clientSockRaw;
#endif
}

void AirPlayVideoReceiver::UdpListenLoop() {
#ifdef _WIN32
    SOCKET sock = static_cast<SOCKET>(udp_sock_);
    std::vector<uint8_t> buf(65536);

    while (running_.load()) {
        sockaddr_in fromAddr{};
        int fromLen = sizeof(fromAddr);
        int n = recvfrom(sock, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                         reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n <= 0) {
            if (!running_.load()) break;
            continue;
        }

        if (n > 12) {
            uint32_t rtpTs = Common::Endian::ReadBe32(buf.data() + 4);
            uint64_t ptsUs = static_cast<uint64_t>(rtpTs) * 1000000ULL / 90000ULL;
            auto status = demuxer_.FeedPayload(Common::ByteSpan(buf.data() + 12, static_cast<size_t>(n - 12)), ptsUs);
            if (status.IsOk()) {
                received_frames_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
#endif
}

} // namespace CarPlayStation::Core::Media
