#include "VideoAdaptationHelper.h"
#include <wrl/client.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// HEVC Main profile GUID: {5b11d51b-dd44-4806-ba02-a040e006a28f}
static const GUID GUID_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN =
    { 0x5b11d51b, 0xdd44, 0x4806, { 0xba, 0x02, 0xa0, 0x40, 0xe0, 0x06, 0xa2, 0x8f } };

// HEVC Main 10 profile GUID: {107af0e0-ef1a-4d19-aba8-67a163073d13}
static const GUID GUID_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10 =
    { 0x107af0e0, 0xef1a, 0x4d19, { 0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13 } };

namespace CarPlayStation::UI {

bool VideoAdaptationHelper::IsGpuHevcDecodeSupported(ID3D11Device* existingDevice) {
    ComPtr<ID3D11Device> device = existingDevice;

    if (!device) {
        UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL levelOut;
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &device,
            &levelOut,
            nullptr
        );
        if (FAILED(hr)) {
            return false;
        }
    }

    ComPtr<ID3D11VideoDevice> videoDevice;
    HRESULT hr = device.As(&videoDevice);
    if (FAILED(hr) || !videoDevice) {
        return false;
    }

    const UINT count = videoDevice->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < count; ++i) {
        GUID profileGuid{};
        if (SUCCEEDED(videoDevice->GetVideoDecoderProfile(i, &profileGuid))) {
            if (IsEqualGUID(profileGuid, GUID_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN) ||
                IsEqualGUID(profileGuid, GUID_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10)) {
                return true;
            }
        }
    }

    return false;
}

uint32_t VideoAdaptationHelper::GetDisplayRefreshRate() {
    DEVMODEW devMode{};
    devMode.dmSize = sizeof(devMode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &devMode)) {
        if (devMode.dmDisplayFrequency > 0) {
            return devMode.dmDisplayFrequency;
        }
    }
    return 60;
}

bool VideoAdaptationHelper::IsBatterySaverActive() {
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status)) {
        // ACLineStatus: 0 = Offline (on battery), 1 = Online, 255 = Unknown
        if (status.ACLineStatus == 0) {
            // SystemStatusFlag: 1 = Battery saver is on
            if (status.SystemStatusFlag == 1) {
                return true;
            }
            // Battery low (<20%)
            if (status.BatteryLifePercent != 255 && status.BatteryLifePercent < 20) {
                return true;
            }
        }
    }
    return false;
}

AdaptationDecision VideoAdaptationHelper::ResolveStreamParameters(
    const Application::VideoPreferences& prefs,
    uint32_t displayWidth,
    uint32_t displayHeight,
    ID3D11Device* device) {

    AdaptationDecision decision;

    // 1. Resolution resolution
    auto [targetW, targetH] = prefs.GetEffectiveDimensions(displayWidth, displayHeight);
    decision.width = targetW;
    decision.height = targetH;

    // 2. Hardware and environment queries
    decision.is_hevc_hardware_accelerated = IsGpuHevcDecodeSupported(device);
    decision.display_refresh_rate = GetDisplayRefreshRate();
    decision.is_battery_saver = IsBatterySaverActive();

    // 3. Codec resolution
    if (prefs.preferred_codec == Application::VideoCodecPreference::HEVC) {
        decision.codec = Core::Common::VideoCodec::HEVC;
    } else if (prefs.preferred_codec == Application::VideoCodecPreference::H264) {
        decision.codec = Core::Common::VideoCodec::H264;
    } else { // Auto
        // Auto rules:
        // Use HEVC only if GPU has hardware acceleration AND target resolution is >= 1080p.
        // Otherwise default to H.264 for maximum compatibility and lowest power consumption.
        if (decision.is_hevc_hardware_accelerated && (decision.width >= 1920 || decision.height >= 1080)) {
            decision.codec = Core::Common::VideoCodec::HEVC;
        } else {
            decision.codec = Core::Common::VideoCodec::H264;
        }
    }

    // 4. FPS resolution
    if (prefs.fps == 30) {
        decision.fps = 30;
    } else if (prefs.fps == 60) {
        decision.fps = 60;
    } else { // Auto or 0
        if (decision.display_refresh_rate >= 60 && !decision.is_battery_saver) {
            decision.fps = 60;
        } else {
            decision.fps = 30;
        }
    }

    return decision;
}

} // namespace CarPlayStation::UI
