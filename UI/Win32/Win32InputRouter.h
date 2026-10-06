#pragma once

#include "Application/IApplicationApi.h"
#include <windows.h>
#include <array>
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
    bool HandlePointer(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // One live touch contact. The index of a pointer in `pointers_` is the HID contact
    // slot it is forwarded as, which is how a second finger reaches the two-contact
    // touchscreen report.
    struct PointerSlot {
        bool down = false;
        bool overlay = false;   // Owned by the HUD, not forwarded to CarPlay.
        float x = 0.5f;
        float y = 0.5f;
    };

    // Contacts the advertised touchscreen descriptor can carry. A third finger is
    // ignored rather than folded onto a slot, so the report stays well formed.
    static constexpr size_t kMaxPointers = 2;

    std::shared_ptr<Application::IApplicationApi> app_api_;
    D3D11Renderer* renderer_{nullptr};
    OverlayRenderer* overlay_{nullptr};

    bool is_mouse_down_{false};
    bool is_overlay_capturing_{false};
    float last_norm_x_{0.5f};
    float last_norm_y_{0.5f};

    // Pointer ids in arrival order. Fixed size keeps the slot index stable without an
    // allocation on the input path.
    std::array<UINT, kMaxPointers> pointer_ids_{};
    std::array<PointerSlot, kMaxPointers> pointer_slots_{};
    size_t pointer_count_ = 0;

    [[nodiscard]] size_t FindPointerSlot(UINT pointerId) const noexcept;
    size_t AddPointerSlot(UINT pointerId) noexcept;
    void RemovePointerSlot(size_t slot) noexcept;
};

} // namespace CarPlayStation::UI
