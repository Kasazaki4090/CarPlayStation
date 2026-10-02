#include "OverlayRenderer.h"
#include <chrono>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace CarPlayStation::UI {

OverlayRenderer::OverlayRenderer() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    last_mouse_interaction_tick_ = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

OverlayRenderer::~OverlayRenderer() {
    Shutdown();
}

Core::Common::Status OverlayRenderer::Init(IDXGISwapChain1* swapChain) {
    if (initialized_) {
        return Core::Common::Status::Ok();
    }

    swap_chain_ = swapChain;

    // 1. Create Direct2D Factory
    D2D1_FACTORY_OPTIONS options{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options, &d2d_factory_);
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "D2D1CreateFactory failed");
    }

    // 2. Create DirectWrite Factory
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf())
    );
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "DWriteCreateFactory failed");
    }

    // 3. Create Text Formats
    hr = dwrite_factory_->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        14.0f,
        L"zh-cn",
        &tf_header_
    );
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "CreateTextFormat header failed");
    }

    hr = dwrite_factory_->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f,
        L"zh-cn",
        &tf_normal_
    );

    hr = dwrite_factory_->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        10.5f,
        L"zh-cn",
        &tf_small_
    );

    // 4. Create Device Resources
    auto status = CreateDeviceResources(swapChain);
    if (status.IsError()) {
        return status;
    }

    initialized_ = true;
    return Core::Common::Status::Ok();
}

Core::Common::Status OverlayRenderer::CreateDeviceResources(IDXGISwapChain1* swapChain) {
    if (!swapChain || !d2d_factory_) {
        return Core::Common::Status::Ok();
    }

    DiscardDeviceResources();

    ComPtr<IDXGISurface> backBufferSurface;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBufferSurface));
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "GetBuffer(0) for DXGI surface failed");
    }

    // Create D2D Render Target over DXGI Surface
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );

    hr = d2d_factory_->CreateDxgiSurfaceRenderTarget(
        backBufferSurface.Get(),
        &props,
        &d2d_render_target_
    );
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError, "CreateDxgiSurfaceRenderTarget failed");
    }

    // Record the size the target was created at so Draw() can detect a
    // mismatch (e.g. after a swapchain resize) and rebuild it.
    if (d2d_render_target_) {
        const D2D1_SIZE_F sz = d2d_render_target_->GetSize();
        target_width_ = static_cast<uint32_t>(sz.width);
        target_height_ = static_cast<uint32_t>(sz.height);
    }

    // Create Brushes
    d2d_render_target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush_text_white_);
    d2d_render_target_->CreateSolidColorBrush(D2D1::ColorF(0.7f, 0.7f, 0.75f, 1.0f), &brush_text_gray_);
    d2d_render_target_->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.10f, 0.85f), &brush_panel_bg_);
    d2d_render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.48f, 1.0f, 1.0f), &brush_accent_blue_);
    d2d_render_target_->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.8f, 0.3f, 1.0f), &brush_status_green_);
    d2d_render_target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.6f, 0.0f, 1.0f), &brush_status_orange_);
    d2d_render_target_->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.25f, 0.30f, 1.0f), &brush_slider_track_);
    d2d_render_target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush_slider_thumb_);

    return Core::Common::Status::Ok();
}

void OverlayRenderer::DiscardDeviceResources() {
    brush_slider_thumb_.Reset();
    brush_slider_track_.Reset();
    brush_status_orange_.Reset();
    brush_status_green_.Reset();
    brush_accent_blue_.Reset();
    brush_panel_bg_.Reset();
    brush_text_gray_.Reset();
    brush_text_white_.Reset();
    d2d_render_target_.Reset();
    // Force the next Draw() to rebuild the target (a null target with a
    // non-zero recorded size would otherwise be treated as a size mismatch
    // and rebuilt anyway, but resetting keeps the invariant clean).
    target_width_ = 0;
    target_height_ = 0;
}

Core::Common::Status OverlayRenderer::Resize(IDXGISwapChain1* swapChain) {
    if (!initialized_) {
        return Core::Common::Status::Ok();
    }
    swap_chain_ = swapChain;
    std::lock_guard<std::mutex> lock(mutex_);
    return CreateDeviceResources(swapChain);
}

void OverlayRenderer::UpdateUiState(const Application::AppUiState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    ui_state_ = state;
    // Reset fade timer upon state transition
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    last_mouse_interaction_tick_ = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    overlay_alpha_ = 1.0f;
}

