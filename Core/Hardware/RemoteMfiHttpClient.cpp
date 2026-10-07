#include "Core/Hardware/RemoteMfiHttpClient.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <chrono>
#include <thread>

#include "Core/Common/Logging.h"

namespace CarPlayStation::Core::Hardware {

namespace {

constexpr std::string_view kLogTag = "RemoteMfi";
constexpr std::size_t kMaxResponseBytes = 2u * 1024u * 1024u;
constexpr std::size_t kMaxPayloadBytes = 65525u;

#ifdef _WIN32
using SocketType = SOCKET;
constexpr SocketType kInvalidSocket = INVALID_SOCKET;

void CloseSocket(SocketType s) { closesocket(s); }

struct WinsockGuard {
    WinsockGuard() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockGuard() { WSACleanup(); }
};
#else
using SocketType = int;
constexpr SocketType kInvalidSocket = -1;
void CloseSocket(SocketType s) { close(s); }
struct WinsockGuard {};
#endif

void EnsureWinsock() {
    static WinsockGuard guard;
}

void SetSocketTimeouts(SocketType s, std::chrono::milliseconds send_to,
                       std::chrono::milliseconds recv_to) {
#ifdef _WIN32
    DWORD snd = static_cast<DWORD>(send_to.count());
    DWORD rcv = static_cast<DWORD>(recv_to.count());
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&snd), sizeof(snd));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&rcv), sizeof(rcv));
#else
    timeval snd{static_cast<time_t>(send_to.count() / 1000),
                static_cast<suseconds_t>((send_to.count() % 1000) * 1000)};
    timeval rcv{static_cast<time_t>(recv_to.count() / 1000),
                static_cast<suseconds_t>((recv_to.count() % 1000) * 1000)};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
#endif
}

// ---------------------------------------------------------------------------
// base64 (RFC 4648, standard alphabet, '=' padding) -?the wire format used by
// the remote-mfi HTTP API for certificate and signature bytes.
// ---------------------------------------------------------------------------

