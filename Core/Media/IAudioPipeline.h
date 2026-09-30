#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include <string_view>
#include <vector>

namespace CarPlayStation::Core::Media {

enum class AudioLane : uint8_t {
    MainMedia     = 0, // Music, Podcasts, System Audio (High fidelity stereo)
    AltNavigation = 1, // Turn-by-turn prompts, alerts (attenuates media during speech)
    SiriTelephony = 2  // Siri interaction, incoming/outgoing calls (mono/stereo bidirectional)
};

[[nodiscard]] constexpr std::string_view AudioLaneToString(AudioLane lane) noexcept {
    switch (lane) {
        case AudioLane::MainMedia:     return "MainMedia";
        case AudioLane::AltNavigation: return "AltNavigation";
        case AudioLane::SiriTelephony: return "SiriTelephony";
    }
    return "Unknown";
}

// Soundcard and rendering pipeline capabilities
struct AudioCapabilities {
    std::vector<uint32_t> supported_sample_rates{32000, 44100, 48000, 88200, 96000, 176400, 192000};
    std::vector<uint8_t> supported_bit_depths{16, 24, 32};
    bool supports_float32{true};
    bool auto_adapted_to_mix_format{true};
    Common::AudioOutputFormat native_mix_format{48000, 2, 16, Common::AudioSampleType::PcmInt16};
};

class IAudioPipeline {
public:
    virtual ~IAudioPipeline() = default;

    // Start audio output pipeline with a concrete negotiated format (no user preferences)
    [[nodiscard]] virtual Common::Status Start(const Common::AudioOutputFormat& format) = 0;

    // Dynamically update the output format (e.g. sample rate/bit depth change after renegotiation)
    [[nodiscard]] virtual Common::Status Reconfigure(const Common::AudioOutputFormat& format) = 0;

    // Push PCM audio chunk to specified audio lane
    [[nodiscard]] virtual Common::Status WriteAudio(AudioLane lane, Common::ByteSpan pcmData) = 0;

    // Set volume level for a specific lane (0.0 to 1.0) — runtime control, not part of format
    virtual void SetLaneVolume(AudioLane lane, float volume) = 0;

    // Media volume multiplier applied while navigation / telephony prompts are active
    virtual void SetDuckingRatio(float ratio) = 0;

    // Query physical soundcard / WASAPI shared mode capabilities
    [[nodiscard]] virtual AudioCapabilities QueryCapabilities() const = 0;

    // Return the total rendered PCM sample count across all lanes
    [[nodiscard]] virtual uint64_t GetRenderedSampleCount() const noexcept = 0;

    // Stop/flush active audio for a specific lane
    virtual void StopLane(AudioLane lane) = 0;

    // Shutdown audio subsystem
    virtual void Shutdown() = 0;
};

} // namespace CarPlayStation::Core::Media

