#include "Core/Hardware/NetworkMfiClient.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#include <thread>
#include <vector>

namespace CarPlayStation::Core::Hardware {

namespace {

#ifdef _WIN32
class WinsockGuard {
public:
    WinsockGuard() {
        WSADATA wsa_data;
        initialized_ = (WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0);
    }
    ~WinsockGuard() {
        if (initialized_) {
            WSACleanup();
        }
    }
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

private:
    bool initialized_{false};
};

static void EnsureWinsock() {
    static WinsockGuard guard;
}
#else
static void EnsureWinsock() {}
#endif

// RAII Socket wrapper to prevent handle leaks
class AutoSocket {
public:
    explicit AutoSocket(SOCKET s = INVALID_SOCKET) noexcept : sock_(s) {}
    ~AutoSocket() { Close(); }

    AutoSocket(const AutoSocket&) = delete;
    AutoSocket& operator=(const AutoSocket&) = delete;

    AutoSocket(AutoSocket&& other) noexcept : sock_(other.sock_) {
        other.sock_ = INVALID_SOCKET;
    }

    AutoSocket& operator=(AutoSocket&& other) noexcept {
        if (this != &other) {
            Close();
            sock_ = other.sock_;
            other.sock_ = INVALID_SOCKET;
        }
        return *this;
    }

    [[nodiscard]] SOCKET Get() const noexcept { return sock_; }
    [[nodiscard]] bool IsValid() const noexcept { return sock_ != INVALID_SOCKET; }

    void Close() noexcept {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET sock_{INVALID_SOCKET};
};

// Connects to a host:port with non-blocking timeout
[[nodiscard]] Common::Result<AutoSocket> ConnectWithTimeout(const std::string& host, uint16_t port, std::chrono::milliseconds timeout) {
    EnsureWinsock();

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);

    int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        return Common::Status::Error(Common::StatusCode::NetworkError, "Failed to resolve host: " + host);
    }

    SOCKET connected_sock = INVALID_SOCKET;
    for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        SOCKET s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (s == INVALID_SOCKET) {
            continue;
        }

#ifdef _WIN32
        u_long non_blocking = 1;
        ioctlsocket(s, FIONBIO, &non_blocking);
#else
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

        int connect_res = connect(s, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen));
        bool connect_success = false;

        if (connect_res == 0) {
            connect_success = true;
        } else {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
#else
            int err = errno;
            if (err == EINPROGRESS) {
#endif
                fd_set write_fds, err_fds;
                FD_ZERO(&write_fds);
                FD_ZERO(&err_fds);
                FD_SET(s, &write_fds);
                FD_SET(s, &err_fds);

                timeval tv{};
                tv.tv_sec = static_cast<long>(timeout.count() / 1000);
                tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

                int sel = select(static_cast<int>(s + 1), nullptr, &write_fds, &err_fds, &tv);
                if (sel > 0 && FD_ISSET(s, &write_fds)) {
                    int so_error = 0;
                    socklen_t len = sizeof(so_error);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
                    if (so_error == 0) {
                        connect_success = true;
                    }
                }
            }
        }

        if (connect_success) {
            connected_sock = s;
            break;
        }

        closesocket(s);
    }

    freeaddrinfo(result);

    if (connected_sock == INVALID_SOCKET) {
        return Common::Status::Error(Common::StatusCode::NetworkError, "Failed to connect to MFi daemon at " + host + ":" + port_str);
    }

    return AutoSocket(connected_sock);
}

// Bounded read covering overall wall-clock deadline
[[nodiscard]] Common::Status ReadExactByDeadline(SOCKET s, uint8_t* buf, size_t len, std::chrono::steady_clock::time_point deadline) {
    size_t total_read = 0;
    while (total_read < len) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return Common::Status::Error(Common::StatusCode::Timeout, "MFi response deadline exceeded");
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

        fd_set read_fds, err_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&err_fds);
        FD_SET(s, &read_fds);
        FD_SET(s, &err_fds);

        timeval tv{};
        tv.tv_sec = static_cast<long>(remaining.count() / 1000);
        tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);

        int sel = select(static_cast<int>(s + 1), &read_fds, nullptr, &err_fds, &tv);
        if (sel == 0) {
            return Common::Status::Error(Common::StatusCode::Timeout, "Socket read timeout");
        }
        if (sel < 0) {
            return Common::Status::Error(Common::StatusCode::NetworkError, "Socket select failed during read");
        }

        int n = recv(s, reinterpret_cast<char*>(buf + total_read), static_cast<int>(len - total_read), 0);
        if (n == 0) {
            return Common::Status::Error(Common::StatusCode::TransportDisconnected, "MFi daemon disconnected during read");
        }
        if (n < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                continue;
            }
            return Common::Status::Error(Common::StatusCode::NetworkError, "Socket recv error: " + std::to_string(err));
#else
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                continue;
            }
            return Common::Status::Error(Common::StatusCode::NetworkError, "Socket recv error: " + std::to_string(err));
#endif
        }

        total_read += static_cast<size_t>(n);
    }
    return Common::Status::Ok();
}

// Full buffer send helper
[[nodiscard]] Common::Status WriteExact(SOCKET s, const uint8_t* buf, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        int n = send(s, reinterpret_cast<const char*>(buf + total_sent), static_cast<int>(len - total_sent), 0);
        if (n <= 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            return Common::Status::Error(Common::StatusCode::NetworkError, "Failed to send data to MFi daemon: " + std::to_string(err));
#else
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            return Common::Status::Error(Common::StatusCode::NetworkError, "Failed to send data to MFi daemon: " + std::to_string(err));
#endif
        }
        total_sent += static_cast<size_t>(n);
    }
    return Common::Status::Ok();
}

} // namespace