constexpr char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(Common::ByteSpan data) {
    std::string out;
    out.reserve(((data.size() + 2u) / 3u) * 4u);
    std::size_t i = 0;
    while (i + 3u <= data.size()) {
        const unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                           (static_cast<unsigned>(data[i + 1]) << 8) |
                           static_cast<unsigned>(data[i + 2]);
        out.push_back(kB64Alphabet[(v >> 18) & 0x3Fu]);
        out.push_back(kB64Alphabet[(v >> 12) & 0x3Fu]);
        out.push_back(kB64Alphabet[(v >> 6) & 0x3Fu]);
        out.push_back(kB64Alphabet[v & 0x3Fu]);
        i += 3u;
    }
    const std::size_t rem = data.size() - i;
    if (rem == 1u) {
        const unsigned v = static_cast<unsigned>(data[i]) << 16;
        out.push_back(kB64Alphabet[(v >> 18) & 0x3Fu]);
        out.push_back(kB64Alphabet[(v >> 12) & 0x3Fu]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2u) {
        const unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                           (static_cast<unsigned>(data[i + 1]) << 8);
        out.push_back(kB64Alphabet[(v >> 18) & 0x3Fu]);
        out.push_back(kB64Alphabet[(v >> 12) & 0x3Fu]);
        out.push_back(kB64Alphabet[(v >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool Base64Decode(std::string_view text, Common::ByteBuffer& out) {
    out.clear();
    unsigned acc = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        const int v = Base64Value(c);
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Minimal flat-JSON reader. The API responses are single-level objects with
// string/number/boolean members, so a small scanner is enough; no JSON library
// is vendored in this project.
// ---------------------------------------------------------------------------

void SkipWs(std::string_view s, std::size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
}

bool ParseJsonString(std::string_view s, std::size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') {
        return false;
    }
    ++i;
    out.clear();
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            ++i;
            return true;
        }
        if (c == '\\') {
            if (i + 1u >= s.size()) {
                return false;
            }
            const char e = s[i + 1];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i + 5u >= s.size()) {
                        return false;
                    }
                    int code = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[i + 2u + static_cast<std::size_t>(k)];
                        if (h >= '0' && h <= '9') {
                            code = code * 16 + (h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            code = code * 16 + (h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            code = code * 16 + (h - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    // UTF-8 encode the BMP code point.
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(
                            static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    i += 5u;
                    continue;
                }
                default:
                    return false;
            }
            i += 2u;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return false;
}

// Skip any JSON value starting at i (used to walk members we do not care about).
bool SkipJsonValue(std::string_view s, std::size_t& i) {
    SkipWs(s, i);
    if (i >= s.size()) {
        return false;
    }
    if (s[i] == '"') {
        std::string tmp;
        return ParseJsonString(s, i, tmp);
    }
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        bool in_string = false;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (in_string) {
                if (c == '\\') {
                    ++i;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                --depth;
                if (depth == 0) {
                    ++i;
                    return true;
                }
            }
        }
        return false;
    }
    // number / true / false / null: scan to delimiter.
    const std::size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           !std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    return i > start;
}

// Find a top-level member by name and parse its value as a string.
bool FindJsonString(std::string_view json, std::string_view name,
                    std::string& out) {
    std::size_t i = 0;
    SkipWs(json, i);
    if (i >= json.size() || json[i] != '{') {
        return false;
    }
    ++i;
    while (i < json.size()) {
        SkipWs(json, i);
        if (i >= json.size()) {
            return false;
        }
        if (json[i] == '}') {
            return false;
        }
        std::string key;
        if (!ParseJsonString(json, i, key)) {
            return false;
        }
        SkipWs(json, i);
        if (i >= json.size() || json[i] != ':') {
            return false;
        }
        ++i;
        SkipWs(json, i);
        if (key == name) {
            return ParseJsonString(json, i, out);
        }
        if (!SkipJsonValue(json, i)) {
            return false;
        }
        SkipWs(json, i);
        if (i < json.size() && json[i] == ',') {
            ++i;
        }
    }
    return false;
}

std::string TrimAscii(std::string_view s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    std::string str(s);
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), not_space));
    str.erase(std::find_if(str.rbegin(), str.rend(), not_space).base(), str.end());
    return str;
}

// Find a top-level member by name and return its raw (unquoted) token. Used for
// the number/boolean members of the API responses.
bool FindJsonToken(std::string_view json, std::string_view name,
                   std::string& out) {
    std::size_t i = 0;
    SkipWs(json, i);
    if (i >= json.size() || json[i] != '{') {
        return false;
    }
    ++i;
    while (i < json.size()) {
        SkipWs(json, i);
        if (i >= json.size() || json[i] == '}') {
            return false;
        }
        std::string key;
        if (!ParseJsonString(json, i, key)) {
            return false;
        }
        SkipWs(json, i);
        if (i >= json.size() || json[i] != ':') {
            return false;
        }
        ++i;
        SkipWs(json, i);
        if (key == name) {
            const std::size_t start = i;
            while (i < json.size() && json[i] != ',' && json[i] != '}' &&
                   json[i] != ']' &&
                   !std::isspace(static_cast<unsigned char>(json[i]))) {
                ++i;
            }
            out = TrimAscii(json.substr(start, i - start));
            return !out.empty();
        }
        if (!SkipJsonValue(json, i)) {
            return false;
        }
        SkipWs(json, i);
        if (i < json.size() && json[i] == ',') {
            ++i;
        }
    }
    return false;
}

std::string NewRequestId() {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    uint8_t b[16];
    const uint64_t lo = dist(rng);
    const uint64_t hi = dist(rng);
    std::memcpy(b, &lo, 8);
    std::memcpy(b + 8, &hi, 8);
    b[6] = static_cast<uint8_t>((b[6] & 0x0Fu) | 0x40u);  // version 4
    b[8] = static_cast<uint8_t>((b[8] & 0x3Fu) | 0x80u);  // variant 10
    char out[37];
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                  b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(out, 36);
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RemoteMfiHttpClient::RemoteMfiHttpClient(Options options)
    : options_(std::move(options)) {}

RemoteMfiHttpClient::~RemoteMfiHttpClient() {
    Shutdown();
}

void RemoteMfiHttpClient::SetOptions(const Options& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_ = options;
    InvalidateCacheLocked();
}

RemoteMfiHttpClient::Options RemoteMfiHttpClient::GetOptions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

void RemoteMfiHttpClient::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
}

bool RemoteMfiHttpClient::IsShutdown() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_;
}

void RemoteMfiHttpClient::InvalidateCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    InvalidateCacheLocked();
}

void RemoteMfiHttpClient::InvalidateCacheLocked() {
    cert_cache_.clear();
    chip_info_ = MFiChipInfo{};
    protocol_major_ = 0;
}

// ---------------------------------------------------------------------------
// HTTP transport
// ---------------------------------------------------------------------------

Common::Result<RemoteMfiHttpClient::HttpResponse> RemoteMfiHttpClient::Request(
    const Options& opt, const std::string& method, const std::string& path,
    const std::string& body) const {
    EnsureWinsock();

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port_str = std::to_string(opt.port);
    struct addrinfo* results = nullptr;
    const int rc = getaddrinfo(opt.host.c_str(), port_str.c_str(), &hints,
                               &results);
    if (rc != 0 || results == nullptr) {
        return Common::Status::Error(Common::StatusCode::NetworkError,
                              "remote-mfi: cannot resolve " + opt.host);
    }

    SocketType sock = kInvalidSocket;
    for (struct addrinfo* p = results; p; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == kInvalidSocket) {
            continue;
        }
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
        const int connect_rc = connect(sock, p->ai_addr,
                                       static_cast<int>(p->ai_addrlen));
        bool connected = (connect_rc == 0);
        if (!connected &&
            WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            const long ms = static_cast<long>(opt.connect_timeout.count());
            timeval tv{ms / 1000, (ms % 1000) * 1000};
            if (select(0, nullptr, &wfds, nullptr, &tv) == 1) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(sock, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&err), &len);
                connected = (err == 0);
            }
        }
#else
        const int connect_rc = connect(sock, p->ai_addr, p->ai_addrlen);
        const bool connected = (connect_rc == 0);
#endif
        if (connected) {
            break;
        }
        CloseSocket(sock);
        sock = kInvalidSocket;
    }
    freeaddrinfo(results);

    if (sock == kInvalidSocket) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
                              "remote-mfi: cannot connect to " + opt.host +
                                  ":" + port_str);
    }

