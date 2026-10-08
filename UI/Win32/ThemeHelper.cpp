#include "ThemeHelper.h"
#include <windows.h>

namespace CarPlayStation::UI {

namespace {

// The value Windows Settings writes for "Choose your mode" (app mode).
// 1 = light, 0 = dark.
constexpr const wchar_t* kPersonalizeSubKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr const wchar_t* kAppsUseLightThemeValue = L"AppsUseLightTheme";

bool QueryRegistryDword(HKEY root, DWORD& outValue) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, kPersonalizeSubKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS || !hKey) {
        return false;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(hKey, kAppsUseLightThemeValue, nullptr, &type,
                                           reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);

    if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) {
        return false;
    }
    outValue = value;
    return true;
}

} // namespace

UiTheme ThemeHelper::CurrentTheme() {
    DWORD value = 0;
    // Per-user setting first, machine-wide default second.
    if (!QueryRegistryDword(HKEY_CURRENT_USER, value)) {
        if (!QueryRegistryDword(HKEY_LOCAL_MACHINE, value)) {
            return UiTheme::Dark;
        }
    }
    return (value != 0) ? UiTheme::Light : UiTheme::Dark;
}

UiPalette ThemeHelper::BuildPalette(UiTheme theme) {
    UiPalette p{};

    if (theme == UiTheme::Light) {
        // Light panels sit on top of the (light) letterbox background, so the
        // panels stay near-white and the text turns dark.
        p.text_primary   = { 0.10f, 0.10f, 0.12f, 1.0f };
        p.text_secondary = { 0.36f, 0.37f, 0.41f, 1.0f };
        p.panel_bg       = { 0.96f, 0.96f, 0.97f, 0.90f };
        p.accent         = { 0.00f, 0.45f, 0.90f, 1.0f };
        p.status_green   = { 0.10f, 0.60f, 0.20f, 1.0f };
        p.status_orange  = { 0.80f, 0.45f, 0.00f, 1.0f };
        p.slider_track   = { 0.72f, 0.73f, 0.76f, 1.0f };
        p.slider_thumb   = { 1.00f, 1.00f, 1.00f, 1.0f };
        p.log_error      = { 0.75f, 0.12f, 0.12f, 1.0f };
        p.letterbox      = { 0.94f, 0.94f, 0.95f, 1.0f };
    } else {
        // The palette this UI has always used.
        p.text_primary   = { 1.00f, 1.00f, 1.00f, 1.0f };
        p.text_secondary = { 0.70f, 0.70f, 0.75f, 1.0f };
        p.panel_bg       = { 0.08f, 0.08f, 0.10f, 0.85f };
        p.accent         = { 0.00f, 0.48f, 1.00f, 1.0f };
        p.status_green   = { 0.20f, 0.80f, 0.30f, 1.0f };
        p.status_orange  = { 1.00f, 0.60f, 0.00f, 1.0f };
        p.slider_track   = { 0.25f, 0.25f, 0.30f, 1.0f };
        p.slider_thumb   = { 1.00f, 1.00f, 1.00f, 1.0f };
        p.log_error      = { 1.00f, 0.30f, 0.30f, 1.0f };
        p.letterbox      = { 0.00f, 0.00f, 0.00f, 1.0f };
    }

    return p;
}

const char* ThemeHelper::ThemeName(UiTheme theme) {
    return (theme == UiTheme::Light) ? "light" : "dark";
}

} // namespace CarPlayStation::UI
