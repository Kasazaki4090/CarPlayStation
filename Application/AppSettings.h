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
    Res720P = 0,     // 1280 x 720 (16:9 标准车机中控屏)
    Res1080P = 1,    // 1920 x 1080 (16:9 高清大屏)
    Res1440P = 2,    // 2560 x 1440 (2K)
    Res2160P = 3,    // 3840 x 2160 (4K)
    Auto = 4,        // Match window or display resolution dynamically
    Custom = 5,      // User-specified custom dimensions
    Car1280x480 = 6, // 1280 x 480 (8:3 宝马/奥迪经典车机长条宽屏)
    Car1920x720 = 7  // 1920 x 720 (8:3 宝马/奔驰双联屏及现代车机高清宽屏)
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
            case VideoResolutionPreset::Res720P:     return {1280, 720};
            case VideoResolutionPreset::Res1080P:    return {1920, 1080};
            case VideoResolutionPreset::Res1440P:    return {2560, 1440};
            case VideoResolutionPreset::Res2160P:    return {3840, 2160};
            case VideoResolutionPreset::Auto:        return {currentDisplayWidth, currentDisplayHeight};
            case VideoResolutionPreset::Custom:      return {custom_width, custom_height};
            case VideoResolutionPreset::Car1280x480: return {1280, 480};
            case VideoResolutionPreset::Car1920x720: return {1920, 720};
        }
        return {1920, 1080};
    }
};

// Audio preferences: only the three lane volumes and ducking attenuation ratio.
//
// The output sample rate is automatically determined from the Windows default
// endpoint mix format (48000 Hz or 44100 Hz, with integer downsampling like 96k->48k
// and 88.2k->44.1k to eliminate fractional SRC). The stream encode bitrate defaults
// to 320 kbps. Neither sample rate nor bitrate is exposed in user settings.
struct AudioPreferences {
    float media_volume{1.0f};     // 0.0 to 1.0
    float navigation_volume{1.0f};// 0.0 to 1.0
    float telephony_volume{1.0f}; // 0.0 to 1.0
    float ducking_ratio{0.20f};   // Media volume multiplier during navigation prompt (0.0 to 1.0)
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
    HardwareI2C = 2,   // Physical USB-to-I2C bridge (CH341) driving an MFi coprocessor (DEC-003)
    RemoteHttp = 3     // Remote MFi service over HTTP (see Reference/remote-mfi-for-xcertplay)
};
// Value 1 was the binary-framed TCP signing daemon (DEC-003). It is gone: the HTTP
// JSON service covers the same use case and is far more general. Configs that still
// say BackendType=1 are migrated to RemoteHttp when the .ini is read.

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
    bool pulse_reset_on_init{false};
    uint8_t reset_gpio_pin{0};   // Reset pin index on bridge chip (e.g. D0 on CH341)
};

struct RemoteMfiPreferences {
    // Remote MFi HTTP service (Reference/remote-mfi-for-xcertplay). The host and
    // port are user-entered in the setup wizard; nothing is hardcoded beyond the
    // service's own default listen port.
    std::string host{"127.0.0.1"};
    uint16_t port{8972};
    std::string bearer_token{};  // Empty = service has no auth configured.
};

struct MFiPreferences {
    MFiBackendType backend_type{MFiBackendType::LocalFile};

    // For LocalFile backend (DEC-006):
    std::string certificate_path{"certs/certificate.p7b"};
    std::string private_key_path{"certs/identity.pk8"};

    // For RemoteHttp backend (the network signing service):
    RemoteMfiPreferences remote_mfi{};

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
