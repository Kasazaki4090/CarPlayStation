#pragma once

#include "Application/IApplicationApi.h"
#include <windows.h>
#include <memory>

namespace CarPlayStation::UI {

class D3D11Renderer;
class OverlayRenderer;

class Win32InputRouter {
public:
    Win32InputRouter(
        std::shared_ptr<Application::IApplicationApi> appApi,
        D3D11Renderer* renderer,
        OverlayRenderer* overlay = nullptr);
    ~Win32InputRouter() = default;

    Win32InputRouter(const Win32InputRouter&) = delete;
    Win32InputRouter& operator=(const Win32InputRouter&) = delete;

    /**
     * @brief Handle raw Win32 window message.
     * @return true if consumed, false if unhandled.
     */
    bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /**
     * @brief Reset input router state (e.g. mouse release on blur).
     */
    void Reset();

    // Set/Update renderer or overlay pointers
    void SetRenderer(D3D11Renderer* renderer) noexcept { renderer_ = renderer; }
    void SetOverlay(OverlayRenderer* overlay) noexcept { overlay_ = overlay; }
    void SetAppApi(std::shared_ptr<Application::IApplicationApi> api) noexcept { app_api_ = std::move(api); }

    [[nodiscard]] bool IsMouseDown() const noexcept { return is_mouse_down_; }

private:
    bool HandleMouse(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool HandleKeyboard(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    std::shared_ptr<Application::IApplicationApi> app_api_;
    D3D11Renderer* renderer_{nullptr};
    OverlayRenderer* overlay_{nullptr};

    bool is_mouse_down_{false};
    bool is_overlay_capturing_{false};
    float last_norm_x_{0.5f};
    float last_norm_y_{0.5f};
};

} // namespace CarPlayStation::UI
