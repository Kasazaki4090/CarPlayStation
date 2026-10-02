#pragma once

#include "Core/Input/IInputEventSink.h"
#include "Core/Input/InputEvents.h"
#include <cstdint>

namespace CarPlayStation::Core::Input {

// ============================================================================
// KeyboardKnobTranslator (Partition 6, Step 3)
//
// Translates platform-neutral keyboard events (InputEvents.h :: Key) into
// CarPlay rotary-knob and automotive button events emitted through
// IInputEventSink. Pure logic — no Win32 coupling.
//
// Mapping (per DEC-003 input strategy):
//   * Up / Down / Left / Right  -> directional knob press (4-way d-pad)
//   * W / A / S / D             -> same directional knob press (alias)
//   * Enter                     -> knob center push / select confirm
//   * Backspace / Escape        -> automotive Back button
//   * PlayPause / Next / Prev   -> media transport buttons
//   * Siri                      -> Siri button
//   * CallAccept / CallReject   -> call control buttons
//   * VolumeUp / VolumeDown     -> volume buttons
//
// The vehicle knob is a 4-way pressable d-pad (not a pure 1-DOF rotary), so
// the directional keys press the knob toward each edge. Rotation is driven by
// the mouse wheel (see MouseTouchTranslator).
//
// NOTE: CarPlay has NO alphanumeric text-input channel (its input model is
// touch + knob + touchpad + buttons only), so letter/number keys are not
// mapped. See TODO_PARTITION_6.md.
// ============================================================================
class KeyboardKnobTranslator {
public:
    struct Config {
        // Number of knob steps a single directional key press produces.
        uint32_t knobStepsPerPress{1};

        // Which automotive button Backspace / Escape maps to.
        ButtonType backButton{ButtonType::Back};
    };

    KeyboardKnobTranslator(IInputEventSink& sink, Config cfg = {});

    void SetConfig(const Config& cfg) { cfg_ = cfg; }
    [[nodiscard]] const Config& GetConfig() const noexcept { return cfg_; }

    // Feed a key event. Only key-downs produce actions (key-ups are ignored),
    // matching how automotive knob/button controls behave.
    void OnKeyEvent(const KeyEvent& ev);

    // Status of the most recent sink emission (surfaced, not swallowed).
    [[nodiscard]] Common::Status LastStatus() const noexcept { return lastStatus_; }

private:
    IInputEventSink& sink_;
    Config cfg_{};
    Common::Status lastStatus_{Common::Status::Ok()};
};

} // namespace CarPlayStation::Core::Input
