#pragma once

#include <windows.h>
#include <string>

namespace CarPlayStation::UI {

enum class PermissionState {
    Allowed,
    Denied,
    Unknown
};

class SystemPermissionHelper {
public:
    /**
     * @brief Check whether Windows Microphone capability access is granted to desktop applications.
     */
    [[nodiscard]] static PermissionState CheckMicrophonePermission();

    /**
     * @brief Check whether Windows Location capability access is granted.
     */
    [[nodiscard]] static PermissionState CheckLocationPermission();

    /**
     * @brief Open Windows Settings page for Microphone Privacy.
     */
    static void OpenMicrophoneSettings();

    /**
     * @brief Open Windows Settings page for Location Privacy.
     */
    static void OpenLocationSettings();

private:
    [[nodiscard]] static PermissionState QueryRegistryConsent(const wchar_t* subKey);
};

} // namespace CarPlayStation::UI
