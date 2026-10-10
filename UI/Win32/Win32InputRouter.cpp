#include "Win32InputRouter.h"
#include "D3D11Renderer.h"
#include "OverlayRenderer.h"
#include "Core/Common/Logging.h"
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
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP:
        case WM_POINTERLEAVE:
            return HandlePointer(hwnd, msg, wParam, lParam);

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
    is_overlay_capturing_ = false;

    // Cancel every live touch contact, not just the pointer the mouse path tracks.
    for (size_t slot = 0; slot < pointer_count_; ++slot) {
        if (!pointer_slots_[slot].down || pointer_slots_[slot].overlay || !app_api_) {
            continue;
        }
        Core::Input::TouchPoint tp{ static_cast<uint32_t>(slot),
            pointer_slots_[slot].x, pointer_slots_[slot].y };
        auto status = app_api_->ForwardTouchEvent(Core::Input::TouchAction::Cancel, tp);
        if (status.IsError()) {
            CPS_LOG_WARN("InputRouter", "Failed to cancel forwarded touch: ", status.Message());
        }
    }
    pointer_count_ = 0;
    pointer_ids_.fill(0);
    pointer_slots_.fill(PointerSlot{});

    if (is_mouse_down_ && app_api_) {
        is_mouse_down_ = false;
        Core::Input::TouchPoint tp{ 0, last_norm_x_, last_norm_y_ };
        auto status = app_api_->ForwardTouchEvent(Core::Input::TouchAction::Cancel, tp);
        if (status.IsError()) {
            CPS_LOG_WARN("InputRouter", "Failed to cancel forwarded touch: ", status.Message());
        }
    }
}

size_t Win32InputRouter::FindPointerSlot(UINT pointerId) const noexcept {
    for (size_t i = 0; i < pointer_count_; ++i) {
        if (pointer_ids_[i] == pointerId) return i;
    }
    return kMaxPointers;
}

size_t Win32InputRouter::AddPointerSlot(UINT pointerId) noexcept {
    if (const size_t existing = FindPointerSlot(pointerId); existing < kMaxPointers) {
        return existing;
    }
    if (pointer_count_ >= kMaxPointers) return kMaxPointers;
    const size_t slot = pointer_count_++;
    pointer_ids_[slot] = pointerId;
    pointer_slots_[slot] = PointerSlot{};
    return slot;
}

void Win32InputRouter::RemovePointerSlot(size_t slot) noexcept {
    if (slot >= pointer_count_) return;
    // Compact the arrays so a later finger reuses the freed slot index.
    for (size_t i = slot; i + 1 < pointer_count_; ++i) {
        pointer_ids_[i] = pointer_ids_[i + 1];
        pointer_slots_[i] = pointer_slots_[i + 1];
    }
    --pointer_count_;
    pointer_ids_[pointer_count_] = 0;
    pointer_slots_[pointer_count_] = PointerSlot{};
}