void OverlayRenderer::UpdateSettings(const Application::AppSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
}

void OverlayRenderer::ToggleSettingsDrawer() {
    std::lock_guard<std::mutex> lock(mutex_);
    show_settings_drawer_ = !show_settings_drawer_;
    overlay_alpha_ = 1.0f;
}

Core::Common::Status OverlayRenderer::Draw(uint32_t width, uint32_t height) {
    if (!initialized_) {
        return Core::Common::Status::Ok();
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Self-heal: after a swapchain resize (windowed <-> fullscreen, WM_SIZE,
    // WM_DPICHANGED) the backbuffer is invalidated and the D2D render target
    // becomes stale. EndDraw() then returns D2DERR_RECREATE_TARGET and the
    // target is discarded. Recreate it against the current backbuffer so the
    // HUD (status pill, welcome guide, settings drawer) and its hit-test
    // geometry keep working at the new size.
    //
    // Recreate whenever the target is missing OR its size no longer matches the
    // requested (backbuffer) size. This is what fixes the "only the gear button
    // responds" and "welcome card appears in the bottom-right after leaving
    // fullscreen" symptoms: a stale target keeps its old dimensions, so the
    // centered card and the drawer/slider hit-test rects drift away from where
    // the mouse actually is.
    const bool size_mismatch =
        (d2d_render_target_ && (width != target_width_ || height != target_height_));
    if ((!d2d_render_target_ || size_mismatch) && swap_chain_) {
        (void)CreateDeviceResources(swap_chain_);
    }
    if (!d2d_render_target_) {
        return Core::Common::Status::Ok();
    }

    // 1. Calculate Alpha Fade-Out
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    if (ui_state_.is_streaming && !show_settings_drawer_) {
        if (nowMs - last_mouse_interaction_tick_ > 3000) {
            // Fade out smoothly
            overlay_alpha_ = std::max(0.12f, overlay_alpha_ - 0.04f);
        }
    } else {
        overlay_alpha_ = 1.0f;
    }

    d2d_render_target_->BeginDraw();

    // 2. Draw Centered Welcome & Pairing Guide Card (when not streaming video)
    if (!ui_state_.is_streaming) {
        DrawWelcomeGuide(static_cast<float>(width), static_cast<float>(height));
    }

    // 3. Draw Status Pill
    DrawStatusPill(static_cast<float>(width), static_cast<float>(height));

    // 4. Draw Settings Drawer (if open)
    if (show_settings_drawer_) {
        DrawSettingsDrawer(static_cast<float>(width), static_cast<float>(height));
    }

    HRESULT hr = d2d_render_target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }

    return Core::Common::Status::Ok();
}

void OverlayRenderer::DrawWelcomeGuide(float width, float height) {
    // Centered card
    const float cardW = 560.0f;
    const float cardH = 320.0f;
    const float cardX = (width - cardW) * 0.5f;
    const float cardY = (height - cardH) * 0.5f;

    D2D1_ROUNDED_RECT cardRRect = D2D1::RoundedRect(
        D2D1::RectF(cardX, cardY, cardX + cardW, cardY + cardH),
        16.0f, 16.0f
    );

    // Card background & subtle border
    if (brush_panel_bg_) {
        brush_panel_bg_->SetOpacity(0.88f);
        d2d_render_target_->FillRoundedRectangle(&cardRRect, brush_panel_bg_.Get());
    }
    if (brush_slider_track_) {
        d2d_render_target_->DrawRoundedRectangle(&cardRRect, brush_slider_track_.Get(), 1.5f);
    }

    // Title: CarPlayStation
    D2D1_RECT_F titleRect = D2D1::RectF(cardX + 24.0f, cardY + 24.0f, cardX + cardW - 24.0f, cardY + 60.0f);
    if (brush_text_white_ && tf_header_) {
        brush_text_white_->SetOpacity(1.0f);
        d2d_render_target_->DrawText(
            L"CarPlayStation — 无线 CarPlay 车机接收端",
            25,
            tf_header_.Get(),
            &titleRect,
            brush_text_white_.Get()
        );
    }

    // Subtitle / Pairing Step 1 & 2
    const std::wstring guideText =
        L"连接指南：\n"
        L"1. 确认本机蓝牙已开启，等待 iPhone 搜索配对设备；\n"
        L"2. 打开 iPhone「设置」->「蓝牙」或「通用 -> CarPlay 车载」；\n"
        L"3. 选择本机蓝牙名称进行配对，并允许 CarPlay 车载与无线连接；\n"
        L"4. 系统将自动完成 iAP2 握手与 Wi-Fi AP 热点切换。\n\n"
        L"快捷键操作：\n"
        L"• F11: 切换全屏沉浸模式\n"
        L"• F2: 打开 / 关闭 3-Lane WASAPI 音频调节面板\n"
        L"• ESC: 返回 / 取消";

    D2D1_RECT_F guideRect = D2D1::RectF(cardX + 24.0f, cardY + 70.0f, cardX + cardW - 24.0f, cardY + cardH - 24.0f);
    if (brush_text_gray_ && tf_normal_) {
        brush_text_gray_->SetOpacity(0.95f);
        d2d_render_target_->DrawText(
            guideText.c_str(),
            static_cast<UINT32>(guideText.length()),
            tf_normal_.Get(),
            &guideRect,
            brush_text_gray_.Get()
        );
    }
}

void OverlayRenderer::DrawStatusPill(float width, float height) {
    (void)height;
    // Status text & color
    std::wstring statusText;
    ID2D1SolidColorBrush* dotBrush = brush_text_gray_.Get();

    switch (ui_state_.session_state) {
        case Core::Session::SessionState::Idle:
            statusText = L"CarPlayStation — 等待 iPhone 连接 (请开启蓝牙配对)";
            dotBrush = brush_status_orange_.Get();
            break;
        case Core::Session::SessionState::BtDiscovered:
            statusText = L"检测到 iPhone 蓝牙配对请求...";
            dotBrush = brush_accent_blue_.Get();
            break;
        case Core::Session::SessionState::iAP2Authenticated:
            statusText = L"iAP2 认证成功，建立 Wi-Fi 连接通道...";
            dotBrush = brush_accent_blue_.Get();
            break;
        case Core::Session::SessionState::WifiHandoff:
            statusText = L"Wi-Fi AP 已握手，启动 AirPlay 会话...";
            dotBrush = brush_accent_blue_.Get();
            break;
        case Core::Session::SessionState::AirPlayStreaming: {
            std::wstring dev = ui_state_.connected_device_name.empty() ? L"iPhone" :
                std::wstring(ui_state_.connected_device_name.begin(), ui_state_.connected_device_name.end());
            statusText = L"已连接: " + dev + L" | " +
                std::to_wstring(ui_state_.current_video_width) + L"x" +
                std::to_wstring(ui_state_.current_video_height) + L" (60 FPS)";
            dotBrush = brush_status_green_.Get();
            break;
        }
        case Core::Session::SessionState::Paused:
        case Core::Session::SessionState::Teardown:
            statusText = L"CarPlay 会话已断开";
            dotBrush = brush_status_orange_.Get();
            break;
    }

    // Pill geometry
    float pillWidth = 420.0f;
    float pillHeight = 32.0f;
    float pillX = 16.0f;
    float pillY = 16.0f;

    D2D1_ROUNDED_RECT pillRect = D2D1::RoundedRect(
        D2D1::RectF(pillX, pillY, pillX + pillWidth, pillY + pillHeight),
        16.0f, 16.0f
    );

    // Apply alpha
    if (brush_panel_bg_) brush_panel_bg_->SetOpacity(0.80f * overlay_alpha_);
    if (brush_text_white_) brush_text_white_->SetOpacity(overlay_alpha_);
    if (dotBrush) dotBrush->SetOpacity(overlay_alpha_);

    d2d_render_target_->FillRoundedRectangle(&pillRect, brush_panel_bg_.Get());

    // Status Dot
    D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(pillX + 16.0f, pillY + 16.0f), 5.0f, 5.0f);
    d2d_render_target_->FillEllipse(&dot, dotBrush);

    // Status Text
    D2D1_RECT_F textRect = D2D1::RectF(pillX + 28.0f, pillY + 7.0f, pillX + pillWidth - 12.0f, pillY + pillHeight);
    d2d_render_target_->DrawText(
        statusText.c_str(),
        static_cast<UINT32>(statusText.length()),
        tf_normal_.Get(),
        &textRect,
        brush_text_white_.Get()
    );

    // Gear button (top right)
    gear_btn_rect_ = D2D1::RectF(width - 52.0f, 16.0f, width - 16.0f, 48.0f);
    D2D1_ROUNDED_RECT gearRect = D2D1::RoundedRect(gear_btn_rect_, 8.0f, 8.0f);
    d2d_render_target_->FillRoundedRectangle(&gearRect, brush_panel_bg_.Get());

    D2D1_RECT_F gearTextRect = D2D1::RectF(width - 48.0f, 21.0f, width - 20.0f, 45.0f);
    d2d_render_target_->DrawText(
        L"设置",
        2,
        tf_small_.Get(),
        &gearTextRect,
        brush_text_white_.Get()
    );
}

