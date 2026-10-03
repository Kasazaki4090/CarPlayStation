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

    // Actively shut down and close all accepted client sockets so worker threads
    // unblock immediately from recv() and join cleanly without hanging.
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        for (uintptr_t s : client_sockets_) {
            if (s != ~0ULL) {
                shutdown(static_cast<SOCKET>(s), SD_BOTH);
                closesocket(static_cast<SOCKET>(s));
            }
        }
        client_sockets_.clear();
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

        // Set TCP_NODELAY to disable Nagle's algorithm for low-latency streaming
        int nodelay = 1;
        setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        // Enable TCP keep-alive
        int keepalive = 1;
        setsockopt(clientSock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepalive), sizeof(keepalive));

        // Set receive timeout (1000ms) so worker thread does not block indefinitely if connection drops
        DWORD timeoutMs = 1000;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        std::lock_guard<std::mutex> lock(client_mutex_);
        client_sockets_.push_back(static_cast<uintptr_t>(clientSock));
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

    sockaddr_in peerAddr{};
    int peerLen = sizeof(peerAddr);
    char peerIp[INET_ADDRSTRLEN] = {0};
    uint16_t peerPort = 0;
    if (getpeername(sock, reinterpret_cast<sockaddr*>(&peerAddr), &peerLen) == 0) {
        inet_ntop(AF_INET, &peerAddr.sin_addr, peerIp, sizeof(peerIp));
        peerPort = ntohs(peerAddr.sin_port);
    }
    CPS_LOG_INFO("VideoReceiver", "Worker thread started for client ", peerIp, ":", std::to_string(peerPort));

    if (decoder_ && !decoder_initialized_.exchange(true)) {
        Common::VideoStreamSpec spec;
        spec.width = 1280;
        spec.height = 720;
        spec.fps = 60;
        spec.codec = Common::VideoCodec::H264;
        auto initSt = decoder_->Init(spec);
        CPS_LOG_INFO("VideoReceiver", "Decoder initialized with 1280x720@60 H.264: ", initSt.IsOk() ? "OK" : initSt.Message());
    }

    auto cleanupSock = [&]() {
        closesocket(sock);
        std::lock_guard<std::mutex> lock(client_mutex_);
        auto it = std::find(client_sockets_.begin(), client_sockets_.end(), clientSockRaw);
        if (it != client_sockets_.end()) {
            client_sockets_.erase(it);
        }
    };

    uint64_t last_stat_time_ms = 0;
    uint64_t frames_since_stat = 0;

    while (running_.load()) {
        size_t hdrRead = 0;
        while (hdrRead < sizeof(hdr) && running_.load()) {
            int n = recv(sock, reinterpret_cast<char*>(hdr + hdrRead), static_cast<int>(sizeof(hdr) - hdrRead), 0);
            if (n <= 0) {
                int err = 0;
#ifdef _WIN32
                if (n < 0) {
                    err = WSAGetLastError();
                    if (err == WSAETIMEDOUT && running_.load()) {
                        continue;
                    }
                }
#endif
                if (n == 0) {
                    CPS_LOG_INFO("VideoReceiver", "TCP peer gracefully closed connection (recv returned 0, hdrRead=",
                                 std::to_string(hdrRead), ")");
                } else {
                    CPS_LOG_WARN("VideoReceiver", "recv(hdr) failed: code=", std::to_string(n),
                                 ", hdrRead=", std::to_string(hdrRead),
                                 ", WSAGetLastError=", std::to_string(err));
                }
                cleanupSock();
                CPS_LOG_INFO("VideoReceiver", "TCP video stream disconnected");
                return;
            }
            hdrRead += static_cast<size_t>(n);
        }

        uint32_t bodySize = Common::Endian::ReadLe32(hdr);
        uint8_t opcode = hdr[4];
        uint8_t flag0 = hdr[5];
        uint8_t flag1 = hdr[6];

        if (bodySize > 10 * 1024 * 1024) {
            CPS_LOG_WARN("VideoReceiver", "Abnormally large video payload: ", std::to_string(bodySize));
            break;
        }

        if (bodySize == 0) {
            // Keep quiet: zero-body keepalive packets arrive constantly.
            continue;
        }

        body.resize(bodySize);
        size_t bodyRead = 0;
        while (bodyRead < bodySize && running_.load()) {
            int n = recv(sock, reinterpret_cast<char*>(body.data() + bodyRead), static_cast<int>(bodySize - bodyRead), 0);
            if (n <= 0) {
                int err = 0;
#ifdef _WIN32
                if (n < 0) {
                    err = WSAGetLastError();
                    if (err == WSAETIMEDOUT && running_.load()) {
                        continue;
                    }
                }
#endif
                CPS_LOG_DEBUG("VideoReceiver", "recv(body) returned ", std::to_string(n),
                              ", bodyRead=", std::to_string(bodyRead), "/", std::to_string(bodySize),
                              ", WSAGetLastError=", std::to_string(err));
                cleanupSock();
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
                    CPS_LOG_WARN("VideoReceiver", "Video frame ChaCha20 decrypt failed at frame ", std::to_string(cnt),
                                 ", ciphertext size=", std::to_string(body.size()));
                    continue;
                }
                if (cnt < 2) {
                    CPS_LOG_INFO("VideoReceiver", "Video frame #", std::to_string(cnt),
                                 " decrypted: sealed=", std::to_string(body.size()),
                                 " plain=", std::to_string(plain.size()));
                }
            } else {
                plain = std::move(body);
            }

            // Convert AVCC length-prefixed NALUs to Annex-B (00 00 00 01 prefix)
            std::vector<uint8_t> annexb;
            annexb.reserve(plain.size() + 16);
            size_t off = 0;
            size_t nalu_count = 0;
            std::string nalu_types_str;
            bool has_idr = false;

            while (off + 4 <= plain.size()) {
                uint32_t naluLen = Common::Endian::ReadBe32(plain.data() + off);
                off += 4;
                if (naluLen == 0 || off + naluLen > plain.size()) {
                    break;
                }
                uint8_t nal_type = plain[off] & 0x1F;
                if (nal_type == 5) has_idr = true;
                if (!nalu_types_str.empty()) nalu_types_str += ",";
                nalu_types_str += std::to_string(nal_type);

                annexb.push_back(0);
                annexb.push_back(0);
                annexb.push_back(0);
                annexb.push_back(1);
                annexb.insert(annexb.end(), plain.data() + off, plain.data() + off + naluLen);
                off += naluLen;
                nalu_count++;
            }

            if (!annexb.empty() && decoder_) {
                auto decSt = decoder_->DecodeAccessUnit(Common::ByteSpan(annexb.data(), annexb.size()), ptsUs);
                if (decSt.IsOk()) {
                    uint64_t total = received_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
                    frames_since_stat++;
                    if (!first_frame_logged_.exchange(true) || has_idr) {
                        CPS_LOG_INFO("VideoReceiver", "Video frame #", std::to_string(total),
                                     (has_idr ? " [KEYFRAME IDR]" : ""), " decoded: ",
                                     std::to_string(annexb.size()), " B, ",
                                     std::to_string(nalu_count), " NALUs (types: ", nalu_types_str, ")");
                    }

                    uint64_t now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                    if (now_ms - last_stat_time_ms >= 1000) {
                        double fps = (last_stat_time_ms > 0) ? (frames_since_stat * 1000.0 / (now_ms - last_stat_time_ms)) : 0.0;
                        CPS_LOG_INFO("VideoReceiver", "Video stream running: total frames=", std::to_string(total),
                                     ", rate=", std::to_string(static_cast<int>(fps + 0.5)), " fps");
                        last_stat_time_ms = now_ms;
                        frames_since_stat = 0;
                    }
                } else {
                    CPS_LOG_WARN("VideoReceiver", "DecodeAccessUnit error: ", decSt.Message());
                }
            }
        } else if (opcode == 1) { // VideoConfig (avcC / hvcC)
            CPS_LOG_INFO("VideoReceiver", "Processing VideoConfig packet (", std::to_string(body.size()),
                         " B, flags=[0x", [&]() { char f[16]; snprintf(f, sizeof(f), "%02X,0x%02X", flag0, flag1); return std::string(f); }(), "])");

            bool is_hevc = (flag1 & 0x08) != 0;
            size_t hvcc_pos = std::string::npos;
            size_t avcc_pos = std::string::npos;
            for (size_t i = 0; i + 4 <= body.size(); ++i) {
                if (memcmp(body.data() + i, "hvcC", 4) == 0) {
                    hvcc_pos = i + 4;
                    is_hevc = true;
                    break;
                }
                if (memcmp(body.data() + i, "avcC", 4) == 0) {
                    avcc_pos = i + 4;
                }
            }

            if (is_hevc && hvcc_pos != std::string::npos && hvcc_pos + 23 <= body.size()) {
                const uint8_t* rec = body.data() + hvcc_pos;
                size_t rec_len = body.size() - hvcc_pos;
                size_t num_arrays = rec[22];
                size_t off = 23;
                std::vector<uint8_t> annexb;
                for (size_t a = 0; a < num_arrays && off + 3 <= rec_len; ++a) {
                    uint16_t num_nalus = (static_cast<uint16_t>(rec[off + 1]) << 8) | rec[off + 2];
                    off += 3;
                    for (uint16_t n = 0; n < num_nalus && off + 2 <= rec_len; ++n) {
                        uint16_t nlen = (static_cast<uint16_t>(rec[off]) << 8) | rec[off + 1];
                        off += 2;
                        if (off + nlen > rec_len) break;
                        annexb.push_back(0); annexb.push_back(0); annexb.push_back(0); annexb.push_back(1);
                        annexb.insert(annexb.end(), rec + off, rec + off + nlen);
                        off += nlen;
                    }
                }
                if (!annexb.empty() && decoder_) {
                    decoder_->DecodeAccessUnit(Common::ByteSpan(annexb.data(), annexb.size()), ptsUs);
                    CPS_LOG_INFO("VideoReceiver", "Processed hvcC HEVC VideoConfig: ",
                                 std::to_string(annexb.size()), " B Annex-B fed to decoder");
                }
            } else {
                size_t cfg_offset = (avcc_pos != std::string::npos) ? avcc_pos : 0;
                const uint8_t* cfg = body.data() + cfg_offset;
                size_t cfg_len = body.size() - cfg_offset;

                if (cfg_len >= 6) {
                    uint8_t num_sps = cfg[5] & 0x1F;
                    size_t off = 6;
                    std::vector<uint8_t> annexb;
                    for (uint8_t i = 0; i < num_sps && off + 2 <= cfg_len; ++i) {
                        uint16_t sps_len = (static_cast<uint16_t>(cfg[off]) << 8) | cfg[off + 1];
                        off += 2;
                        if (off + sps_len > cfg_len) break;
                        annexb.push_back(0); annexb.push_back(0); annexb.push_back(0); annexb.push_back(1);
                        annexb.insert(annexb.end(), cfg + off, cfg + off + sps_len);
                        off += sps_len;
                    }
                    if (off < cfg_len) {
                        uint8_t num_pps = cfg[off++];
                        for (uint8_t i = 0; i < num_pps && off + 2 <= cfg_len; ++i) {
                            uint16_t pps_len = (static_cast<uint16_t>(cfg[off]) << 8) | cfg[off + 1];
                            off += 2;
                            if (off + pps_len > cfg_len) break;
                            annexb.push_back(0); annexb.push_back(0); annexb.push_back(0); annexb.push_back(1);
                            annexb.insert(annexb.end(), cfg + off, cfg + off + pps_len);
                            off += pps_len;
                        }
                    }
                    if (!annexb.empty() && decoder_) {
                        decoder_->DecodeAccessUnit(Common::ByteSpan(annexb.data(), annexb.size()), ptsUs);
                        CPS_LOG_INFO("VideoReceiver", "Processed avcC H.264 VideoConfig (", std::to_string(num_sps),
                                     " SPS, ", std::to_string(annexb.size()), " B Annex-B fed to decoder)");
                    }
                } else {
                    CPS_LOG_WARN("VideoReceiver", "VideoConfig payload too small: ", std::to_string(cfg_len), " B");
                }
            }
        } else {
            CPS_LOG_INFO("VideoReceiver", "Unknown packet opcode: ", std::to_string(opcode),
                         ", bodySize=", std::to_string(bodySize));
        }
    }

    cleanupSock();
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