#ifdef _WIN32
    u_long mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
#endif
    SetSocketTimeouts(sock, opt.send_timeout, opt.receive_timeout);

    // HTTP/1.1 with Connection: close, so the response is delimited by the
    // server closing the socket and no chunked-transfer decoding is needed.
    std::string request = method + " " + path + " HTTP/1.1\r\n";
    request += "Host: " + opt.host + ":" + port_str + "\r\n";
    request += "User-Agent: carplaystation/1.0\r\n";
    request += "Accept: application/json\r\n";
    if (!opt.bearer_token.empty()) {
        request += "Authorization: Bearer " + opt.bearer_token + "\r\n";
    }
    if (!body.empty()) {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    request += body;

    std::size_t sent = 0;
    while (sent < request.size()) {
        const int n = send(sock, request.data() + sent,
                           static_cast<int>(request.size() - sent), 0);
        if (n <= 0) {
            CloseSocket(sock);
            return Common::Status::Error(Common::StatusCode::NetworkError,
                                  "remote-mfi: send failed");
        }
        sent += static_cast<std::size_t>(n);
    }

    std::string raw;
    char buffer[4096];
    for (;;) {
        const int n = recv(sock, buffer, sizeof(buffer), 0);
        if (n > 0) {
            raw.append(buffer, static_cast<std::size_t>(n));
            if (raw.size() > kMaxResponseBytes) {
                CloseSocket(sock);
                return Common::Status::Error(Common::StatusCode::ProtocolError,
                                     "remote-mfi: response exceeds 2 MiB");
            }
            continue;
        }
        if (n == 0) {
            break;  // server closed the connection: the response is complete
        }
        CloseSocket(sock);
        return Common::Status::Error(Common::StatusCode::Timeout,
                             "remote-mfi: receive timed out on " + method +
                                 " " + path);
    }
    CloseSocket(sock);

    const std::string head_end = "\r\n\r\n";
    const std::size_t sep = raw.find(head_end);
    if (sep == std::string::npos) {
        return Common::Status::Error(Common::StatusCode::ProtocolError,
                              "remote-mfi: malformed HTTP response");
    }
    const std::string head = raw.substr(0, sep);
    HttpResponse resp;
    resp.body = raw.substr(sep + head_end.size());

    const std::size_t status_line_end = head.find("\r\n");
    const std::string status_line = head.substr(
        0, status_line_end == std::string::npos ? head.size() : status_line_end);
    if (status_line.compare(0, 5, "HTTP/") != 0) {
        return Common::Status::Error(Common::StatusCode::ProtocolError,
                              "remote-mfi: not an HTTP response");
    }
    const std::size_t sp1 = status_line.find(' ');
    if (sp1 == std::string::npos) {
        return Common::Status::Error(Common::StatusCode::ProtocolError,
                              "remote-mfi: malformed status line");
    }
    resp.status = std::atoi(status_line.c_str() + sp1 + 1);

    // If the server sent Content-Length, wait for the body to match it before
    // trusting a truncated read.
    const std::size_t cl_pos = head.find("Content-Length:");
    if (cl_pos != std::string::npos) {
        const std::size_t expected = static_cast<std::size_t>(
            std::strtoul(head.c_str() + cl_pos + 15, nullptr, 10));
        if (resp.body.size() < expected) {
            return Common::Status::Error(Common::StatusCode::ProtocolError,
                                  "remote-mfi: truncated response body");
        }
    }
    return resp;
}

