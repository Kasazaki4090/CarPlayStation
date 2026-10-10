#include "AppController.h"
#include "VideoAdaptationHelper.h"
#include "Core/Common/Logging.h"
#include <cstdio>
#include <windows.h>

namespace CarPlayStation::UI {

namespace {
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    if (wlen > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                            wide.data(), wlen);
    }
    return wide;
}
} // namespace

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

    // 1. Restore persisted settings, then create the native Win32 window.
    //
    // The order matters: the saved video mode decides the window's client area,
    // so the client size, the swapchain and the geometry advertised to the phone
    // in the RTSP /info response all agree. Changing the resolution therefore
    // takes effect on the next start of the app.
    wizard_.LoadMFiSettings(settings_);
    wizard_.LoadVideoAudioSettings(settings_);

    // Write the loaded values straight back (headless runs never touch the disk).
    // This makes the [Video] / [Audio] keys exist in the file, so a resolution
    // can be hand typed in and picked up on the next start.
    if (!headless_) {
        wizard_.SaveVideoAudioSettings(settings_);
    }

    WindowConfig config = winConfig;
    if (!headless_) {
        uint32_t winW = 0;
        uint32_t winH = 0;
        if (settings_.video.resolution_preset == Application::VideoResolutionPreset::Custom) {
            // A hand written size (e.g. 1920x720) is used as is. The renderer
            // letterboxes to the video aspect, so an off aspect window is safe.
            winW = settings_.video.custom_width;
            winH = settings_.video.custom_height;
        } else if (settings_.video.resolution_preset == Application::VideoResolutionPreset::Auto) {
            winW = static_cast<uint32_t>(::GetSystemMetrics(SM_CXSCREEN));
            winH = static_cast<uint32_t>(::GetSystemMetrics(SM_CYSCREEN));
        } else {
            auto dims = settings_.video.GetEffectiveDimensions();
            winW = dims.first;
            winH = dims.second;
        }

        // Never ask for a window that is bigger than the desktop.
        const uint32_t maxW = static_cast<uint32_t>(::GetSystemMetrics(SM_CXSCREEN));
        const uint32_t maxH = static_cast<uint32_t>(::GetSystemMetrics(SM_CYSCREEN));
        if (winW == 0 || winW > maxW || winH == 0 || winH > maxH) {
            CPS_LOG_WARN("AppController", "Ignoring unusable window size ", winW, "x", winH,
                         " (desktop ", maxW, "x", maxH, ")");
            winW = 1280;
            winH = 720;
        }
        config.client_width = winW;
        config.client_height = winH;
        CPS_LOG_INFO("AppController", "Window client area from settings: ", winW, "x", winH,
                     " preset=", static_cast<unsigned>(settings_.video.resolution_preset),
                     " codec=", static_cast<unsigned>(settings_.video.preferred_codec),
                     " fps=", settings_.video.fps);
    }

    auto winStatus = window_.Create(config);
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

    // Match the window frame, the letterbox bars and the HUD to the Windows
    // light/dark setting the user currently has ("Choose your mode").
    ApplyCurrentTheme(true);
    window_.SetThemeChangedCallback([this]() {
        ApplyCurrentTheme(true);
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
        // A saved fullscreen preference is applied here (after the renderer and
        // the overlay resize callback are wired, so the WM_SIZE this triggers can
        // rebuild the swapchain and the D2D target).
        if (settings_.video.display_mode != Application::WindowDisplayMode::Windowed) {
            window_.SetDisplayMode(WindowDisplayMode::BorderlessFullscreen);
        }
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

    // 4. Write back settings the user changed in the drawer (debounced).
    FlushPendingSettings();

    // 5. Keep the HUD's video readout current. Core events only arrive on
    //    connection/media changes, so poll the state snapshot once a second to
    //    pick up the codec correction from the first VideoConfig.
    RefreshUiState();

    // Belt-and-braces: some theme changes (or a missed WM_SETTINGCHANGE) are
    // picked up by re-reading the registry on the same once-a-second cadence.
    ApplyCurrentTheme();

    if (::IsIconic(window_.GetHwnd())) {
        ::Sleep(16);
    } else {
        // Frame pacing safeguard: if Present(1, 0) returned without waiting
        // (driver VSync override, occluded window, or non-vsync display),
        // pace the loop so it does not spin at 1000+ FPS pegging the CPU at 100%.
        auto now = std::chrono::steady_clock::now();
        auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - last_step_time_).count();
        if (elapsedUs < 8000) {
            ::Sleep(1);
        }
        last_step_time_ = std::chrono::steady_clock::now();
    }

    return true;
}

void AppController::FlushPendingSettings() {
    if (!settings_dirty_) {
        return;
    }
    const uint64_t now = ::GetTickCount64();
    if (now - settings_dirty_tick_ < kSettingsSaveQuietMs) {
        return;
    }
    settings_dirty_ = false;
    wizard_.SaveVideoAudioSettings(settings_);
}

