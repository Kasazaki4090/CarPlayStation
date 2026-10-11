#include "SetupWizard.h"
#include "Core/Common/Logging.h"
#include <algorithm>
#include <commctrl.h>
#include <shlobj.h>
#include <commdlg.h>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace CarPlayStation::UI {

// True if the (UTF-8) file path exists on disk.
static bool FileExistsOnDisk(const std::string& utf8_path) {
    if (utf8_path.empty()) {
        return false;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        return false;
    }
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, wpath.data(), wlen);
    DWORD attrs = GetFileAttributesW(wpath.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static const wchar_t* kDisclaimerWndClass = L"CarPlayStation_DisclaimerModalClass";
static const wchar_t* kMfiWizardWndClass   = L"CarPlayStation_MfiWizardModalClass";

// --- DPI scaling for the two startup modals ---------------------------------
// Both dialogs are laid out below in 96-DPI *design* units. The process runs
// Per-Monitor V2 DPI aware (MainWindow::Create), so Windows does not bitmap
// stretch them: on a display with scaling enabled the controls would keep their
// unscaled pixel size, which makes them tiny and effectively unclickable. Every
// length is therefore converted to the device pixels of the monitor the dialog
// is shown on, and the child controls get the system UI font at the matching
// size so the drawn text and the hit area agree.

constexpr int kBaseDpi = 96;

// DPI of the monitor the dialog will appear on. The dialogs are centred on the
// main window, so the parent's window DPI is the right one to use.
UINT DialogDpi(HWND parent) {
    HWND ref = parent ? parent : GetDesktopWindow();
    UINT dpi = GetDpiForWindow(ref);
    if (dpi == 0) {
        dpi = kBaseDpi;
    }
    return dpi;
}

// Convert one design-unit length to device pixels.
int Scaled(UINT dpi, int design) {
    return MulDiv(design, static_cast<int>(dpi), kBaseDpi);
}

// The system UI font, resized for the dialog's DPI. Returns nullptr if the
// metrics cannot be read, in which case controls keep the default font.
HFONT CreateScaledUiFont(UINT dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);

    // Prefer SystemParametersInfoForDpi (Windows 10 1607+) so non-client metrics
    // are queried directly at the target DPI without double-scaling.
    using SystemParametersInfoForDpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static auto pfnSystemParametersInfoForDpi = reinterpret_cast<SystemParametersInfoForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));

    if (pfnSystemParametersInfoForDpi) {
        if (pfnSystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
            return CreateFontIndirectW(&ncm.lfMessageFont);
        }
    }

    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0) == 0) {
        return nullptr;
    }

    LOGFONTW lf = ncm.lfMessageFont;
    using GetDpiForSystemFn = UINT(WINAPI*)();
    static auto pfnGetDpiForSystem = reinterpret_cast<GetDpiForSystemFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    UINT sysDpi = pfnGetDpiForSystem ? pfnGetDpiForSystem() : static_cast<UINT>(kBaseDpi);
    if (sysDpi == 0) {
        sysDpi = kBaseDpi;
    }
    lf.lfHeight = MulDiv(lf.lfHeight, static_cast<int>(dpi), static_cast<int>(sysDpi));
    return CreateFontIndirectW(&lf);
}

// Create a child control from design-unit coordinates and apply the UI font.
HWND CreateScaledChild(UINT dpi, HFONT font, DWORD ex_style, const wchar_t* class_name,
                       const wchar_t* text, DWORD style, int x, int y, int w, int h,
                       HWND parent, HMENU id) {
    HWND child = CreateWindowExW(
        ex_style, class_name, text, style,
        Scaled(dpi, x), Scaled(dpi, y), Scaled(dpi, w), Scaled(dpi, h),
        parent, id, GetModuleHandleW(nullptr), nullptr);
    if (child && font) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return child;
}

// Outer window size, in device pixels, for a client area given in design units.
void ScaledWindowSize(UINT dpi, DWORD style, DWORD ex_style, int client_w, int client_h,
                      int& out_w, int& out_h) {
    RECT rc{ 0, 0, Scaled(dpi, client_w), Scaled(dpi, client_h) };
    if (AdjustWindowRectExForDpi(&rc, style, FALSE, ex_style, dpi) == 0) {
        rc.right += Scaled(dpi, 16);
        rc.bottom += Scaled(dpi, 39);
    }
    out_w = rc.right - rc.left;
    out_h = rc.bottom - rc.top;
}

SetupWizard::SetupWizard() {
    preferences_.config_file_path = GetDefaultConfigPath();
    LoadPreferences();
}

std::wstring SetupWizard::GetDefaultConfigPath() {
    // 1. Portable Mode: check if executable directory is writable
    wchar_t exePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
            std::wstring portablePath = std::wstring(exePath) + L"carplaystation_ui.ini";
            HANDLE hTest = CreateFileW(
                portablePath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (hTest != INVALID_HANDLE_VALUE) {
                CloseHandle(hTest);
                return portablePath; // Portable green version confirmed
            }
        }
    }

    // 2. Installed Mode: fallback to %APPDATA%\CarPlayStation\carplaystation_ui.ini
    wchar_t appData[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\CarPlayStation";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\carplaystation_ui.ini";
    }

    return L"carplaystation_ui.ini";
}

void SetupWizard::LoadPreferences() {
    UINT val = GetPrivateProfileIntW(
        L"Preferences",
        L"SafetyDisclaimerDismissed",
        0,
        preferences_.config_file_path.c_str()
    );
    preferences_.safety_disclaimer_dismissed = (val == 1);

    val = GetPrivateProfileIntW(
        L"Preferences",
        L"FirstRunCompleted",
        0,
        preferences_.config_file_path.c_str()
    );
    preferences_.first_run_completed = (val == 1);
}

void SetupWizard::SavePreferences() {
    WritePrivateProfileStringW(
        L"Preferences",
        L"SafetyDisclaimerDismissed",
        preferences_.safety_disclaimer_dismissed ? L"1" : L"0",
        preferences_.config_file_path.c_str()
    );

    WritePrivateProfileStringW(
        L"Preferences",
        L"FirstRunCompleted",
        preferences_.first_run_completed ? L"1" : L"0",
        preferences_.config_file_path.c_str()
    );
}

