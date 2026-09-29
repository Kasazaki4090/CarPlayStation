#pragma once

#include "Core/Hardware/IMfiSigner.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <string>
#include <chrono>
#include <atomic>
#include <mutex>

namespace CarPlayStation::Core::Hardware {

// TCP Client that communicates with an MFi signing oracle / daemon service
// (e.g. mfi_oracle on Raspberry Pi/CCPA or a remote signing server).
//
// Wire Protocol:
//   Request:  `op (1 byte) | len (2 bytes BE) | payload`
//     - op 0x01: FetchCertificate (len = 0, no payload)
//     - op 0x02: CreateSignature (len = challenge.size, payload = 20-byte SHA-1 digest)
//   Response: `status (1 byte) | len (2 bytes BE) | payload`
//     - status 0x00: OK
//     - status != 0x00: Failure
class NetworkMfiClient : public IMfiSigner {
public:
    struct Options {
        uint32_t max_retries{5};
        std::chrono::milliseconds retry_delay{400};
        std::chrono::milliseconds connect_timeout{2000};
        std::chrono::milliseconds response_deadline{3000};
    };

    explicit NetworkMfiClient(Common::IpEndpoint endpoint, Options options = {});
    NetworkMfiClient(std::string host, uint16_t port, Options options = {});
    ~NetworkMfiClient() override;

    // Fetch the accessory certificate (op 0x01, ~945 bytes).
    [[nodiscard]] Common::Result<Common::ByteBuffer> FetchCertificate() override;

    // Sign an authentication challenge digest (op 0x02, 20 bytes SHA-1 -> 128 bytes RSA signature).
    [[nodiscard]] Common::Result<Common::ByteBuffer> SignChallenge(Common::ByteSpan challenge) override;

    // Query chip info / status.
    [[nodiscard]] MFiChipInfo GetChipInfo() const noexcept override;

    // Release resources and mark signer as shut down.
    void Shutdown() override;

    // Check if the remote daemon is reachable with a short connection probe without reading the chip.
    [[nodiscard]] bool IsReachable(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const;

    // Dynamic endpoint reconfiguration (supplied by Application layer).
    void SetEndpoint(Common::IpEndpoint endpoint);
    [[nodiscard]] Common::IpEndpoint GetEndpoint() const;

    // Update operational options (retries, timeouts).
    void SetOptions(Options options);
    [[nodiscard]] Options GetOptions() const;

private:
    [[nodiscard]] Common::Result<Common::ByteBuffer> ExecuteRequestWithRetry(uint8_t op, Common::ByteSpan payload);
    [[nodiscard]] Common::Result<Common::ByteBuffer> ExecuteSingleRequest(uint8_t op, Common::ByteSpan payload, const Common::IpEndpoint& ep);

    mutable std::mutex mutex_;
    Common::IpEndpoint endpoint_;
    Options options_;
    std::atomic<bool> is_shutdown_{false};
};

} // namespace CarPlayStation::Core::Hardware
