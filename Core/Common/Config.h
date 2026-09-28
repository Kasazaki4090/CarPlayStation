#pragma once

#include "Core/Common/Types.h"
#include <string>
#include <cstdint>
#include <utility>

namespace CarPlayStation::Core::Common {

// Display Window Mode
enum class WindowDisplayMode : uint8_t {
    Windowed = 0,             // Normal resizable / framed window
    BorderlessFullscreen = 1, // Borderless window matching monitor size
    ExclusiveFullscreen = 2   // Exclusive hardware fullscreen
};

// Video Resolution Preset
enum class VideoResolutionPreset : uint8_t {
    Res720P = 0,   // 1280 x 720
    Res1080P = 1,  // 1920 x 1080
    Res1440P = 2,  // 2560 x 1440 (2K)
    Res2160P = 3,  // 3840 x 2160 (4K)
    Auto = 4,      // Match window or display resolution dynamically
    Custom = 5     // User-specified custom dimensions
};

// Preferred Video Codec negotiation (advisory preference advertised to iPhone)
enum class VideoCodecPreference : uint8_t {
    H264 = 0,
    HEVC = 1,
    Auto = 2
};

struct VideoConfig {
    VideoResolutionPreset resolution_preset{VideoResolutionPreset::Res1080P};
    uint32_t custom_width{1920};
    uint32_t custom_height{1080};
    uint32_t fps{60};                     // 30 or 60 fps
    uint32_t screen_dpi{160};              // Physical DPI density
    VideoCodecPreference preferred_codec{VideoCodecPreference::H264};
    WindowDisplayMode display_mode{WindowDisplayMode::Windowed};

    // Calculate effective width & height based on preset or display boundaries
    [[nodiscard]] std::pair<uint32_t, uint32_t> GetEffectiveDimensions(
        uint32_t currentDisplayWidth = 1920,
        uint32_t currentDisplayHeight = 1080) const noexcept {
        switch (resolution_preset) {
            case VideoResolutionPreset::Res720P:  return {1280, 720};
            case VideoResolutionPreset::Res1080P: return {1920, 1080};
            case VideoResolutionPreset::Res1440P: return {2560, 1440};
            case VideoResolutionPreset::Res2160P: return {3840, 2160};
            case VideoResolutionPreset::Auto:     return {currentDisplayWidth, currentDisplayHeight};
            case VideoResolutionPreset::Custom:   return {custom_width, custom_height};
        }
        return {1920, 1080};
    }
};

// Audio Configuration
struct AudioConfig {
    uint32_t sample_rate{48000};  // e.g., 44100, 48000, 96000 Hz
    uint32_t bitrate_kbps{320};   // e.g., 128, 192, 256, 320 kbps
    uint8_t channels{2};          // 1 = Mono, 2 = Stereo
    uint8_t bit_depth{16};        // 16-bit or 24-bit PCM
    float media_volume{1.0f};     // 0.0 to 1.0
    float navigation_volume{1.0f};// 0.0 to 1.0
    float telephony_volume{1.0f}; // 0.0 to 1.0
    float ducking_ratio{0.25f};   // Media volume multiplier during navigation prompt
};

// Network & Endpoint Configuration (Fully configurable, zero hardcoding)
struct NetworkConfig {
    IpEndpoint mfi_daemon_endpoint{"127.0.0.1", 9000}; // MFi TCP proxy daemon address
    std::string rtsp_bind_address{"0.0.0.0"};          // AirPlay RTSP bind IP
    uint16_t rtsp_port{5000};                          // AirPlay RTSP listening port
    uint16_t airplay_audio_port{7000};                 // RAIO/AirPlay audio port
    uint16_t airplay_video_port{7100};                 // AirPlay H.264 video port
    std::string wifi_softap_ssid{"CarPlayStation"};
    std::string wifi_softap_password{"12345678"};
    std::string wifi_softap_gateway{"192.168.1.1"};
    bool enable_hardware_mfi{false};                   // If true, attempt USB CH341 I2C; else daemon
};

// Global Configuration Container
struct CarPlayStationConfig {
    std::string device_name{"CarPlayStation"};
    std::string manufacturer{"OpenSource"};
    std::string model{"CarPlayStationWin"};
    VideoConfig video;
    AudioConfig audio;
    NetworkConfig network;
};

} // namespace CarPlayStation::Core::Common
