#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <cstdint>

namespace CarPlayStation::Core::Input {

enum class TouchAction {
    Down,
    Move,
    Up,
    Cancel
};

struct TouchPoint {
    uint32_t id{0};
    float x_normalized{0.0f}; // 0.0 to 1.0 (relative to display width)
    float y_normalized{0.0f}; // 0.0 to 1.0 (relative to display height)
};

enum class KnobAction {
    RotateClockwise,
    RotateCounterClockwise,
    PushDown,
    PushUp
};

enum class ButtonType {
    Home,
    Back,
    PlayPause,
    NextTrack,
    PrevTrack,
    Siri,
    CallAccept,
    CallReject,
    VolumeUp,
    VolumeDown
};

class IInputEventSink {
public:
    virtual ~IInputEventSink() = default;

    // Send touch point event
    [[nodiscard]] virtual Common::Status SendTouchEvent(TouchAction action, const TouchPoint& point) = 0;

    // Send rotary knob event
    [[nodiscard]] virtual Common::Status SendKnobEvent(KnobAction action, uint32_t stepCount = 1) = 0;

    // Send hardware or simulated steering/dashboard button event
    [[nodiscard]] virtual Common::Status SendButtonEvent(ButtonType button, bool isPressed) = 0;
};

} // namespace CarPlayStation::Core::Input

