#include "Core/Session/AirPlayRtspServer.h"
#include "Core/Common/Logging.h"
#include "Core/Hardware/IMfiSigner.h"
#include "Core/Session/Pairing/AuthSetup.h"
#include "Core/Session/Pairing/Bplist.h"
#include "Core/Session/Pairing/Crypto.h"
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>
#include <sstream>
#include <algorithm>

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

#include <chrono>
#include <memory>

namespace {

inline uint64_t NtpNow() {
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count() % 1000000000;
    uint64_t ntp_secs = static_cast<uint64_t>(secs) + 2208988800ULL;
    uint64_t ntp_frac = (static_cast<uint64_t>(nanos) << 32) / 1000000000ULL;
    return (ntp_secs << 32) | ntp_frac;
}

struct ClientSession : public std::enable_shared_from_this<ClientSession> {
    socket_t sock{INVALID_SOCK};
    std::unique_ptr<Pairing::PairSetupServer> pair_setup;
    std::unique_ptr<Pairing::PairVerifyServer> pair_verify;
    std::unique_ptr<Pairing::AuthSetupServer> auth_setup;
    std::unique_ptr<pairing::ControlChannel> control_channel;
    bool encrypted{false};
    bool pending_encryption{false};
    std::array<uint8_t, 32> shared_secret{0};

    // Phase 1 session sockets
    socket_t timing_sock{INVALID_SOCK};
    uint16_t timing_port{0};
    std::thread timing_thread;

    socket_t event_listen_sock{INVALID_SOCK};
    uint16_t event_port{0};
    socket_t event_stream_sock{INVALID_SOCK};
    std::unique_ptr<pairing::ControlChannel> event_channel;
    std::thread event_drain_thread;
    std::atomic<uint32_t> event_cseq{0};
    std::mutex event_send_mutex;

    socket_t keepalive_sock{INVALID_SOCK};
    uint16_t keepalive_port{0};
    std::thread keepalive_thread;

    std::atomic<bool> alive{true};

    ~ClientSession() {
        StopSessionSockets();
    }

    void StopSessionSockets() {
        alive.store(false);
        if (timing_sock != INVALID_SOCK) {
            CLOSE_SOCK(timing_sock);
            timing_sock = INVALID_SOCK;
        }
        if (timing_thread.joinable()) {
            timing_thread.join();
        }
        if (event_listen_sock != INVALID_SOCK) {
            CLOSE_SOCK(event_listen_sock);
            event_listen_sock = INVALID_SOCK;
        }
        {
            std::lock_guard<std::mutex> lock(event_send_mutex);
            if (event_stream_sock != INVALID_SOCK) {
                CLOSE_SOCK(event_stream_sock);
                event_stream_sock = INVALID_SOCK;
            }
            event_channel.reset();
        }
        if (event_drain_thread.joinable()) {
            event_drain_thread.join();
        }
        if (keepalive_sock != INVALID_SOCK) {
            CLOSE_SOCK(keepalive_sock);
            keepalive_sock = INVALID_SOCK;
        }
        if (keepalive_thread.joinable()) {
            keepalive_thread.join();
        }
    }

