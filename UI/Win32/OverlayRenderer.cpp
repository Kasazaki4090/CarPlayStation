#include "OverlayRenderer.h"
#include "Core/Common/Logging.h"
#include <chrono>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace CarPlayStation::UI {

namespace {
// UTF-8 (std::string from Application/Core) -> UTF-16 for Direct2D text.
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

// Palette colour -> Direct2D colour.
inline D2D1_COLOR_F ToD2D(const UiColor& c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}
}  // namespace

OverlayRenderer::OverlayRenderer() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    last_mouse_interaction_tick_ = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    // Start in whatever appearance Windows is currently set to.
    theme_ = ThemeHelper::CurrentTheme();
    palette_ = ThemeHelper::BuildPalette(theme_);
}

OverlayRenderer::~OverlayRenderer() {
    Shutdown();
}

Core::Common::Status OverlayRenderer::Init(IDXGISwapChain1* swapChain, uint32_t backBufferIndex) {
    if (initialized_) {
        return Core::Common::Status::Ok();
    }

    swap_chain_ = swapChain;
    back_buffer_index_ = backBufferIndex;

    // 1. Create a standalone Direct2D factory. When the render target is later
    //    created over the swapchain's backbuffer (a DXGI surface), D2D
    //    internally binds to the D3D11 device that owns that surface, so the
    //    overlay composes with the D3D11 video on the same backbuffer.
    D2D1_FACTORY_OPTIONS options{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    __uuidof(ID2D1Factory),
                                    &options,
                                    &d2d_factory_);
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
    auto status = CreateDeviceResources(swapChain, backBufferIndex);
    if (status.IsError()) {
        return status;
    }

    initialized_ = true;
    return Core::Common::Status::Ok();
}

Core::Common::Status OverlayRenderer::CreateDeviceResources(IDXGISwapChain1* swapChain, uint32_t backBufferIndex) {
    if (!swapChain || !d2d_factory_) {
        return Core::Common::Status::Ok();
    }

    DiscardDeviceResources();

    // For a flip-model swapchain the back buffer is the LAST buffer, not
    // buffer 0 (buffer 0 is the front buffer on screen). The D2D target must
    // render into the same back buffer the D3D11 video draws into, otherwise
    // the overlay composites into a buffer that is never presented.
    const uint32_t bufIndex = backBufferIndex;
    ComPtr<IDXGISurface> backBufferSurface;
    HRESULT hr = swapChain->GetBuffer(bufIndex, IID_PPV_ARGS(&backBufferSurface));
    if (FAILED(hr)) {
        return Core::Common::MakeError(Core::Common::StatusCode::MediaPipelineError,
            "GetBuffer(" + std::to_string(bufIndex) + ") for DXGI surface failed");
    }

    // Create D2D Render Target over DXGI Surface
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );

    // Create the render target over the swapchain's backbuffer. D2D binds to
    // the D3D11 device that owns this surface, so the overlay composes with
    // the D3D11 video on the same backbuffer.
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

    // Create Brushes from the active palette (see ThemeHelper).
    d2d_render_target_->CreateSolidColorBrush(ToD2D(palette_.text_primary), &brush_text_white_);
    d2d_render_target_->CreateSolidColorBrush(ToD2D(palette_.text_secondary), &brush_text_gray_);
    d2d_render_target_->CreateSolidColorBrush(ToD2D(palette_.panel_bg), &brush_panel_bg_);
    d2d_render_target_->CreateSolidColorBrush(ToD2D(palette_.accent), &brush_accent_blue_);
    d2d_render_target_->CreateSolidColorBrush(ToD2D(palette_.status_green), &brush_status_green_);
    d2d_render_target_->CreateSolidColorBrush(ToD2D(palette_.status_orange), &brush_status_orange_);
    d2d_render_target_->CreateSolidColorBrush(ToD2D(palette_.slider_track), &brush_slider_track_);
    d2d_render_target_->CreateSolidColorBrush(ToD2D(palette_.slider_thumb), &brush_slider_thumb_);
    d2d_render_target_->CreateSolidColorBrush(ToD2D(palette_.log_error), &brush_log_error_);

    return Core::Common::Status::Ok();
}

void OverlayRenderer::ApplyPaletteToBrushes() {
    if (brush_text_white_) brush_text_white_->SetColor(ToD2D(palette_.text_primary));
    if (brush_text_gray_) brush_text_gray_->SetColor(ToD2D(palette_.text_secondary));
    if (brush_panel_bg_) brush_panel_bg_->SetColor(ToD2D(palette_.panel_bg));
    if (brush_accent_blue_) brush_accent_blue_->SetColor(ToD2D(palette_.accent));
    if (brush_status_green_) brush_status_green_->SetColor(ToD2D(palette_.status_green));
    if (brush_status_orange_) brush_status_orange_->SetColor(ToD2D(palette_.status_orange));
    if (brush_slider_track_) brush_slider_track_->SetColor(ToD2D(palette_.slider_track));
    if (brush_slider_thumb_) brush_slider_thumb_->SetColor(ToD2D(palette_.slider_thumb));
    if (brush_log_error_) brush_log_error_->SetColor(ToD2D(palette_.log_error));
}

