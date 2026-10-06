#include "AppController.h"
#include "VideoAdaptationHelper.h"
#include "Core/Common/Logging.h"
#include <cstdio>

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

    // Restore the last recorded phone (iPhone) addresses so the device is
    // recognized on the next launch (continuous broadcasting / no re-pairing).
    wizard_.LoadDeviceAddresses(persisted_bt_mac_, persisted_wifi_mac_);

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

    // Inject the renderer's D3D11 device/context into the video decoder so its
    // shared texture is created on the same device the renderer samples from.
    app_api_->SetVideoSharedDevice(renderer_.GetDevice(), renderer_.GetContext());

    // 4. Initialize Direct2D Overlay Renderer
    (void)overlay_.Init(renderer_.GetSwapChain(), renderer_.GetBackBufferIndex());
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

    // Ask for raw pointer messages so a second finger is visible. Without this the
    // window only ever receives the synthesized single-point WM_MOUSE* stream.
    if (!RegisterTouchWindow(window_.GetHwnd(), 0)) {
        CPS_LOG_WARN("AppController", "RegisterTouchWindow failed; multi-touch disabled");
    }

    window_.SetCloseCallback([this]() {
        Shutdown();
    });

    // When the window resizes (windowed <-> fullscreen, WM_SIZE, WM_DPICHANGED)
    // the swapchain backbuffer is invalidated, which makes the D2D overlay
    // render target stale. Recreate it against the new backbuffer so the HUD
    // (status pill, welcome guide, settings drawer) and its hit-test geometry
    // keep working at the new size.
    window_.SetResizeCallback([this](uint32_t /*w*/, uint32_t /*h*/) {
        (void)overlay_.Resize(renderer_.GetSwapChain(), renderer_.GetBackBufferIndex());
    });

    // 6. Connect Application Callbacks
    app_api_->RegisterUiStateCallback([this](const Application::AppUiState& state) {
        OnUiStateChanged(state);
    });

    app_api_->RegisterFrameConsumer([this](uintptr_t handle, uint32_t w, uint32_t h, uint64_t pts) {
        renderer_.ConsumeFrame(handle, w, h, pts);
    });

    // CPU frame path (MpvVideoPlayer): the decoder hands decoded BGRA pixels
    // to the renderer, which uploads them to a local texture on its own
    // immediate context (no D3D11 shared texture / keyed mutex).
    app_api_->RegisterCpuFrameConsumer([this](Core::Media::CpuFrameBuffer* buffer) {
        renderer_.ConsumeCpuFrame(buffer);
    });

    // 7. Show Window immediately so user gets instant visual feedback
    if (!headless_) {
        window_.Show();
    }

    // 8. Start CarPlay
    auto startSt = app_api_->StartCarPlay(settings_);
    if (startSt.IsError()) {
        char buf[512];
        snprintf(buf, sizeof(buf), "[AppController] StartCarPlay failed: %s\n",
                 startSt.Message().c_str());
        OutputDebugStringA(buf);
    }

    is_initialized_ = true;
    is_running_ = true;
    return Core::Common::Status::Ok();
}

int AppController::Run() {
    if (!is_initialized_) {
        CPS_LOG_ERROR("AppController", "Run called but controller is not initialized");
        return -1;
    }

    CPS_LOG_INFO("AppController", "Entering Run message loop...");
    uint64_t step_count = 0;
    while (is_running_ && window_.IsOpen()) {
        if (!Step()) {
            CPS_LOG_INFO("AppController", "Step returned false at step ", step_count);
            break;
        }
        ++step_count;
    }

    CPS_LOG_INFO("AppController", "Run loop finished. is_running=", is_running_ ? "true" : "false",
                 " window.IsOpen=", window_.IsOpen() ? "true" : "false", " total_steps=", step_count);
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

    // 3. Present the combined frame to the screen.
    //
    // Present(1, 0) blocks until the next vertical blank, so it already paces
    // this loop at the display refresh rate. An extra Sleep(1) here used to add
    // up to a full timer tick (15.6 ms with the default system timer
    // resolution) on top of that, which is directly visible as input-to-screen
    // lag. Only idle when the window is minimized, where Present() returns
    // immediately and the loop would otherwise spin.
    (void)renderer_.Present();

    if (::IsIconic(window_.GetHwnd())) {
        ::Sleep(16);
    }

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

    // Persist the recorded phone (iPhone) addresses to the INI so the device
    // does not need to be re-paired on the next launch. Only write when the
    // recorded addresses actually change (avoids redundant INI writes).
    if (!state.connected_device_bt_mac.empty() || !state.connected_device_wifi_mac.empty()) {
        if (state.connected_device_bt_mac != persisted_bt_mac_ ||
            state.connected_device_wifi_mac != persisted_wifi_mac_) {
            wizard_.SaveDeviceAddresses(state.connected_device_bt_mac, state.connected_device_wifi_mac);
            persisted_bt_mac_ = state.connected_device_bt_mac;
            persisted_wifi_mac_ = state.connected_device_wifi_mac;
        }
    }
}

void AppController::OnSettingsChanged(const Application::AppSettings& newSettings) {
    bool mfi_changed = (newSettings.mfi.backend_type != settings_.mfi.backend_type ||
                        newSettings.mfi.certificate_path != settings_.mfi.certificate_path ||
                        newSettings.mfi.private_key_path != settings_.mfi.private_key_path);
    settings_ = newSettings;
    if (app_api_) {
        (void)app_api_->ApplySettings(settings_);
    }
    // Avoid synchronous disk writes during slider drags — only persist if MFi config changed
    if (mfi_changed) {
        wizard_.SaveMFiSettings(settings_);
    }
}

} // namespace CarPlayStation::UI
