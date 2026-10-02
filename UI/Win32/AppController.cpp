#include "AppController.h"
#include "VideoAdaptationHelper.h"

namespace CarPlayStation::UI {

AppController::AppController() = default;

AppController::~AppController() {
    Shutdown();
}

Core::Common::Status AppController::Initialize(
    std::shared_ptr<Application::IApplicationApi> appApi,
    const WindowConfig& winConfig,
    bool headless) {

    if (!appApi) {
        return Core::Common::MakeError(Core::Common::StatusCode::InvalidArgument, "appApi is null");
    }

    app_api_ = std::move(appApi);
    headless_ = headless;

    // 1. Create native Win32 window
    auto winStatus = window_.Create(winConfig);
    if (winStatus.IsError()) {
        return winStatus;
    }

    // 2. First-Run Wizards (Safety Disclaimer & MFi setup)
    if (wizard_.ShouldShowDisclaimer()) {
        if (!wizard_.ShowSafetyDisclaimer(window_.GetHwnd(), headless_)) {
            // User rejected safety disclaimer
            window_.Close();
            return Core::Common::MakeError(Core::Common::StatusCode::PermissionDenied,
                "User rejected safety disclaimer");
        }
    }

    // Restore any persisted MFi backend selection before deciding whether the
    // first-run wizard must be shown again (e.g. missing cert/key files).
    wizard_.LoadMFiSettings(settings_);

    if (wizard_.ShouldShowMFiWizard(settings_)) {
        wizard_.ShowMFiWizard(window_.GetHwnd(), settings_, headless_);
        // Persist the (possibly newly chosen) MFi backend so it survives restart.
        wizard_.SaveMFiSettings(settings_);
    }

    // 3. Initialize Direct3D 11 Renderer
    auto rStatus = renderer_.Init(window_.GetHwnd(), window_.GetClientWidth(), window_.GetClientHeight());
    if (rStatus.IsError()) {
        window_.Close();
        return rStatus;
    }
    window_.AttachRenderer(&renderer_);

    // 4. Initialize Direct2D Overlay Renderer
    (void)overlay_.Init(renderer_.GetSwapChain());
    overlay_.UpdateSettings(settings_);
    overlay_.SetSettingsCallback([this](const Application::AppSettings& s) {
        OnSettingsChanged(s);
    });

    // 5. Wire Input Router to Window Message Hook
    input_router_ = std::make_unique<Win32InputRouter>(app_api_, &renderer_, &overlay_);
    window_.SetMessageHook([this](HWND h, UINT m, WPARAM w, LPARAM l) {
        if (input_router_) {
            return input_router_->OnWindowMessage(h, m, w, l);
        }
        return false;
    });

    window_.SetCloseCallback([this]() {
        Shutdown();
    });

    // When the window resizes (windowed <-> fullscreen, WM_SIZE, WM_DPICHANGED)
    // the swapchain backbuffer is invalidated, which makes the D2D overlay
    // render target stale. Recreate it against the new backbuffer so the HUD
    // (status pill, welcome guide, settings drawer) and its hit-test geometry
    // keep working at the new size.
    window_.SetResizeCallback([this](uint32_t /*w*/, uint32_t /*h*/) {
        (void)overlay_.Resize(renderer_.GetSwapChain());
    });

    // 6. Connect Application Callbacks
    app_api_->RegisterUiStateCallback([this](const Application::AppUiState& state) {
        OnUiStateChanged(state);
    });

    app_api_->RegisterFrameConsumer([this](uintptr_t handle, uint32_t w, uint32_t h, uint64_t pts) {
        renderer_.ConsumeFrame(handle, w, h, pts);
    });

    // 7. Start CarPlay
    (void)app_api_->StartCarPlay(settings_);

    // 8. Show Window
    if (!headless_) {
        window_.Show();
    }

    is_initialized_ = true;
    is_running_ = true;
    return Core::Common::Status::Ok();
}

int AppController::Run() {
    if (!is_initialized_) {
        return -1;
    }

    while (is_running_ && window_.IsOpen()) {
        if (!Step()) {
            break;
        }
    }

    Shutdown();
    return 0;
}

bool AppController::Step() {
    if (!is_running_) {
        return false;
    }

    // Pump window messages
    if (!window_.ProcessMessages()) {
        is_running_ = false;
        return false;
    }

    // 1. Render D3D11 video frame (or letterbox background) without premature present
    (void)renderer_.Render(/*autoPresent=*/false);

    // 2. Render Direct2D overlay HUD onto the backbuffer
    (void)overlay_.Draw(window_.GetClientWidth(), window_.GetClientHeight());

    // 3. Present the combined frame to the screen
    (void)renderer_.Present();

    return true;
}

void AppController::Shutdown() {
    if (!is_initialized_) {
        return;
    }

    is_running_ = false;

    if (app_api_) {
        app_api_->StopCarPlay();
    }

    overlay_.Shutdown();
    renderer_.Shutdown();
    window_.Close();

    is_initialized_ = false;
}

void AppController::OnUiStateChanged(const Application::AppUiState& state) {
    overlay_.UpdateUiState(state);
}

void AppController::OnSettingsChanged(const Application::AppSettings& newSettings) {
    settings_ = newSettings;
    if (app_api_) {
        (void)app_api_->ApplySettings(settings_);
    }
    // Keep the persisted MFi backend in sync with runtime changes.
    wizard_.SaveMFiSettings(settings_);
}

} // namespace CarPlayStation::UI