// --- MFi settings persistence ------------------------------------------------
// The MFi backend selection is NOT part of the wizard flags above; it lives in
// Application::AppSettings (in-memory). We mirror it into the same INI under a
// dedicated "MFi" section so the choice survives an app restart.

namespace {

// Read a UTF-8 string value from the INI; returns empty if the key is absent.
std::string ReadIniUtf8(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[4096] = { 0 };
    DWORD n = GetPrivateProfileStringW(
        section, key, L"", buffer,
        static_cast<DWORD>(std::size(buffer)), ini.c_str());
    if (n == 0) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(n), result.data(), len, nullptr, nullptr);
    return result;
}

// Write a UTF-8 string value into the INI.
void WriteIniUtf8(const std::wstring& ini, const wchar_t* section, const wchar_t* key,
                  const std::string& utf8_value) {
    if (utf8_value.empty()) {
        return;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_value.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        return;
    }
    std::wstring wvalue(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_value.c_str(), -1, wvalue.data(), wlen);
    WritePrivateProfileStringW(section, key, wvalue.c_str(), ini.c_str());
}

} // namespace

namespace {

// Case-insensitive string equality check
bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Trim whitespace from string
std::string Trim(std::string_view s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(start, end - start + 1));
}

} // namespace

void SetupWizard::LoadMFiSettings(Application::AppSettings& settings) const {
    const auto& ini = preferences_.config_file_path;

    // Backend type: support readable text names as well as legacy numeric values.
    // Text: HardwareI2C, Hardware, I2C, CH341, CH347, FTDI -> HardwareI2C
    //       RemoteHttp, Remote, Network, Http               -> RemoteHttp
    //       LocalFile, Local, File                         -> LocalFile
    // Numeric: 0 or 1 -> LocalFile (Option 1 in UI, or enum 0)
    //          2      -> RemoteHttp (Option 2 in UI) OR legacy HardwareI2C (if host is empty)
    //          3      -> HardwareI2C (Option 3 in UI) OR RemoteHttp (enum 3)
    std::string backend_str = Trim(ReadIniUtf8(ini, L"MFi", L"BackendType"));
    if (!backend_str.empty()) {
        if (EqualsIgnoreCase(backend_str, "HardwareI2C") ||
            EqualsIgnoreCase(backend_str, "Hardware") ||
            EqualsIgnoreCase(backend_str, "I2C") ||
            EqualsIgnoreCase(backend_str, "CH341") ||
            EqualsIgnoreCase(backend_str, "CH347") ||
            EqualsIgnoreCase(backend_str, "FTDI")) {
            settings.mfi.backend_type = Application::MFiBackendType::HardwareI2C;
        } else if (EqualsIgnoreCase(backend_str, "RemoteHttp") ||
                   EqualsIgnoreCase(backend_str, "Remote") ||
                   EqualsIgnoreCase(backend_str, "Network") ||
                   EqualsIgnoreCase(backend_str, "Http")) {
            settings.mfi.backend_type = Application::MFiBackendType::RemoteHttp;
        } else if (EqualsIgnoreCase(backend_str, "LocalFile") ||
                   EqualsIgnoreCase(backend_str, "Local") ||
                   EqualsIgnoreCase(backend_str, "File")) {
            settings.mfi.backend_type = Application::MFiBackendType::LocalFile;
        } else {
            // Numeric parsing
            try {
                int backend_num = std::stoi(backend_str);
                if (backend_num == 0) {
                    settings.mfi.backend_type = Application::MFiBackendType::LocalFile;
                } else if (backend_num == 1) {
                    // UI Option 1: LocalFile. (Or legacy TCP daemon migrated to RemoteHttp)
                    // If cert paths exist or are referenced, it's LocalFile; default to LocalFile for 1.
                    settings.mfi.backend_type = Application::MFiBackendType::LocalFile;
                } else if (backend_num == 2) {
                    // Could be UI Option 2 (RemoteHttp) or enum value 2 (HardwareI2C).
                    // Distinguish by checking whether RemoteHost was specified:
                    std::string r_host = ReadIniUtf8(ini, L"MFi", L"RemoteHost");
                    if (!r_host.empty()) {
                        settings.mfi.backend_type = Application::MFiBackendType::RemoteHttp;
                    } else {
                        settings.mfi.backend_type = Application::MFiBackendType::HardwareI2C;
                    }
                } else if (backend_num == 3) {
                    // Could be UI Option 3 (HardwareI2C) or enum value 3 (RemoteHttp).
                    // If RemoteHost was specified, treat as RemoteHttp; else HardwareI2C.
                    std::string r_host = ReadIniUtf8(ini, L"MFi", L"RemoteHost");
                    if (!r_host.empty()) {
                        settings.mfi.backend_type = Application::MFiBackendType::RemoteHttp;
                    } else {
                        settings.mfi.backend_type = Application::MFiBackendType::HardwareI2C;
                    }
                }
            } catch (...) {}
        }
    }

    // LocalFile paths.
    std::string cert = ReadIniUtf8(ini, L"MFi", L"CertificatePath");
    if (!cert.empty()) {
        settings.mfi.certificate_path = std::move(cert);
    }
    std::string key = ReadIniUtf8(ini, L"MFi", L"PrivateKeyPath");
    if (!key.empty()) {
        settings.mfi.private_key_path = std::move(key);
    }

    // RemoteHttp endpoint.
    std::string remote_host = ReadIniUtf8(ini, L"MFi", L"RemoteHost");
    if (!remote_host.empty()) {
        settings.mfi.remote_mfi.host = std::move(remote_host);
    }
    UINT remote_port = GetPrivateProfileIntW(L"MFi", L"RemotePort", 0, ini.c_str());
    if (remote_port != 0) {
        settings.mfi.remote_mfi.port = static_cast<uint16_t>(remote_port);
    }
    std::string remote_token = ReadIniUtf8(ini, L"MFi", L"RemoteToken");
    if (!remote_token.empty()) {
        settings.mfi.remote_mfi.bearer_token = std::move(remote_token);
    }

    // HardwareI2C preferences.
    std::string driver_str = Trim(ReadIniUtf8(ini, L"MFi", L"I2cDriver"));
    if (!driver_str.empty()) {
        if (EqualsIgnoreCase(driver_str, "CH341")) {
            settings.mfi.hardware_i2c.driver_type = Application::HardwareI2cDriverType::CH341;
        } else if (EqualsIgnoreCase(driver_str, "CH347")) {
            settings.mfi.hardware_i2c.driver_type = Application::HardwareI2cDriverType::CH347;
        } else if (EqualsIgnoreCase(driver_str, "FTDI")) {
            settings.mfi.hardware_i2c.driver_type = Application::HardwareI2cDriverType::FTDI;
        } else if (EqualsIgnoreCase(driver_str, "Mock")) {
            settings.mfi.hardware_i2c.driver_type = Application::HardwareI2cDriverType::Mock;
        } else if (EqualsIgnoreCase(driver_str, "Auto")) {
            settings.mfi.hardware_i2c.driver_type = Application::HardwareI2cDriverType::Auto;
        } else {
            try {
                int d = std::stoi(driver_str);
                if (d >= 0 && d <= 4) {
                    settings.mfi.hardware_i2c.driver_type = static_cast<Application::HardwareI2cDriverType>(d);
                }
            } catch (...) {}
        }
    }

    // Slave address: support hex (0x11, 0x10) or decimal (17, 16)
    std::string slave_str = Trim(ReadIniUtf8(ini, L"MFi", L"I2cSlaveAddress"));
    if (!slave_str.empty()) {
        try {
            int slave = std::stoi(slave_str, nullptr, 0); // handles 0x prefix
            if (slave > 0 && slave <= 127) {
                // If 0x10 was read, map to standard 0x11
                settings.mfi.hardware_i2c.slave_address = static_cast<uint8_t>(slave == 0x10 ? 0x11 : slave);
            }
        } catch (...) {}
    }

    UINT speed = GetPrivateProfileIntW(L"MFi", L"I2cSpeedKhz", 0, ini.c_str());
    if (speed != 0) {
        settings.mfi.hardware_i2c.speed_khz = speed;
    }
    UINT resetPin = GetPrivateProfileIntW(L"MFi", L"I2cResetGpioPin", 0, ini.c_str());
    if (resetPin != 0) {
        settings.mfi.hardware_i2c.reset_gpio_pin = static_cast<uint8_t>(resetPin);
    }
}