void OverlayRenderer::DrawSettingsDrawer(float width, float height) {
    (void)height;
    float drawerW = 340.0f;
    float drawerH = 260.0f;
    float drawerX = width - drawerW - 16.0f;
    float drawerY = 56.0f;
    drawer_rect_ = D2D1::RectF(drawerX, drawerY, drawerX + drawerW, drawerY + drawerH);

    D2D1_ROUNDED_RECT drawerRRect = D2D1::RoundedRect(drawer_rect_, 12.0f, 12.0f);
    if (brush_panel_bg_) brush_panel_bg_->SetOpacity(0.92f);
    d2d_render_target_->FillRoundedRectangle(&drawerRRect, brush_panel_bg_.Get());

    // Header Title
    D2D1_RECT_F titleRect = D2D1::RectF(drawerX + 16.0f, drawerY + 12.0f, drawerX + drawerW - 44.0f, drawerY + 36.0f);
    d2d_render_target_->DrawText(
        L"音频与音量控制面板 (3-Lane WASAPI)",
        24,
        tf_header_.Get(),
        &titleRect,
        brush_text_white_.Get()
    );

    // Close button [X]
    drawer_close_btn_rect_ = D2D1::RectF(drawerX + drawerW - 36.0f, drawerY + 10.0f, drawerX + drawerW - 12.0f, drawerY + 34.0f);
    D2D1_ROUNDED_RECT closeRRect = D2D1::RoundedRect(drawer_close_btn_rect_, 6.0f, 6.0f);
    if (brush_slider_track_) {
        d2d_render_target_->FillRoundedRectangle(&closeRRect, brush_slider_track_.Get());
    }
    D2D1_RECT_F closeTextRect = D2D1::RectF(drawerX + drawerW - 32.0f, drawerY + 12.0f, drawerX + drawerW - 16.0f, drawerY + 32.0f);
    d2d_render_target_->DrawText(
        L"✕",
        1,
        tf_small_.Get(),
        &closeTextRect,
        brush_text_white_.Get()
    );

    // Sliders setup
    sliders_.clear();
    sliders_.push_back({ L"媒体音乐音量 (MainMedia)", &settings_.audio.media_volume, 0.0f, 1.0f, {}, false });
    sliders_.push_back({ L"导航播报音量 (AltNav)", &settings_.audio.navigation_volume, 0.0f, 1.0f, {}, false });
    sliders_.push_back({ L"通话/Siri音量 (Telephony)", &settings_.audio.telephony_volume, 0.0f, 1.0f, {}, false });
    sliders_.push_back({ L"智能压音比率 (Ducking)", &settings_.audio.ducking_ratio, 0.0f, 1.0f, {}, false });

    float curY = drawerY + 46.0f;
    for (size_t i = 0; i < sliders_.size(); ++i) {
        auto& sl = sliders_[i];
        float val = (sl.value_ptr) ? *sl.value_ptr : 1.0f;

        // Label + percentage
        std::wstring labelWithVal = sl.label + L": " + std::to_wstring(static_cast<int>(val * 100.0f)) + L"%";
        D2D1_RECT_F labelRect = D2D1::RectF(drawerX + 16.0f, curY, drawerX + drawerW - 16.0f, curY + 18.0f);
        d2d_render_target_->DrawText(
            labelWithVal.c_str(),
            static_cast<UINT32>(labelWithVal.length()),
            tf_small_.Get(),
            &labelRect,
            brush_text_gray_.Get()
        );

        // Track rect
        curY += 20.0f;
        float trackLeft = drawerX + 16.0f;
        float trackRight = drawerX + drawerW - 16.0f;
        float trackWidth = trackRight - trackLeft;
        sl.track_rect = D2D1::RectF(trackLeft, curY, trackRight, curY + 6.0f);

        D2D1_ROUNDED_RECT trackRRect = D2D1::RoundedRect(sl.track_rect, 3.0f, 3.0f);
        d2d_render_target_->FillRoundedRectangle(&trackRRect, brush_slider_track_.Get());

        // Filled active track
        float fillRight = trackLeft + trackWidth * std::clamp(val, 0.0f, 1.0f);
        D2D1_ROUNDED_RECT activeRRect = D2D1::RoundedRect(
            D2D1::RectF(trackLeft, curY, fillRight, curY + 6.0f),
            3.0f, 3.0f
        );
        d2d_render_target_->FillRoundedRectangle(&activeRRect, brush_accent_blue_.Get());

        // Thumb knob
        D2D1_ELLIPSE thumb = D2D1::Ellipse(D2D1::Point2F(fillRight, curY + 3.0f), 7.0f, 7.0f);
        d2d_render_target_->FillEllipse(&thumb, brush_slider_thumb_.Get());

        curY += 24.0f;
    }
}

