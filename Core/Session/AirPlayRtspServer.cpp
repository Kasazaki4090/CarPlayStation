#include "Core/Session/AirPlayRtspServer.h"
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
constexpr int SOCK_ERR = SOCKET_ERROR;
#define CLOSE_SOCK(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;
constexpr int SOCK_ERR = -1;
#define CLOSE_SOCK(s) close(s)
#endif

namespace CarPlayStation::Core::Session {

class AirPlayRtspServer::Impl {
public:
    explicit Impl(Options options) : options_(std::move(options)) {
        params_.video_width = options_.default_video_width;
        params_.video_height = options_.default_video_height;
        params_.fps = options_.default_fps;
        params_.session_id = "12345678";
    }

    ~Impl() {
        Stop();
    }

    Common::Status Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return Common::Status::Ok();
        }

#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return Common::MakeError(Common::StatusCode::Unavailable, "WSAStartup failed");
        }
#endif

        listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock_ == INVALID_SOCK) {
            return Common::MakeError(Common::StatusCode::IoError, "Failed to create TCP socket");
        }

        int opt = 1;
        setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(options_.port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR) {
            CLOSE_SOCK(listen_sock_);
            listen_sock_ = INVALID_SOCK;
            return Common::MakeError(Common::StatusCode::IoError, "Failed to bind RTSP TCP port");
        }

        sockaddr_in bound_addr{};
        int bound_len = sizeof(bound_addr);
        if (getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&bound_addr), &bound_len) == 0) {
            bound_port_ = ntohs(bound_addr.sin_port);
        } else {
            bound_port_ = options_.port;
        }

        if (listen(listen_sock_, SOMAXCONN) == SOCK_ERR) {
            CLOSE_SOCK(listen_sock_);
            listen_sock_ = INVALID_SOCK;
            return Common::MakeError(Common::StatusCode::IoError, "Failed to listen on RTSP socket");
        }

        running_ = true;
        server_thread_ = std::thread(&Impl::AcceptLoop, this);
        return Common::Status::Ok();
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            running_ = false;

            if (listen_sock_ != INVALID_SOCK) {
                CLOSE_SOCK(listen_sock_);
                listen_sock_ = INVALID_SOCK;
            }

            for (socket_t client : client_sockets_) {
                if (client != INVALID_SOCK) {
                    CLOSE_SOCK(client);
                }
            }
            client_sockets_.clear();
        }

        if (server_thread_.joinable()) {
            server_thread_.join();
        }

        for (auto& th : worker_threads_) {
            if (th.joinable()) {
                th.join();
            }
        }
        worker_threads_.clear();
    }

    [[nodiscard]] bool IsRunning() const noexcept {
        return running_;
    }

    [[nodiscard]] uint16_t GetBoundPort() const noexcept {
        return bound_port_;
    }

    [[nodiscard]] size_t GetActiveClientCount() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return client_sockets_.size();
    }

    void SetRequestHandler(RtspRequestHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        custom_handler_ = std::move(handler);
    }

    [[nodiscard]] NegotiatedMediaParams GetNegotiatedParams() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return params_;
    }

    void SetNegotiatedParams(NegotiatedMediaParams params) {
        std::lock_guard<std::mutex> lock(mutex_);
        params_ = std::move(params);
    }

    RtspResponse ProcessSimulatedRequest(const RtspRequest& req) {
        std::lock_guard<std::mutex> lock(mutex_);
        return HandleRequestLocked(req);
    }