void SetupWizard::SaveMFiSettings(const Application::AppSettings& settings) const {
    const auto& ini = preferences_.config_file_path;
    const auto& mfi = settings.mfi;

    // Write readable backend name
    const wchar_t* backend_text = L"HardwareI2C";
    switch (mfi.backend_type) {
        case Application::MFiBackendType::LocalFile:   backend_text = L"LocalFile"; break;
        case Application::MFiBackendType::RemoteHttp:  backend_text = L"RemoteHttp"; break;
        case Application::MFiBackendType::HardwareI2C: backend_text = L"HardwareI2C"; break;
    }
    WritePrivateProfileStringW(L"MFi", L"BackendType", backend_text, ini.c_str());

    WriteIniUtf8(ini, L"MFi", L"CertificatePath", mfi.certificate_path);
    WriteIniUtf8(ini, L"MFi", L"PrivateKeyPath", mfi.private_key_path);

    WriteIniUtf8(ini, L"MFi", L"RemoteHost", mfi.remote_mfi.host);
    WritePrivateProfileStringW(
        L"MFi", L"RemotePort",
        std::to_wstring(static_cast<unsigned>(mfi.remote_mfi.port)).c_str(), ini.c_str());
    WriteIniUtf8(ini, L"MFi", L"RemoteToken", mfi.remote_mfi.bearer_token);

    // Write readable driver name
    const wchar_t* driver_text = L"Auto";
    switch (mfi.hardware_i2c.driver_type) {
        case Application::HardwareI2cDriverType::Auto:  driver_text = L"Auto"; break;
        case Application::HardwareI2cDriverType::CH341: driver_text = L"CH341"; break;
        case Application::HardwareI2cDriverType::CH347: driver_text = L"CH347"; break;
        case Application::HardwareI2cDriverType::FTDI:  driver_text = L"FTDI"; break;
        case Application::HardwareI2cDriverType::Mock:  driver_text = L"Mock"; break;
    }
    WritePrivateProfileStringW(L"MFi", L"I2cDriver", driver_text, ini.c_str());

    // Write address in standard hex format (0x11)
    wchar_t addr_buf[16];
    swprintf_s(addr_buf, L"0x%02X", mfi.hardware_i2c.slave_address);
    WritePrivateProfileStringW(L"MFi", L"I2cSlaveAddress", addr_buf, ini.c_str());

    WritePrivateProfileStringW(
        L"MFi", L"I2cSpeedKhz",
        std::to_wstring(mfi.hardware_i2c.speed_khz).c_str(), ini.c_str());
    WritePrivateProfileStringW(
        L"MFi", L"I2cResetGpioPin",
        std::to_wstring(static_cast<unsigned>(mfi.hardware_i2c.reset_gpio_pin)).c_str(), ini.c_str());
}

// --- Video / audio settings persistence ------------------------------------
// The settings drawer edits these at runtime, so they must survive a restart.
// Volumes are stored as whole percentages (0..100) instead of floats: the Win32
// profile API only has integer and string accessors, and an integer is far
// easier to hand-edit in the file. Resolution keys are read verbatim so an
// odd size typed into the file (1920x720 for instance) is honoured.