bool OverlayRenderer::ApplyTheme(UiTheme theme) {
    if (theme == theme_) return false;
    theme_ = theme;
    palette_ = ThemeHelper::BuildPalette(theme);
    ApplyPaletteToBrushes();
    return true;
}

void OverlayRenderer::DiscardDeviceResources() {
    brush_log_error_.Reset();
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

Core::Common::Status OverlayRenderer::Resize(IDXGISwapChain1* swapChain, uint32_t backBufferIndex) {
    if (!initialized_) {
        return Core::Common::Status::Ok();
    }
    swap_chain_ = swapChain;
    back_buffer_index_ = backBufferIndex;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return CreateDeviceResources(swapChain, backBufferIndex);
}

void OverlayRenderer::UpdateUiState(const Application::AppUiState& state) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ui_state_ = state;
    // Reset fade timer upon state transition
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    last_mouse_interaction_tick_ = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    overlay_alpha_ = 1.0f;
}

void OverlayRenderer::UpdateSettings(const Application::AppSettings& settings) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    settings_ = settings;
}

void OverlayRenderer::ToggleSettingsDrawer() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    show_settings_drawer_ = !show_settings_drawer_;
    overlay_alpha_ = 1.0f;
}

void OverlayRenderer::ToggleLogPanel() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    show_log_panel_ = !show_log_panel_;
    overlay_alpha_ = 1.0f;
}

void OverlayRenderer::ToggleWelcomeGuide() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    show_welcome_guide_ = !show_welcome_guide_;
    overlay_alpha_ = 1.0f;
}

void OverlayRenderer::DismissWelcomeGuide() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    show_welcome_guide_ = false;
}

bool OverlayRenderer::IsWelcomeGuideVisible() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return show_welcome_guide_ && !ui_state_.is_streaming;
}

