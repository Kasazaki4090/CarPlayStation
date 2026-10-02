#include "Core/Input/MouseTouchTranslator.h"

#include <algorithm>
#include <cstdlib>

namespace CarPlayStation::Core::Input {

namespace {

float Clamp01(float v) noexcept {
    return std::clamp(v, 0.0f, 1.0f);
}

} // namespace

MouseTouchTranslator::MouseTouchTranslator(IInputEventSink& sink, Config cfg)
    : sink_(sink), cfg_(cfg) {}

float MouseTouchTranslator::NormalizeX(float clientX) const noexcept {
    const float w = static_cast<float>(cfg_.windowWidth > 0 ? cfg_.windowWidth : 1u);
    return Clamp01(clientX / w);
}

float MouseTouchTranslator::NormalizeY(float clientY) const noexcept {
    const float h = static_cast<float>(cfg_.windowHeight > 0 ? cfg_.windowHeight : 1u);
    return Clamp01(clientY / h);
}

TouchPoint MouseTouchTranslator::LastPoint() const noexcept {
    TouchPoint p;
    p.id = activePointerId_;
    p.x_normalized = lastNx_;
    p.y_normalized = lastNy_;
    return p;
}

void MouseTouchTranslator::EmitTouch(TouchAction action, float nx, float ny, uint32_t pointerId) {
    TouchPoint p;
    p.id = pointerId;
    p.x_normalized = nx;
    p.y_normalized = ny;
    lastStatus_ = sink_.SendTouchEvent(action, p);
}

void MouseTouchTranslator::OnPointerEvent(const PointerEvent& ev) {
    const float nx = NormalizeX(ev.x);
    const float ny = NormalizeY(ev.y);

    // Right button maps to an automotive button (Back / Home), not a touch.
    if (ev.button == PointerButton::Right) {
        lastStatus_ = sink_.SendButtonEvent(cfg_.rightClickButton, ev.isDown);
        return;
    }

    // Only the left button drives touch.
    if (ev.button != PointerButton::Left) {
        return;
    }

    if (ev.isDown) {
        if (!touchActive_) {
            touchActive_ = true;
            activePointerId_ = ev.pointerId;
            lastNx_ = nx;
            lastNy_ = ny;
            EmitTouch(TouchAction::Down, nx, ny, ev.pointerId);
        } else {
            // Already down: this is a move.
            lastNx_ = nx;
            lastNy_ = ny;
            EmitTouch(TouchAction::Move, nx, ny, ev.pointerId);
        }
    } else {
        if (touchActive_) {
            touchActive_ = false;
            lastNx_ = nx;
            lastNy_ = ny;
            EmitTouch(TouchAction::Up, nx, ny, ev.pointerId);
        }
    }
}

void MouseTouchTranslator::OnWheelEvent(const WheelEvent& ev) {
    if (ev.IsEmpty()) {
        return;
    }

    // The wheel behaves like the vehicle's rotary scroll wheel (directional
    // navigation), NOT a touch swipe. Each notch rotates the knob by
    // `knobStepsPerNotch` steps.
    //
    // Sign convention (see InputEvents.h):
    //   * deltaY > 0 (scroll up)    -> counter-clockwise
    //   * deltaY < 0 (scroll down)  -> clockwise
    //   * deltaX > 0 (scroll right) -> clockwise
    //   * deltaX < 0 (scroll left)  -> counter-clockwise
    //
    // A 4-axis mouse may report both axes in one event; each is emitted as its
    // own knob rotation.
    if (ev.deltaY != 0) {
        const KnobAction action = (ev.deltaY > 0) ? KnobAction::RotateCounterClockwise
                                                  : KnobAction::RotateClockwise;
        EmitKnob(action, static_cast<uint32_t>(std::abs(ev.deltaY)) * cfg_.knobStepsPerNotch);
    }
    if (ev.deltaX != 0) {
        const KnobAction action = (ev.deltaX > 0) ? KnobAction::RotateClockwise
                                                  : KnobAction::RotateCounterClockwise;
        EmitKnob(action, static_cast<uint32_t>(std::abs(ev.deltaX)) * cfg_.knobStepsPerNotch);
    }
}

void MouseTouchTranslator::EmitKnob(KnobAction action, uint32_t steps) {
    if (steps == 0) {
        return;
    }
    lastStatus_ = sink_.SendKnobEvent(action, steps);
}

void MouseTouchTranslator::Reset() {
    touchActive_ = false;
    lastNx_ = 0.5f;
    lastNy_ = 0.5f;
    activePointerId_ = 0;
}

} // namespace CarPlayStation::Core::Input