namespace {

// Smallest / largest custom frame we accept from the INI. Anything outside this
// range is treated as a typo and falls back to the built-in default.
constexpr uint32_t kMinCustomDimension = 320;
constexpr uint32_t kMaxCustomDimension = 4096;

// Read an integer percentage and turn it into a 0..1 volume. Absent key -> default.
float ReadIniPercentAsUnit(const std::wstring& ini, const wchar_t* section, const wchar_t* key,
                          int default_percent) {
    const int raw = static_cast<int>(GetPrivateProfileIntW(section, key, default_percent, ini.c_str()));
    // GetPrivateProfileIntW hands the default back as a UINT, so the "not present"
    // sentinel (-1) arrives as 0xFFFFFFFF. Report that as a negative value before
    // clamping so callers can restore their built-in default.
    if (raw < 0) return -1.0f;
    return static_cast<float>(std::clamp(raw, 0, 100)) / 100.0f;
}

// Write a 0..1 volume as a whole percentage.
void WriteUnitAsIniPercent(const std::wstring& ini, const wchar_t* section, const wchar_t* key, float value) {
    const int percent = static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 100.0f + 0.5f);
    WritePrivateProfileStringW(section, key, std::to_wstring(percent).c_str(), ini.c_str());
}

} // namespace

void SetupWizard::LoadVideoAudioSettings(Application::AppSettings& settings) const {
    const auto& ini = preferences_.config_file_path;

    // --- [Video] ---
    std::string preset_str = Trim(ReadIniUtf8(ini, L"Video", L"ResolutionPreset"));
    if (!preset_str.empty()) {
        if (EqualsIgnoreCase(preset_str, "720P") || EqualsIgnoreCase(preset_str, "Res720P")) {
            settings.video.resolution_preset = Application::VideoResolutionPreset::Res720P;
        } else if (EqualsIgnoreCase(preset_str, "1080P") || EqualsIgnoreCase(preset_str, "Res1080P")) {
            settings.video.resolution_preset = Application::VideoResolutionPreset::Res1080P;
        } else if (EqualsIgnoreCase(preset_str, "1440P") || EqualsIgnoreCase(preset_str, "2K") || EqualsIgnoreCase(preset_str, "Res1440P")) {
            settings.video.resolution_preset = Application::VideoResolutionPreset::Res1440P;
        } else if (EqualsIgnoreCase(preset_str, "2160P") || EqualsIgnoreCase(preset_str, "4K") || EqualsIgnoreCase(preset_str, "Res2160P")) {
            settings.video.resolution_preset = Application::VideoResolutionPreset::Res2160P;
        } else if (EqualsIgnoreCase(preset_str, "Auto")) {
            settings.video.resolution_preset = Application::VideoResolutionPreset::Auto;
        } else if (EqualsIgnoreCase(preset_str, "Custom")) {
            settings.video.resolution_preset = Application::VideoResolutionPreset::Custom;
        } else if (EqualsIgnoreCase(preset_str, "Car1280x480") || EqualsIgnoreCase(preset_str, "1280x480")) {
            settings.video.resolution_preset = Application::VideoResolutionPreset::Car1280x480;
        } else if (EqualsIgnoreCase(preset_str, "Car1920x720") || EqualsIgnoreCase(preset_str, "1920x720")) {
            settings.video.resolution_preset = Application::VideoResolutionPreset::Car1920x720;
        } else {
            try {
                int p = std::stoi(preset_str);
                if (p >= 0 && p <= 7) {
                    settings.video.resolution_preset = static_cast<Application::VideoResolutionPreset>(p);
                }
            } catch (...) {}
        }
    }

    // A hand-written custom size is accepted as-is, then clamped into a legal
    // range and aligned to even pixels (what every hardware decoder wants).
    UINT cw = GetPrivateProfileIntW(L"Video", L"CustomWidth", 0, ini.c_str());
    UINT ch = GetPrivateProfileIntW(L"Video", L"CustomHeight", 0, ini.c_str());
    if (cw > 0 && ch > 0) {
        if (cw >= kMinCustomDimension && cw <= kMaxCustomDimension &&
            ch >= kMinCustomDimension && ch <= kMaxCustomDimension) {
            settings.video.custom_width = (cw + 1) & ~1u;
            settings.video.custom_height = (ch + 1) & ~1u;

            // If the user specified a custom dimension in the INI file and preset
            // was set to Custom or not specified, ensure Custom mode is active.
            if (settings.video.resolution_preset == Application::VideoResolutionPreset::Custom ||
                (preset_str.empty() && (cw != 1280 || ch != 720))) {
                settings.video.resolution_preset = Application::VideoResolutionPreset::Custom;
            }
        } else {
            CPS_LOG_WARN("Settings",
                "Ignoring out-of-range custom resolution ", std::to_string(cw), "x", std::to_string(ch));
            settings.video.resolution_preset = Application::VideoResolutionPreset::Res720P;
        }
    }

    UINT fps = GetPrivateProfileIntW(L"Video", L"Fps", 0, ini.c_str());
    if (fps == 30 || fps == 60) {
        settings.video.fps = fps;
    }

    std::string codec_str = Trim(ReadIniUtf8(ini, L"Video", L"PreferredCodec"));
    if (!codec_str.empty()) {
        if (EqualsIgnoreCase(codec_str, "H264") || EqualsIgnoreCase(codec_str, "AVC")) {
            settings.video.preferred_codec = Application::VideoCodecPreference::H264;
        } else if (EqualsIgnoreCase(codec_str, "HEVC") || EqualsIgnoreCase(codec_str, "H265")) {
            settings.video.preferred_codec = Application::VideoCodecPreference::HEVC;
        } else if (EqualsIgnoreCase(codec_str, "Auto")) {
            settings.video.preferred_codec = Application::VideoCodecPreference::Auto;
        } else {
            try {
                int c = std::stoi(codec_str);
                if (c >= 0 && c <= 2) {
                    settings.video.preferred_codec = static_cast<Application::VideoCodecPreference>(c);
                }
            } catch (...) {}
        }
    }

    std::string disp_str = Trim(ReadIniUtf8(ini, L"Video", L"DisplayMode"));
    if (!disp_str.empty()) {
        if (EqualsIgnoreCase(disp_str, "Windowed")) {
            settings.video.display_mode = Application::WindowDisplayMode::Windowed;
        } else if (EqualsIgnoreCase(disp_str, "BorderlessFullscreen") || EqualsIgnoreCase(disp_str, "Borderless")) {
            settings.video.display_mode = Application::WindowDisplayMode::BorderlessFullscreen;
        } else if (EqualsIgnoreCase(disp_str, "ExclusiveFullscreen") || EqualsIgnoreCase(disp_str, "Fullscreen")) {
            settings.video.display_mode = Application::WindowDisplayMode::ExclusiveFullscreen;
        } else {
            try {
                int dm = std::stoi(disp_str);
                if (dm >= 0 && dm <= 2) {
                    settings.video.display_mode = static_cast<Application::WindowDisplayMode>(dm);
                }
            } catch (...) {}
        }
    }

    UINT hwdec = GetPrivateProfileIntW(L"Video", L"HardwareDecode", 1, ini.c_str());
    settings.video.hardware_decode = (hwdec != 0);

    // --- [Audio] ---
    // Volumes and ducking ratio are user settings. SampleRate follows the
    // Windows default endpoint (with integer multiple downsampling to reduce SRC),
    // and bitrate is fixed at default 320 kbps.
    settings.audio.media_volume = ReadIniPercentAsUnit(ini, L"Audio", L"MediaVolume", -1);
    settings.audio.navigation_volume = ReadIniPercentAsUnit(ini, L"Audio", L"NavigationVolume", -1);
    settings.audio.telephony_volume = ReadIniPercentAsUnit(ini, L"Audio", L"TelephonyVolume", -1);
    settings.audio.ducking_ratio = ReadIniPercentAsUnit(ini, L"Audio", L"DuckingRatio", -1);
    // GetPrivateProfileIntW returns the default when the key is missing, so the
    // -1 default above means "not present": restore the built-in value instead.
    if (settings.audio.media_volume < 0.0f) settings.audio.media_volume = Application::AudioPreferences{}.media_volume;
    if (settings.audio.navigation_volume < 0.0f) settings.audio.navigation_volume = Application::AudioPreferences{}.navigation_volume;
    if (settings.audio.telephony_volume < 0.0f) settings.audio.telephony_volume = Application::AudioPreferences{}.telephony_volume;
    if (settings.audio.ducking_ratio < 0.0f) settings.audio.ducking_ratio = Application::AudioPreferences{}.ducking_ratio;
}