    void StartTimingResponder() {
        if (timing_sock == INVALID_SOCK) return;
        auto self = shared_from_this();
        timing_thread = std::thread([this, self]() {
            uint8_t buf[64];
            sockaddr_in peer{};
            #ifdef _WIN32
            DWORD tv = 500;
            setsockopt(timing_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
            #else
            struct timeval tv{0, 500000};
            setsockopt(timing_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
            #endif

            while (alive.load()) {
                int peer_len = sizeof(peer);
                int n = recvfrom(timing_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&peer), &peer_len);
                if (n >= 32 && buf[1] == 210) { // kRTCPTypeTimeSyncRequest
                    uint64_t t2 = NtpNow();
                    uint8_t r[32] = {0};
                    r[0] = 0x80;
                    r[1] = 211; // kRTCPTypeTimeSyncResponse
                    r[2] = 0;
                    r[3] = 7; // length = 32 / 4 - 1
                    memcpy(&r[4], &buf[4], 4); // echo rtpTimestamp
                    memcpy(&r[8], &buf[24], 8); // client T1 (transmit) -> our originate
                    Common::Endian::WriteBe64(&r[16], t2); // T2 receive
                    Common::Endian::WriteBe64(&r[24], NtpNow()); // T3 transmit
                    sendto(timing_sock, reinterpret_cast<const char*>(r), sizeof(r), 0,
                           reinterpret_cast<const sockaddr*>(&peer), peer_len);
                }
            }
        });
    }

    void StartKeepAliveDrain() {
        if (keepalive_sock == INVALID_SOCK) return;
        auto self = shared_from_this();
        keepalive_thread = std::thread([this, self]() {
            uint8_t buf[128];
            #ifdef _WIN32
            DWORD tv = 1000;
            setsockopt(keepalive_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
            #else
            struct timeval tv{1, 0};
            setsockopt(keepalive_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
            #endif
            while (alive.load()) {
                recv(keepalive_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            }
        });
    }

    bool SendEventCommand(const std::vector<uint8_t>& body_plist) {
        std::lock_guard<std::mutex> lock(event_send_mutex);
        if (event_stream_sock == INVALID_SOCK || !event_channel) {
            return false;
        }
        uint32_t cseq = ++event_cseq;
        std::string header = "POST /command RTSP/1.0\r\nCSeq: " + std::to_string(cseq) +
                             "\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: " +
                             std::to_string(body_plist.size()) + "\r\n\r\n";
        std::vector<uint8_t> msg(header.begin(), header.end());
        msg.insert(msg.end(), body_plist.begin(), body_plist.end());

        auto frame = event_channel->EncryptMessage(msg.data(), msg.size());
        size_t total_sent = 0;
        while (total_sent < frame.size()) {
            int sent = send(event_stream_sock, reinterpret_cast<const char*>(frame.data() + total_sent),
                            static_cast<int>(frame.size() - total_sent), 0);
            if (sent <= 0) return false;
            total_sent += sent;
        }
        return true;
    }

    bool HasEventChannel() {
        std::lock_guard<std::mutex> lock(event_send_mutex);
        return event_stream_sock != INVALID_SOCK && event_channel != nullptr;
    }

    bool SendHidReport(uint32_t device_uid, Common::ByteSpan report) {
        using namespace Pairing;
        BplistDict command;
        command.emplace_back("type", BplistValue("hidSendReport"));
        command.emplace_back("uuid", BplistValue(std::to_string(device_uid)));
        command.emplace_back("hidReport",
            BplistValue(std::vector<uint8_t>(report.begin(), report.end())));
        return SendEventCommand(BplistEncoder::Encode(BplistValue(std::move(command))));
    }

    bool SendRequestUI() {
        using namespace Pairing;
        BplistDict d;
        d.emplace_back("type", BplistValue("requestUI"));
        auto body = BplistEncoder::Encode(BplistValue(d));
        bool ok = SendEventCommand(body);
        CPS_LOG_INFO("AirPlayRtsp", "Sent event command: requestUI, success=", ok ? "true" : "false");
        return ok;
    }

    bool SendTakeScreen() {
        using namespace Pairing;
        BplistDict r;
        r.emplace_back("resourceID", BplistValue(1)); // MainScreen
        r.emplace_back("transferType", BplistValue(1)); // Take
        r.emplace_back("transferPriority", BplistValue(500)); // UserInitiated
        r.emplace_back("takeConstraint", BplistValue(100)); // Anytime
        r.emplace_back("borrowConstraint", BplistValue(100)); // Anytime

        BplistArray res_arr;
        res_arr.push_back(BplistValue(r));

        BplistDict params;
        params.emplace_back("resources", BplistValue(res_arr));
        params.emplace_back("reasonStr", BplistValue("video focus"));

        BplistDict d;
        d.emplace_back("type", BplistValue("changeModes"));
        d.emplace_back("params", BplistValue(params));
        auto body = BplistEncoder::Encode(BplistValue(d));
        bool ok = SendEventCommand(body);
        CPS_LOG_INFO("AirPlayRtsp", "Sent event command: takeScreen (changeModes), success=", ok ? "true" : "false");
        return ok;
    }

    bool SendForceKeyFrame() {
        using namespace Pairing;
        BplistDict d;
        d.emplace_back("type", BplistValue("forceKeyFrame"));
        auto body = BplistEncoder::Encode(BplistValue(d));
        bool ok = SendEventCommand(body);
        CPS_LOG_INFO("AirPlayRtsp", "Sent event command: forceKeyFrame, success=", ok ? "true" : "false");
        return ok;
    }

    void StartEventDrain() {
        if (event_stream_sock == INVALID_SOCK || !event_channel) return;
        auto self = shared_from_this();
        event_drain_thread = std::thread([this, self]() {
            uint8_t temp[2048];
            std::vector<uint8_t> rx;
            #ifdef _WIN32
            DWORD tv = 500;
            setsockopt(event_stream_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
            #else
            struct timeval tv{0, 500000};
            setsockopt(event_stream_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
            #endif

            while (alive.load()) {
                int n = recv(event_stream_sock, reinterpret_cast<char*>(temp), sizeof(temp), 0);
                if (n <= 0) {
                    #ifdef _WIN32
                    int err = WSAGetLastError();
                    if (err == WSAETIMEDOUT) continue;
                    #else
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    #endif
                    break;
                }
                rx.insert(rx.end(), temp, temp + n);
                while (rx.size() >= 2) {
                    size_t consumed = 0;
                    bool auth_failed = false;
                    auto pt = event_channel->DecryptFrame(rx.data(), rx.size(), consumed, auth_failed);
                    if (auth_failed || !pt) break;
                    rx.erase(rx.begin(), rx.begin() + consumed);
                    std::string first_line;
                    std::string ev_ascii;
                    for (uint8_t c : *pt) {
                        if (first_line.size() < 128 && c != '\r' && c != '\n' && ev_ascii.empty()) {
                            first_line += static_cast<char>(c);
                        }
                        if (c >= 32 && c <= 126) ev_ascii += static_cast<char>(c);
                        else ev_ascii += '.';
                    }
                    CPS_LOG_INFO("AirPlayRtsp", "Event channel received: '", first_line, "' (", std::to_string(pt->size()), " B), body=[", ev_ascii, "]");
                }
            }
            CPS_LOG_INFO("AirPlayRtsp", "Event channel reader thread finished");
        });
    }

    bool TryAcceptEventChannel(int timeout_ms = 500) {
        if (event_listen_sock == INVALID_SOCK) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(event_send_mutex);
            if (event_stream_sock != INVALID_SOCK && event_channel) {
                return true;
            }
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(event_listen_sock, &rfds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int sel = select(static_cast<int>(event_listen_sock + 1), &rfds, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(event_listen_sock, &rfds)) {
            sockaddr_in peer{};
            int peer_len = sizeof(peer);
            socket_t accepted = accept(event_listen_sock, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (accepted != INVALID_SOCK) {
                {
                    std::lock_guard<std::mutex> lock(event_send_mutex);
                    event_stream_sock = accepted;
                    auto ekeys = pairing::DeriveEventKeys(shared_secret);
                    event_channel = std::make_unique<pairing::ControlChannel>(ekeys);
                }
                CPS_LOG_INFO("AirPlayRtsp", "Event channel TCP accepted!");
                #ifdef _WIN32
                int nodelay = 1;
                setsockopt(event_stream_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
                #endif
                StartEventDrain();
                SendRequestUI();
                SendTakeScreen();
                SendForceKeyFrame();
                return true;
            }
        }
        return false;
    }
};

} // namespace

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
            return Common::MakeError(Common::StatusCode::NetworkError, "Failed to create RTSP socket");
        }

        int opt = 1;
        setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(options_.port);
        if (options_.bind_address.empty() || options_.bind_address == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            inet_pton(AF_INET, options_.bind_address.c_str(), &addr.sin_addr);
        }

        if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR) {
            CLOSE_SOCK(listen_sock_);
            listen_sock_ = INVALID_SOCK;
            return Common::MakeError(Common::StatusCode::NetworkError, "Failed to bind RTSP socket to port " + std::to_string(options_.port));
        }

        if (listen(listen_sock_, 8) == SOCK_ERR) {
            CLOSE_SOCK(listen_sock_);
            listen_sock_ = INVALID_SOCK;
            return Common::MakeError(Common::StatusCode::NetworkError, "Failed to listen on RTSP socket");
        }

        sockaddr_in bound_addr{};
        int bound_len = sizeof(bound_addr);
        if (getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&bound_addr), &bound_len) == 0) {
            bound_port_ = ntohs(bound_addr.sin_port);
        } else {
            bound_port_ = options_.port;
        }

        running_ = true;
        server_thread_ = std::thread(&Impl::AcceptLoop, this);
        CPS_LOG_INFO("AirPlayRtsp", "Server started on port ", std::to_string(bound_port_));
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

            for (socket_t sock : client_sockets_) {
                if (sock != INVALID_SOCK) {
                    CLOSE_SOCK(sock);
                }
            }
            client_sockets_.clear();
            sessions_.clear();
        }

        if (server_thread_.joinable()) {
            server_thread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& th : worker_threads_) {
                if (th.joinable()) {
                    th.join();
                }
            }
            worker_threads_.clear();
        }
        CPS_LOG_INFO("AirPlayRtsp", "Server stopped");
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

    void SetStreamingStartedCallback(StreamingStartedCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        streaming_cb_ = std::move(callback);
    }

    void SetPairSetupIdentity(Pairing::AccessoryIdentity identity, Pairing::PeerSaver saver) {
        std::lock_guard<std::mutex> lock(mutex_);
        pair_setup_enabled_ = true;
        pair_setup_identity_ = std::move(identity);
        pair_setup_saver_ = std::move(saver);
    }

    void SetPairVerify(Pairing::AccessoryIdentity identity, Pairing::PeerFinder finder) {
        std::lock_guard<std::mutex> lock(mutex_);
        pair_verify_enabled_ = true;
        pair_verify_identity_ = std::move(identity);
        pair_verify_finder_ = std::move(finder);
    }

    void SetMfiSigner(std::shared_ptr<Hardware::IMfiSigner> signer) {
        std::lock_guard<std::mutex> lock(mutex_);
        mfi_signer_ = std::move(signer);
    }

    [[nodiscard]] NegotiatedMediaParams GetNegotiatedParams() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return params_;
    }

