#pragma once

#include "Core/Common/Types.h"
#include <string>
#include <cstdint>
#include <string_view>
#include <utility>

namespace CarPlayStation::Core::Common {

// ============================================================================
// Core configuration policy (see DEC-004):
//
//   * Everything below is either a concrete negotiated protocol fact or an
//     environment / endpoint fact. Core subsystems consume only these.
//   * User-tunable preferences (resolution presets, window mode, codec "Auto"
//     preference, bitrate request, volumes, ducking) live in the Application
//     layer (Application/AppSettings.h) and are resolved into concrete values
//     before crossing into Core.
//   * MFi authentication backend selection and its daemon endpoint are also
//     user preferences: see Application::MFiPreferences (DEC-003, DEC-004).
// ============================================================================

// Protocol-level video codecs (negotiated facts; no "Auto" — that is a user preference)
enum class VideoCodec : uint8_t {
    H264 = 0,
    HEVC = 1
};

[[nodiscard]] constexpr std::string_view VideoCodecToString(VideoCodec codec) noexcept {
    switch (codec) {
        case VideoCodec::H264: return "H264";
        case VideoCodec::HEVC: return "HEVC";
    }
    return "Unknown";
}

// Concrete negotiated video stream specification consumed by the decoder.
struct VideoStreamSpec {
    VideoCodec codec{VideoCodec::H264}; // Final codec agreed with iPhone during negotiation
    uint32_t width{1920};
    uint32_t height{1080};
    uint32_t fps{60};                   // 30 or 60 fps
};

// Concrete audio output format consumed by the pipeline.
// Volumes and ducking are runtime controls, not part of the format.
struct AudioOutputFormat {
    uint32_t sample_rate{48000};  // e.g., 44100, 48000, 96000 Hz
    uint8_t channels{2};          // 1 = Mono, 2 = Stereo
    uint8_t bit_depth{16};        // 16-bit or 24-bit PCM
};

// Device identity advertised during Bluetooth / protocol handshake.
struct DeviceIdentity {
    std::string device_name{"CarPlayStation"};
    std::string manufacturer{"OpenSource"};
    std::string model{"CarPlayStationWin"};
};

// Wireless receive-side endpoints (mutable at runtime).
// Consumed only by Transport, never by Session or Media.
// NOTE: the MFi signing daemon endpoint is NOT here — it is an Application-layer
// preference (Application/AppSettings.h :: MFiPreferences), because choosing and
// pointing at a signing backend is user configuration, not an environment fact.
struct NetworkEndpoints {
    std::string rtsp_bind_address{"0.0.0.0"};   // AirPlay RTSP bind IP
    uint16_t rtsp_port{5000};                   // AirPlay RTSP listening port
    uint16_t airplay_audio_port{7000};          // RAIO/AirPlay audio port
    uint16_t airplay_video_port{7100};          // AirPlay H.264 video port
    std::string wifi_softap_ssid{"CarPlayStation"};
    std::string wifi_softap_password{"12345678"};
    std::string wifi_softap_gateway{"192.168.1.1"};
};

// Core runtime environment configuration (the only config Core needs at init).
struct CoreEnvironmentConfig {
    DeviceIdentity identity;
    NetworkEndpoints network;
};

} // namespace CarPlayStation::Core::Common