void SetupWizard::SaveVideoAudioSettings(const Application::AppSettings& settings) const {
    const auto& ini = preferences_.config_file_path;
    const auto& video = settings.video;
    const auto& audio = settings.audio;

    const wchar_t* preset_text = L"720P";
    switch (video.resolution_preset) {
        case Application::VideoResolutionPreset::Res720P:     preset_text = L"720P"; break;
        case Application::VideoResolutionPreset::Res1080P:    preset_text = L"1080P"; break;
        case Application::VideoResolutionPreset::Res1440P:    preset_text = L"2K"; break;
        case Application::VideoResolutionPreset::Res2160P:    preset_text = L"4K"; break;
        case Application::VideoResolutionPreset::Auto:        preset_text = L"Auto"; break;
        case Application::VideoResolutionPreset::Custom:      preset_text = L"Custom"; break;
        case Application::VideoResolutionPreset::Car1280x480: preset_text = L"Car1280x480"; break;
        case Application::VideoResolutionPreset::Car1920x720: preset_text = L"Car1920x720"; break;
    }
    WritePrivateProfileStringW(L"Video", L"ResolutionPreset", preset_text, ini.c_str());

    WritePrivateProfileStringW(L"Video", L"CustomWidth",
        std::to_wstring(video.custom_width).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Video", L"CustomHeight",
        std::to_wstring(video.custom_height).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Video", L"Fps",
        std::to_wstring(video.fps).c_str(), ini.c_str());

    const wchar_t* codec_text = L"H264";
    switch (video.preferred_codec) {
        case Application::VideoCodecPreference::H264: codec_text = L"H264"; break;
        case Application::VideoCodecPreference::HEVC: codec_text = L"HEVC"; break;
        case Application::VideoCodecPreference::Auto: codec_text = L"Auto"; break;
    }
    WritePrivateProfileStringW(L"Video", L"PreferredCodec", codec_text, ini.c_str());

    const wchar_t* mode_text = L"Windowed";
    switch (video.display_mode) {
        case Application::WindowDisplayMode::Windowed:             mode_text = L"Windowed"; break;
        case Application::WindowDisplayMode::BorderlessFullscreen: mode_text = L"BorderlessFullscreen"; break;
        case Application::WindowDisplayMode::ExclusiveFullscreen:  mode_text = L"ExclusiveFullscreen"; break;
    }
    WritePrivateProfileStringW(L"Video", L"DisplayMode", mode_text, ini.c_str());

    WritePrivateProfileStringW(L"Video", L"HardwareDecode",
        video.hardware_decode ? L"1" : L"0", ini.c_str());

    WriteUnitAsIniPercent(ini, L"Audio", L"MediaVolume", audio.media_volume);
    WriteUnitAsIniPercent(ini, L"Audio", L"NavigationVolume", audio.navigation_volume);
    WriteUnitAsIniPercent(ini, L"Audio", L"TelephonyVolume", audio.telephony_volume);
    WriteUnitAsIniPercent(ini, L"Audio", L"DuckingRatio", audio.ducking_ratio);
}

// --- Recorded device (iPhone) address persistence ---------------------------
// The last phone that joined the SoftAP is recorded under a dedicated "Device"
// section so the device does not need to be re-paired on the next launch.

void SetupWizard::LoadDeviceAddresses(std::string& bluetooth_mac, std::string& wifi_mac) const {
    const auto& ini = preferences_.config_file_path;
    bluetooth_mac = ReadIniUtf8(ini, L"Device", L"BluetoothMac");
    wifi_mac = ReadIniUtf8(ini, L"Device", L"WifiMac");
}

void SetupWizard::SaveDeviceAddresses(const std::string& bluetooth_mac, const std::string& wifi_mac) const {
    const auto& ini = preferences_.config_file_path;
    WriteIniUtf8(ini, L"Device", L"BluetoothMac", bluetooth_mac);
    WriteIniUtf8(ini, L"Device", L"WifiMac", wifi_mac);
}

// --- HID input capability ---------------------------------------------------
// Not persisted: the number of touch contacts advertised in /info comes from
// Core/Hardware/TouchscreenCapabilities, which reads the host's digitizer.

