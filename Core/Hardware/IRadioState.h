#pragma once

#include "Core/Common/Result.h"
#include <string_view>

namespace CarPlayStation::Core::Hardware {

// State of the wireless radios used by Wireless CarPlay (Bluetooth RFCOMM + Wi-Fi).
enum class RadioState : uint8_t {
    Off = 0,
    On = 1,
    Unavailable = 2 // No usable adapter / driver on this machine
};

[[nodiscard]] constexpr std::string_view RadioStateToString(RadioState state) noexcept {
    switch (state) {
        case RadioState::Off: return "Off";
        case RadioState::On: return "On";
        case RadioState::Unavailable: return "Unavailable";
    }
    return "Unknown";
}

// Control and query for the local radio stack. Implementation lives in Lib (Windows:
// Bluetooth / WLAN subsystem APIs). Core uses this to gate wireless discovery.
class IRadioState {
public:
    virtual ~IRadioState() = default;

    // Query current state of a specific radio ("bluetooth" or "wifi").
    [[nodiscard]] virtual RadioState GetState(std::string_view radio) const noexcept = 0;

    // Request the radio to be turned on/off (may require user consent on Windows).
    [[nodiscard]] virtual Common::Status SetEnabled(std::string_view radio, bool enabled) = 0;
};

} // namespace CarPlayStation::Core::Hardware