bool Win32InputRouter::HandlePointer(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    const UINT pointerId = LOWORD(wParam);

    POINTER_INFO info{};
    if (!GetPointerInfo(pointerId, &info)) {
        return false;
    }
    // Only digitizer contact reaches CarPlay. Mouse-origin pointers keep going through
    // the WM_MOUSE* path, which also carries the right-button Back and wheel gestures.
    if (info.pointerType != PT_TOUCH) {
        return false;
    }

    POINTER_TOUCH_INFO touch{};
    if (!GetPointerTouchInfo(pointerId, &touch)) {
        return false;
    }

    // Screen coordinates -> client coordinates, then the same letterbox mapping the
    // mouse path uses.
    POINT pt{ touch.pointerInfo.ptPixelLocation.x, touch.pointerInfo.ptPixelLocation.y };
    if (!ScreenToClient(hwnd, &pt)) {
        return false;
    }
    const float clientX = static_cast<float>(pt.x);
    const float clientY = static_cast<float>(pt.y);

    if (msg == WM_POINTERDOWN && overlay_) {
        if (overlay_->OnMouseDown(clientX, clientY)) {
            // The HUD owns this finger; keep the slot so its moves and release stay
            // out of the CarPlay stream.
            if (const size_t slot = AddPointerSlot(pointerId); slot < kMaxPointers) {
                pointer_slots_[slot] = PointerSlot{ true, true, clientX, clientY };
            }
            is_overlay_capturing_ = true;
            SetCapture(hwnd);
            return true;
        }
    }

    const size_t slot = FindPointerSlot(pointerId);
    if (slot >= kMaxPointers) {
        if (slot == kMaxPointers && msg != WM_POINTERUP && msg != WM_POINTERLEAVE) {
            // A third finger: more contacts than the advertised descriptor declares.
            CPS_LOG_DEBUG("InputRouter", "Ignoring pointer beyond the advertised contact count");
        }
        return false;
    }
    if (pointer_slots_[slot].overlay) {
        if (msg == WM_POINTERUP || msg == WM_POINTERLEAVE) {
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            is_overlay_capturing_ = false;
            if (overlay_) {
                (void)overlay_->OnMouseUp(clientX, clientY);
            }
            RemovePointerSlot(slot);
        } else if (overlay_) {
            (void)overlay_->OnMouseMove(clientX, clientY);
        }
        return true;
    }

    if (!app_api_ || !renderer_) {
        return false;
    }

    const NormalizedPoint normPt = renderer_->WindowToNormalized(clientX, clientY);
    PointerSlot& ps = pointer_slots_[slot];
    ps.x = normPt.x;
    ps.y = normPt.y;
    last_norm_x_ = normPt.x;
    last_norm_y_ = normPt.y;

    Core::Input::TouchPoint tp{ static_cast<uint32_t>(slot), normPt.x, normPt.y };
    switch (msg) {
        case WM_POINTERDOWN:
            if (!normPt.inside_viewport) {
                RemovePointerSlot(slot);
                return false;
            }
            ps.down = true;
            if (pointer_count_ == 1) {
                // Keep the mouse bookkeeping consistent for a single finger.
                is_mouse_down_ = true;
                SetCapture(hwnd);
            }
            if (auto status = app_api_->ForwardTouchEvent(Core::Input::TouchAction::Down, tp);
                status.IsError()) {
                CPS_LOG_WARN("InputRouter", "Failed to forward touch down: ", status.Message());
            }
            return true;

        case WM_POINTERUPDATE:
            if (!ps.down) return false;
            (void)app_api_->ForwardTouchEvent(Core::Input::TouchAction::Move, tp);
            return true;

        case WM_POINTERUP:
        case WM_POINTERLEAVE: {
            const bool last = pointer_count_ == 1;
            (void)app_api_->ForwardTouchEvent(
                msg == WM_POINTERLEAVE ? Core::Input::TouchAction::Cancel
                                       : Core::Input::TouchAction::Up,
                tp);
            RemovePointerSlot(slot);
            if (last) {
                is_mouse_down_ = false;
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
            }
            return true;
        }

        default:
            return false;
    }
}

bool Win32InputRouter::HandleMouse(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const float clientX = static_cast<float>(GET_X_LPARAM(lParam));
    const float clientY = static_cast<float>(GET_Y_LPARAM(lParam));

    // 1. Give Overlay HUD first chance to consume mouse events
    if (overlay_) {
        if (msg == WM_LBUTTONDOWN && overlay_->OnMouseDown(clientX, clientY)) {
            is_overlay_capturing_ = true;
            SetCapture(hwnd);
            return true;
        }
        if (msg == WM_MOUSEMOVE) {
            if (is_overlay_capturing_) {
                (void)overlay_->OnMouseMove(clientX, clientY);
                return true;
            }
            if (overlay_->OnMouseMove(clientX, clientY)) {
                return true;
            }
        }
        if (msg == WM_LBUTTONUP) {
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            if (is_overlay_capturing_) {
                is_overlay_capturing_ = false;
                (void)overlay_->OnMouseUp(clientX, clientY);
                return true;
            }
            if (overlay_->OnMouseUp(clientX, clientY)) {
                return true;
            }
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
                auto status = app_api_->ForwardTouchEvent(Core::Input::TouchAction::Down, tp);
                if (status.IsError()) {
                    CPS_LOG_WARN("InputRouter", "Failed to forward touch down: ", status.Message());
                }
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
                auto status = app_api_->ForwardTouchEvent(Core::Input::TouchAction::Up, tp);
                if (status.IsError()) {
                    CPS_LOG_WARN("InputRouter", "Failed to forward touch up: ", status.Message());
                }
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

    // Ctrl + F4 toggles the standalone log panel (to prevent accidental trigger)
    if (wParam == VK_F4 && (GetKeyState(VK_CONTROL) & 0x8000) && isPressed && overlay_) {
        overlay_->ToggleLogPanel();
        return true;
    }

    // F2 toggles overlay settings drawer
    if (wParam == VK_F2 && isPressed && overlay_) {
        overlay_->ToggleSettingsDrawer();
        return true;
    }

    // ESC closes settings drawer or welcome guide if visible
    if (wParam == VK_ESCAPE && isPressed) {
        if (overlay_ && overlay_->IsSettingsDrawerOpen()) {
            overlay_->ToggleSettingsDrawer();
            return true;
        }
        if (overlay_ && overlay_->IsWelcomeGuideVisible()) {
            overlay_->DismissWelcomeGuide();
            return true;
        }
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