Core::Common::Status OverlayRenderer::Draw(uint32_t width, uint32_t height) {
    if (!initialized_) {
        return Core::Common::Status::Ok();
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

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
        (void)CreateDeviceResources(swap_chain_, back_buffer_index_);
    }
    if (!d2d_render_target_ || !brush_panel_bg_ || !brush_text_white_ || !brush_text_gray_ ||
        !brush_accent_blue_ || !brush_slider_track_ || !brush_slider_thumb_ ||
        !tf_header_ || !tf_normal_ || !tf_small_) {
        return Core::Common::Status::Ok();
    }

    // 1. Calculate Alpha Fade-Out
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    if (ui_state_.is_streaming && !show_settings_drawer_ && !show_log_panel_) {
        if (nowMs - last_mouse_interaction_tick_ > 3000) {
            // Fade out smoothly
            overlay_alpha_ = std::max(0.12f, overlay_alpha_ - 0.04f);
        }
    } else {
        overlay_alpha_ = 1.0f;
    }

    d2d_render_target_->BeginDraw();

    // 2. Draw Centered Welcome & Pairing Guide Card (when not streaming video and not dismissed)
    if (!ui_state_.is_streaming && show_welcome_guide_) {
        DrawWelcomeGuide(static_cast<float>(width), static_cast<float>(height));
    }

    // 3. Draw Status Pill
    DrawStatusPill(static_cast<float>(width), static_cast<float>(height));

    // 3.5 Draw Telephony Call Banner (if active/incoming call)
    if (ui_state_.call_state.has_call) {
        DrawCallBanner(static_cast<float>(width), static_cast<float>(height));
    }

    // 4. Draw Settings Drawer (if open)
    if (show_settings_drawer_) {
        DrawSettingsDrawer(static_cast<float>(width), static_cast<float>(height));
    }

    // 5. Draw standalone Log Panel (if open) — kept separate from the video/HUD.
    if (show_log_panel_) {
        DrawLogPanel(static_cast<float>(width), static_cast<float>(height));
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
    const float cardH = 340.0f;
    const float cardX = (width - cardW) * 0.5f;
    const float cardY = (height - cardH) * 0.5f;

    welcome_card_rect_ = D2D1::RectF(cardX, cardY, cardX + cardW, cardY + cardH);

    D2D1_ROUNDED_RECT cardRRect = D2D1::RoundedRect(
        welcome_card_rect_,
        16.0f, 16.0f
    );

    // Card background & subtle border
    if (brush_panel_bg_) {
        brush_panel_bg_->SetOpacity(0.92f);
        d2d_render_target_->FillRoundedRectangle(&cardRRect, brush_panel_bg_.Get());
    }
    if (brush_slider_track_) {
        d2d_render_target_->DrawRoundedRectangle(&cardRRect, brush_slider_track_.Get(), 1.5f);
    }

    // Title: CarPlayStation
    const std::wstring titleText = L"CarPlayStation — 无线 CarPlay 车机接收端";
    D2D1_RECT_F titleRect = D2D1::RectF(cardX + 24.0f, cardY + 20.0f, cardX + cardW - 60.0f, cardY + 56.0f);
    if (brush_text_white_ && tf_header_) {
        brush_text_white_->SetOpacity(1.0f);
        d2d_render_target_->DrawText(
            titleText.c_str(),
            static_cast<UINT32>(titleText.length()),
            tf_header_.Get(),
            &titleRect,
            brush_text_white_.Get()
        );
    }

    // Close button [X] in top-right of Welcome card
    welcome_close_btn_rect_ = D2D1::RectF(cardX + cardW - 38.0f, cardY + 16.0f, cardX + cardW - 14.0f, cardY + 40.0f);
    D2D1_ROUNDED_RECT closeRRect = D2D1::RoundedRect(welcome_close_btn_rect_, 6.0f, 6.0f);
    if (brush_slider_track_) {
        d2d_render_target_->FillRoundedRectangle(&closeRRect, brush_slider_track_.Get());
    }
    D2D1_RECT_F closeTextRect = D2D1::RectF(cardX + cardW - 34.0f, cardY + 18.0f, cardX + cardW - 18.0f, cardY + 38.0f);
    if (brush_text_white_ && tf_small_) {
        d2d_render_target_->DrawText(
            L"X",
            1,
            tf_small_.Get(),
            &closeTextRect,
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
        L"• F1: 唤醒 / 交互 Siri (长按/按击)\n"
        L"• F3 / Enter: 接听电话 | F4 / Esc: 挂断/拒接电话\n"
        L"• F2: 切换音频与视频设置抽屉 | Ctrl + F4: 切换运行日志面板\n"
        L"• F11: 全屏切换 | ESC 或 点击 [X]: 隐藏此指引卡片";

    D2D1_RECT_F guideRect = D2D1::RectF(cardX + 24.0f, cardY + 62.0f, cardX + cardW - 24.0f, cardY + cardH - 16.0f);
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

void OverlayRenderer::DrawCallBanner(float width, float height) {
    (void)height;
    const auto& call = ui_state_.call_state;
    if (!call.has_call) return;

    // Centered at top, below status pill
    const float cardW = 440.0f;
    const float cardH = 88.0f;
    const float cardX = (width - cardW) * 0.5f;
    const float cardY = 56.0f;

    call_card_rect_ = D2D1::RectF(cardX, cardY, cardX + cardW, cardY + cardH);
    D2D1_ROUNDED_RECT cardRRect = D2D1::RoundedRect(call_card_rect_, 14.0f, 14.0f);

    // Dark solid translucent background
    if (brush_panel_bg_) {
        brush_panel_bg_->SetOpacity(0.96f);
        d2d_render_target_->FillRoundedRectangle(&cardRRect, brush_panel_bg_.Get());
    }
    if (brush_slider_track_) {
        d2d_render_target_->DrawRoundedRectangle(&cardRRect, brush_slider_track_.Get(), 1.5f);
    }

    // Call status & caller name
    std::wstring callerName = Utf8ToWide(call.display_name.empty() ? call.remote_id : call.display_name);
    if (callerName.empty()) callerName = L"未知来电";

    std::wstring statusLabel = call.is_incoming ? L"📞 来电振铃中..." : L"📞 通话中...";
    if (call.status == 1) statusLabel = L"📞 呼叫中...";

    // Draw caller name
    D2D1_RECT_F nameRect = D2D1::RectF(cardX + 18.0f, cardY + 14.0f, cardX + 260.0f, cardY + 44.0f);
    if (brush_text_white_ && tf_header_) {
        brush_text_white_->SetOpacity(1.0f);
        d2d_render_target_->DrawText(
            callerName.c_str(),
            static_cast<UINT32>(callerName.length()),
            tf_header_.Get(),
            &nameRect,
            brush_text_white_.Get()
        );
    }

    // Draw status / phone number
    std::wstring subText = statusLabel;
    if (!call.remote_id.empty() && call.remote_id != call.display_name) {
        subText += L" (" + Utf8ToWide(call.remote_id) + L")";
    }
    D2D1_RECT_F subRect = D2D1::RectF(cardX + 18.0f, cardY + 48.0f, cardX + 260.0f, cardY + 74.0f);
    if (brush_text_gray_ && tf_small_) {
        brush_text_gray_->SetOpacity(0.95f);
        d2d_render_target_->DrawText(
            subText.c_str(),
            static_cast<UINT32>(subText.length()),
            tf_small_.Get(),
            &subRect,
            brush_text_gray_.Get()
        );
    }

    // Buttons
    // [接听 (Enter/F3)] (only if incoming)
    // [挂断 (Esc/F4)]
    const float btnH = 34.0f;
    const float btnY = cardY + (cardH - btnH) * 0.5f;

    if (call.is_incoming) {
        const float acceptW = 76.0f;
        const float acceptX = cardX + cardW - 170.0f;
        call_accept_btn_rect_ = D2D1::RectF(acceptX, btnY, acceptX + acceptW, btnY + btnH);
        D2D1_ROUNDED_RECT acceptRRect = D2D1::RoundedRect(call_accept_btn_rect_, 8.0f, 8.0f);
        if (brush_status_green_) {
            brush_status_green_->SetOpacity(0.9f);
            d2d_render_target_->FillRoundedRectangle(&acceptRRect, brush_status_green_.Get());
        }
        D2D1_RECT_F acceptTextRect = D2D1::RectF(acceptX, btnY + 7.0f, acceptX + acceptW, btnY + btnH);
        if (brush_text_white_ && tf_small_) {
            d2d_render_target_->DrawText(
                L"接听",
                2,
                tf_small_.Get(),
                &acceptTextRect,
                brush_text_white_.Get()
            );
        }
    } else {
        call_accept_btn_rect_ = {};
    }

    const float rejectW = 76.0f;
    const float rejectX = cardX + cardW - 86.0f;
    call_reject_btn_rect_ = D2D1::RectF(rejectX, btnY, rejectX + rejectW, btnY + btnH);
    D2D1_ROUNDED_RECT rejectRRect = D2D1::RoundedRect(call_reject_btn_rect_, 8.0f, 8.0f);
    if (brush_log_error_) {
        brush_log_error_->SetOpacity(0.9f);
        d2d_render_target_->FillRoundedRectangle(&rejectRRect, brush_log_error_.Get());
    }
    D2D1_RECT_F rejectTextRect = D2D1::RectF(rejectX, btnY + 7.0f, rejectX + rejectW, btnY + btnH);
    const std::wstring rejectLabel = call.is_incoming ? L"拒接" : L"挂断";
    if (brush_text_white_ && tf_small_) {
        d2d_render_target_->DrawText(
            rejectLabel.c_str(),
            static_cast<UINT32>(rejectLabel.length()),
            tf_small_.Get(),
            &rejectTextRect,
            brush_text_white_.Get()
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
                Utf8ToWide(ui_state_.connected_device_name);
            // 分辨率/帧率/编码优先显示 Core 实际在推的码流（由
            // ApplicationService::SnapshotVideoFacts 回填）。Core 还没给出码流时
            // 退回设置里的有效分辨率，编码显示「检测中」。
            uint32_t w = ui_state_.current_video_width;
            uint32_t h = ui_state_.current_video_height;
            uint32_t fps = ui_state_.current_video_fps;
            if (w == 0 || h == 0) {
                auto [dw, dh] = settings_.video.GetEffectiveDimensions();
                w = dw;
                h = dh;
            }
            std::wstring codec = Utf8ToWide(ui_state_.current_video_codec);
            if (codec.empty()) codec = L"检测中";
            std::wstring hwText = ui_state_.is_hardware_decode ? L" [硬解]" : L" [软解]";
            statusText = L"已连接: " + dev + L" | " +
                std::to_wstring(w) + L"x" + std::to_wstring(h) + L" @ " +
                std::to_wstring(fps ? fps : settings_.video.fps) + L" FPS " + codec + hwText;
            if (ui_state_.now_playing.is_playing && !ui_state_.now_playing.title.empty()) {
                statusText += L" | 🎵 " + Utf8ToWide(ui_state_.now_playing.title);
                if (!ui_state_.now_playing.artist.empty()) {
                    statusText += L" - " + Utf8ToWide(ui_state_.now_playing.artist);
                }
            }
            dotBrush = brush_status_green_.Get();
            break;
        }
        case Core::Session::SessionState::Paused:
        case Core::Session::SessionState::Teardown:
            statusText = L"CarPlay 会话已断开";
            dotBrush = brush_status_orange_.Get();
            break;
    }

    // Pill geometry. The width follows the measured status text so the longer
    // "WxH @ fps CODEC" string still fits; it never grows into the gear button.
    float pillHeight = 32.0f;
    float pillX = 16.0f;
    float pillY = 16.0f;
    float textWidth = 0.0f;
    if (dwrite_factory_ && tf_normal_) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                statusText.c_str(), static_cast<UINT32>(statusText.size()),
                tf_normal_.Get(), 2000.0f, 64.0f, &layout))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                textWidth = metrics.widthIncludingTrailingWhitespace;
            }
        }
    }
    float pillWidth = std::max(420.0f, textWidth + 62.0f);
    const float maxPillWidth = width - 76.0f - pillX;  // keep clear of the gear button
    if (maxPillWidth > 200.0f && pillWidth > maxPillWidth) pillWidth = maxPillWidth;

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
    float drawerW = 360.0f;
    float drawerH = (height <= 500.0f) ? (height - 20.0f) : 410.0f;
    float drawerX = width - drawerW - 16.0f;
    float drawerY = (height <= 500.0f) ? 10.0f : 56.0f;
    drawer_rect_ = D2D1::RectF(drawerX, drawerY, drawerX + drawerW, drawerY + drawerH);

    D2D1_ROUNDED_RECT drawerRRect = D2D1::RoundedRect(drawer_rect_, 12.0f, 12.0f);
    if (brush_panel_bg_) brush_panel_bg_->SetOpacity(0.95f);
    d2d_render_target_->FillRoundedRectangle(&drawerRRect, brush_panel_bg_.Get());

    // Header Title
    D2D1_RECT_F titleRect = D2D1::RectF(drawerX + 16.0f, drawerY + 12.0f, drawerX + drawerW - 44.0f, drawerY + 36.0f);
    std::wstring drawerTitle = L"设置与音量控制面板";
    d2d_render_target_->DrawText(
        drawerTitle.c_str(),
        static_cast<UINT32>(drawerTitle.length()),
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
        L"X",
        1,
        tf_small_.Get(),
        &closeTextRect,
        brush_text_white_.Get()
    );

    drawer_buttons_.clear();
    float curY = drawerY + 44.0f;

    // --- Section 1: 视频分辨率与编码 ---
    {
        std::wstring resTitle = L"视频分辨率 (Resolution):";
        D2D1_RECT_F secRect = D2D1::RectF(drawerX + 16.0f, curY, drawerX + drawerW - 16.0f, curY + 18.0f);
        d2d_render_target_->DrawText(
            resTitle.c_str(),
            static_cast<UINT32>(resTitle.length()),
            tf_small_.Get(),
            &secRect,
            brush_accent_blue_.Get()
        );
        curY += 20.0f;

        // Resolution buttons Row 1: 720P, 1080P, 2K
        float btnW3 = (drawerW - 32.0f - 16.0f) / 3.0f;
        float btnH = 24.0f;

        auto addResBtn = [&](float bX, float bY, float bW, const std::wstring& text, Application::VideoResolutionPreset preset) {
            D2D1_RECT_F r = D2D1::RectF(bX, bY, bX + bW, bY + btnH);
            bool active = (settings_.video.resolution_preset == preset);
            drawer_buttons_.push_back({text, r, active, [this, preset]() {
                settings_.video.resolution_preset = preset;
                auto [w, h] = settings_.video.GetEffectiveDimensions();
                settings_.video.custom_width = w;
                settings_.video.custom_height = h;
            }});
        };

        addResBtn(drawerX + 16.0f + 0 * (btnW3 + 8.0f), curY, btnW3, L"720P", Application::VideoResolutionPreset::Res720P);
        addResBtn(drawerX + 16.0f + 1 * (btnW3 + 8.0f), curY, btnW3, L"1080P", Application::VideoResolutionPreset::Res1080P);
        addResBtn(drawerX + 16.0f + 2 * (btnW3 + 8.0f), curY, btnW3, L"2K", Application::VideoResolutionPreset::Res1440P);
        curY += btnH + 6.0f;

        // Resolution buttons Row 2: Car Ultrawide presets
        float btnW2 = (drawerW - 32.0f - 8.0f) / 2.0f;
        addResBtn(drawerX + 16.0f + 0 * (btnW2 + 8.0f), curY, btnW2, L"1280x480 (车机宽屏)", Application::VideoResolutionPreset::Car1280x480);
        addResBtn(drawerX + 16.0f + 1 * (btnW2 + 8.0f), curY, btnW2, L"1920x720 (高清宽屏)", Application::VideoResolutionPreset::Car1920x720);
        curY += btnH + 8.0f;

        // FPS & Codec
        std::wstring fpsTitle = L"帧率与编解码器 (FPS & Codec):";
        D2D1_RECT_F fpsSecRect = D2D1::RectF(drawerX + 16.0f, curY, drawerX + drawerW - 16.0f, curY + 18.0f);
        d2d_render_target_->DrawText(
            fpsTitle.c_str(),
            static_cast<UINT32>(fpsTitle.length()),
            tf_small_.Get(),
            &fpsSecRect,
            brush_accent_blue_.Get()
        );
        curY += 22.0f;

        float fBtnW = (drawerW - 32.0f - 24.0f) / 4.0f;
        auto addFpsBtn = [&](int idx, const std::wstring& text, uint32_t fps) {
            float bX = drawerX + 16.0f + idx * (fBtnW + 8.0f);
            D2D1_RECT_F r = D2D1::RectF(bX, curY, bX + fBtnW, curY + btnH);
            bool active = (settings_.video.fps == fps);
            drawer_buttons_.push_back({text, r, active, [this, fps]() {
                settings_.video.fps = fps;
            }});
        };
        auto addCodecBtn = [&](int idx, const std::wstring& text, Application::VideoCodecPreference codec) {
            float bX = drawerX + 16.0f + idx * (fBtnW + 8.0f);
            D2D1_RECT_F r = D2D1::RectF(bX, curY, bX + fBtnW, curY + btnH);
            bool active = (settings_.video.preferred_codec == codec);
            drawer_buttons_.push_back({text, r, active, [this, codec]() {
                settings_.video.preferred_codec = codec;
            }});
        };

        addFpsBtn(0, L"30 FPS", 30);
        addFpsBtn(1, L"60 FPS", 60);
        addCodecBtn(2, L"H.264", Application::VideoCodecPreference::H264);
        addCodecBtn(3, L"HEVC", Application::VideoCodecPreference::HEVC);
        curY += btnH + 14.0f;

        // Draw buttons
        for (const auto& btn : drawer_buttons_) {
            D2D1_ROUNDED_RECT bRect = D2D1::RoundedRect(btn.rect, 4.0f, 4.0f);
            if (btn.is_active) {
                d2d_render_target_->FillRoundedRectangle(&bRect, brush_accent_blue_.Get());
            } else {
                d2d_render_target_->FillRoundedRectangle(&bRect, brush_slider_track_.Get());
            }
            D2D1_RECT_F tRect = D2D1::RectF(btn.rect.left, btn.rect.top + 4.0f, btn.rect.right, btn.rect.bottom);
            d2d_render_target_->DrawText(
                btn.label.c_str(),
                static_cast<UINT32>(btn.label.length()),
                tf_small_.Get(),
                &tRect,
                btn.is_active ? brush_text_white_.Get() : brush_text_gray_.Get()
            );
        }
    }

    // --- Section 2: 音量与压音控制 ---
    std::wstring audioSecTitle = L"音量与压音调节 (WASAPI 3-Lane，采样率跟随系统):";
    D2D1_RECT_F audioSecRect = D2D1::RectF(drawerX + 16.0f, curY, drawerX + drawerW - 16.0f, curY + 18.0f);
    d2d_render_target_->DrawText(
        audioSecTitle.c_str(),
        static_cast<UINT32>(audioSecTitle.length()),
        tf_small_.Get(),
        &audioSecRect,
        brush_accent_blue_.Get()
    );
    curY += 22.0f;

    // Sliders setup
    sliders_.clear();
    sliders_.push_back({ L"媒体音乐音量 (MainMedia)", &settings_.audio.media_volume, 0.0f, 1.0f, {}, false });
    sliders_.push_back({ L"导航播报音量 (AltNav)", &settings_.audio.navigation_volume, 0.0f, 1.0f, {}, false });
    sliders_.push_back({ L"通话/Siri音量 (Telephony)", &settings_.audio.telephony_volume, 0.0f, 1.0f, {}, false });
    sliders_.push_back({ L"智能压音比率 (Ducking)", &settings_.audio.ducking_ratio, 0.0f, 1.0f, {}, false });

    for (size_t i = 0; i < sliders_.size(); ++i) {
        auto& sl = sliders_[i];
        float val = (sl.value_ptr) ? *sl.value_ptr : 1.0f;

        // Label + percentage
        std::wstring labelWithVal = sl.label + L": " + std::to_wstring(static_cast<int>(val * 100.0f)) + L"%";
        D2D1_RECT_F labelRect = D2D1::RectF(drawerX + 16.0f, curY, drawerX + drawerW - 16.0f, curY + 16.0f);
        d2d_render_target_->DrawText(
            labelWithVal.c_str(),
            static_cast<UINT32>(labelWithVal.length()),
            tf_small_.Get(),
            &labelRect,
            brush_text_gray_.Get()
        );

        // Track rect
        curY += 18.0f;
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

        curY += 22.0f;
    }
}

