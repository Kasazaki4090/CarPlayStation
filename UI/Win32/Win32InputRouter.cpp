#include "Win32InputRouter.h"
#include "D3D11Renderer.h"
#include "OverlayRenderer.h"
#include <windowsx.h>

namespace CarPlayStation::UI {

Win32InputRouter::Win32InputRouter(
    std::shared_ptr<Application::IApplicationApi> appApi,
    D3D11Renderer* renderer,
    OverlayRenderer* overlay)
    : app_api_(std::move(appApi)),
      renderer_(renderer),
      overlay_(overlay) {
}

bool Win32InputRouter::OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEWHEEL:
            return HandleMouse(hwnd, msg, wParam, lParam);

        case WM_KEYDOWN:
        case WM_KEYUP:
            return HandleKeyboard(hwnd, msg, wParam, lParam);

        case WM_KILLFOCUS:
            Reset();
            return false;

        default:
            break;
    }
    return false;
}

void Win32InputRouter::Reset() {
    if (is_mouse_down_ && app_api_) {
        is_mouse_down_ = false;
        Core::Input::TouchPoint tp{ 0, last_norm_x_, last_norm_y_ };
        (void)app_api_->ForwardTouchEvent(Core::Input::TouchAction::Cancel, tp);
    }
}

bool Win32InputRouter::HandleMouse(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const float clientX = static_cast<float>(GET_X_LPARAM(lParam));
    const float clientY = static_cast<float>(GET_Y_LPARAM(lParam));

    // 1. Give Overlay HUD first chance to consume mouse events
    if (overlay_) {
        if (msg == WM_LBUTTONDOWN && overlay_->OnMouseDown(clientX, clientY)) {
            return true;
        }
        if (msg == WM_MOUSEMOVE && overlay_->OnMouseMove(clientX, clientY)) {
            return true;
        }
        if (msg == WM_LBUTTONUP && overlay_->OnMouseUp(clientX, clientY)) {
            return true;
        }
    }

    if (!app_api_ || !renderer_) {
        return false;
    }

    // 2. Compute Letterbox Normalized Coordinates [0.0, 1.0]
    NormalizedPoint normPt = renderer_->WindowToNormalized(clientX, clientY);
    last_norm_x_ = normPt.x;
    last_norm_y_ = normPt.y;

    switch (msg) {
        case WM_LBUTTONDOWN: {
            if (normPt.inside_viewport) {
                is_mouse_down_ = true;
                SetCapture(hwnd);
                Core::Input::TouchPoint tp{ 0, normPt.x, normPt.y };
                (void)app_api_->ForwardTouchEvent(Core::Input::TouchAction::Down, tp);
                return true;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (is_mouse_down_) {
                Core::Input::TouchPoint tp{ 0, normPt.x, normPt.y };
                (void)app_api_->ForwardTouchEvent(Core::Input::TouchAction::Move, tp);
                return true;
            }
            break;
        }

        case WM_LBUTTONUP: {
            if (is_mouse_down_) {
                is_mouse_down_ = false;
                ReleaseCapture();
                Core::Input::TouchPoint tp{ 0, normPt.x, normPt.y };
                (void)app_api_->ForwardTouchEvent(Core::Input::TouchAction::Up, tp);
                return true;
            }
            break;
        }

        case WM_RBUTTONDOWN: {
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::Back, true);
            return true;
        }

        case WM_RBUTTONUP: {
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::Back, false);
            return true;
        }

        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0) {
                (void)app_api_->ForwardKnobEvent(Core::Input::KnobAction::RotateCounterClockwise, 1);
            } else if (delta < 0) {
                (void)app_api_->ForwardKnobEvent(Core::Input::KnobAction::RotateClockwise, 1);
            }
            return true;
        }

        default:
            break;
    }

    return false;
}

bool Win32InputRouter::HandleKeyboard(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)hwnd;
    (void)lParam;
    if (!app_api_) {
        return false;
    }

    const bool isPressed = (msg == WM_KEYDOWN);

    // F2 toggles overlay settings drawer
    if (wParam == VK_F2 && isPressed && overlay_) {
        overlay_->ToggleSettingsDrawer();
        return true;
    }

    // ESC closes settings drawer if open
    if (wParam == VK_ESCAPE && isPressed && overlay_ && overlay_->IsSettingsDrawerOpen()) {
        overlay_->ToggleSettingsDrawer();
        return true;
    }

    // Only emit knob/button on key-press to avoid double triggers
    if (!isPressed) {
        if (wParam == VK_BACK || wParam == VK_ESCAPE) {
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::Back, false);
            return true;
        }
        return false;
    }

    switch (wParam) {
        // Rotary knob directional navigation
        case VK_UP:
        case 'W':
            (void)app_api_->ForwardKnobEvent(Core::Input::KnobAction::PressUp, 1);
            return true;

        case VK_DOWN:
        case 'S':
            (void)app_api_->ForwardKnobEvent(Core::Input::KnobAction::PressDown, 1);
            return true;

        case VK_LEFT:
        case 'A':
            (void)app_api_->ForwardKnobEvent(Core::Input::KnobAction::PressLeft, 1);
            return true;

        case VK_RIGHT:
        case 'D':
            (void)app_api_->ForwardKnobEvent(Core::Input::KnobAction::PressRight, 1);
            return true;

        // Rotary knob select / push
        case VK_RETURN:
            // Ignore Alt+Enter (handled by MainWindow for fullscreen)
            if (!(GetKeyState(VK_MENU) & 0x8000)) {
                (void)app_api_->ForwardKnobEvent(Core::Input::KnobAction::PushDown, 1);
                return true;
            }
            break;

        // Automotive Back button
        case VK_BACK:
        case VK_ESCAPE:
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::Back, true);
            return true;

        // Media controls
        case VK_SPACE:
        case VK_MEDIA_PLAY_PAUSE:
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::PlayPause, true);
            return true;

        case VK_MEDIA_NEXT_TRACK:
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::NextTrack, true);
            return true;

        case VK_MEDIA_PREV_TRACK:
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::PrevTrack, true);
            return true;

        // Siri activation
        case VK_F1:
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::Siri, true);
            return true;

        // Telephony call controls
        case VK_F3:
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::CallAccept, true);
            return true;

        case VK_F4:
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::CallReject, true);
            return true;

        // Volume controls
        case VK_VOLUME_UP:
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::VolumeUp, true);
            return true;

        case VK_VOLUME_DOWN:
            (void)app_api_->ForwardButtonEvent(Core::Input::ButtonType::VolumeDown, true);
            return true;

        default:
            break;
    }

    return false;
}

} // namespace CarPlayStation::UI
