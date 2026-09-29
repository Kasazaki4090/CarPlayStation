#pragma once

#include "Core/Protocol/Iap2LinkEngine.h"
#include "Core/Protocol/Iap2MessageCatalog.h"
#include "Core/Hardware/IMfiSigner.h"
#include <functional>
#include <mutex>
#include <string>

namespace CarPlayStation::Core::Protocol {

// ============================================================================
// iAP2 Authentication / Identification State Machine States
// ============================================================================
enum class Iap2AuthState : uint8_t {
    Init = 0,         // Waiting for RequestAuthenticationCertificate (0xAA00)
    CertSent = 1,     // Sent 0xAA01 AuthenticationCertificate
    SignSent = 2,     // Sent 0xAA03 AuthenticationResponse
    Authenticated = 3,// Received 0xAA05 AuthenticationSucceeded
    IdentSent = 4,    // Sent 0x1D01 IdentificationInformation
    IdentRetried = 5, // Re-sent 0x1D01 after a first rejection (param-strip retry)
    Identified = 6    // Received 0x1D02 IdentificationAccepted
};

// ============================================================================
// Actions the session manager requests from its driver / caller
// ============================================================================
enum class Iap2Action : uint8_t {
    None,
    SendCert,            // Fetch MFi cert and send as 0xAA01
    SignChallenge,       // Sign challenge and send as 0xAA03
    SendIdentify,        // Build & send IdentifyInformation (0x1D01)
    RetryIdentify,       // Re-send 0x1D01 with rejected params stripped
    Note,                // Milestone event (AuthSuccess / IdentifyAccept)
    Abort,               // Authentication failed — tear down
};

// Event callback signature for protocol-level notifications.
using Iap2EventCallback = std::function<void(Iap2AuthState newState, Iap2Action action, std::string_view description)>;

/**
 * @brief iAP2 session manager: owns the auth/identify state machine and
 *        coordinates MFi challenge-response via IMfiSigner.
 *
 * Feeds received control-session messages (post-framing) into the state machine
 * and emits actions for the caller to execute (send cert, sign challenge, etc.).
 */
class Iap2SessionManager {
public:
    struct Config {
        std::string accessoryName{"CarPlayStation"};
        Common::MacAddress btMac{}; // Bluetooth MAC for wireless identify declaration
    };

    explicit Iap2SessionManager(const Config& config);

    // --- State Machine ---

    // Feed a received control-session message (msgId + body after the 6-byte [40 40] header).
    // Returns the action to perform. Advances state optimistically.
    [[nodiscard]] Iap2Action OnMessage(uint16_t msgId, Common::ByteSpan payload);

    [[nodiscard]] Iap2AuthState GetState() const noexcept { return state_; }

    // --- Action Execution Helpers (called by driver after receiving an action) ---

    // Execute SendCert: fetch cert from MFi signer, wrap in group_one(0, ·), build 0xAA01 packet.
    [[nodiscard]] Common::Status ExecuteSendCert(Iap2LinkEngine& link);

    // Execute SignChallenge: sign the challenge via MFi signer, wrap, build 0xAA03 packet.
    [[nodiscard]] Common::Status ExecuteSignChallenge(Common::ByteSpan challenge, Iap2LinkEngine& link);

    // Execute SendIdentify / RetryIdentify: build and send 0x1D01.
    [[nodiscard]] Common::Status ExecuteSendIdentify(Iap2LinkEngine& link, const std::vector<uint16_t>& excludeParams = {});

    // --- MFi Signer Access ---
    void SetMfiSigner(Hardware::IMfiSigner* signer) { mfi_signer_ = signer; }

    // Register event callback for state transitions.
    void SetEventCallback(Iap2EventCallback cb) { event_cb_ = std::move(cb); }

private:
    void TransitionTo(Iap2AuthState newState, Iap2Action action, std::string_view desc);

    Config config_;
    Iap2AuthState state_{Iap2AuthState::Init};
    Hardware::IMfiSigner* mfi_signer_{nullptr};
    Iap2EventCallback event_cb_;
    mutable std::mutex mutex_;
};

} // namespace CarPlayStation::Core::Protocol
