#pragma once

#include "Core/Common/Result.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <functional>

namespace CarPlayStation::UI {

class D3D11Renderer;

enum class WindowDisplayMode : uint8_t {
    Windowed = 0,
    BorderlessFullscreen = 1
};

struct WindowConfig {
    std::wstring title{L"CarPlayStation"};
    uint32_t client_width{1280};
    uint32_t client_height{720};
    bool allow_single_instance_only{true};
    std::wstring single_instance_mutex_name{L"Local\\CarPlayStation_SingleInstance_Mutex"};
};

using WindowMessageCallback = std::function<bool(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)>;
using WindowCloseCallback = std::function<void()>;
using WindowResizeCallback = std::function<void(uint32_t width, uint32_t height)>;

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    /**
     * @brief Create the main Win32 window, register class, and handle high DPI.
     */
    [[nodiscard]] Core::Common::Status Create(const WindowConfig& config = {});

    /**
     * @brief Attach the Direct3D 11 renderer for automated resize synchronization.
     */
    void AttachRenderer(D3D11Renderer* renderer);

    /**
     * @brief Switch between Windowed and BorderlessFullscreen display modes.
     */
    void SetDisplayMode(WindowDisplayMode mode);

    /**
     * @brief Toggle between Windowed and BorderlessFullscreen (F11 / Alt+Enter).
     */
    void ToggleFullscreen();

    /**
     * @brief Show and bring window to front.
     */
    void Show(int nCmdShow = SW_SHOWNORMAL);

    /**
     * @brief Run a non-blocking pump iteration of window messages.
     * @return false if WM_QUIT received, true otherwise.
     */
    bool ProcessMessages();

    /**
     * @brief Close and destroy the window.
     */
    void Close();

    // --- Message & Lifecycle Hooks ---
    void SetMessageHook(WindowMessageCallback callback) { message_hook_ = std::move(callback); }
    void SetCloseCallback(WindowCloseCallback callback) { close_callback_ = std::move(callback); }
    void SetResizeCallback(WindowResizeCallback callback) { resize_callback_ = std::move(callback); }

    // --- Accessors ---
    [[nodiscard]] HWND GetHwnd() const noexcept { return hwnd_; }
    [[nodiscard]] bool IsOpen() const noexcept { return hwnd_ != nullptr && is_open_; }
    [[nodiscard]] WindowDisplayMode GetDisplayMode() const noexcept { return display_mode_; }
    [[nodiscard]] uint32_t GetClientWidth() const noexcept { return client_width_; }
    [[nodiscard]] uint32_t GetClientHeight() const noexcept { return client_height_; }
    [[nodiscard]] uint32_t GetDpi() const noexcept { return dpi_; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_{nullptr};
    HINSTANCE hinstance_{nullptr};
    HANDLE single_instance_mutex_{nullptr};
    bool is_open_{false};

    uint32_t client_width_{1280};
    uint32_t client_height_{720};
    uint32_t dpi_{96};

    WindowDisplayMode display_mode_{WindowDisplayMode::Windowed};
    WINDOWPLACEMENT prev_placement_{ sizeof(WINDOWPLACEMENT) };
    LONG prev_style_{0};

    D3D11Renderer* renderer_{nullptr};
    WindowMessageCallback message_hook_;
    WindowCloseCallback close_callback_;
    WindowResizeCallback resize_callback_;
};

} // namespace CarPlayStation::UI
