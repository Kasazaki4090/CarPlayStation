#include "Application/ApplicationService.h"
#include "Core/Service/CarPlayCoreService.h"
#include "Core/Common/Logging.h"
#include "AppController.h"
#include <iostream>
#include <string>
#include <string_view>
#include <string.h>
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <cstdio>

namespace {
// Route all log output to a file next to the executable (append mode) so the
// background activity (Bluetooth / iAP2 / MFi auth) is visible in a GUI app.
void AttachFileLogSink() {
    wchar_t exePath[MAX_PATH] = { 0 };
    std::string logPath = "carplaystation.log";
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::wstring dir(exePath);
        const auto pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            dir.resize(pos + 1);
            dir += L"carplaystation.log";
            const int len = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(),
                                                 static_cast<int>(dir.size()),
                                                 nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                logPath.assign(static_cast<size_t>(len), '\0');
                WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()),
                                    logPath.data(), len, nullptr, nullptr);
            }
        }
    }
    auto& logger = CarPlayStation::Core::Common::Logger::Instance();
    auto multi = std::make_shared<CarPlayStation::Core::Common::detail::MultiLogSink>();
    multi->Add(std::make_shared<CarPlayStation::Core::Common::detail::ConsoleLogSink>());
    multi->Add(std::make_shared<CarPlayStation::Core::Common::detail::FileLogSink>(logPath));
    // Also feed the in-memory ring buffer that the GUI log panel renders, so
    // background activity is visible inside the app (kept separate from the
    // video/HUD, per the "单独分开放" requirement).
    multi->Add(&CarPlayStation::Core::Common::detail::RingBufferLogSink::Instance());
    logger.SetSink(multi);
}

// Install a top-level exception filter that writes a full minidump next to the
// executable on any unhandled exception (e.g. heap corruption). This lets us
// capture the real crash stack in a normal (non-debugger) run, where the
// debugger's timing masks the race.
LONG WINAPI CrashDumpFilter(EXCEPTION_POINTERS* ep) {
    (void)ep;
    wchar_t exePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    std::wstring dir(exePath);
    const auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        dir.resize(pos + 1);
    }
    std::wstring dumpPath = dir + L"crash_dump.dmp";
    HANDLE hFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD pid = GetCurrentProcessId();
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ExceptionPointers = ep;
        info.ThreadId = pid;
        // Numeric MINIDUMP_TYPE flags (the SDK's dbghelp.h predates the named
        // constants): FULL_MEMORY | THREAD_CONTEXTS | STACKS | MODULE_LIST |
        // HANDLE_TABLE.
        const MINIDUMP_TYPE kFull =
            static_cast<MINIDUMP_TYPE>(0x00000001 | 0x00000010 | 0x00000002 | 0x00000008 | 0x00000040);
        if (!MiniDumpWriteDump(GetCurrentProcess(), pid, hFile, kFull,
                               &info, nullptr, nullptr)) {
            // Fall back to a smaller dump if the full one fails.
            const MINIDUMP_TYPE kSmall =
                static_cast<MINIDUMP_TYPE>(0x00000002 | 0x00000008);
            MiniDumpWriteDump(GetCurrentProcess(), pid, hFile, kSmall, &info, nullptr, nullptr);
        }
        CloseHandle(hFile);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallCrashDumpHandler() {
    // dbghelp.dll is loaded on demand; the loader resolves MiniDumpWriteDump.
    SetUnhandledExceptionFilter(CrashDumpFilter);
}

// Returns true if the current process is running with administrator privileges.
bool IsElevated() {
    BOOL elevated = FALSE;
    TOKEN_ELEVATION te{};
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &te, sizeof(te), nullptr)) {
            elevated = te.TokenIsElevated ? TRUE : FALSE;
        }
        CloseHandle(token);
    }
    return elevated != FALSE;
}

} // namespace

int main(int argc, char* argv[]) {
    // 0. Attach a file log sink so background activity is captured to disk.
    AttachFileLogSink();

    // 0b. Install a minidump writer so a crash in a normal run is captured.
    InstallCrashDumpHandler();

    if (IsElevated()) {
        CPS_LOG_INFO("CarPlayStation", "Process privilege: Administrator.");
    } else {
        CPS_LOG_INFO("CarPlayStation", "Process privilege: Standard User.");
    }

    // 1. Initialize COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    (void)hr;

    CPS_LOG_INFO("CarPlayStation", "CarPlayStation Receiver Initializing (Windows C++20)...");
    CPS_LOG_INFO("CarPlayStation", "Architecture layers: UI -> Application -> Core -> Lib");

    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--headless" || std::string_view(argv[i]) == "-h") {
            headless = true;
        }
    }

    // 2. Instantiate Core Service Facade and Application Service (DEC-002)
    auto coreService = std::make_shared<CarPlayStation::Core::Service::CarPlayCoreService>();
    auto appService = std::make_shared<CarPlayStation::Application::ApplicationService>(coreService);

    // 3. Initialize UI Controller
    CarPlayStation::UI::AppController controller;
    CarPlayStation::UI::WindowConfig winConfig;
    winConfig.title = L"CarPlayStation — Wireless CarPlay Receiver";
    winConfig.client_width = 1280;
    winConfig.client_height = 720;

    auto status = controller.Initialize(appService, winConfig, headless);
    if (status.IsError()) {
        CPS_LOG_ERROR("CarPlayStation", "Initialization failed: ", status.Message());
        CoUninitialize();
        return (status.Code() == CarPlayStation::Core::Common::StatusCode::Unavailable) ? 0 : 1;
    }
    CPS_LOG_INFO("CarPlayStation", "Controller initialized successfully. Entering UI loop...");

    // 4. Run UI Loop
    int exitCode = 0;
    if (!headless) {
        exitCode = controller.Run();
    } else {
        // Headless single-frame step verification
        controller.Step();
        controller.Shutdown();
    }

    CPS_LOG_INFO("CarPlayStation", "UI loop exited with code ", exitCode);
    CoUninitialize();
    return exitCode;
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nShowCmd*/) {
    return main(__argc, __argv);
}

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int /*nCmdShow*/) {
    return main(__argc, __argv);
}
#endif