bool SetupWizard::ShouldShowMFiWizard(const Application::AppSettings& settings) const noexcept {
    if (!preferences_.first_run_completed) {
        return true;
    }
    if (settings.mfi.backend_type == Application::MFiBackendType::LocalFile) {
        // Re-show the wizard (with the import dialog) if the configured
        // certificate / key files are missing or don't exist on disk.
        if (settings.mfi.certificate_path.empty() || settings.mfi.private_key_path.empty()) {
            return true;
        }
        if (!FileExistsOnDisk(settings.mfi.certificate_path) ||
            !FileExistsOnDisk(settings.mfi.private_key_path)) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Safety Disclaimer Modal Implementation
// ============================================================================

struct DisclaimerDialogContext {
    int remaining_seconds{5};
    bool accepted{false};
    bool dont_show_again{false};
    HWND hwnd_btn{nullptr};
    HWND hwnd_chk{nullptr};
    UINT dpi{static_cast<UINT>(kBaseDpi)};
    HFONT font{nullptr};
};

static LRESULT CALLBACK DisclaimerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<DisclaimerDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<DisclaimerDialogContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            // Disclaimer text
            CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"STATIC",
                L"【行车安全须知】\r\n\r\n"
                L"1. 请严格遵守道路交通安全法规，行车途中请勿注视或操作屏幕。\r\n"
                L"2. 本软件仅供车载辅助互联与导航显示，驾驶员须全程保持专注于路况。\r\n"
                L"3. 点击确认即代表您已充分知晓并承诺安全驾驶。",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                24, 20, 492, 120,
                hwnd, nullptr
            );

            // "不再提示" Checkbox
            ctx->hwnd_chk = CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"BUTTON",
                L"我已熟知，下次启动不再提示",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                24, 150, 280, 26,
                hwnd, (HMENU)101
            );

            // Confirm button (initially disabled)
            ctx->hwnd_btn = CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"BUTTON",
                L"确认进入 (5s)",
                WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_DEFPUSHBUTTON,
                336, 146, 180, 34,
                hwnd, (HMENU)IDOK
            );

            SetTimer(hwnd, 1, 1000, nullptr);
            return 0;
        }

        case WM_TIMER: {
            if (ctx && ctx->remaining_seconds > 0) {
                ctx->remaining_seconds--;
                if (ctx->remaining_seconds > 0) {
                    std::wstring txt = L"确认进入 (" + std::to_wstring(ctx->remaining_seconds) + L"s)";
                    SetWindowTextW(ctx->hwnd_btn, txt.c_str());
                } else {
                    KillTimer(hwnd, 1);
                    SetWindowTextW(ctx->hwnd_btn, L"我已知晓并同意");
                    EnableWindow(ctx->hwnd_btn, TRUE);
                }
            }
            return 0;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDOK && ctx) {
                ctx->accepted = true;
                ctx->dont_show_again = (SendMessageW(ctx->hwnd_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                DestroyWindow(hwnd);
                return 0;
            } else if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_CLOSE: {
            DestroyWindow(hwnd);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool SetupWizard::ShowSafetyDisclaimer(HWND parent, bool headless) {
    if (preferences_.safety_disclaimer_dismissed) {
        return true;
    }
    if (headless) {
        preferences_.safety_disclaimer_dismissed = true;
        SavePreferences();
        return true;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = DisclaimerWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kDisclaimerWndClass;
    RegisterClassExW(&wc);

    DisclaimerDialogContext ctx;

    RECT parentRc = { 0 };
    if (parent) {
        GetWindowRect(parent, &parentRc);
    } else {
        parentRc.left = 100;
        parentRc.top = 100;
        parentRc.right = 1100;
        parentRc.bottom = 700;
    }

    // 按父窗口所在显示器的 DPI 放大窗口、控件与字体（进程为 Per-Monitor V2，系统不会拉伸）
    ctx.dpi = DialogDpi(parent);
    ctx.font = CreateScaledUiFont(ctx.dpi);

    int width = 0;
    int height = 0;
    ScaledWindowSize(ctx.dpi, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                     WS_EX_DLGMODALFRAME, 540, 210, width, height);
    int posX = parentRc.left + ((parentRc.right - parentRc.left) - width) / 2;
    int posY = parentRc.top + ((parentRc.bottom - parentRc.top) - height) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kDisclaimerWndClass,
        L"CarPlayStation — 安全警告须知",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        posX, posY, width, height,
        parent, nullptr, hInst, &ctx
    );

    if (!hDlg) {
        if (ctx.font) {
            DeleteObject(ctx.font);
        }
        return true;
    }

    // Modal message loop
    if (parent) {
        EnableWindow(parent, FALSE);
    }

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }

    if (ctx.font) {
        DeleteObject(ctx.font);
        ctx.font = nullptr;
    }

    if (ctx.accepted && ctx.dont_show_again) {
        preferences_.safety_disclaimer_dismissed = true;
        SavePreferences();
    }

    return ctx.accepted;
}

// ============================================================================
// First-Run MFi Setup Wizard Modal Implementation
// ============================================================================

// Default port offered when the port field is left empty (the service's own default).
constexpr int kHttpDefaultPort = 8972;

struct MfiWizardDialogContext {
    bool confirmed{false};
    Application::MFiBackendType selected_type{Application::MFiBackendType::LocalFile};
    HWND hwnd_rad_file{nullptr};
    HWND hwnd_rad_net{nullptr};
    HWND hwnd_rad_hw{nullptr};
    HWND hwnd_btn_browse_cert{nullptr};
    HWND hwnd_btn_browse_key{nullptr};
    HWND hwnd_lbl_cert{nullptr};
    HWND hwnd_lbl_key{nullptr};
    HWND hwnd_edit_host{nullptr};
    HWND hwnd_edit_port{nullptr};
    HWND hwnd_edit_token{nullptr};
    std::string cert_path;      // chosen certificate file (UTF-8)
    std::string key_path;       // chosen private key file (UTF-8)
    std::string host;           // service endpoint (prefilled from settings)
    std::string port;
    std::string token;          // optional bearer token
    UINT dpi{static_cast<UINT>(kBaseDpi)};
    HFONT font{nullptr};
};

// Open a Win32 file picker; returns the chosen path as UTF-8 (empty on cancel).
static std::string PickFile(HWND owner, const wchar_t* title, const wchar_t* filter_desc) {
    wchar_t buffer[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter_desc;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = title;
    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, result.data(), len, nullptr, nullptr);
    return result;
}

// Show a UTF-8 path in a static label (truncated to fit).
static void SetLabelFromUtf8(HWND label, const std::string& utf8_path) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        return;
    }
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, wpath.data(), wlen);
    SetWindowTextW(label, wpath.c_str());
}

