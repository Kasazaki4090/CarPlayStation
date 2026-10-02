#include "SystemPermissionHelper.h"
#include <shellapi.h>
#include <vector>

namespace CarPlayStation::UI {

PermissionState SystemPermissionHelper::QueryRegistryConsent(const wchar_t* subKey) {
    std::wstring path = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    path += subKey;

    HKEY hKey = nullptr;
    // Check HKCU first
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &hKey);
    if (status != ERROR_SUCCESS) {
        // Fallback to HKLM
        status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &hKey);
    }

    if (status == ERROR_SUCCESS && hKey) {
        wchar_t buffer[64] = { 0 };
        DWORD bufferSize = sizeof(buffer);
        DWORD type = 0;
        status = RegQueryValueExW(hKey, L"Value", nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &bufferSize);
        RegCloseKey(hKey);

        if (status == ERROR_SUCCESS && type == REG_SZ) {
            std::wstring val(buffer);
            if (val == L"Allow") {
                return PermissionState::Allowed;
            } else if (val == L"Deny") {
                return PermissionState::Denied;
            }
        }
    }

    // Default to Allowed on standard Windows desktops if registry key is not customized
    return PermissionState::Allowed;
}

PermissionState SystemPermissionHelper::CheckMicrophonePermission() {
    return QueryRegistryConsent(L"microphone");
}

PermissionState SystemPermissionHelper::CheckLocationPermission() {
    return QueryRegistryConsent(L"location");
}

void SystemPermissionHelper::OpenMicrophoneSettings() {
    ShellExecuteW(
        nullptr,
        L"open",
        L"ms-settings:privacy-microphone",
        nullptr,
        nullptr,
        SW_SHOWNORMAL
    );
}

void SystemPermissionHelper::OpenLocationSettings() {
    ShellExecuteW(
        nullptr,
        L"open",
        L"ms-settings:privacy-location",
        nullptr,
        nullptr,
        SW_SHOWNORMAL
    );
}

} // namespace CarPlayStation::UI