bool OverlayRenderer::OnMouseDown(float clientX, float clientY) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    last_mouse_interaction_tick_ = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    overlay_alpha_ = 1.0f;

    // 1. Check Gear button
    if (clientX >= gear_btn_rect_.left && clientX <= gear_btn_rect_.right &&
        clientY >= gear_btn_rect_.top && clientY <= gear_btn_rect_.bottom) {
        show_settings_drawer_ = !show_settings_drawer_;
        return true;
    }

    // 2. Check inside Drawer
    if (show_settings_drawer_) {
        // 2.1 Check Close [X] button
        if (clientX >= drawer_close_btn_rect_.left && clientX <= drawer_close_btn_rect_.right &&
            clientY >= drawer_close_btn_rect_.top && clientY <= drawer_close_btn_rect_.bottom) {
            show_settings_drawer_ = false;
            return true;
        }

        if (clientX >= drawer_rect_.left && clientX <= drawer_rect_.right &&
            clientY >= drawer_rect_.top && clientY <= drawer_rect_.bottom) {

            // Check if clicking a slider track
            for (size_t i = 0; i < sliders_.size(); ++i) {
                auto& sl = sliders_[i];
                if (clientY >= sl.track_rect.top - 8.0f && clientY <= sl.track_rect.bottom + 8.0f &&
                    clientX >= sl.track_rect.left && clientX <= sl.track_rect.right) {
                    active_dragging_slider_ = static_cast<int>(i);
                    float w = sl.track_rect.right - sl.track_rect.left;
                    if (w > 0.0f && sl.value_ptr) {
                        *sl.value_ptr = std::clamp((clientX - sl.track_rect.left) / w, sl.min_val, sl.max_val);
                        if (settings_callback_) {
                            settings_callback_(settings_);
                        }
                    }
                    return true;
                }
            }
            return true; // Click inside drawer consumed
        } else {
            // Click outside drawer dismisses the drawer
            show_settings_drawer_ = false;
            return true;
        }
    }

    return false;
}

