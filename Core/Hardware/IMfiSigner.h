#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <cstdint>
#include <string>

namespace CarPlayStation::Core::Hardware {

// Information about the local MFi authentication coprocessor (chip behind a USB-I2C bridge,
// or the daemon that proxies one). Used for diagnostics and capability checks only.
struct MFiChipInfo {
    uint32_t chip_id{0};             // Vendor-specific silicon identifier
    std::string firmware_version{};  // Coprocessor / daemon firmware version string
    bool present{false};             // False when no backend is reachable
};

// Abstract MFi authentication signer.
//
// Backends (implemented in Lib, selected and constructed by the Application layer):
//   * USB-I2C hardware coprocessor behind a bridge chip (WCH CH341 or equivalent) —
//     talks to the OS driver directly; no network involved.
//   * Network signing daemon over TCP — endpoint is Application-layer configuration
//     (see Application/AppSettings.h MFiPreferences); Core never hardcodes it.
class IMfiSigner {
public:
    virtual ~IMfiSigner() = default;

    // Fetch the device's signed MFi certificate (used during iAP2 authentication).
    [[nodiscard]] virtual Common::Result<Common::ByteBuffer> FetchCertificate() = 0;

    // Sign an iAP2 authentication challenge. Apple's protocol signs the SHA-1 digest of
    // the received challenge payload; implementations may expose raw or digested variants,
    // but this interface always takes and returns opaque bytes.
    [[nodiscard]] virtual Common::Result<Common::ByteBuffer> SignChallenge(Common::ByteSpan challenge) = 0;

    // Query coprocessor identity / firmware for diagnostics.
    [[nodiscard]] virtual MFiChipInfo GetChipInfo() const noexcept = 0;

    // Release backend resources (driver handle, socket, ...) without invalidating the signer contract.
    virtual void Shutdown() = 0;
};

} // namespace CarPlayStation::Core::Hardware