void OverlayRenderer::DrawLogPanel(float width, float height) {
    (void)width;
    // Standalone log panel, anchored bottom-left so it never overlaps the
    // status pill (top-left) or the settings drawer / gear button (top-right).
    const float panelW = 480.0f;
    const float panelH = 240.0f;
    const float panelX = 16.0f;
    const float panelY = height - panelH - 16.0f;
    log_panel_rect_ = D2D1::RectF(panelX, panelY, panelX + panelW, panelY + panelH);

    D2D1_ROUNDED_RECT panelRRect = D2D1::RoundedRect(log_panel_rect_, 12.0f, 12.0f);
    if (brush_panel_bg_) {
        brush_panel_bg_->SetOpacity(0.92f);
        d2d_render_target_->FillRoundedRectangle(&panelRRect, brush_panel_bg_.Get());
    }

    // Header title
    D2D1_RECT_F titleRect = D2D1::RectF(panelX + 16.0f, panelY + 12.0f, panelX + panelW - 44.0f, panelY + 36.0f);
    d2d_render_target_->DrawText(
        L"运行日志 (Log)",
        6,
        tf_header_.Get(),
        &titleRect,
        brush_text_white_.Get()
    );

    // Close button [X]
    log_panel_close_btn_rect_ = D2D1::RectF(panelX + panelW - 36.0f, panelY + 10.0f, panelX + panelW - 12.0f, panelY + 34.0f);
    D2D1_ROUNDED_RECT closeRRect = D2D1::RoundedRect(log_panel_close_btn_rect_, 6.0f, 6.0f);
    if (brush_slider_track_) {
        d2d_render_target_->FillRoundedRectangle(&closeRRect, brush_slider_track_.Get());
    }
    D2D1_RECT_F closeTextRect = D2D1::RectF(panelX + panelW - 32.0f, panelY + 12.0f, panelX + panelW - 16.0f, panelY + 32.0f);
    d2d_render_target_->DrawText(
        L"X",
        1,
        tf_small_.Get(),
        &closeTextRect,
        brush_text_white_.Get()
    );

    // Body: most-recent log lines, oldest at the top, newest at the bottom.
    const auto lines =
        CarPlayStation::Core::Common::detail::RingBufferLogSink::Instance().Snapshot(
            static_cast<std::size_t>(log_panel_line_count_));

    const float bodyTop = panelY + 44.0f;
    const float lineH = 16.0f;
    const float bodyLeft = panelX + 14.0f;
    const float bodyRight = panelX + panelW - 14.0f;

    // Start from the newest line and walk backwards so the newest lines sit at
    // the bottom of the panel (terminal-style).
    const int total = static_cast<int>(lines.size());
    for (int i = total - 1; i >= 0; --i) {
        const int rowFromBottom = total - 1 - i;
        const float y = (panelY + panelH - 12.0f) - (rowFromBottom + 1) * lineH;
        if (y < bodyTop) {
            break;
        }

        const std::string& line = lines[static_cast<std::size_t>(i)];
        const bool isError = line.find("[ERROR]") != std::string::npos;
        const bool isWarn = line.find("[WARN ]") != std::string::npos;

        // Convert UTF-8 log line to wide for D2D.
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, line.c_str(),
                                              static_cast<int>(line.size()), nullptr, 0);
        std::wstring wline(static_cast<std::size_t>(wlen), L'\0');
        if (wlen > 0) {
            MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                wline.data(), wlen);
        }

        ID2D1SolidColorBrush* lineBrush = brush_text_gray_.Get();
        if (isError && brush_log_error_) {
            lineBrush = brush_log_error_.Get();
        } else if (isWarn && brush_status_orange_) {
            lineBrush = brush_status_orange_.Get();
        }

        D2D1_RECT_F lineRect = D2D1::RectF(bodyLeft, y, bodyRight, y + lineH);
        d2d_render_target_->DrawText(
            wline.c_str(),
            static_cast<UINT32>(wline.size()),
            tf_small_.Get(),
            &lineRect,
            lineBrush
        );
    }
}