Common::Status RemoteMfiHttpClient::ErrorFromResponse(
    const HttpResponse& resp) {
    std::string detail;
    if (!FindJsonString(resp.body, "detail", detail) || detail.empty()) {
        detail = "HTTP " + std::to_string(resp.status);
    }
    switch (resp.status) {
        case 400:
            return Common::Status::Error(Common::StatusCode::InvalidArgument, detail);
        case 401:
            return Common::Status::Error(Common::StatusCode::AuthenticationFailed,
                                  detail);
        case 404:
            return Common::Status::Error(Common::StatusCode::NotFound, detail);
        case 503:
            return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
                                  detail);
        case 500:
            return Common::Status::Error(Common::StatusCode::HardwareError, detail);
        default:
            return Common::Status::Error(Common::StatusCode::NetworkError, detail);
    }
}

Common::Result<RemoteMfiHttpClient::HttpResponse> RemoteMfiHttpClient::RequestWithRetry(
    const Options& opt, const std::string& method, const std::string& path,
    const std::string& body) const {
    Common::Status last = Common::Status::Error(Common::StatusCode::UnknownError, "remote-mfi: no attempt");
    const int attempts = opt.max_retries + 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0) {
            CPS_LOG_WARN(kLogTag, "retrying ", method, " ", path, " (attempt ", attempt + 1, "/", attempts, ")");
            std::this_thread::sleep_for(opt.retry_delay);
        }
        auto resp = Request(opt, method, path, body);
        if (resp.IsOk()) {
            if (resp->status >= 200 && resp->status < 300) {
                return *resp;
            }
            last = ErrorFromResponse(*resp);
            // Only the chip-busy / unavailable condition is transient.
            if (resp->status != 503) {
                return last;
            }
        } else {
            last = resp.Error();
        }
    }
    return last;
}

// ---------------------------------------------------------------------------
// IMfiSigner
// ---------------------------------------------------------------------------

Common::Result<Common::ByteBuffer> RemoteMfiHttpClient::FetchCertificate() {
    Options opt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return Common::Status::Error(Common::StatusCode::InvalidState,
                                  "remote-mfi: client is shut down");
        }
        if (!cert_cache_.empty()) {
            return cert_cache_;
        }
        opt = options_;
    }

    if (opt.use_tls) {
        return Common::Status::Error(Common::StatusCode::NotSupported,
                              "remote-mfi: https not supported by this client "
                              "(set use_tls=false or use a TLS proxy)");
    }

    auto resp = RequestWithRetry(opt, "GET", "/mfi/certificate", std::string());
    if (resp.IsError()) {
        return resp.Error();
    }

    std::string type, cert_b64;
    if (!FindJsonString(resp->body, "type", type) || type != "mfi") {
        return Common::Status::Error(Common::StatusCode::ProtocolError,
                              "remote-mfi: response is not an mfi certificate");
    }
    if (!FindJsonString(resp->body, "certificate", cert_b64)) {
        return Common::Status::Error(Common::StatusCode::ProtocolError,
                              "remote-mfi: certificate field missing");
    }
    Common::ByteBuffer der;
    if (!Base64Decode(cert_b64, der) || der.empty()) {
        return Common::Status::Error(Common::StatusCode::ProtocolError,
                              "remote-mfi: certificate is not valid base64");
    }
    if (der.size() > kMaxPayloadBytes) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument,
                              "remote-mfi: certificate exceeds 65525 bytes");
    }

    int major = 0;
    std::string major_str;
    if (FindJsonToken(resp->body, "protocolMajor", major_str)) {
        major = std::atoi(major_str.c_str());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    cert_cache_ = std::move(der);
    protocol_major_ = major;
    // AuthSetup derives the v2/v3 digest from chip_id / firmware_version, so
    // encode protocolMajor in both fields the same way the Apple chips read.
    chip_info_ = MFiChipInfo{};
    chip_info_.present = true;
    if (major == 2) {
        chip_info_.chip_id = 0x00020000u;
        chip_info_.firmware_version = "remote-mfi 2.0";
    } else if (major == 3) {
        chip_info_.chip_id = 0x00030000u;
        chip_info_.firmware_version = "remote-mfi 3.0";
    } else {
        chip_info_.firmware_version = "remote-mfi";
    }
    CPS_LOG_INFO(kLogTag, "remote-mfi: certificate fetched (", cert_cache_.size(), " bytes, protocolMajor=", major, ")");
    return cert_cache_;
}

