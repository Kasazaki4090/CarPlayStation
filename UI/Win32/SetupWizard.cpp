#include "SetupWizard.h"
#include <commctrl.h>
#include <shlobj.h>
#include <commdlg.h>
#include <fstream>
#include <vector>

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

void SetupWizard::LoadMFiSettings(Application::AppSettings& settings) const {
    const auto& ini = preferences_.config_file_path;

    // Backend type (only override when the key is present).
    UINT backend = GetPrivateProfileIntW(L"MFi", L"BackendType", -1, ini.c_str());
    if (backend != static_cast<UINT>(-1)) {
        settings.mfi.backend_type = static_cast<Application::MFiBackendType>(backend);
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

    // NetworkDaemon endpoint.
    std::string host = ReadIniUtf8(ini, L"MFi", L"DaemonHost");
    if (!host.empty()) {
        settings.mfi.daemon_endpoint.address = std::move(host);
    }
    UINT port = GetPrivateProfileIntW(L"MFi", L"DaemonPort", 0, ini.c_str());
    if (port != 0) {
        settings.mfi.daemon_endpoint.port = static_cast<uint16_t>(port);
    }

    // HardwareI2C preferences.
    UINT driver = GetPrivateProfileIntW(L"MFi", L"I2cDriver", -1, ini.c_str());
    if (driver != static_cast<UINT>(-1)) {
        settings.mfi.hardware_i2c.driver_type = static_cast<Application::HardwareI2cDriverType>(driver);
    }
    UINT slave = GetPrivateProfileIntW(L"MFi", L"I2cSlaveAddress", 0, ini.c_str());
    if (slave != 0) {
        // Older releases persisted the default 0x10; the known MFi coprocessor uses 0x11.
        settings.mfi.hardware_i2c.slave_address = static_cast<uint8_t>(slave == 0x10 ? 0x11 : slave);
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

    WritePrivateProfileStringW(
        L"MFi", L"BackendType",
        std::to_wstring(static_cast<unsigned>(mfi.backend_type)).c_str(), ini.c_str());

    WriteIniUtf8(ini, L"MFi", L"CertificatePath", mfi.certificate_path);
    WriteIniUtf8(ini, L"MFi", L"PrivateKeyPath", mfi.private_key_path);

    WriteIniUtf8(ini, L"MFi", L"DaemonHost", mfi.daemon_endpoint.address);
    WritePrivateProfileStringW(
        L"MFi", L"DaemonPort",
        std::to_wstring(static_cast<unsigned>(mfi.daemon_endpoint.port)).c_str(), ini.c_str());

    WritePrivateProfileStringW(
        L"MFi", L"I2cDriver",
        std::to_wstring(static_cast<unsigned>(mfi.hardware_i2c.driver_type)).c_str(), ini.c_str());
    WritePrivateProfileStringW(
        L"MFi", L"I2cSlaveAddress",
        std::to_wstring(static_cast<unsigned>(mfi.hardware_i2c.slave_address)).c_str(), ini.c_str());
    WritePrivateProfileStringW(
        L"MFi", L"I2cSpeedKhz",
        std::to_wstring(mfi.hardware_i2c.speed_khz).c_str(), ini.c_str());
    WritePrivateProfileStringW(
        L"MFi", L"I2cResetGpioPin",
        std::to_wstring(static_cast<unsigned>(mfi.hardware_i2c.reset_gpio_pin)).c_str(), ini.c_str());
}

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
};

static LRESULT CALLBACK DisclaimerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<DisclaimerDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<DisclaimerDialogContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            // Disclaimer text
            CreateWindowExW(
                0, L"STATIC",
                L"【行车安全须知】\r\n\r\n"
                L"1. 请严格遵守道路交通安全法规，行车途中请勿注视或操作屏幕。\r\n"
                L"2. 本软件仅供车载辅助互联与导航显示，驾驶员须全程保持专注于路况。\r\n"
                L"3. 点击确认即代表您已充分知晓并承诺安全驾驶。",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                24, 20, 480, 110,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
            );

            // "不再提示" Checkbox
            ctx->hwnd_chk = CreateWindowExW(
                0, L"BUTTON",
                L"我已熟知，下次启动不再提示",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                24, 140, 280, 24,
                hwnd, (HMENU)101, GetModuleHandleW(nullptr), nullptr
            );

            // Confirm button (initially disabled)
            ctx->hwnd_btn = CreateWindowExW(
                0, L"BUTTON",
                L"确认进入 (5s)",
                WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_DEFPUSHBUTTON,
                340, 138, 160, 32,
                hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr
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

    int width = 540;
    int height = 230;
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

    if (ctx.accepted && ctx.dont_show_again) {
        preferences_.safety_disclaimer_dismissed = true;
        SavePreferences();
    }

    return ctx.accepted;
}

// ============================================================================
// First-Run MFi Setup Wizard Modal Implementation
// ============================================================================

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
    std::string cert_path;   // chosen certificate file (UTF-8)
    std::string key_path;    // chosen private key file (UTF-8)
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

static LRESULT CALLBACK MfiWizardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<MfiWizardDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<MfiWizardDialogContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            // Intro text
            CreateWindowExW(
                0, L"STATIC",
                L"欢迎使用 CarPlayStation！初次启动请选择 Apple MFi 认证通道：\r\n\r\n"
                L"【法律合规声明】本软件为中立开源互联引擎，不内置任何专有证书或私钥材料。"
                L"请依据自身环境选择合适的方式：",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                24, 16, 520, 64,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
            );

            // Radio 1: Local File
            ctx->hwnd_rad_file = CreateWindowExW(
                0, L"BUTTON",
                L"1. 本地证书/私钥文件导入 (LocalFile: 默认读取 certs/ 目录)",
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                24, 88, 500, 24,
                hwnd, (HMENU)201, GetModuleHandleW(nullptr), nullptr
            );

            // Radio 2: Network Daemon
            ctx->hwnd_rad_net = CreateWindowExW(
                0, L"BUTTON",
                L"2. TCP 网络签名守护服务 (NetworkDaemon: 默认 127.0.0.1:9000)",
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                24, 116, 500, 24,
                hwnd, (HMENU)202, GetModuleHandleW(nullptr), nullptr
            );

            // Radio 3: Hardware I2C Bridge
            ctx->hwnd_rad_hw = CreateWindowExW(
                0, L"BUTTON",
                L"3. 物理 USB-to-I2C 桥接芯片 (HardwareI2C: CH341/CH347/FTDI)",
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                24, 144, 500, 24,
                hwnd, (HMENU)203, GetModuleHandleW(nullptr), nullptr
            );

            // Select Radio 1 by default
            SendMessageW(ctx->hwnd_rad_file, BM_SETCHECK, BST_CHECKED, 0);

            // Local-file import: browse buttons + path labels (only meaningful
            // when the LocalFile backend is selected).
            ctx->hwnd_btn_browse_cert = CreateWindowExW(
                0, L"BUTTON",
                L"选择证书文件 (.p7b)…",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                24, 172, 150, 28,
                hwnd, (HMENU)210, GetModuleHandleW(nullptr), nullptr
            );
            ctx->hwnd_lbl_cert = CreateWindowExW(
                0, L"STATIC",
                L"(未选择证书文件)",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                184, 176, 360, 20,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
            );

            ctx->hwnd_btn_browse_key = CreateWindowExW(
                0, L"BUTTON",
                L"选择私钥文件 (.pk8)…",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                24, 204, 150, 28,
                hwnd, (HMENU)211, GetModuleHandleW(nullptr), nullptr
            );
            ctx->hwnd_lbl_key = CreateWindowExW(
                0, L"STATIC",
                L"(未选择私钥文件)",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                184, 208, 360, 20,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
            );

            // Confirm button
            CreateWindowExW(
                0, L"BUTTON",
                L"确定并进入",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                380, 240, 140, 32,
                hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr
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
                    ctx->selected_type = Application::MFiBackendType::NetworkDaemon;
                } else if (SendMessageW(ctx->hwnd_rad_hw, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    ctx->selected_type = Application::MFiBackendType::HardwareI2C;
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

    RECT parentRc = { 0 };
    if (parent) {
        GetWindowRect(parent, &parentRc);
    } else {
        parentRc.left = 100;
        parentRc.top = 100;
        parentRc.right = 1100;
        parentRc.bottom = 700;
    }

    int width = 570;
    int height = 300;
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

    if (ctx.confirmed) {
        settings.mfi.backend_type = ctx.selected_type;
        // Persist the chosen local-file paths (only used by the LocalFile backend).
        if (!ctx.cert_path.empty()) {
            settings.mfi.certificate_path = ctx.cert_path;
        }
        if (!ctx.key_path.empty()) {
            settings.mfi.private_key_path = ctx.key_path;
        }
        preferences_.first_run_completed = true;
        SavePreferences();
    }

    return ctx.confirmed;
}

} // namespace CarPlayStation::UI
