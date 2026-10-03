#include "Core/Media/AirPlayVideoReceiver.h"
#include "Core/Common/Logging.h"
#include "Core/Session/Pairing/Crypto.h"
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
    frame_counter_ = 0;

    if (decoder_ && !decoder_initialized_.exchange(true)) {
        Common::VideoStreamSpec spec;
        spec.width = 1280;
        spec.height = 720;
        spec.fps = 60;
        spec.codec = Common::VideoCodec::H264;
        auto initSt = decoder_->Init(spec);
        CPS_LOG_INFO("VideoReceiver", "Decoder initialized with 1280x720@60 H.264: ", initSt.IsOk() ? "OK" : initSt.Message());
    }

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

        if (opcode == 0) { // VideoFrame
            std::vector<uint8_t> plain;
            if (decrypt_key_set_.load()) {
                uint8_t nonce[12] = {0};
                uint64_t cnt = frame_counter_++;
                memcpy(nonce + 4, &cnt, 8);

                if (!pairing::ChaCha20Poly1305OpenWithAad(
                        decrypt_key_.data(), nonce, hdr, sizeof(hdr),
                        body.data(), body.size(), plain)) {
                    CPS_LOG_WARN("VideoReceiver", "Video frame ChaCha20 decrypt failed at frame ", std::to_string(cnt));
                    continue;
                }
            } else {
                plain = std::move(body);
            }

            // Convert AVCC length-prefixed NALUs to Annex-B (00 00 00 01 prefix)
            std::vector<uint8_t> annexb;
            annexb.reserve(plain.size() + 16);
            size_t off = 0;
            while (off + 4 <= plain.size()) {
                uint32_t naluLen = Common::Endian::ReadBe32(plain.data() + off);
                off += 4;
                if (naluLen == 0 || off + naluLen > plain.size()) {
                    break;
                }
                annexb.push_back(0);
                annexb.push_back(0);
                annexb.push_back(0);
                annexb.push_back(1);
                annexb.insert(annexb.end(), plain.data() + off, plain.data() + off + naluLen);
                off += naluLen;
            }

            if (!annexb.empty() && decoder_) {
                auto decSt = decoder_->DecodeAccessUnit(Common::ByteSpan(annexb.data(), annexb.size()), ptsUs);
                if (decSt.IsOk()) {
                    uint64_t total = received_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (!first_frame_logged_.exchange(true)) {
                        CPS_LOG_INFO("VideoReceiver", "First video frame received & decoded (TCP) — total frames: ", std::to_string(total));
                    }
                } else {
                    CPS_LOG_WARN("VideoReceiver", "DecodeAccessUnit error: ", decSt.Message());
                }
            }
        } else if (opcode == 1) { // VideoConfig (avcC)
            if (body.size() >= 6) {
                uint8_t num_sps = body[5] & 0x1F;
                size_t off = 6;
                std::vector<uint8_t> annexb;
                for (uint8_t i = 0; i < num_sps && off + 2 <= body.size(); ++i) {
                    uint16_t sps_len = (static_cast<uint16_t>(body[off]) << 8) | body[off + 1];
                    off += 2;
                    if (off + sps_len > body.size()) break;
                    annexb.push_back(0);
                    annexb.push_back(0);
                    annexb.push_back(0);
                    annexb.push_back(1);
                    annexb.insert(annexb.end(), body.data() + off, body.data() + off + sps_len);
                    off += sps_len;
                }
                if (off < body.size()) {
                    uint8_t num_pps = body[off++];
                    for (uint8_t i = 0; i < num_pps && off + 2 <= body.size(); ++i) {
                        uint16_t pps_len = (static_cast<uint16_t>(body[off]) << 8) | body[off + 1];
                        off += 2;
                        if (off + pps_len > body.size()) break;
                        annexb.push_back(0);
                        annexb.push_back(0);
                        annexb.push_back(0);
                        annexb.push_back(1);
                        annexb.insert(annexb.end(), body.data() + off, body.data() + off + pps_len);
                        off += pps_len;
                    }
                }
                if (!annexb.empty() && decoder_) {
                    decoder_->DecodeAccessUnit(Common::ByteSpan(annexb.data(), annexb.size()), ptsUs);
                    CPS_LOG_INFO("VideoReceiver", "Processed avcC VideoConfig (", std::to_string(num_sps), " SPS extracted, ", std::to_string(annexb.size()), " B Annex-B)");
                }
            }
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
                uint64_t total = received_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (!first_frame_logged_.exchange(true)) {
                    CPS_LOG_INFO("VideoReceiver", "First video frame received (UDP) — total frames: ", std::to_string(total));
                }
            }
        }
    }
#endif
}

} // namespace CarPlayStation::Core::Media
