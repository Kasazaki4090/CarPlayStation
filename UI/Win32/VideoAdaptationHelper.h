#pragma once

#include "Application/AppSettings.h"
#include "Core/Common/Config.h"
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

namespace CarPlayStation::UI {

/**
 * @brief Strategy output for video stream adaptation.
 */
struct AdaptationDecision {
    Core::Common::VideoCodec codec{Core::Common::VideoCodec::H264};
    uint32_t width{1280};
    uint32_t height{720};
    uint32_t fps{30};
    bool is_hevc_hardware_accelerated{false};
    uint32_t display_refresh_rate{60};
    bool is_battery_saver{false};
};

/**
 * @brief Helper for querying GPU hardware capabilities, monitor refresh rate,
 * power status, and dynamically resolving optimal stream parameters.
 */
class VideoAdaptationHelper {
public:
    /**
     * @brief Query whether the host GPU supports hardware-accelerated HEVC (H.265) decoding.
     * @param existingDevice Optional D3D11 device to reuse. If nullptr, a transient device is created.
     */
    [[nodiscard]] static bool IsGpuHevcDecodeSupported(ID3D11Device* existingDevice = nullptr);

    /**
     * @brief Query the primary monitor's current display refresh rate in Hz.
     * Defaults to 60 if query fails or returns 0.
     */
    [[nodiscard]] static uint32_t GetDisplayRefreshRate();

    /**
     * @brief Check whether Windows Battery Saver mode is active or battery is below 20%.
     */
    [[nodiscard]] static bool IsBatterySaverActive();

    /**
     * @brief Resolve concrete stream parameters from user preferences and runtime facts.
     *
     * Auto Decision Rules:
     *   - Baseline: 1280x720, 30 FPS, H.264
     *   - Resolution: resolves preset or custom dimensions
     *   - Codec (if Auto):
     *       If GPU has hardware HEVC decode AND (width >= 1920 || height >= 1080) -> HEVC
     *       Else -> H.264 (universal compatibility, negligible CPU/GPU load)
     *   - FPS (if Auto):
     *       If display refresh rate >= 60Hz AND !BatterySaver -> 60 FPS
     *       Else -> 30 FPS
     */
    [[nodiscard]] static AdaptationDecision ResolveStreamParameters(
        const Application::VideoPreferences& prefs,
        uint32_t displayWidth = 1280,
        uint32_t displayHeight = 720,
        ID3D11Device* device = nullptr);
};

} // namespace CarPlayStation::UI
