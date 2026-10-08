#pragma once

#include "Application/IApplicationApi.h"
#include "Application/AppSettings.h"
#include "Core/Common/Result.h"
#include "ThemeHelper.h"
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

struct ButtonItem {
    std::wstring label;
    D2D1_RECT_F rect{};
    bool is_active{false};
    std::function<void()> on_click;
};

class OverlayRenderer {
public:
    OverlayRenderer();
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    /**
     * @brief Initialize Direct2D and DirectWrite resources bound to the DXGI swapchain.
     * @param swapChain The swapchain whose backbuffer the D2D target renders into.
     * @param backBufferIndex Index of the swapchain buffer that is the current
     *        back buffer (BufferCount-1 for a flip-model swapchain, 0 for a
     *        discard-model swapchain). The D2D target must render into this
     *        buffer so it composes with the D3D11 video and is presented.
     */
    [[nodiscard]] Core::Common::Status Init(IDXGISwapChain1* swapChain, uint32_t backBufferIndex);

    /**
     * @brief Recreate render target upon swapchain buffer resize.
     */
    [[nodiscard]] Core::Common::Status Resize(IDXGISwapChain1* swapChain, uint32_t backBufferIndex);

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
     * @brief Toggle the standalone log panel visibility (e.g. via F4).
     */
    void ToggleLogPanel();

    /**
     * @brief Toggle or dismiss the centered welcome pairing guide (e.g. via ESC or [X]).
     */
    void ToggleWelcomeGuide();
    void DismissWelcomeGuide();

    /**
     * @brief Set the number of most-recent log lines the panel displays.
     */
    void SetLogPanelLineCount(int count) { log_panel_line_count_ = count > 0 ? count : 12; }

    /**
     * @brief Re-colour the HUD for a theme (Windows light/dark switch).
     *
     * Only the brush colours change; the render target, text formats and
     * hit-test geometry are untouched, so this is safe to call from the frame
     * loop. Returns true when the theme actually changed.
     */
    bool ApplyTheme(UiTheme theme);

    /**
     * @brief The theme the HUD is currently painted in.
     */
    [[nodiscard]] UiTheme GetTheme() const noexcept { return theme_; }

    /**
     * @brief Set callback for when user modifies settings in the HUD.
     */
    void SetSettingsCallback(SettingsChangeCallback callback) {
        settings_callback_ = std::move(callback);
    }

    void Shutdown();

    // --- Accessors ---
    [[nodiscard]] bool IsSettingsDrawerOpen() const noexcept { return show_settings_drawer_; }
    [[nodiscard]] bool IsWelcomeGuideVisible() const noexcept;
    [[nodiscard]] float GetOverlayAlpha() const noexcept { return overlay_alpha_; }

private:
    [[nodiscard]] Core::Common::Status CreateDeviceResources(IDXGISwapChain1* swapChain, uint32_t backBufferIndex);
    void DiscardDeviceResources();
    // Push palette_ into the live brushes (created in CreateDeviceResources).
    void ApplyPaletteToBrushes();
    void DrawStatusPill(float width, float height);
    void DrawSettingsDrawer(float width, float height);
    void DrawWelcomeGuide(float width, float height);
    void DrawLogPanel(float width, float height);

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
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_log_error_;

    // Text Formats
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tf_header_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tf_normal_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tf_small_;

    // State & Settings
    mutable std::recursive_mutex mutex_;
    Application::AppUiState ui_state_{};
    Application::AppSettings settings_{};
    SettingsChangeCallback settings_callback_;

    // Swapchain the D2D target is bound to (kept so Draw can self-heal after a resize).
    IDXGISwapChain1* swap_chain_{nullptr};
    // Index of the swapchain buffer that is the current back buffer.
    uint32_t back_buffer_index_{0};

    // Size (in physical pixels) the current D2D target was created at. Draw()
    // recreates the target whenever the requested size differs, so the target
    // and its hit-test geometry always match the live backbuffer.
    uint32_t target_width_{0};
    uint32_t target_height_{0};

    bool show_settings_drawer_{false};
    bool show_log_panel_{false};
    bool show_welcome_guide_{true};
    float overlay_alpha_{1.0f};
    uint64_t last_mouse_interaction_tick_{0};

    // UI Interactive Rectangles
    D2D1_RECT_F gear_btn_rect_{};
    D2D1_RECT_F drawer_rect_{};
    D2D1_RECT_F drawer_close_btn_rect_{};
    D2D1_RECT_F welcome_card_rect_{};
    D2D1_RECT_F welcome_close_btn_rect_{};
    D2D1_RECT_F log_panel_rect_{};
    D2D1_RECT_F log_panel_close_btn_rect_{};
    std::vector<SliderItem> sliders_;
    std::vector<ButtonItem> drawer_buttons_;
    int active_dragging_slider_{-1};

    // Standalone log panel: number of most-recent lines to display.
    int log_panel_line_count_{12};

    // Appearance follows the Windows app light/dark setting; the palette is
    // rebuilt only when that setting changes.
    UiTheme theme_{UiTheme::Dark};
    UiPalette palette_{};

    bool initialized_{false};
};

} // namespace CarPlayStation::UI