void AppController::ApplyCurrentTheme(bool force) {
    const uint64_t now = ::GetTickCount64();
    if (!force && now - theme_check_tick_ < kUiRefreshMs) {
        return;
    }
    theme_check_tick_ = now;

    const UiTheme theme = ThemeHelper::CurrentTheme();
    if (!force && theme == applied_theme_) {
        return;
    }
    applied_theme_ = theme;

    // Window frame/caption, letterbox bars, then the HUD brushes. All three are
    // cheap and take effect on the next frame.
    window_.ApplyTheme(theme);
    renderer_.SetLetterboxColor(ThemeHelper::BuildPalette(theme).letterbox);
    (void)overlay_.ApplyTheme(theme);

    // The video on screen is rendered by the iPhone, so re-coloring anything on this
    // side cannot darken it. The only way the CarPlay UI itself follows the desktop
    // theme is the AirPlay night-mode command.
    const bool night_mode = (theme == UiTheme::Dark);
    Core::Common::Status status = Core::Common::MakeError(
        Core::Common::StatusCode::Unavailable, "Application API not available");
    if (app_api_) {
        status = app_api_->SetHostNightMode(night_mode);
    }
    CPS_LOG_INFO("AppController", "UI theme switched to ", ThemeHelper::ThemeName(theme),
                 ", CarPlay night mode requested=", night_mode ? "true" : "false",
                 ", commandWritten=", status.IsOk() ? "true" : "false",
                 " (a successful write does not prove the iPhone changed its appearance)");
}

void AppController::RefreshUiState() {
    if (!app_api_) {
        return;
    }
    const uint64_t now = ::GetTickCount64();
    if (now - ui_refresh_tick_ < kUiRefreshMs) {
        return;
    }
    ui_refresh_tick_ = now;
    // Cheap: ApplicationService copies its own snapshot and asks Core for the
    // video spec currently in effect. Only forward the state when the video
    // facts actually changed, so the overlay's fade timer is not reset each
    // second.
    auto state = app_api_->GetCurrentState();
    if (state.current_video_width == pushed_video_width_ &&
        state.current_video_height == pushed_video_height_ &&
        state.current_video_fps == pushed_video_fps_ &&
        state.current_video_codec == pushed_video_codec_ &&
        state.is_hardware_decode == pushed_is_hardware_decode_) {
        return;
    }
    pushed_video_width_ = state.current_video_width;
    pushed_video_height_ = state.current_video_height;
    pushed_video_fps_ = state.current_video_fps;
    pushed_video_codec_ = state.current_video_codec;
    pushed_is_hardware_decode_ = state.is_hardware_decode;
    OnUiStateChanged(state);
}

void AppController::Shutdown() {
    if (!is_initialized_) {
        return;
    }

    is_running_ = false;

    // Do not lose a resolution / volume change made moments before closing.
    if (settings_dirty_) {
        settings_dirty_ = false;
        wizard_.SaveVideoAudioSettings(settings_);
    }

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
    pushed_video_width_ = state.current_video_width;
    pushed_video_height_ = state.current_video_height;
    pushed_video_fps_ = state.current_video_fps;
    pushed_video_codec_ = state.current_video_codec;

    // Update window title with Now Playing track if playing
    if (state.now_playing.is_playing && !state.now_playing.title.empty()) {
        std::wstring title = L"🎵 " + Utf8ToWide(state.now_playing.title);
        if (!state.now_playing.artist.empty()) {
            title += L" — " + Utf8ToWide(state.now_playing.artist);
        }
        title += L" | CarPlayStation";
        window_.SetTitle(title);
    } else {
        window_.SetTitle(L"CarPlayStation — Wireless CarPlay Receiver");
    }

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
    bool video_audio_changed = (newSettings.video.resolution_preset != settings_.video.resolution_preset ||
                                newSettings.video.custom_width != settings_.video.custom_width ||
                                newSettings.video.custom_height != settings_.video.custom_height ||
                                newSettings.video.fps != settings_.video.fps ||
                                newSettings.video.preferred_codec != settings_.video.preferred_codec ||
                                newSettings.video.display_mode != settings_.video.display_mode ||
                                newSettings.audio.media_volume != settings_.audio.media_volume ||
                                newSettings.audio.navigation_volume != settings_.audio.navigation_volume ||
                                newSettings.audio.telephony_volume != settings_.audio.telephony_volume ||
                                newSettings.audio.ducking_ratio != settings_.audio.ducking_ratio);
    settings_ = newSettings;
    if (app_api_) {
        (void)app_api_->ApplySettings(settings_);
    }
    if (mfi_changed) {
        wizard_.SaveMFiSettings(settings_);
    }
    // The drawer fires this callback on every slider drag frame, so the video and
    // audio settings are written to the INI only once the values go quiet
    // (see FlushPendingSettings).
    if (video_audio_changed) {
        settings_dirty_ = true;
        settings_dirty_tick_ = ::GetTickCount64();
    }
}

} // namespace CarPlayStation::UI