    void SetNegotiatedParams(NegotiatedMediaParams params) {
        std::lock_guard<std::mutex> lock(mutex_);
        params_ = std::move(params);
    }

    Common::Status SendHidReport(uint32_t device_uid, Common::ByteSpan report) {
        std::vector<std::shared_ptr<ClientSession>> candidates;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            candidates.reserve(sessions_.size());
            for (const auto& [socket, session] : sessions_) {
                (void)socket;
                candidates.push_back(session);
            }
        }

        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
            if ((*it)->HasEventChannel()) {
                if ((*it)->SendHidReport(device_uid, report)) {
                    return Common::Status::Ok();
                }
                return Common::MakeError(Common::StatusCode::Unavailable,
                    "Failed to send HID report over the AirPlay event channel");
            }
        }
        return Common::MakeError(Common::StatusCode::Unavailable,
            "No active AirPlay event channel is available for HID input");
    }

    RtspResponse ProcessSimulatedRequest(const RtspRequest& req) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto dummy_sess = std::make_shared<ClientSession>();
        return HandleRequestLocked(req, dummy_sess);
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

            char ipStr[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &client_addr.sin_addr, ipStr, sizeof(ipStr));
            CPS_LOG_INFO("AirPlayRtsp", "iPhone RTSP control channel connected from ", ipStr, ":", std::to_string(ntohs(client_addr.sin_port)));

            auto sess = std::make_shared<ClientSession>();
            sess->sock = client_sock;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                client_sockets_.push_back(client_sock);
                sessions_[client_sock] = sess;
                worker_threads_.emplace_back(&Impl::ClientWorker, this, client_sock, sess);
            }
        }
    }

    void ClientWorker(socket_t sock, std::shared_ptr<ClientSession> sess) {
#ifdef _WIN32
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
#endif
        std::vector<uint8_t> rx_raw;
        rx_raw.reserve(4096);
        std::vector<uint8_t> plaintext_buffer;
        plaintext_buffer.reserve(4096);
        uint8_t temp[2048];

        while (running_) {
            int bytes_read = recv(sock, reinterpret_cast<char*>(temp), sizeof(temp), 0);
            if (bytes_read <= 0) {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (bytes_read == 0) {
                    CPS_LOG_INFO("AirPlayRtsp", "recv() returned 0 (iPhone sent TCP FIN - graceful close)");
                } else {
                    CPS_LOG_ERROR("AirPlayRtsp", "recv() error: ret=", std::to_string(bytes_read), ", WSAGetLastError=", std::to_string(err));
                }
#else
                int err = errno;
                if (bytes_read == 0) {
                    CPS_LOG_INFO("AirPlayRtsp", "recv() returned 0 (iPhone sent TCP FIN - graceful close)");
                } else {
                    CPS_LOG_ERROR("AirPlayRtsp", "recv() error: ret=", std::to_string(bytes_read), ", errno=", std::to_string(err));
                }
#endif
                break;
            }

            rx_raw.insert(rx_raw.end(), temp, temp + bytes_read);

            if (sess->encrypted && sess->control_channel) {
                while (rx_raw.size() >= 2) {
                    size_t consumed = 0;
                    bool auth_failed = false;
                    auto pt = sess->control_channel->DecryptFrame(
                        rx_raw.data(), rx_raw.size(), consumed, auth_failed);
                    if (auth_failed) {
                        CPS_LOG_ERROR("AirPlayRtsp", "Encrypted control channel authentication failed");
                        break;
                    }
                    if (!pt) {
                        // Incomplete frame, wait for more ciphertext bytes
                        break;
                    }
                    plaintext_buffer.insert(plaintext_buffer.end(), pt->begin(), pt->end());
                    rx_raw.erase(rx_raw.begin(), rx_raw.begin() + consumed);
                }
            } else {
                plaintext_buffer.insert(plaintext_buffer.end(), rx_raw.begin(), rx_raw.end());
                rx_raw.clear();
            }

            // Attempt to parse complete RTSP requests from plaintext buffer
            while (!plaintext_buffer.empty()) {
                size_t consumed = 0;
                auto parse_res = AirPlayRtspCodec::ParseRequest(plaintext_buffer, consumed);
                if (parse_res.IsError()) {
                    CPS_LOG_ERROR("AirPlayRtsp", "Failed to parse RTSP request: ", parse_res.Error().Message());
                    auto err_resp = AirPlayRtspCodec::MakeErrorResponse(RtspStatus::BadRequest);
                    auto wire_bytes = AirPlayRtspCodec::SerializeResponse(err_resp);
                    send(sock, reinterpret_cast<const char*>(wire_bytes.data()), static_cast<int>(wire_bytes.size()), 0);
                    plaintext_buffer.clear();
                    break;
                }

                if (!parse_res.Value().has_value()) {
                    // Need more bytes for a full RTSP request
                    break;
                }

                // Complete request parsed
                RtspRequest req = std::move(*parse_res.Value());
                plaintext_buffer.erase(plaintext_buffer.begin(), plaintext_buffer.begin() + consumed);

                CPS_LOG_INFO("AirPlayRtsp", "RTSP request: ", RtspMethodToString(req.method), " ", req.uri,
                             " (CSeq=", std::to_string(req.GetCSeq()), ", body_len=", std::to_string(req.body.size()), ")");
                for (const auto& [hdr_name, hdr_val] : req.headers) {
                    CPS_LOG_INFO("AirPlayRtsp", "    ", hdr_name, ": ", hdr_val);
                }

                try {
                    RtspResponse resp;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        resp = HandleRequestLocked(req, sess);
                    }

                    auto out_bytes = AirPlayRtspCodec::SerializeResponse(resp);
                    std::string header_str(reinterpret_cast<const char*>(out_bytes.data()),
                                           (std::min)(out_bytes.size(), (size_t)(out_bytes.size() - resp.body.size())));
                    CPS_LOG_INFO("AirPlayRtsp", "RTSP response wire headers:\n", header_str);

                    std::vector<uint8_t> to_send;
                    if (sess->encrypted && sess->control_channel) {
                        to_send = sess->control_channel->EncryptMessage(out_bytes.data(), out_bytes.size());
                    } else {
                        to_send = std::move(out_bytes);
                    }

                    // Send loop to guarantee all bytes are sent
                    size_t total_sent = 0;
                    while (total_sent < to_send.size()) {
                        int sent = send(sock, reinterpret_cast<const char*>(to_send.data() + total_sent),
                                        static_cast<int>(to_send.size() - total_sent), 0);
                        if (sent <= 0) {
#ifdef _WIN32
                            int send_err = WSAGetLastError();
#else
                            int send_err = errno;
#endif
                            CPS_LOG_ERROR("AirPlayRtsp", "send() failed: sent=", std::to_string(sent),
                                          ", err=", std::to_string(send_err));
                            break;
                        }
                        total_sent += sent;
                    }
                    CPS_LOG_INFO("AirPlayRtsp", "RTSP response sent: ", std::to_string(resp.status_code), " ", resp.status_phrase,
                                 " (CSeq=", std::to_string(resp.cseq), ", bytes=", std::to_string(total_sent), "/", std::to_string(to_send.size()), ")");

                    // If pair-verify just completed, flip the channel to encrypted mode NOW
                    if (sess->pending_encryption) {
                        sess->encrypted = true;
                        sess->pending_encryption = false;
                        CPS_LOG_INFO("AirPlayRtsp", "Switched RTSP control channel to ChaCha20-Poly1305 encryption");
                    }

                } catch (const std::exception& ex) {
                    CPS_LOG_ERROR("AirPlayRtsp", "Exception handling RTSP request: ", ex.what());
                    auto err_resp = AirPlayRtspCodec::MakeErrorResponse(RtspStatus::InternalServerError, req.GetCSeq());
                    auto out_bytes = AirPlayRtspCodec::SerializeResponse(err_resp);
                    send(sock, reinterpret_cast<const char*>(out_bytes.data()), static_cast<int>(out_bytes.size()), 0);
                } catch (...) {
                    CPS_LOG_ERROR("AirPlayRtsp", "Unknown exception handling RTSP request");
                }
            }
        }

        CPS_LOG_INFO("AirPlayRtsp", "iPhone RTSP connection closed");
        CLOSE_SOCK(sock);

        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(sock);
        for (auto it = client_sockets_.begin(); it != client_sockets_.end(); ++it) {
            if (*it == sock) {
                client_sockets_.erase(it);
                break;
            }
        }
    }

    RtspResponse HandleRequestLocked(const RtspRequest& req, std::shared_ptr<ClientSession> sess) {
        if (custom_handler_) {
            return custom_handler_(req);
        }

        uint32_t cseq = req.GetCSeq();
        std::string path = std::string(req.Path());

        if (!req.body.empty() && (path == "/command" || path == "/feedback" || req.method == RtspMethod::Teardown || req.method == RtspMethod::Post || req.method == RtspMethod::Setup)) {
            std::string printable;
            std::string hexDump;
            for (size_t i = 0; i < req.body.size(); ++i) {
                uint8_t b = req.body[i];
                if (b >= 32 && b <= 126) printable += static_cast<char>(b);
                else printable += '.';
                if (i < 64) {
                    char hbuf[4];
                    snprintf(hbuf, sizeof(hbuf), "%02x ", b);
                    hexDump += hbuf;
                }
            }
            CPS_LOG_INFO("AirPlayRtsp", "Request [", RtspMethodToString(req.method), " ", path, "] body len=",
                         std::to_string(req.body.size()), ", hex[:64]=[", hexDump, "], ascii=[", printable, "]");
        }

        if (path == "/pair-setup") {
            return HandlePairSetup(req, cseq, sess);
        }
        if (path == "/pair-verify") {
            return HandlePairVerify(req, cseq, sess);
        }
        if (path == "/auth-setup") {
            return HandleAuthSetup(req, cseq, sess);
        }
        if (path == "/info") {
            return HandleInfo(req, cseq);
        }
        if (path == "/command") {
            return HandleCommand(req, cseq, sess);
        }
        if (path == "/feedback" || path == "/diagInfo" || path == "/diag-info" || path == "/log" || path == "/logs") {
            auto resp = AirPlayRtspCodec::MakeOkResponse(cseq);
            resp.SetHeader("Content-Length", "0");
            return resp;
        }

        switch (req.method) {
            case RtspMethod::Options: {
                auto resp = AirPlayRtspCodec::MakeOkResponse(cseq);
                resp.SetHeader("Public", "OPTIONS, ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, GET_PARAMETER, SET_PARAMETER, POST, GET, PUT");
                return resp;
            }

            case RtspMethod::Announce: {
                if (params_.session_id.empty()) params_.session_id = "1";
                return AirPlayRtspCodec::MakeOkResponse(cseq, params_.session_id);
            }

            case RtspMethod::Setup: {
                return HandleSetup(req, cseq, sess);
            }

            case RtspMethod::Record: {
                if (params_.session_id.empty()) params_.session_id = "1";

                if (sess) {
                    sess->TryAcceptEventChannel(2000);
                }

                auto resp = AirPlayRtspCodec::MakeOkResponse(cseq, params_.session_id);
                resp.SetHeader("Audio-Latency", "11025");
                CPS_LOG_INFO("AirPlayRtsp", "RECORD received — streaming start (video data port ",
                             std::to_string(params_.video_data_port), ", control port ",
                             std::to_string(params_.video_control_port), ")");
                if (streaming_cb_) {
                    streaming_cb_(params_);
                }
                return resp;
            }

            case RtspMethod::GetParameter: {
                auto resp = AirPlayRtspCodec::MakeOkResponse(cseq);
                resp.SetHeader("Content-Type", "text/parameters");
                std::string body = "volume: 0.000000\r\n";
                resp.body.assign(body.begin(), body.end());
                return resp;
            }

            case RtspMethod::SetParameter:
            case RtspMethod::Teardown: {
                if (params_.session_id.empty()) params_.session_id = "1";
                return AirPlayRtspCodec::MakeOkResponse(cseq, params_.session_id);
            }

            default: {
                return AirPlayRtspCodec::MakeOkResponse(cseq);
            }
        }
    }

    RtspResponse HandlePairSetup(const RtspRequest& req, uint32_t cseq, std::shared_ptr<ClientSession>& sess) {
        if (!pair_setup_enabled_) {
            CPS_LOG_WARN("AirPlayRtsp", "Pair-setup requested but pair-setup is not enabled");
            return AirPlayRtspCodec::MakeErrorResponse(RtspStatus::NotFound, cseq);
        }

        if (!sess->pair_setup) {
            CPS_LOG_INFO("AirPlayRtsp", "Creating new PairSetupServer session for connection");
            sess->pair_setup = std::make_unique<Pairing::PairSetupServer>(pair_setup_identity_, pair_setup_saver_);
        }

        CPS_LOG_INFO("AirPlayRtsp", "Processing pair-setup body (len=", std::to_string(req.body.size()), ")");
        std::string req_hex;
        for (size_t i = 0; i < (std::min)(req.body.size(), (size_t)32); ++i) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", req.body[i]);
            req_hex += buf;
        }
        CPS_LOG_INFO("AirPlayRtsp", "PairSetup request body hex: ", req_hex);

        auto resp = sess->pair_setup->Handle(req.body);
        if (!resp) {
            CPS_LOG_ERROR("AirPlayRtsp", "PairSetupServer::Handle failed (fatal SRP/crypto error)");
            sess->pair_setup.reset();
            return AirPlayRtspCodec::MakeErrorResponse(RtspStatus::InternalServerError, cseq);
        }

        std::string resp_hex;
        for (size_t i = 0; i < (std::min)(resp->size(), (size_t)32); ++i) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", (*resp)[i]);
            resp_hex += buf;
        }
        CPS_LOG_INFO("AirPlayRtsp", "PairSetupServer responded with ", std::to_string(resp->size()),
                     " bytes, done=", sess->pair_setup->IsDone() ? "true" : "false", ", hex preview: ", resp_hex);
        auto out = AirPlayRtspCodec::MakeOkResponse(cseq);
        out.body = std::move(*resp);
        out.SetHeader("Content-Type", "application/octet-stream");
        if (sess->pair_setup->IsDone()) {
            CPS_LOG_INFO("AirPlayRtsp", "Pair-setup completed successfully! Device pairing recorded.");
            sess->pair_setup.reset();
        }
        return out;
    }

    RtspResponse HandlePairVerify(const RtspRequest& req, uint32_t cseq, std::shared_ptr<ClientSession>& sess) {
        if (!pair_verify_enabled_) {
            CPS_LOG_WARN("AirPlayRtsp", "Pair-verify requested but pair-verify is not enabled");
            return AirPlayRtspCodec::MakeErrorResponse(RtspStatus::NotFound, cseq);
        }

        if (!sess->pair_verify) {
            CPS_LOG_INFO("AirPlayRtsp", "Creating new PairVerifyServer session for connection");
            sess->pair_verify = std::make_unique<Pairing::PairVerifyServer>(pair_verify_identity_, pair_verify_finder_);
        }

        CPS_LOG_INFO("AirPlayRtsp", "Processing pair-verify body (len=", std::to_string(req.body.size()), ")");
        std::string req_hex;
        for (size_t i = 0; i < (std::min)(req.body.size(), (size_t)32); ++i) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", req.body[i]);
            req_hex += buf;
        }
        CPS_LOG_INFO("AirPlayRtsp", "PairVerify request body hex: ", req_hex);

        auto resp = sess->pair_verify->Handle(req.body);
        if (!resp) {
            CPS_LOG_ERROR("AirPlayRtsp", "PairVerifyServer::Handle failed (fatal auth/crypto error)");
            sess->pair_verify.reset();
            return AirPlayRtspCodec::MakeErrorResponse(RtspStatus::InternalServerError, cseq);
        }

        std::string resp_hex;
        for (size_t i = 0; i < (std::min)(resp->size(), (size_t)32); ++i) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", (*resp)[i]);
            resp_hex += buf;
        }
        CPS_LOG_INFO("AirPlayRtsp", "PairVerifyServer responded with ", std::to_string(resp->size()),
                     " bytes, done=", sess->pair_verify->IsDone() ? "true" : "false", ", hex preview: ", resp_hex);
        auto out = AirPlayRtspCodec::MakeOkResponse(cseq);
        out.body = std::move(*resp);
        out.SetHeader("Content-Type", "application/octet-stream");

        if (sess->pair_verify->IsDone()) {
            CPS_LOG_INFO("AirPlayRtsp", "Pair-verify completed successfully! Shared secret established.");
            auto secret = sess->pair_verify->GetSharedSecret();
            sess->shared_secret = secret;
            auto ckeys = pairing::DeriveControlKeys(secret);
            sess->control_channel = std::make_unique<pairing::ControlChannel>(ckeys);
            sess->pending_encryption = true;
            sess->pair_verify.reset();
        }
        return out;
    }

    RtspResponse HandleAuthSetup(const RtspRequest& req, uint32_t cseq, std::shared_ptr<ClientSession>& sess) {
        if (!mfi_signer_) {
            CPS_LOG_WARN("AirPlayRtsp", "POST /auth-setup requested but no MFi signer configured");
            return AirPlayRtspCodec::MakeErrorResponse(RtspStatus::BadRequest, cseq);
        }

        if (!sess->auth_setup) {
            CPS_LOG_INFO("AirPlayRtsp", "Creating AuthSetupServer for /auth-setup");
            sess->auth_setup = std::make_unique<Pairing::AuthSetupServer>(mfi_signer_);
        }

        auto resp_body = sess->auth_setup->Handle(req.body);
        if (!resp_body) {
            CPS_LOG_ERROR("AirPlayRtsp", "AuthSetupServer::Handle failed");
            return AirPlayRtspCodec::MakeErrorResponse(RtspStatus::InternalServerError, cseq);
        }

        auto out = AirPlayRtspCodec::MakeOkResponse(cseq);
        out.body = std::move(*resp_body);
        out.SetHeader("Content-Type", "application/octet-stream");
        return out;
    }

    RtspResponse HandleInfo(const RtspRequest& /*req*/, uint32_t cseq) {
        CPS_LOG_INFO("AirPlayRtsp", "Building /info binary plist response...");
        using namespace Pairing;

        std::string dev_id = "00:11:22:33:44:55";
        if (!pair_setup_identity_.id.empty()) {
            dev_id.assign(pair_setup_identity_.id.begin(), pair_setup_identity_.id.end());
        }

        BplistDict root;
        root.emplace_back("name", BplistValue("CarPlay"));
        root.emplace_back("model", BplistValue("CarPlayStation"));
        root.emplace_back("manufacturer", BplistValue("CarPlayStation"));
        root.emplace_back("sourceVersion", BplistValue("320.17"));
        root.emplace_back("firmwareRevision", BplistValue("1.0"));
        root.emplace_back("deviceID", BplistValue(dev_id));
        root.emplace_back("pi", BplistValue(dev_id));
        root.emplace_back("features", BplistValue(static_cast<int64_t>(0x0000006144440B80ULL)));
        root.emplace_back("statusFlags", BplistValue(static_cast<int64_t>(4)));
        root.emplace_back("keepAliveLowPower", BplistValue(true));
        root.emplace_back("keepAliveSendStatsAsBody", BplistValue(true));

        // modes
        BplistDict modes;
        BplistArray res_arr;
        auto make_res = [](int64_t rid) {
            BplistDict r;
            r.emplace_back("resourceID", BplistValue(rid));
            r.emplace_back("transferType", BplistValue(1)); // Take
            r.emplace_back("transferPriority", BplistValue(100)); // NiceToHave
            r.emplace_back("takeConstraint", BplistValue(100));
            r.emplace_back("borrowConstraint", BplistValue(100));
            return BplistValue(r);
        };
        res_arr.push_back(make_res(1)); // MainScreen
        res_arr.push_back(make_res(2)); // MainAudio
        modes.emplace_back("resources", BplistValue(res_arr));
        root.emplace_back("modes", BplistValue(modes));

        // audioLatencies
        BplistDict lat;
        lat.emplace_back("inputLatencyMicros", BplistValue(0));
        lat.emplace_back("outputLatencyMicros", BplistValue(0));
        BplistArray lat_arr;
        lat_arr.push_back(BplistValue(lat));
        root.emplace_back("audioLatencies", BplistValue(lat_arr));

        // audioFormats
        BplistArray af_arr;
        auto make_af = [](int64_t ty, int64_t inp, int64_t outp) {
            BplistDict e;
            e.emplace_back("type", BplistValue(ty));
            if (inp != 0) e.emplace_back("audioInputFormats", BplistValue(inp));
            e.emplace_back("audioOutputFormats", BplistValue(outp));
            return BplistValue(e);
        };
        af_arr.push_back(make_af(100, 0x10, 0x8000 | 0x10)); // media PCM
        af_arr.push_back(make_af(101, 0x10, 0x8000));        // alt PCM
        af_arr.push_back(make_af(102, 0, 0x00800000));       // AAC-LC
        root.emplace_back("audioFormats", BplistValue(af_arr));

        // displays
        BplistDict disp;
        disp.emplace_back("uuid", BplistValue("E0CB6FB0-0000-0000-0000-0000C0FFEE01"));
        disp.emplace_back("features", BplistValue(0x0A));
        disp.emplace_back("primaryInputDevice", BplistValue(0));
        disp.emplace_back("maxFPS", BplistValue(60));
        disp.emplace_back("widthPixels", BplistValue(static_cast<int64_t>(options_.default_video_width)));
        disp.emplace_back("heightPixels", BplistValue(static_cast<int64_t>(options_.default_video_height)));
        // widthPhysical/heightPhysical describe the physical shape of the screen.
        // The phone uses that shape to lay out the CarPlay UI, then encodes it
        // into widthPixels x heightPixels. If the two aspect ratios disagree the
        // phone's layout gets squashed into the video (with 240x90 the icons came
        // out 1.5x too tall), so keep the physical size proportional to pixels.
        const int64_t physical_w = 240;
        const int64_t physical_h = (physical_w * static_cast<int64_t>(options_.default_video_height) +
                                    static_cast<int64_t>(options_.default_video_width) / 2) /
                                   static_cast<int64_t>(options_.default_video_width);
        disp.emplace_back("widthPhysical", BplistValue(physical_w));
        disp.emplace_back("heightPhysical", BplistValue(physical_h));
        disp.emplace_back("type", BplistValue(110));
        BplistArray disp_arr;
        disp_arr.push_back(BplistValue(disp));
        root.emplace_back("displays", BplistValue(disp_arr));

        // hidDevices
        BplistArray hid_arr;
        {
            BplistDict hd;
            hd.emplace_back("uuid", BplistValue("1"));
            hd.emplace_back("name", BplistValue("CarPlay Touchscreen"));
            hd.emplace_back("displayUUID", BplistValue("E0CB6FB0-0000-0000-0000-0000C0FFEE01"));
            hd.emplace_back("hidProductID", BplistValue(1));
            hd.emplace_back("hidVendorID", BplistValue(0x05AC));
            hd.emplace_back("hidCountryCode", BplistValue(0));
            std::vector<uint8_t> desc = {
                0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01,
                0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,
                0x05, 0x0D, 0x09, 0x33, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
                0x75, 0x07, 0x95, 0x01, 0x81, 0x01,
                0x05, 0x01, 0x09, 0x30, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
                0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
                0xC0, 0xC0
            };
            uint16_t w = static_cast<uint16_t>(options_.default_video_width);
            uint16_t h = static_cast<uint16_t>(options_.default_video_height);
            desc[39] = static_cast<uint8_t>(w & 0xFF);
            desc[40] = static_cast<uint8_t>((w >> 8) & 0xFF);
            desc[52] = static_cast<uint8_t>(h & 0xFF);
            desc[53] = static_cast<uint8_t>((h >> 8) & 0xFF);
            hd.emplace_back("hidDescriptor", BplistValue(desc));
            hid_arr.push_back(BplistValue(hd));
        }
        {
            BplistDict hd;
            hd.emplace_back("uuid", BplistValue("2"));
            hd.emplace_back("name", BplistValue("CarPlay Media Buttons"));
            hd.emplace_back("displayUUID", BplistValue("E0CB6FB0-0000-0000-0000-0000C0FFEE01"));
            hd.emplace_back("hidProductID", BplistValue(2));
            hd.emplace_back("hidVendorID", BplistValue(0x05AC));
            hd.emplace_back("hidCountryCode", BplistValue(0));
            std::vector<uint8_t> desc = {
                0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
                0x15, 0x00, 0x25, 0x05, 0x05, 0x0C,
                0x0A, 0x00, 0x00,
                0x0A, 0xB0, 0x00,
                0x0A, 0xB1, 0x00,
                0x0A, 0xCD, 0x00,
                0x0A, 0xB5, 0x00,
                0x0A, 0xB6, 0x00,
                0x75, 0x08, 0x95, 0x01, 0x81, 0x00,
                0xC0
            };
            hd.emplace_back("hidDescriptor", BplistValue(desc));
            hid_arr.push_back(BplistValue(hd));
        }
        root.emplace_back("hidDevices", BplistValue(hid_arr));

        auto bplist = BplistEncoder::Encode(BplistValue(root));
        auto resp = AirPlayRtspCodec::MakeOkResponse(cseq);
        resp.SetHeader("Content-Type", "application/x-apple-binary-plist");
        resp.body = std::move(bplist);
        return resp;
    }

    RtspResponse HandleCommand(const RtspRequest& req, uint32_t cseq, std::shared_ptr<ClientSession>& sess) {
        using namespace Pairing;
        (void)sess;
        std::string cmd_type = "unknown";
        if (!req.body.empty()) {
            auto bplist = BplistDecoder::Decode(req.body.data(), req.body.size());
            if (const auto* ty = bplist.Find("type")) {
                cmd_type = ty->AsString();
            }
        }
        CPS_LOG_INFO("AirPlayRtsp", "Handling /command: type='", cmd_type, "' (body len=", std::to_string(req.body.size()), ")");

        auto resp = AirPlayRtspCodec::MakeOkResponse(cseq);
        resp.SetHeader("Content-Type", "application/x-apple-binary-plist");
        // Always respond with the standard 42-byte empty binary plist dict matching Apple CarPlay reference
        resp.body = BplistEncoder::Encode(BplistValue(BplistDict{}));
        return resp;
    }

    RtspResponse HandleSetup(const RtspRequest& req, uint32_t cseq, std::shared_ptr<ClientSession>& sess) {
        using namespace Pairing;

        if (auto it = req.headers.find("Transport"); it != req.headers.end()) {
            params_.video_data_port = 7100;
            params_.video_control_port = 7101;
            auto resp = AirPlayRtspCodec::MakeOkResponse(cseq, params_.session_id);
            if (it->second.find("control_port") != std::string::npos) {
                resp.SetHeader("Transport", it->second + ";server_port=7100;control_port=7101");
            } else {
                resp.SetHeader("Transport", "RTP/AVP/UDP;unicast;server_port=7100;control_port=7101");
            }
            return resp;
        }

        auto root = BplistDecoder::Decode(req.body.data(), req.body.size());
        const auto* streams_val = root.Find("streams");

        if (streams_val && streams_val->AsArray()) {
            CPS_LOG_INFO("AirPlayRtsp", "SETUP Phase 2 (Streams) received with ",
                         std::to_string(streams_val->AsArray()->size()), " stream(s)");

            BplistArray out_streams;
            bool has_screen = false;

            for (const auto& s_entry : *streams_val->AsArray()) {
                int64_t ty = s_entry.Find("type") ? s_entry.Find("type")->AsInt() : 0;
                uint64_t scid = s_entry.Find("streamConnectionID") ? s_entry.Find("streamConnectionID")->AsUInt() : 0;
                CPS_LOG_INFO("AirPlayRtsp", "SETUP Phase 2 stream: type=", std::to_string(ty),
                             ", streamConnectionID=", std::to_string(scid));

                if (ty == 110) { // Screen
                    has_screen = true;
                    if (scid != 0 && sess) {
                        auto skeys = pairing::DeriveStreamKeys(sess->shared_secret, scid);
                        params_.video_decrypt_key = skeys.output;
                        params_.video_encrypted = true;
                        CPS_LOG_INFO("AirPlayRtsp", "SETUP Phase 2: screen streamConnectionID=", std::to_string(scid),
                                     ", video encryption key derived successfully");
                    }

                    BplistDict stream_dict;
                    stream_dict.emplace_back("type", BplistValue(110));
                    stream_dict.emplace_back("dataPort", BplistValue(7100));
                    // Note: Per genuine Apple CarPlay / carplayd / xcertplay, Screen (110)
                    // MUST NOT include controlPort, uuid, or streamConnectionID in response!
                    out_streams.push_back(BplistValue(stream_dict));
                } else if (ty >= 100 && ty <= 102) { // Audio
                    BplistDict stream_dict;
                    stream_dict.emplace_back("type", BplistValue(ty));
                    if (scid != 0) {
                        stream_dict.emplace_back("streamConnectionID", BplistValue(scid));
                    }
                    uint16_t dport = (ty == 102) ? 7002 : (ty == 101 ? 7001 : 7000);
                    stream_dict.emplace_back("dataPort", BplistValue(dport));
                    if (ty == 102) {
                        stream_dict.emplace_back("controlPort", BplistValue(dport + 1));
                    }
                    out_streams.push_back(BplistValue(stream_dict));
                } else {
                    CPS_LOG_WARN("AirPlayRtsp", "SETUP Phase 2: unhandled stream type ", std::to_string(ty));
                }
            }

            if (out_streams.empty()) {
                // Default fallback if iPhone provided empty array
                BplistDict stream_dict;
                stream_dict.emplace_back("type", BplistValue(110));
                stream_dict.emplace_back("dataPort", BplistValue(7100));
                out_streams.push_back(BplistValue(stream_dict));
                has_screen = true;
            }

            BplistDict resp_dict;
            resp_dict.emplace_back("streams", BplistValue(out_streams));
            // In SETUP Phase 2, the root dictionary ONLY contains "streams".
            // Do NOT echo eventPort or timingPort here (they belong to Phase 1 only).

            auto bplist = BplistEncoder::Encode(BplistValue(resp_dict));
            auto resp = AirPlayRtspCodec::MakeOkResponse(cseq);
            resp.SetHeader("Content-Type", "application/x-apple-binary-plist");
            resp.body = std::move(bplist);

            params_.video_data_port = 7100;
            params_.video_control_port = 0; // Screen stream has no RTCP control port
            if (has_screen) {
                if (streaming_cb_) {
                    streaming_cb_(params_);
                }
                // NOTE: Do NOT send takeScreen here. Per the genuine Apple CarPlay SDK
                // reference (c_flow_chatty.log) and carplayd (session.rs:424-435),
                // takeScreen is sent ONCE AND ONLY ONCE at RECORD (via TryAcceptEventChannel).
                // Sending a duplicate takeScreen after SETUP Phase 2 confuses iOS's
                // FigVirtualDisplay controller, causing an immediate TEARDOWN of stream 110.
            }
            return resp;
        } else {
            CPS_LOG_INFO("AirPlayRtsp", "SETUP Phase 1 (Session) received");

            if (sess) {
                // 1. Timing UDP socket
                if (sess->timing_sock == INVALID_SOCK) {
                    sess->timing_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    sockaddr_in t_addr{};
                    t_addr.sin_family = AF_INET;
                    t_addr.sin_addr.s_addr = INADDR_ANY;
                    t_addr.sin_port = 0; // ephemeral
                    bind(sess->timing_sock, reinterpret_cast<sockaddr*>(&t_addr), sizeof(t_addr));
                    int t_len = sizeof(t_addr);
                    getsockname(sess->timing_sock, reinterpret_cast<sockaddr*>(&t_addr), &t_len);
                    sess->timing_port = ntohs(t_addr.sin_port);
                    sess->StartTimingResponder();
                }

                // 2. Event TCP listener socket
                if (sess->event_listen_sock == INVALID_SOCK) {
                    sess->event_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    int opt = 1;
                    setsockopt(sess->event_listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
                    sockaddr_in e_addr{};
                    e_addr.sin_family = AF_INET;
                    e_addr.sin_addr.s_addr = INADDR_ANY;
                    e_addr.sin_port = 0; // ephemeral
                    bind(sess->event_listen_sock, reinterpret_cast<sockaddr*>(&e_addr), sizeof(e_addr));
                    listen(sess->event_listen_sock, 4);
                    int e_len = sizeof(e_addr);
                    getsockname(sess->event_listen_sock, reinterpret_cast<sockaddr*>(&e_addr), &e_len);
                    sess->event_port = ntohs(e_addr.sin_port);
                }

                // 3. Keepalive UDP socket
                if (sess->keepalive_sock == INVALID_SOCK) {
                    sess->keepalive_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    sockaddr_in k_addr{};
                    k_addr.sin_family = AF_INET;
                    k_addr.sin_addr.s_addr = INADDR_ANY;
                    k_addr.sin_port = 0; // ephemeral
                    bind(sess->keepalive_sock, reinterpret_cast<sockaddr*>(&k_addr), sizeof(k_addr));
                    int k_len = sizeof(k_addr);
                    getsockname(sess->keepalive_sock, reinterpret_cast<sockaddr*>(&k_addr), &k_len);
                    sess->keepalive_port = ntohs(k_addr.sin_port);
                    sess->StartKeepAliveDrain();
                }
            }

            uint16_t t_port = sess ? sess->timing_port : 5001;
            uint16_t e_port = sess ? sess->event_port : 5004;
            uint16_t k_port = sess ? sess->keepalive_port : 5005;

            CPS_LOG_INFO("AirPlayRtsp", "SETUP Phase 1 response: timingPort=", std::to_string(t_port),
                         ", eventPort=", std::to_string(e_port), ", keepAlivePort=", std::to_string(k_port));

            BplistDict resp_dict;
            resp_dict.emplace_back("timingPort", BplistValue(t_port));
            resp_dict.emplace_back("eventPort", BplistValue(e_port));
            resp_dict.emplace_back("keepAlivePort", BplistValue(k_port));

            BplistArray feat_arr;
            feat_arr.push_back(BplistValue("mainBuffered"));
            feat_arr.push_back(BplistValue("videoPlayback"));
            resp_dict.emplace_back("features", BplistValue(feat_arr));

            auto bplist = BplistEncoder::Encode(BplistValue(resp_dict));
            auto resp = AirPlayRtspCodec::MakeOkResponse(cseq);
            resp.SetHeader("Content-Type", "application/x-apple-binary-plist");
            resp.body = std::move(bplist);
            return resp;
        }
    }

    Options options_;
    NegotiatedMediaParams params_;
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    socket_t listen_sock_{INVALID_SOCK};
    uint16_t bound_port_{0};
    std::vector<socket_t> client_sockets_;
    std::map<socket_t, std::shared_ptr<ClientSession>> sessions_;
    std::thread server_thread_;
    std::vector<std::thread> worker_threads_;
    RtspRequestHandler custom_handler_;
    StreamingStartedCallback streaming_cb_;

    // Pair-setup (POST /pair-setup) state.
    bool pair_setup_enabled_{false};
    Pairing::AccessoryIdentity pair_setup_identity_;
    Pairing::PeerSaver pair_setup_saver_;

    // Pair-verify (POST /pair-verify) state.
    bool pair_verify_enabled_{false};
    Pairing::AccessoryIdentity pair_verify_identity_;
    Pairing::PeerFinder pair_verify_finder_;

    // MFi signer
    std::shared_ptr<Hardware::IMfiSigner> mfi_signer_;
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

void AirPlayRtspServer::SetStreamingStartedCallback(StreamingStartedCallback callback) {
    impl_->SetStreamingStartedCallback(std::move(callback));
}

void AirPlayRtspServer::SetPairSetupIdentity(Pairing::AccessoryIdentity identity, Pairing::PeerSaver saver) {
    impl_->SetPairSetupIdentity(std::move(identity), std::move(saver));
}

void AirPlayRtspServer::SetPairVerify(Pairing::AccessoryIdentity identity, Pairing::PeerFinder finder) {
    impl_->SetPairVerify(std::move(identity), std::move(finder));
}

void AirPlayRtspServer::SetMfiSigner(std::shared_ptr<Hardware::IMfiSigner> signer) {
    impl_->SetMfiSigner(std::move(signer));
}

NegotiatedMediaParams AirPlayRtspServer::GetNegotiatedParams() const noexcept {
    return impl_->GetNegotiatedParams();
}

void AirPlayRtspServer::SetNegotiatedParams(NegotiatedMediaParams params) {
    impl_->SetNegotiatedParams(std::move(params));
}

Common::Status AirPlayRtspServer::SendHidReport(uint32_t deviceUid, Common::ByteSpan report) {
    return impl_->SendHidReport(deviceUid, report);
}

RtspResponse AirPlayRtspServer::ProcessSimulatedRequest(const RtspRequest& req) {
    return impl_->ProcessSimulatedRequest(req);
}

} // namespace CarPlayStation::Core::Session
