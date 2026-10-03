#pragma once

#include "Application/IApplicationApi.h"
#include "Application/AppSettings.h"
#include "MainWindow.h"
#include "D3D11Renderer.h"
#include "OverlayRenderer.h"
#include "SetupWizard.h"
#include "Win32InputRouter.h"
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

    bool is_initialized_{false};
    bool is_running_{false};
    bool headless_{false};
};

} // namespace CarPlayStation::UI