private:
    void AcceptLoop() {
        while (running_) {
            sockaddr_in client_addr{};
            int client_len = sizeof(client_addr);
            socket_t client_sock = accept(listen_sock_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_sock == INVALID_SOCK) {
                if (!running_) break;
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                client_sockets_.push_back(client_sock);
                worker_threads_.emplace_back(&Impl::ClientWorker, this, client_sock);
            }
        }
    }

    void ClientWorker(socket_t sock) {
        std::vector<uint8_t> rx_buffer;
        rx_buffer.reserve(4096);
        uint8_t temp[2048];

        while (running_) {
            int bytes_read = recv(sock, reinterpret_cast<char*>(temp), sizeof(temp), 0);
            if (bytes_read <= 0) {
                break;
            }

            rx_buffer.insert(rx_buffer.end(), temp, temp + bytes_read);

            // Attempt to parse complete RTSP requests from buffer
            while (!rx_buffer.empty()) {
                size_t consumed = 0;
                auto parse_res = AirPlayRtspCodec::ParseRequest(rx_buffer, consumed);
                if (parse_res.IsError()) {
                    // Send 400 Bad Request and close
                    auto err_resp = AirPlayRtspCodec::MakeErrorResponse(RtspStatus::BadRequest);
                    auto wire_bytes = AirPlayRtspCodec::SerializeResponse(err_resp);
                    send(sock, reinterpret_cast<const char*>(wire_bytes.data()), static_cast<int>(wire_bytes.size()), 0);
                    rx_buffer.clear();
                    break;
                }

                if (!parse_res.Value().has_value()) {
                    // Need more bytes
                    break;
                }

                // Complete request parsed
                RtspRequest req = std::move(*parse_res.Value());
                rx_buffer.erase(rx_buffer.begin(), rx_buffer.begin() + consumed);

                RtspResponse resp;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    resp = HandleRequestLocked(req);
                }

                auto out_bytes = AirPlayRtspCodec::SerializeResponse(resp);
                send(sock, reinterpret_cast<const char*>(out_bytes.data()), static_cast<int>(out_bytes.size()), 0);

                if (req.method == RtspMethod::Teardown) {
                    break;
                }
            }
        }

        CLOSE_SOCK(sock);

        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = client_sockets_.begin(); it != client_sockets_.end(); ++it) {
            if (*it == sock) {
                client_sockets_.erase(it);
                break;
            }
        }
    }

    RtspResponse HandleRequestLocked(const RtspRequest& req) {
        if (custom_handler_) {
            return custom_handler_(req);
        }

        uint32_t cseq = req.GetCSeq();

        switch (req.method) {
            case RtspMethod::Options: {
                auto resp = AirPlayRtspCodec::MakeOkResponse(cseq);
                resp.SetHeader("Public", "OPTIONS, ANNOUNCE, SETUP, RECORD, SET_PARAMETER, TEARDOWN");
                return resp;
            }

            case RtspMethod::Announce: {
                auto resp = AirPlayRtspCodec::MakeOkResponse(cseq, params_.session_id);
                return resp;
            }

            case RtspMethod::Setup: {
                auto resp = AirPlayRtspCodec::MakeOkResponse(cseq, params_.session_id);
                params_.video_data_port = 5000;
                params_.video_control_port = 5001;
                std::string transport_hdr = std::string(req.GetHeader("Transport"));
                if (transport_hdr.find("timing_port") != std::string::npos ||
                    transport_hdr.find("client_port") != std::string::npos) {
                    resp.SetHeader("Transport", transport_hdr + ";server_port=5000;control_port=5001");
                } else {
                    resp.SetHeader("Transport", "RTP/AVP/UDP;unicast;server_port=5000;control_port=5001");
                }
                return resp;
            }

            case RtspMethod::Record: {
                auto resp = AirPlayRtspCodec::MakeOkResponse(cseq, params_.session_id);
                resp.SetHeader("Audio-Latency", "11025");
                return resp;
            }

            case RtspMethod::SetParameter: {
                return AirPlayRtspCodec::MakeOkResponse(cseq, params_.session_id);
            }

            case RtspMethod::Teardown: {
                return AirPlayRtspCodec::MakeOkResponse(cseq, params_.session_id);
            }

            default: {
                return AirPlayRtspCodec::MakeErrorResponse(RtspStatus::MethodNotAllowed, cseq);
            }
        }
    }

    Options options_;
    NegotiatedMediaParams params_;
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    socket_t listen_sock_{INVALID_SOCK};
    uint16_t bound_port_{0};
    std::vector<socket_t> client_sockets_;
    std::thread server_thread_;
    std::vector<std::thread> worker_threads_;
    RtspRequestHandler custom_handler_;
};

AirPlayRtspServer::AirPlayRtspServer(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

AirPlayRtspServer::~AirPlayRtspServer() = default;

Common::Status AirPlayRtspServer::Start() {
    return impl_->Start();
}

void AirPlayRtspServer::Stop() {
    impl_->Stop();
}

bool AirPlayRtspServer::IsRunning() const noexcept {
    return impl_->IsRunning();
}

uint16_t AirPlayRtspServer::GetBoundPort() const noexcept {
    return impl_->GetBoundPort();
}

size_t AirPlayRtspServer::GetActiveClientCount() const noexcept {
    return impl_->GetActiveClientCount();
}

void AirPlayRtspServer::SetRequestHandler(RtspRequestHandler handler) {
    impl_->SetRequestHandler(std::move(handler));
}

NegotiatedMediaParams AirPlayRtspServer::GetNegotiatedParams() const noexcept {
    return impl_->GetNegotiatedParams();
}

void AirPlayRtspServer::SetNegotiatedParams(NegotiatedMediaParams params) {
    impl_->SetNegotiatedParams(std::move(params));
}

RtspResponse AirPlayRtspServer::ProcessSimulatedRequest(const RtspRequest& req) {
    return impl_->ProcessSimulatedRequest(req);
}

} // namespace CarPlayStation::Core::Session
