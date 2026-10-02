#pragma once

#include "Core/Common/Types.h"
#include <cstdint>
#include <string>
#include <utility>

namespace CarPlayStation::Application {

// ============================================================================
// User-tunable preferences (see DEC-004).
// Everything here is UI-editable at runtime. The Application layer resolves
// these into concrete Core parameters (Core::Common::VideoStreamSpec,
// AudioOutputFormat, ...) before anything crosses into Core.
// ============================================================================

enum class VideoResolutionPreset : uint8_t {
    Res720P = 0,   // 1280 x 720
    Res1080P = 1,  // 1920 x 1080
    Res1440P = 2,  // 2560 x 1440 (2K)
    Res2160P = 3,  // 3840 x 2160 (4K)
    Auto = 4,      // Match window or display resolution dynamically
    Custom = 5     // User-specified custom dimensions
};

// Pure presentation-layer concept: how the video surface is shown.
enum class WindowDisplayMode : uint8_t {
    Windowed = 0,             // Normal resizable / framed window
    BorderlessFullscreen = 1, // Borderless window matching monitor size
    ExclusiveFullscreen = 2   // Exclusive hardware fullscreen
};

// Advisory codec preference advertised to iPhone ("Auto" is allowed here only).
enum class VideoCodecPreference : uint8_t {
    H264 = 0,
    HEVC = 1,
    Auto = 2
};

struct VideoPreferences {
    VideoResolutionPreset resolution_preset{VideoResolutionPreset::Res720P};
    VideoCodecPreference preferred_codec{VideoCodecPreference::H264};
    uint32_t custom_width{1280};
    uint32_t custom_height{720};
    uint32_t fps{30};                     // 30 or 60 fps
    WindowDisplayMode display_mode{WindowDisplayMode::Windowed};

    // Resolve preset / custom into concrete dimensions (UI helper; stays out of Core)
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

struct AudioPreferences {
    uint32_t sample_rate{48000};  // Preferred output sample rate (44100 / 48000 / 96000)
    uint32_t bitrate_kbps{320};   // Requested stream bitrate (advisory; final value negotiated)
    float media_volume{1.0f};     // 0.0 to 1.0
    float navigation_volume{1.0f};// 0.0 to 1.0
    float telephony_volume{1.0f}; // 0.0 to 1.0
    float ducking_ratio{0.25f};   // Media volume multiplier during navigation prompt
};

struct DevicePreferences {
    std::string device_name{"CarPlayStation"};
    std::string manufacturer{"OpenSource"};
    std::string model{"CarPlayStationWin"};
};

// MFi authentication backend selection (DEC-003, DEC-006: pluggable backends).
// The Application layer constructs the matching Core::Hardware::IMfiSigner
// implementation from these settings — Core never hardcodes a daemon address or file path.
enum class MFiBackendType : uint8_t {
    LocalFile = 0,     // Pure offline software signer using imported cert & private key (DEC-006)
    NetworkDaemon = 1, // Remote/Local TCP daemon at daemon_endpoint (DEC-003)
    HardwareI2C = 2    // Physical USB-to-I2C bridge (CH341) driving an MFi coprocessor (DEC-003)
};

enum class HardwareI2cDriverType : uint8_t {
    Auto = 0,    // Probe available drivers in order: CH341 -> CH347 -> FTDI
    CH341 = 1,   // WCH CH341 USB-to-I2C (CH341DLL.DLL)
    CH347 = 2,   // WCH CH347 High-Speed USB-to-I2C (CH347DLL.DLL)
    FTDI = 3,    // FTDI MPSSE (libMPSSE / ftd2xx.dll)
    Mock = 4     // In-memory mock adapter for offline testing
};

struct HardwareI2cPreferences {
    HardwareI2cDriverType driver_type{HardwareI2cDriverType::Auto};
    uint32_t device_index{0};
    uint8_t slave_address{0x11}; // 7-bit I2C address of the MFi coprocessor
    uint32_t speed_khz{100};     // Bus clock speed in kHz (e.g. 100 or 400)
    bool pulse_reset_on_init{true};
    uint8_t reset_gpio_pin{0};   // Reset pin index on bridge chip (e.g. D0 on CH341)
};

struct MFiPreferences {
    MFiBackendType backend_type{MFiBackendType::LocalFile};

    // For LocalFile backend (DEC-006):
    std::string certificate_path{"certs/certificate.p7b"};
    std::string private_key_path{"certs/identity.pk8"};

    // For NetworkDaemon backend (DEC-003):
    Core::Common::IpEndpoint daemon_endpoint{"127.0.0.1", 9000};

    // For HardwareI2C backend (DEC-003, DEC-007):
    HardwareI2cPreferences hardware_i2c{};
};

// Full user-facing settings container, editable at runtime via IApplicationApi.
struct AppSettings {
    VideoPreferences video;
    AudioPreferences audio;
    DevicePreferences device;
    MFiPreferences mfi;
};

} // namespace CarPlayStation::Application
