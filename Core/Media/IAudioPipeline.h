#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include <string_view>

namespace CarPlayStation::Core::Media {

enum class AudioLane {
    MainMedia,      // Music, Podcasts, System Audio (High fidelity stereo)
    AltNavigation,  // Turn-by-turn prompts, alerts (attenuates media during speech)
    SiriTelephony   // Siri interaction, incoming/outgoing calls (mono/stereo bidirectional)
};

[[nodiscard]] constexpr std::string_view AudioLaneToString(AudioLane lane) noexcept {
    switch (lane) {
        case AudioLane::MainMedia: return "MainMedia";
        case AudioLane::AltNavigation: return "AltNavigation";
        case AudioLane::SiriTelephony: return "SiriTelephony";
    }
    return "Unknown";
}

class IAudioPipeline {
public:
    virtual ~IAudioPipeline() = default;

    // Start audio output pipeline with configurable sample rate, bit depth and channels
    [[nodiscard]] virtual Common::Status Start(const Common::AudioConfig& config) = 0;

    // Dynamically update audio parameters (sample rates, volumes, ducking ratio)
    [[nodiscard]] virtual Common::Status Reconfigure(const Common::AudioConfig& config) = 0;

    // Push PCM audio chunk to specified audio lane
    [[nodiscard]] virtual Common::Status WriteAudio(AudioLane lane, Common::ByteSpan pcmData) = 0;

    // Set volume level for a specific lane (0.0 to 1.0)
    virtual void SetLaneVolume(AudioLane lane, float volume) = 0;

    // Stop/flush active audio for a specific lane
    virtual void StopLane(AudioLane lane) = 0;

    // Shutdown audio subsystem
    virtual void Shutdown() = 0;
};

} // namespace CarPlayStation::Core::Media