bool OverlayRenderer::OnMouseDown(float clientX, float clientY) {
    SettingsChangeCallback cb;
    Application::AppSettings settings_copy;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        last_mouse_interaction_tick_ = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        overlay_alpha_ = 1.0f;

        // 1. Check Gear button
        if (clientX >= gear_btn_rect_.left - 6.0f && clientX <= gear_btn_rect_.right + 6.0f &&
            clientY >= gear_btn_rect_.top - 6.0f && clientY <= gear_btn_rect_.bottom + 6.0f) {
            show_settings_drawer_ = !show_settings_drawer_;
            CPS_LOG_INFO("Overlay", "Gear button clicked, show_settings_drawer_ = ", show_settings_drawer_ ? "true" : "false");
            return true;
        }

        // 1.2 Check Telephony Call Banner buttons
        if (ui_state_.call_state.has_call) {
            if (ui_state_.call_state.is_incoming &&
                clientX >= call_accept_btn_rect_.left && clientX <= call_accept_btn_rect_.right &&
                clientY >= call_accept_btn_rect_.top && clientY <= call_accept_btn_rect_.bottom) {
                CPS_LOG_INFO("Overlay", "Call Banner [接听] clicked");
                if (app_api_) {
                    (void)app_api_->AcceptCall();
                }
                return true;
            }
            if (clientX >= call_reject_btn_rect_.left && clientX <= call_reject_btn_rect_.right &&
                clientY >= call_reject_btn_rect_.top && clientY <= call_reject_btn_rect_.bottom) {
                CPS_LOG_INFO("Overlay", "Call Banner [挂断/拒接] clicked");
                if (app_api_) {
                    (void)app_api_->EndCall();
                }
                return true;
            }
            if (clientX >= call_card_rect_.left && clientX <= call_card_rect_.right &&
                clientY >= call_card_rect_.top && clientY <= call_card_rect_.bottom) {
                return true; // Click inside call banner background consumed
            }
        }

        // 1.5 Check standalone Log Panel (independent of the settings drawer)
        if (show_log_panel_) {
            if (clientX >= log_panel_close_btn_rect_.left - 8.0f && clientX <= log_panel_close_btn_rect_.right + 8.0f &&
                clientY >= log_panel_close_btn_rect_.top - 8.0f && clientY <= log_panel_close_btn_rect_.bottom + 8.0f) {
                show_log_panel_ = false;
                CPS_LOG_INFO("Overlay", "Log panel [X] clicked, closed log panel");
                return true;
            }
            if (clientX >= log_panel_rect_.left && clientX <= log_panel_rect_.right &&
                clientY >= log_panel_rect_.top && clientY <= log_panel_rect_.bottom) {
                return true; // Click inside log panel consumed
            }
        }

        // 1.8 Check Centered Welcome Guide Card
        if (show_welcome_guide_ && !ui_state_.is_streaming) {
            if (clientX >= welcome_close_btn_rect_.left - 10.0f && clientX <= welcome_close_btn_rect_.right + 10.0f &&
                clientY >= welcome_close_btn_rect_.top - 10.0f && clientY <= welcome_close_btn_rect_.bottom + 10.0f) {
                show_welcome_guide_ = false;
                CPS_LOG_INFO("Overlay", "Welcome guide [X] button clicked, dismissed welcome guide");
                return true;
            }
            if (clientX >= welcome_card_rect_.left && clientX <= welcome_card_rect_.right &&
                clientY >= welcome_card_rect_.top && clientY <= welcome_card_rect_.bottom) {
                // The guide is informational, not modal: dismiss it and let
                // the same click reach the CarPlay display underneath.
                show_welcome_guide_ = false;
                CPS_LOG_INFO("Overlay", "Welcome guide dismissed by click-through");
            }
        }

        // 2. Check inside Drawer
        if (show_settings_drawer_) {
            // 2.1 Check Close [X] button (generous hit padding)
            if (clientX >= drawer_close_btn_rect_.left - 10.0f && clientX <= drawer_close_btn_rect_.right + 10.0f &&
                clientY >= drawer_close_btn_rect_.top - 10.0f && clientY <= drawer_close_btn_rect_.bottom + 10.0f) {
                show_settings_drawer_ = false;
                CPS_LOG_INFO("Overlay", "Drawer [X] button clicked, closed settings drawer");
                return true;
            }

            if (clientX >= drawer_rect_.left && clientX <= drawer_rect_.right &&
                clientY >= drawer_rect_.top && clientY <= drawer_rect_.bottom) {

                // Check button clicks first
                for (const auto& btn : drawer_buttons_) {
                    if (clientX >= btn.rect.left && clientX <= btn.rect.right &&
                        clientY >= btn.rect.top && clientY <= btn.rect.bottom) {
                        if (btn.on_click) {
                            btn.on_click();
                            cb = settings_callback_;
                            settings_copy = settings_;
                            CPS_LOG_INFO("Overlay", "Drawer button clicked");
                        }
                        break;
                    }
                }

                if (!cb) {
                    // Check if clicking a slider (generous area around track and label)
                    for (size_t i = 0; i < sliders_.size(); ++i) {
                        auto& sl = sliders_[i];
                        if (clientY >= sl.track_rect.top - 20.0f && clientY <= sl.track_rect.bottom + 16.0f &&
                            clientX >= sl.track_rect.left - 10.0f && clientX <= sl.track_rect.right + 10.0f) {
                            active_dragging_slider_ = static_cast<int>(i);
                            float w = sl.track_rect.right - sl.track_rect.left;
                            if (w > 0.0f && sl.value_ptr) {
                                *sl.value_ptr = std::clamp((clientX - sl.track_rect.left) / w, sl.min_val, sl.max_val);
                                cb = settings_callback_;
                                settings_copy = settings_;
                                CPS_LOG_INFO("Overlay", "Slider started dragging");
                            }
                            break;
                        }
                    }
                }
                // Consumed click inside drawer
                if (!cb) {
                    return true;
                }
            } else {
                // Click outside drawer dismisses the drawer
                show_settings_drawer_ = false;
                CPS_LOG_INFO("Overlay", "Clicked outside drawer, dismissed settings drawer");
                return true;
            }
        }
    }

    if (cb) {
        cb(settings_copy);
        return true;
    }

    return false;
}

