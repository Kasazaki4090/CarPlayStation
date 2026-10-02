#include "MainWindow.h"
#include "D3D11Renderer.h"
#include <windowsx.h>

namespace CarPlayStation::UI {

static const wchar_t* kWindowClassName = L"CarPlayStation_MainWindowClass";

MainWindow::MainWindow() {
    hinstance_ = GetModuleHandleW(nullptr);
}

MainWindow::~MainWindow() {
    Close();
}

Core::Common::Status MainWindow::Create(const WindowConfig& config) {
    if (hwnd_) {
        return Core::Common::Status::Ok();
    }

    client_width_ = (config.client_width > 0) ? config.client_width : 1280;
    client_height_ = (config.client_height > 0) ? config.client_height : 720;

    // 1. Single Instance Check
    if (config.allow_single_instance_only) {
        single_instance_mutex_ = CreateMutexW(
            nullptr,
            TRUE,
            config.single_instance_mutex_name.c_str()
        );
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            // Another instance is running; bring it to foreground
            HWND existingWnd = FindWindowW(kWindowClassName, nullptr);
            if (existingWnd) {
                ShowWindow(existingWnd, SW_RESTORE);
                SetForegroundWindow(existingWnd);
            }
            if (single_instance_mutex_) {
                CloseHandle(single_instance_mutex_);
                single_instance_mutex_ = nullptr;
            }
            return Core::Common::MakeError(Core::Common::StatusCode::Unavailable,
                "Another CarPlayStation instance is already running");
        }
    }

    // 2. High-DPI Awareness (Per-Monitor V2)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 3. Register Window Class
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = hinstance_;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClassName;

    // Register or ignore if already registered
    RegisterClassExW(&wc);

    // 4. Calculate Window Dimensions for Target Client Area
    RECT rc = { 0, 0, static_cast<LONG>(client_width_), static_cast<LONG>(client_height_) };
    DWORD dwStyle = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&rc, dwStyle, FALSE, 0);

    int windowWidth = rc.right - rc.left;
    int windowHeight = rc.bottom - rc.top;

    // Center window on primary monitor
    int posX = (GetSystemMetrics(SM_CXSCREEN) - windowWidth) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - windowHeight) / 2;
    if (posX < 0) posX = CW_USEDEFAULT;
    if (posY < 0) posY = CW_USEDEFAULT;

    // 5. Create Window
    hwnd_ = CreateWindowExW(
        0,
        kWindowClassName,
        config.title.c_str(),
        dwStyle,
        posX, posY,
        windowWidth, windowHeight,
        nullptr,
        nullptr,
        hinstance_,
        this
    );

    if (!hwnd_) {
        return Core::Common::MakeError(Core::Common::StatusCode::IoError,
            "CreateWindowExW failed: err=" + std::to_string(GetLastError()));
    }

    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) {
        dpi_ = 96;
    }

    is_open_ = true;
    prev_style_ = dwStyle;

    return Core::Common::Status::Ok();
}

void MainWindow::AttachRenderer(D3D11Renderer* renderer) {
    renderer_ = renderer;
}

void MainWindow::Show(int nCmdShow) {
    if (hwnd_) {
        ShowWindow(hwnd_, nCmdShow);
        UpdateWindow(hwnd_);
    }
}

void MainWindow::SetDisplayMode(WindowDisplayMode mode) {
    if (!hwnd_ || display_mode_ == mode) {
        return;
    }

    if (mode == WindowDisplayMode::BorderlessFullscreen) {
        // Switch to Borderless Fullscreen
        GetWindowPlacement(hwnd_, &prev_placement_);
        prev_style_ = GetWindowLongW(hwnd_, GWL_STYLE);

        HMONITOR hMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(MONITORINFO);
        if (GetMonitorInfoW(hMon, &mi)) {
            SetWindowLongW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(
                hwnd_,
                HWND_TOP,
                mi.rcMonitor.left,
                mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED
            );
            display_mode_ = WindowDisplayMode::BorderlessFullscreen;
        }
    } else {
        // Switch back to Windowed
        SetWindowLongW(hwnd_, GWL_STYLE, prev_style_ | WS_VISIBLE);
        SetWindowPlacement(hwnd_, &prev_placement_);
        SetWindowPos(
            hwnd_,
            nullptr,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED
        );
        display_mode_ = WindowDisplayMode::Windowed;
    }
}

void MainWindow::ToggleFullscreen() {
    if (display_mode_ == WindowDisplayMode::Windowed) {
        SetDisplayMode(WindowDisplayMode::BorderlessFullscreen);
    } else {
        SetDisplayMode(WindowDisplayMode::Windowed);
    }
}

bool MainWindow::ProcessMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            is_open_ = false;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return is_open_;
}

void MainWindow::Close() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    is_open_ = false;

    if (single_instance_mutex_) {
        ReleaseMutex(single_instance_mutex_);
        CloseHandle(single_instance_mutex_);
        single_instance_mutex_ = nullptr;
    }
}

LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // 1. Give message hook first chance (for input router)
    if (message_hook_) {
        if (message_hook_(hwnd, msg, wParam, lParam)) {
            return 0;
        }
    }

    switch (msg) {
        case WM_SIZE: {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                client_width_ = width;
                client_height_ = height;
                if (renderer_) {
                    (void)renderer_->Resize(client_width_, client_height_);
                }
                // Notify overlay so it can recreate its D2D target against the
                // new backbuffer (the swapchain resize invalidates the old one).
                if (resize_callback_) {
                    resize_callback_(client_width_, client_height_);
                }
            }
            return 0;
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            auto* prcNew = reinterpret_cast<RECT*>(lParam);
            if (prcNew) {
                SetWindowPos(
                    hwnd,
                    nullptr,
                    prcNew->left,
                    prcNew->top,
                    prcNew->right - prcNew->left,
                    prcNew->bottom - prcNew->top,
                    SWP_NOZORDER | SWP_NOACTIVATE
                );
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (wParam == VK_F11) {
                ToggleFullscreen();
                return 0;
            }
            break;
        }

        case WM_SYSKEYDOWN: {
            // Alt + Enter
            if (wParam == VK_RETURN && (lParam & (1 << 29))) {
                ToggleFullscreen();
                return 0;
            }
            break;
        }

        case WM_CLOSE: {
            if (close_callback_) {
                close_callback_();
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            hwnd_ = nullptr;
            is_open_ = false;
            PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace CarPlayStation::UI
