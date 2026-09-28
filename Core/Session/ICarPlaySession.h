#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <functional>
#include <string_view>

namespace CarPlayStation::Core::Session {

enum class SessionState {
    Idle,
    BtDiscovered,
    iAP2Authenticated,
    WifiHandoff,
    AirPlayStreaming,
    Paused,
    Teardown,
    Error
};

[[nodiscard]] constexpr std::string_view SessionStateToString(SessionState state) noexcept {
    switch (state) {
        case SessionState::Idle: return "Idle";
        case SessionState::BtDiscovered: return "BtDiscovered";
        case SessionState::iAP2Authenticated: return "iAP2Authenticated";
        case SessionState::WifiHandoff: return "WifiHandoff";
        case SessionState::AirPlayStreaming: return "AirPlayStreaming";
        case SessionState::Paused: return "Paused";
        case SessionState::Teardown: return "Teardown";
        case SessionState::Error: return "Error";
    }
    return "Unknown";
}

using SessionStateObserver = std::function<void(SessionState oldState, SessionState newState, Common::Status status)>;

class ICarPlaySession {
public:
    virtual ~ICarPlaySession() = default;

    // Start discovery and session listener.
    // All session parameters (codec, resolution, sample rate, ...) are negotiated with the
    // iPhone at runtime — nothing user-configurable is accepted here (see DEC-004).
    [[nodiscard]] virtual Common::Status Start() = 0;

    // Stop active session
    virtual void Stop() = 0;

    // Get current state
    [[nodiscard]] virtual SessionState GetState() const noexcept = 0;

    // Register state transition observer
    virtual void RegisterStateObserver(SessionStateObserver observer) = 0;
};

} // namespace CarPlayStation::Core::Session