// Convert a UTF-8 string to a wide string (for pre-filling edit controls).
static std::wstring Utf8ToWideLocal(const std::string& utf8_value) {
    if (utf8_value.empty()) {
        return {};
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_value.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        return {};
    }
    std::wstring wvalue(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_value.c_str(), -1, wvalue.data(), wlen);
    return wvalue;
}

// Read the current text of an edit control as UTF-8, trimmed of ASCII spaces.
static std::string GetEditUtf8(HWND edit) {
    if (!edit) {
        return {};
    }
    int len = GetWindowTextLengthW(edit);
    if (len <= 0) {
        return {};
    }
    std::wstring wtext(static_cast<size_t>(len), L'\0');
    GetWindowTextW(edit, wtext.data(), len + 1);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(u8len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), -1, utf8.data(), u8len, nullptr, nullptr);
    utf8.resize(strlen(utf8.c_str()));
    size_t b = utf8.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    size_t e = utf8.find_last_not_of(" \t\r\n");
    return utf8.substr(b, e - b + 1);
}

// Validate a decimal port string; returns the port, or 0 when out of range.
static int ParsePortField(const std::string& text) {
    if (text.empty() || text.size() > 5) {
        return 0;
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            return 0;
        }
    }
    long value = std::strtol(text.c_str(), nullptr, 10);
    if (value < 1 || value > 65535) {
        return 0;
    }
    return static_cast<int>(value);
}

