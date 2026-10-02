#pragma once

#include "Application/IApplicationApi.h"
#include "Application/AppSettings.h"
#include "Core/Common/Result.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <functional>
#include <mutex>

namespace CarPlayStation::UI {

using SettingsChangeCallback = std::function<void(const Application::AppSettings&)>;

struct SliderItem {
    std::wstring label;
    float* value_ptr{nullptr};
    float min_val{0.0f};
    float max_val{1.0f};
    D2D1_RECT_F track_rect{};
    bool dragging{false};
};

class OverlayRenderer {
public:
    OverlayRenderer();
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    /**
     * @brief Initialize Direct2D and DirectWrite resources bound to the DXGI swapchain.
     */
    [[nodiscard]] Core::Common::Status Init(IDXGISwapChain1* swapChain);

    /**
     * @brief Recreate render target upon swapchain buffer resize.
     */
    [[nodiscard]] Core::Common::Status Resize(IDXGISwapChain1* swapChain);

    /**
     * @brief Update application state snapshot for HUD display.
     */
    void UpdateUiState(const Application::AppUiState& state);

    /**
     * @brief Update settings snapshot.
     */
    void UpdateSettings(const Application::AppSettings& settings);

    /**
     * @brief Render the HUD overlays (status pill, watermark, settings drawer).
     */
    [[nodiscard]] Core::Common::Status Draw(uint32_t width, uint32_t height);

    /**
     * @brief Handle mouse input for HUD interaction (sliders, toggle buttons).
     * @return true if consumed by HUD overlay, false if should pass through to CarPlay.
     */
    bool OnMouseDown(float clientX, float clientY);
    bool OnMouseMove(float clientX, float clientY);
    bool OnMouseUp(float clientX, float clientY);

    /**
     * @brief Toggle settings drawer visibility (e.g. via F2).
     */
    void ToggleSettingsDrawer();

    /**
     * @brief Set callback for when user modifies settings in the HUD.
     */
    void SetSettingsCallback(SettingsChangeCallback callback) {
        settings_callback_ = std::move(callback);
    }

    void Shutdown();

    // --- Accessors ---
    [[nodiscard]] bool IsSettingsDrawerOpen() const noexcept { return show_settings_drawer_; }
    [[nodiscard]] float GetOverlayAlpha() const noexcept { return overlay_alpha_; }

private:
    [[nodiscard]] Core::Common::Status CreateDeviceResources(IDXGISwapChain1* swapChain);
    void DiscardDeviceResources();
    void DrawStatusPill(float width, float height);
    void DrawSettingsDrawer(float width, float height);
    void DrawWelcomeGuide(float width, float height);

    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> d2d_render_target_;

    // Brushes
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_text_white_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_text_gray_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_panel_bg_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_accent_blue_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_status_green_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_status_orange_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_slider_track_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_slider_thumb_;

    // Text Formats
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tf_header_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tf_normal_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tf_small_;

    // State & Settings
    mutable std::mutex mutex_;
    Application::AppUiState ui_state_{};
    Application::AppSettings settings_{};
    SettingsChangeCallback settings_callback_;

    // Swapchain the D2D target is bound to (kept so Draw can self-heal after a resize).
    IDXGISwapChain1* swap_chain_{nullptr};

    // Size (in physical pixels) the current D2D target was created at. Draw()
    // recreates the target whenever the requested size differs, so the target
    // and its hit-test geometry always match the live backbuffer.
    uint32_t target_width_{0};
    uint32_t target_height_{0};

    bool show_settings_drawer_{false};
    float overlay_alpha_{1.0f};
    uint64_t last_mouse_interaction_tick_{0};

    // UI Interactive Rectangles
    D2D1_RECT_F gear_btn_rect_{};
    D2D1_RECT_F drawer_rect_{};
    D2D1_RECT_F drawer_close_btn_rect_{};
    std::vector<SliderItem> sliders_;
    int active_dragging_slider_{-1};

    bool initialized_{false};
};

} // namespace CarPlayStation::UI
