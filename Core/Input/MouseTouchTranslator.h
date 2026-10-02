#pragma once

#include "Core/Input/IInputEventSink.h"
#include "Core/Input/InputEvents.h"
#include <cstdint>

namespace CarPlayStation::Core::Input {

// ============================================================================
// MouseTouchTranslator (Partition 6, Step 2)
//
// Translates raw pointer / wheel events (InputEvents.h) into CarPlay touch
// events emitted through IInputEventSink. Pure logic — no Win32 coupling.
//
//   * Left button down            -> Touch Down
//   * Left button down + motion   -> Touch Move
//   * Left button up              -> Touch Up
//   * Right button down / up      -> automotive Back / Home button press / release
//   * Wheel (vertical or 4-axis)  -> rotary knob rotation, like the vehicle's
//                                     scroll wheel (directional navigation).
//                                     Vertical wheel -> up/down knob steps;
//                                     horizontal wheel -> left/right knob steps.
//
// Coordinates arrive in the UI window's client pixel space and are normalized
// to 0.0-1.0 against the window client size (which is expected to match the
// CarPlay stream aspect ratio).
// ============================================================================
class MouseTouchTranslator {
public:
    struct Config {
        // UI window client size in pixels (the space pointer x/y are in).
        uint32_t windowWidth{1920};
        uint32_t windowHeight{720};

        // Which automotive button a right-click maps to (Back or Home).
        ButtonType rightClickButton{ButtonType::Back};

        // Wheel -> rotary knob. The wheel behaves like the vehicle's scroll
        // wheel (directional navigation), NOT a touch swipe. Each wheel notch
        // rotates the knob by this many steps.
        uint32_t knobStepsPerNotch{1};
    };

    MouseTouchTranslator(IInputEventSink& sink, Config cfg = {});

    void SetConfig(const Config& cfg) { cfg_ = cfg; }
    [[nodiscard]] const Config& GetConfig() const noexcept { return cfg_; }

    // Feed a pointer sample. Drives touch down/move/up and right-click buttons.
    void OnPointerEvent(const PointerEvent& ev);

    // Feed a wheel event (vertical and/or horizontal). Synthesizes a swipe.
    void OnWheelEvent(const WheelEvent& ev);

    // Drop any in-flight touch state (e.g. on session teardown).
    void Reset();

    // Test / introspection accessors.
    [[nodiscard]] bool IsTouchActive() const noexcept { return touchActive_; }
    [[nodiscard]] TouchPoint LastPoint() const noexcept;

    // Status of the most recent sink emission (the sink is the Session-layer
    // consumer; the translator is a fire-and-forget producer, so it surfaces
    // the last result here rather than swallowing it).
    [[nodiscard]] Common::Status LastStatus() const noexcept { return lastStatus_; }

private:
    [[nodiscard]] float NormalizeX(float clientX) const noexcept;
    [[nodiscard]] float NormalizeY(float clientY) const noexcept;
    void EmitTouch(TouchAction action, float nx, float ny, uint32_t pointerId);
    void EmitKnob(KnobAction action, uint32_t steps);

    IInputEventSink& sink_;
    Config cfg_{};
    bool touchActive_{false};
    float lastNx_{0.5f};
    float lastNy_{0.5f};
    uint32_t activePointerId_{0};
    Common::Status lastStatus_{Common::Status::Ok()};
};

} // namespace CarPlayStation::Core::Input