static LRESULT CALLBACK MfiWizardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<MfiWizardDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<MfiWizardDialogContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            // Intro text
            CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"STATIC",
                L"欢迎使用 CarPlayStation！初次启动请选择 Apple MFi 认证通道：\r\n\r\n"
                L"【法律合规声明】本软件为中立开源互联引擎，不内置任何专有证书或私钥材料。"
                L"请依据自身环境选择合适的方式：",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                24, 16, 540, 80,
                hwnd, nullptr
            );

            // Radio 1: Local File
            ctx->hwnd_rad_file = CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"BUTTON",
                L"1. 本地证书/私钥文件导入 (LocalFile: 默认读取 certs/ 目录)",
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                24, 104, 540, 24,
                hwnd, (HMENU)201
            );

            // Local-file import: browse buttons + path labels (only meaningful
            // when the LocalFile backend is selected).
            ctx->hwnd_btn_browse_cert = CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"BUTTON",
                L"选择证书文件 (.p7b)…",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                40, 134, 165, 26,
                hwnd, (HMENU)210
            );
            ctx->hwnd_lbl_cert = CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"STATIC",
                L"(未选择证书文件)",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                215, 137, 345, 20,
                hwnd, nullptr
            );

            ctx->hwnd_btn_browse_key = CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"BUTTON",
                L"选择私钥文件 (.pk8)…",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                40, 166, 165, 26,
                hwnd, (HMENU)211
            );
            ctx->hwnd_lbl_key = CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"STATIC",
                L"(未选择私钥文件)",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                215, 169, 345, 20,
                hwnd, nullptr
            );

            // Radio 2: network signing service (HTTP JSON API).
            ctx->hwnd_rad_net = CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"BUTTON",
                L"2. 网络 MFi 认证服务 (远程签名服务, 需填写服务地址与端口)",
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                24, 200, 540, 24,
                hwnd, (HMENU)202
            );
            CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"STATIC", L"服务地址:",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                40, 232, 64, 20,
                hwnd, nullptr
            );
            ctx->hwnd_edit_host = CreateScaledChild(
                ctx->dpi, ctx->font,
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                108, 230, 196, 24,
                hwnd, (HMENU)222
            );
            CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"STATIC", L"端口:",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                314, 232, 40, 20,
                hwnd, nullptr
            );
            ctx->hwnd_edit_port = CreateScaledChild(
                ctx->dpi, ctx->font,
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP,
                358, 230, 60, 24,
                hwnd, (HMENU)223
            );
            CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"STATIC", L"令牌(可选):",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                40, 264, 84, 20,
                hwnd, nullptr
            );
            ctx->hwnd_edit_token = CreateScaledChild(
                ctx->dpi, ctx->font,
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP,
                128, 262, 290, 24,
                hwnd, (HMENU)224
            );
            CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"STATIC",
                L"提示: 服务提供 GET /mfi/certificate、POST /mfi/sign、POST /mfi/reset。",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                40, 294, 520, 22,
                hwnd, nullptr
            );

            // Radio 3: Hardware I2C Bridge
            ctx->hwnd_rad_hw = CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"BUTTON",
                L"3. 物理 USB-to-I2C 桥接芯片 (HardwareI2C: CH341/CH347/FTDI)",
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                24, 324, 540, 24,
                hwnd, (HMENU)203
            );

            // Pre-fill the endpoint fields from the current settings.
            SetWindowTextW(ctx->hwnd_edit_host, Utf8ToWideLocal(ctx->host).c_str());
            SetWindowTextW(ctx->hwnd_edit_port, Utf8ToWideLocal(ctx->port).c_str());
            SetWindowTextW(ctx->hwnd_edit_token, Utf8ToWideLocal(ctx->token).c_str());
            SetLabelFromUtf8(ctx->hwnd_lbl_cert, ctx->cert_path.empty() ? "(未选择证书文件)" : ctx->cert_path);
            SetLabelFromUtf8(ctx->hwnd_lbl_key, ctx->key_path.empty() ? "(未选择私钥文件)" : ctx->key_path);
            if (ctx->port.empty()) {
                SetWindowTextW(ctx->hwnd_edit_port, Utf8ToWideLocal(std::to_string(kHttpDefaultPort)).c_str());
            }

            // Restore the previously selected backend.
            HWND initial = ctx->hwnd_rad_file;
            switch (ctx->selected_type) {
                case Application::MFiBackendType::RemoteHttp: initial = ctx->hwnd_rad_net; break;
                case Application::MFiBackendType::HardwareI2C: initial = ctx->hwnd_rad_hw; break;
                case Application::MFiBackendType::LocalFile:
                default: break;
            }
            SendMessageW(initial, BM_SETCHECK, BST_CHECKED, 0);

            // Confirm button
            CreateScaledChild(
                ctx->dpi, ctx->font,
                0, L"BUTTON",
                L"确定并进入",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                394, 360, 146, 34,
                hwnd, (HMENU)IDOK
            );

            return 0;
        }

        case WM_COMMAND: {
            if (!ctx) break;
            // Browse for the certificate file.
            if (LOWORD(wParam) == 210) {
                std::string picked = PickFile(hwnd, L"选择 MFi 证书文件", L"证书文件 (*.p7b;*.cer;*.crt)\0*.p7b;*.cer;*.crt\0所有文件 (*.*)\0*.*\0");
                if (!picked.empty()) {
                    ctx->cert_path = picked;
                    SetLabelFromUtf8(ctx->hwnd_lbl_cert, picked);
                }
                return 0;
            }
            // Browse for the private key file.
            if (LOWORD(wParam) == 211) {
                std::string picked = PickFile(hwnd, L"选择 MFi 私钥文件", L"私钥文件 (*.pk8;*.pem;*.key)\0*.pk8;*.pem;*.key\0所有文件 (*.*)\0*.*\0");
                if (!picked.empty()) {
                    ctx->key_path = picked;
                    SetLabelFromUtf8(ctx->hwnd_lbl_key, picked);
                }
                return 0;
            }
            if (LOWORD(wParam) == IDOK) {
                if (SendMessageW(ctx->hwnd_rad_file, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    ctx->selected_type = Application::MFiBackendType::LocalFile;
                } else if (SendMessageW(ctx->hwnd_rad_net, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    ctx->selected_type = Application::MFiBackendType::RemoteHttp;
                } else if (SendMessageW(ctx->hwnd_rad_hw, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    ctx->selected_type = Application::MFiBackendType::HardwareI2C;
                }

                // Read the endpoint fields. Only the network option needs them, so
                // blank fields never block the other backends.
                ctx->host = GetEditUtf8(ctx->hwnd_edit_host);
                ctx->port = GetEditUtf8(ctx->hwnd_edit_port);
                ctx->token = GetEditUtf8(ctx->hwnd_edit_token);

                if (ctx->selected_type == Application::MFiBackendType::RemoteHttp) {
                    if (ctx->host.empty() || ParsePortField(ctx->port) == 0) {
                        MessageBoxW(hwnd, L"请填写网络 MFi 认证服务的地址与端口 (端口范围 1-65535)。",
                                    L"参数无效", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                }

                ctx->confirmed = true;
                DestroyWindow(hwnd);
                return 0;
            } else if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_CLOSE: {
            DestroyWindow(hwnd);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool SetupWizard::ShowMFiWizard(HWND parent, Application::AppSettings& settings, bool headless) {
    if (headless) {
        preferences_.first_run_completed = true;
        SavePreferences();
        return true;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MfiWizardWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kMfiWizardWndClass;
    RegisterClassExW(&wc);

    MfiWizardDialogContext ctx;
    // Pre-fill the dialog from the current settings so the user edits real values.
    ctx.selected_type = settings.mfi.backend_type;
    ctx.cert_path = settings.mfi.certificate_path;
    ctx.key_path = settings.mfi.private_key_path;
    ctx.host = settings.mfi.remote_mfi.host;
    ctx.port = std::to_string(settings.mfi.remote_mfi.port);
    ctx.token = settings.mfi.remote_mfi.bearer_token;

    RECT parentRc = { 0 };
    if (parent) {
        GetWindowRect(parent, &parentRc);
    } else {
        parentRc.left = 100;
        parentRc.top = 100;
        parentRc.right = 1100;
        parentRc.bottom = 700;
    }

    // 按父窗口所在显示器的 DPI 放大窗口、控件与字体（进程为 Per-Monitor V2，系统不会拉伸）
    ctx.dpi = DialogDpi(parent);
    ctx.font = CreateScaledUiFont(ctx.dpi);

    int width = 0;
    int height = 0;
    ScaledWindowSize(ctx.dpi, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                     WS_EX_DLGMODALFRAME, 580, 414, width, height);
    int posX = parentRc.left + ((parentRc.right - parentRc.left) - width) / 2;
    int posY = parentRc.top + ((parentRc.bottom - parentRc.top) - height) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kMfiWizardWndClass,
        L"CarPlayStation — 首次运行认证向导",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        posX, posY, width, height,
        parent, nullptr, hInst, &ctx
    );

    if (!hDlg) {
        if (ctx.font) {
            DeleteObject(ctx.font);
        }
        return true;
    }

    if (parent) {
        EnableWindow(parent, FALSE);
    }

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }

    if (ctx.font) {
        DeleteObject(ctx.font);
        ctx.font = nullptr;
    }

    if (ctx.confirmed) {
        settings.mfi.backend_type = ctx.selected_type;
        // Persist the chosen local-file paths (only used by the LocalFile backend).
        if (!ctx.cert_path.empty()) {
            settings.mfi.certificate_path = ctx.cert_path;
        }
        if (!ctx.key_path.empty()) {
            settings.mfi.private_key_path = ctx.key_path;
        }
        // Persist the typed endpoint.
        if (int port = ParsePortField(ctx.port); !ctx.host.empty() && port != 0) {
            if (ctx.selected_type == Application::MFiBackendType::RemoteHttp) {
                settings.mfi.remote_mfi.host = ctx.host;
                settings.mfi.remote_mfi.port = static_cast<uint16_t>(port);
                settings.mfi.remote_mfi.bearer_token = ctx.token;
            }
        }
        preferences_.first_run_completed = true;
        SavePreferences();
        SaveMFiSettings(settings);
    }

    return ctx.confirmed;
}

} // namespace CarPlayStation::UI
