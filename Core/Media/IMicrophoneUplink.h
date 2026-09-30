#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include <functional>
#include <vector>

namespace CarPlayStation::Core::Media {

// Uplink microphone audio delivery callback
using UplinkAudioCallback = std::function<void(Common::ByteSpan pcmData, uint64_t timestampUs)>;

// Microphone hardware capabilities
struct MicrophoneCapabilities {
    std::vector<uint32_t> supported_sample_rates{16000, 24000, 48000};
    bool supports_mono{true};
    bool supports_stereo{true};
    bool supports_hardware_aec{false}; // Acoustic Echo Cancellation
};

class IMicrophoneUplink {
public:
    virtual ~IMicrophoneUplink() = default;

    // Start microphone capture session for negotiated format (typically 16kHz or 24kHz mono PCM)
    [[nodiscard]] virtual Common::Status Start(const Common::AudioOutputFormat& format) = 0;

    // Set callback to receive captured and resampled microphone audio buffers
    virtual void SetUplinkCallback(UplinkAudioCallback callback) = 0;

    // Stop active microphone capture
    [[nodiscard]] virtual Common::Status Stop() = 0;

    // Query active recording status
    [[nodiscard]] virtual bool IsCapturing() const noexcept = 0;

    // Query hardware capture capabilities
    [[nodiscard]] virtual MicrophoneCapabilities QueryCapabilities() const = 0;

    // Set software input gain multiplier (0.0 to 2.0, 1.0 = unity)
    virtual void SetInputGain(float gain) = 0;

    // Return the total captured audio sample count
    [[nodiscard]] virtual uint64_t GetCapturedSampleCount() const noexcept = 0;

    // Release microphone resources
    virtual void Shutdown() = 0;
};

} // namespace CarPlayStation::Core::Media

