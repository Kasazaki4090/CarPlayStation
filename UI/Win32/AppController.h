#pragma once

#include "Application/IApplicationApi.h"
#include "Application/AppSettings.h"
#include "MainWindow.h"
#include "D3D11Renderer.h"
#include "OverlayRenderer.h"
#include "SetupWizard.h"
#include "Win32InputRouter.h"
#include <chrono>
#include <memory>
#include <thread>

namespace CarPlayStation::UI {

class AppController {
public:
    AppController();
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    /**
     * @brief Initialize all UI components and wire them to ApplicationService.
     * @param appApi Application API facade.
     * @param winConfig Main window configuration.
     * @param headless If true, runs without interactive popups (for automated testing).
     */
    [[nodiscard]] Core::Common::Status Initialize(
        std::shared_ptr<Application::IApplicationApi> appApi,
        const WindowConfig& winConfig = {},
        bool headless = false);

    /**
     * @brief Run the interactive rendering and message loop.
     */
    int Run();

    /**
     * @brief Perform a single iteration of message processing and frame rendering.
     * @return true if running, false if application exit requested.
     */
    bool Step();

    /**
     * @brief Stop CarPlay session and destroy all UI/device resources.
     */
    void Shutdown();

    // --- Accessors for Testing & Inspection ---
    [[nodiscard]] MainWindow& GetWindow() noexcept { return window_; }
    [[nodiscard]] D3D11Renderer& GetRenderer() noexcept { return renderer_; }
    [[nodiscard]] OverlayRenderer& GetOverlay() noexcept { return overlay_; }
    [[nodiscard]] SetupWizard& GetWizard() noexcept { return wizard_; }
    [[nodiscard]] Win32InputRouter* GetInputRouter() noexcept { return input_router_.get(); }
    [[nodiscard]] const Application::AppSettings& GetSettings() const noexcept { return settings_; }
    [[nodiscard]] bool IsInitialized() const noexcept { return is_initialized_; }

private:
    void OnUiStateChanged(const Application::AppUiState& state);
    void OnSettingsChanged(const Application::AppSettings& newSettings);

    // Write the debounced [Video] / [Audio] settings back to the INI. Called
    // from Step() once the user has stopped dragging a slider, and from
    // Shutdown() so nothing is lost on exit.
    void FlushPendingSettings();

    // Push the current UI state to the overlay once a second so the HUD's video
    // readout follows what Core is actually streaming (Core events only cover
    // connection/media changes, not the codec correction from the first
    // VideoConfig). Only pushes when the facts changed, so the overlay's fade
    // timer is not reset every second.
    void RefreshUiState();

    MainWindow window_;
    D3D11Renderer renderer_;
    OverlayRenderer overlay_;
    SetupWizard wizard_;
    std::unique_ptr<Win32InputRouter> input_router_;

    std::shared_ptr<Application::IApplicationApi> app_api_;
    Application::AppSettings settings_;

    // Last recorded phone addresses persisted to the INI (to avoid redundant writes).
    std::string persisted_bt_mac_;
    std::string persisted_wifi_mac_;

    // The settings drawer fires its callback on every slider drag frame, so the
    // video/audio settings are written to the INI only after the values have
    // been stable for kSettingsSaveQuietMs.
    static constexpr uint64_t kSettingsSaveQuietMs = 500;
    bool settings_dirty_{false};
    uint64_t settings_dirty_tick_{0};

    // Video facts last pushed to the overlay (see RefreshUiState).
    static constexpr uint64_t kUiRefreshMs = 1000;
    uint64_t ui_refresh_tick_{0};
    uint32_t pushed_video_width_{0};
    uint32_t pushed_video_height_{0};
    uint32_t pushed_video_fps_{0};
    std::string pushed_video_codec_;
    bool pushed_is_hardware_decode_{false};
    std::chrono::steady_clock::time_point last_step_time_{};

    bool is_initialized_{false};
    bool is_running_{false};
    bool headless_{false};
};

} // namespace CarPlayStation::UI
