#pragma once

#include "Core/Common/Types.h"
#include <cstdint>

namespace CarPlayStation::Core::Media {

// ============================================================================
// AudioDuckingManager (DEC-008, Partition 5 Step 3.1)
//
// A small, deterministic state machine that produces the *media* gain
// multiplier to apply to the MainMedia lane while a higher-priority prompt
// (turn-by-turn navigation or Siri telephony) is active.
//
//   * Attack  : on prompt start, the media gain fades 100% -> 20% within
//              `attackMs` (default 100ms).
//   * Release : on prompt end, the media gain restores 20% -> 100% over
//              `releaseMs` (default 400ms, within the 300-500ms spec).
//
// The manager is time-driven via Tick(dtMs) so it is fully deterministic and
// unit-testable without a real clock. The resulting gain is fed to
// IAudioPipeline::SetDuckingRatio().
// ============================================================================
class AudioDuckingManager {
public:
    struct Config {
        float fullGain{1.0f};    // 100% media level (no prompt active)
        float duckedGain{0.20f}; // 20% media level (prompt active)
        uint32_t attackMs{100};  // 100% -> 20% fade time
        uint32_t releaseMs{400}; // 20% -> 100% restore time (300-500ms)
    };

    AudioDuckingManager() = default;
    explicit AudioDuckingManager(Config cfg) : cfg_(cfg) {}

    void SetConfig(const Config& cfg) { cfg_ = cfg; }
    [[nodiscard]] const Config& GetConfig() const noexcept { return cfg_; }

    // Activity-state inputs (any active prompt keeps the media ducked).
    void SetNavigationActive(bool active) { navigationActive_ = active; }
    void SetTelephonyActive(bool active) { telephonyActive_ = active; }

    // Advance the state machine by dtMs and return the current media gain.
    [[nodiscard]] float Tick(uint32_t dtMs);

    // Current media gain (0.0 - 1.0).
    [[nodiscard]] float CurrentGain() const noexcept { return currentGain_; }

    // True while any prompt is active (target is the ducked level).
    [[nodiscard]] bool IsDucked() const noexcept {
        return navigationActive_ || telephonyActive_;
    }

    // True once the gain has fully reached its target (no in-flight ramp).
    [[nodiscard]] bool IsSettled() const noexcept;

    void Reset() {
        currentGain_ = cfg_.fullGain;
        navigationActive_ = false;
        telephonyActive_ = false;
    }

private:
    [[nodiscard]] float TargetGain() const noexcept {
        return IsDucked() ? cfg_.duckedGain : cfg_.fullGain;
    }

    Config cfg_{};
    float currentGain_{1.0f};
    bool navigationActive_{false};
    bool telephonyActive_{false};
};

} // namespace CarPlayStation::Core::Media