bool OverlayRenderer::OnMouseMove(float clientX, float clientY) {
    SettingsChangeCallback cb;
    Application::AppSettings settings_copy;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        last_mouse_interaction_tick_ = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        overlay_alpha_ = 1.0f;

        if (active_dragging_slider_ >= 0 && active_dragging_slider_ < static_cast<int>(sliders_.size())) {
            auto& sl = sliders_[active_dragging_slider_];
            float w = sl.track_rect.right - sl.track_rect.left;
            if (w > 0.0f && sl.value_ptr) {
                *sl.value_ptr = std::clamp((clientX - sl.track_rect.left) / w, sl.min_val, sl.max_val);
                cb = settings_callback_;
                settings_copy = settings_;
            }
        } else if (show_settings_drawer_) {
            // Check hover inside drawer
            if (clientX >= drawer_rect_.left && clientX <= drawer_rect_.right &&
                clientY >= drawer_rect_.top && clientY <= drawer_rect_.bottom) {
                return true;
            }
        }
    }

    if (cb) {
        cb(settings_copy);
        return true;
    }

    return false;
}

bool OverlayRenderer::OnMouseUp(float clientX, float clientY) {
    (void)clientX;
    (void)clientY;
    SettingsChangeCallback cb;
    Application::AppSettings settings_copy;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (active_dragging_slider_ >= 0) {
            active_dragging_slider_ = -1;
            cb = settings_callback_;
            settings_copy = settings_;
        }
    }

    if (cb) {
        cb(settings_copy);
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