bool OverlayRenderer::OnMouseMove(float clientX, float clientY) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    last_mouse_interaction_tick_ = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    overlay_alpha_ = 1.0f;

    if (active_dragging_slider_ >= 0 && active_dragging_slider_ < static_cast<int>(sliders_.size())) {
        auto& sl = sliders_[active_dragging_slider_];
        float w = sl.track_rect.right - sl.track_rect.left;
        if (w > 0.0f && sl.value_ptr) {
            *sl.value_ptr = std::clamp((clientX - sl.track_rect.left) / w, sl.min_val, sl.max_val);
            if (settings_callback_) {
                settings_callback_(settings_);
            }
        }
        return true;
    }

    // Check hover
    if (show_settings_drawer_) {
        if (clientX >= drawer_rect_.left && clientX <= drawer_rect_.right &&
            clientY >= drawer_rect_.top && clientY <= drawer_rect_.bottom) {
            return true;
        }
    }

    return false;
}

bool OverlayRenderer::OnMouseUp(float clientX, float clientY) {
    (void)clientX;
    (void)clientY;
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_dragging_slider_ >= 0) {
        active_dragging_slider_ = -1;
        return true;
    }
    return false;
}

void OverlayRenderer::Shutdown() {
    initialized_ = false;
    DiscardDeviceResources();
    tf_small_.Reset();
    tf_normal_.Reset();
    tf_header_.Reset();
    dwrite_factory_.Reset();
    d2d_factory_.Reset();
}

} // namespace CarPlayStation::UI