NetworkMfiClient::NetworkMfiClient(Common::IpEndpoint endpoint, Options options)
    : endpoint_(std::move(endpoint)), options_(options) {
    EnsureWinsock();
}

NetworkMfiClient::NetworkMfiClient(std::string host, uint16_t port, Options options)
    : endpoint_{std::move(host), port}, options_(options) {
    EnsureWinsock();
}

NetworkMfiClient::~NetworkMfiClient() {
    Shutdown();
}

Common::Result<Common::ByteBuffer> NetworkMfiClient::FetchCertificate() {
    // Op 0x01 = CopyCertificate (~945 bytes payload)
    return ExecuteRequestWithRetry(0x01, {});
}

Common::Result<Common::ByteBuffer> NetworkMfiClient::SignChallenge(Common::ByteSpan challenge) {
    // Op 0x02 = CreateSignature (20-byte SHA-1 payload -> 128-byte RSA signature)
    return ExecuteRequestWithRetry(0x02, challenge);
}

MFiChipInfo NetworkMfiClient::GetChipInfo() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    MFiChipInfo info{};
    info.chip_id = 0x000200C0; // Remote MFi coprocessor profile
    info.firmware_version = "Remote-MFi-Daemon (" + endpoint_.ToString() + ")";
    info.present = IsReachable(std::chrono::milliseconds(500));
    return info;
}

void NetworkMfiClient::Shutdown() {
    is_shutdown_ = true;
}

bool NetworkMfiClient::IsReachable(std::chrono::milliseconds timeout) const {
    Common::IpEndpoint ep;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ep = endpoint_;
    }

    auto conn_res = ConnectWithTimeout(ep.address, ep.port, timeout);
    return conn_res.IsOk();
}

void NetworkMfiClient::SetEndpoint(Common::IpEndpoint endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    endpoint_ = std::move(endpoint);
}

Common::IpEndpoint NetworkMfiClient::GetEndpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return endpoint_;
}

void NetworkMfiClient::SetOptions(Options options) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_ = options;
}

NetworkMfiClient::Options NetworkMfiClient::GetOptions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

Common::Result<Common::ByteBuffer> NetworkMfiClient::ExecuteRequestWithRetry(uint8_t op, Common::ByteSpan payload) {
    if (is_shutdown_) {
        return Common::Status::Error(Common::StatusCode::TransportDisconnected, "NetworkMfiClient is shut down");
    }

    Options opts;
    Common::IpEndpoint ep;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        opts = options_;
        ep = endpoint_;
    }

    Common::Status last_error = Common::Status::Error(Common::StatusCode::UnknownError, "No attempts made");
    for (uint32_t attempt = 0; attempt < opts.max_retries; ++attempt) {
        if (is_shutdown_) {
            return Common::Status::Error(Common::StatusCode::TransportDisconnected, "NetworkMfiClient was shut down during retry");
        }

        auto res = ExecuteSingleRequest(op, payload, ep);
        if (res.IsOk()) {
            return res;
        }

        last_error = res.Error();
        if (attempt + 1 < opts.max_retries && opts.retry_delay.count() > 0) {
            std::this_thread::sleep_for(opts.retry_delay);
        }
    }
    return last_error;
}

Common::Result<Common::ByteBuffer> NetworkMfiClient::ExecuteSingleRequest(uint8_t op, Common::ByteSpan payload, const Common::IpEndpoint& ep) {
    Options opts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        opts = options_;
    }

    auto conn_res = ConnectWithTimeout(ep.address, ep.port, opts.connect_timeout);
    if (conn_res.IsError()) {
        return conn_res.Error();
    }
    AutoSocket sock = std::move(conn_res.Value());

    // Build Request: op (1 byte) | len (2 bytes BE) | payload
    if (payload.size() > 0xFFFF) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "Payload size exceeds 64KB limit");
    }

    uint16_t payload_len = static_cast<uint16_t>(payload.size());
    std::vector<uint8_t> req;
    req.reserve(3 + payload.size());
    req.push_back(op);
    req.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
    req.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    req.insert(req.end(), payload.begin(), payload.end());

    auto send_status = WriteExact(sock.Get(), req.data(), req.size());
    if (send_status.IsError()) {
        return send_status;
    }

    // Read Response: status (1 byte) | len (2 bytes BE) | payload
    auto deadline = std::chrono::steady_clock::now() + opts.response_deadline;
    uint8_t hdr[3] = {0, 0, 0};
    auto read_hdr_status = ReadExactByDeadline(sock.Get(), hdr, sizeof(hdr), deadline);
    if (read_hdr_status.IsError()) {
        return read_hdr_status;
    }

    uint8_t status_code = hdr[0];
    uint16_t resp_len = static_cast<uint16_t>((static_cast<uint16_t>(hdr[1]) << 8) | static_cast<uint16_t>(hdr[2]));

    if (status_code != 0x00) {
        // Non-zero status indicates oracle error
        return Common::Status::Error(Common::StatusCode::AuthenticationFailed,
                                     "MFi daemon returned non-zero status: " + std::to_string(status_code));
    }

    if (resp_len == 0 || resp_len > 4096) {
        return Common::Status::Error(Common::StatusCode::ProtocolError,
                                     "Invalid response payload length from MFi daemon: " + std::to_string(resp_len));
    }

    Common::ByteBuffer body(resp_len);
    auto read_body_status = ReadExactByDeadline(sock.Get(), body.data(), body.size(), deadline);
    if (read_body_status.IsError()) {
        return read_body_status;
    }

    return body;
}

} // namespace CarPlayStation::Core::Hardware