Common::Result<Common::ByteBuffer> RemoteMfiHttpClient::SignChallenge(
    Common::ByteSpan challenge) {
    Options opt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) {
            return Common::Status::Error(Common::StatusCode::InvalidState,
                                  "remote-mfi: client is shut down");
        }
        opt = options_;
    }

    if (opt.use_tls) {
        return Common::Status::Error(Common::StatusCode::NotSupported,
                              "remote-mfi: https not supported by this client");
    }
    if (challenge.empty() || challenge.size() > 128u) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument,
                              "remote-mfi: challenge must be 1..128 bytes");
    }

    // A fresh id per attempt: the server caches successful signatures for 60
    // seconds per requestId and rejects a reused id with a different challenge.
    const std::string request_id = NewRequestId();
    const std::string body = "{\"challenge\":\"" + Base64Encode(challenge) +
                             "\",\"requestId\":\"" + request_id + "\"}";

    auto resp = RequestWithRetry(opt, "POST", "/mfi/sign", body);
    if (resp.IsError()) {
        return resp.Error();
    }
    std::string sig_b64;
    if (!FindJsonString(resp->body, "signature", sig_b64)) {
        return Common::Status::Error(Common::StatusCode::ProtocolError,
                              "remote-mfi: signature field missing");
    }
    Common::ByteBuffer sig;
    if (!Base64Decode(sig_b64, sig) || sig.empty()) {
        return Common::Status::Error(Common::StatusCode::ProtocolError,
                              "remote-mfi: signature is not valid base64");
    }
    if (sig.size() > kMaxPayloadBytes) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument,
                              "remote-mfi: signature exceeds 65525 bytes");
    }
    return sig;
}

MFiChipInfo RemoteMfiHttpClient::GetChipInfo() const noexcept {
    // The chip identity comes from the certificate fetch; force it once.
    auto self = const_cast<RemoteMfiHttpClient*>(this);
    auto cert = self->FetchCertificate();
    std::lock_guard<std::mutex> lock(mutex_);
    if (cert.IsError()) {
        return MFiChipInfo{};
    }
    return chip_info_;
}

// ---------------------------------------------------------------------------
// Backend-specific helpers
// ---------------------------------------------------------------------------

Common::Result<RemoteMfiHttpClient::HealthStatus>
RemoteMfiHttpClient::GetHealth() const {
    Options opt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        opt = options_;
    }
    auto resp = Request(opt, "GET", "/healthz", std::string());
    if (resp.IsError()) {
        return resp.Error();
    }
    if (resp->status < 200 || resp->status >= 300) {
        return ErrorFromResponse(*resp);
    }
    HealthStatus health;
    std::string ok_str, chip;
    if (FindJsonToken(resp->body, "ok", ok_str)) {
        health.ok = (ok_str == "true");
    }
    if (FindJsonString(resp->body, "chip", chip)) {
        health.chip = std::move(chip);
    }
    FindJsonString(resp->body, "reason", health.reason);
    return health;
}

Common::Status RemoteMfiHttpClient::ResetChip() {
    Options opt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        opt = options_;
    }
    auto resp = RequestWithRetry(opt, "POST", "/mfi/reset", "{}");
    if (resp.IsError()) {
        return resp.Error();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    InvalidateCacheLocked();
    CPS_LOG_INFO(kLogTag, "remote-mfi: chip reset requested");
    return Common::Status::Ok();
}

bool RemoteMfiHttpClient::IsReachable() const {
    auto health = GetHealth();
    return health.IsOk();
}

}  // namespace CarPlayStation
