#pragma once

#include "Core/Hardware/IMfiSigner.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace CarPlayStation::Core::Hardware {

// MFi signing backend that talks to a Remote MFi service over HTTP
// (Reference/remote-mfi-for-xcertplay). The service owns the physical MFi
// coprocessor and exposes a small flat-JSON API:
//
//   GET  /mfi/certificate -> {"type":"mfi","protocolMajor":2|3,
//                            "certificate":"<base64>","certificateSha256":"<hex>"}
//   POST /mfi/sign        -> {"challenge":"<base64 1..128B>","requestId":"<uuid>"}
//                            -> {"signature":"<base64>"}
//   POST /mfi/reset       -> {} -> {"detail":""}
//   GET  /healthz         -> {"ok":bool,"chip":"ready|missing|error","reason":"..."}
//
// Errors are flat {"detail":"..."} bodies; the status code carries the class
// (400 bad request, 401 unauthorized, 503 chip unavailable, 500 chip failure).
// When the service is configured with a bearer token, every business endpoint
// requires `Authorization: Bearer <token>`.
//
// The transport is plain Winsock with `Connection: close`, so no HTTP library
// (and no TLS stack) is added to the link line. Set use_tls=false; https needs
// a TLS proxy in front of the service.
class RemoteMfiHttpClient : public IMfiSigner {
public:
    struct Options {
        std::string host{"127.0.0.1"};
        uint16_t port{8972};                 // Service default listen port.
        bool use_tls{false};
        std::string bearer_token{};          // Empty = service has no auth configured.

        std::chrono::milliseconds connect_timeout{3000};
        // The service holds its chip gate for up to 8 s before answering, so the
        // read timeouts must be comfortably longer than that.
        std::chrono::milliseconds send_timeout{12000};
        std::chrono::milliseconds receive_timeout{12000};

        uint32_t max_retries{2};             // Retries apply to connect failures and 503.
        std::chrono::milliseconds retry_delay{500};
    };

    struct HealthStatus {
        bool ok{false};
        std::string chip{};   // "ready" | "missing" | "error"
        std::string reason{};
    };

    explicit RemoteMfiHttpClient(Options options = {});
    ~RemoteMfiHttpClient() override;

    RemoteMfiHttpClient(const RemoteMfiHttpClient&) = delete;
    RemoteMfiHttpClient& operator=(const RemoteMfiHttpClient&) = delete;

    // IMfiSigner --------------------------------------------------------------

    // Fetches (and caches) the accessory certificate. The service caches it
    // process-wide after the first successful read, so this is cheap to call.
    [[nodiscard]] Common::Result<Common::ByteBuffer> FetchCertificate() override;

    // Signs the iAP2 digest. Sends a fresh UUID requestId per attempt so the
    // service's 60 s replay cache never sees a reused id with a new challenge.
    [[nodiscard]] Common::Result<Common::ByteBuffer> SignChallenge(Common::ByteSpan challenge) override;

    // Derived from protocolMajor reported by /mfi/certificate: the generation is
    // placed in bits 16-23 (0x00020000 / 0x00030000) and mirrored into
    // firmware_version so the existing v2/v3 digest selection keeps working.
    [[nodiscard]] MFiChipInfo GetChipInfo() const noexcept override;

    void Shutdown() override;

    // ------------------------------------------------------------------------

    // GET /healthz. Never authenticated by the service, so it doubles as the
    // reachability probe.
    [[nodiscard]] Common::Result<HealthStatus> GetHealth() const;

    // POST /mfi/reset (compat no-op on the reference service).
    [[nodiscard]] Common::Status ResetChip();

    [[nodiscard]] bool IsReachable() const;

    // Drop the locally cached certificate / chip identity (next call re-fetches).
    void InvalidateCache();

    void SetOptions(const Options& options);
    [[nodiscard]] Options GetOptions() const;
    [[nodiscard]] bool IsShutdown() const;

private:
    struct HttpResponse {
        int status{0};
        std::string body{};
    };

    [[nodiscard]] Common::Result<HttpResponse> Request(const Options& options,
                                                       const std::string& method,
                                                       const std::string& path,
                                                       const std::string& body) const;

    // Maps a non-2xx service reply (flat {"detail":"..."} body) onto our status codes.
    [[nodiscard]] static Common::Status ErrorFromResponse(const HttpResponse& response);

    [[nodiscard]] Common::Result<HttpResponse> RequestWithRetry(const Options& options,
                                                                const std::string& method,
                                                                const std::string& path,
                                                                const std::string& body) const;

    void InvalidateCacheLocked();

    mutable std::mutex mutex_;
    Options options_;
    bool shutdown_{false};

    // Cached certificate material, filled by FetchCertificate().
    Common::ByteBuffer cert_cache_{};
    MFiChipInfo chip_info_{};
    int protocol_major_{0};
};

}  // namespace CarPlayStation::Core::Hardware
