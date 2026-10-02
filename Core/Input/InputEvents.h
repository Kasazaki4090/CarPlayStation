#pragma once

#include "Core/Common/Types.h"
#include <cstdint>

namespace CarPlayStation::Core::Input {

// ============================================================================
// Raw input event structs (Partition 6, Step 1)
//
// These are the *input* side of the input pipeline: pure data structs produced
// by the UI layer (Win32 message pump) and fed into the input translators
// (MouseTouchTranslator / KeyboardKnobTranslator / MultiTouchInjector).
//
// They carry NO Win32 types (no HWND, no WPARAM/LPARAM, no VK_*). The UI layer
// is responsible for translating OS messages into these platform-neutral
// structs before crossing into Core (DEC-002 / zero-collision rule 4).
//
// The *output* side — what the translators emit toward the Session layer — is
// defined in IInputEventSink.h (TouchAction/TouchPoint, KnobAction, ButtonType).
// ============================================================================

// The physical pointer source that produced an event.
enum class PointerType {
    Mouse,
    Touch,
    Pen
};

// Which pointer button is involved in a pointer event.
enum class PointerButton {
    None,
    Left,
    Right,
    Middle
};

// A single pointer (mouse / touch / pen) sample.
//
// x/y are in the UI window's client pixel space. The translator is responsible
// for normalizing them to 0.0-1.0 against the CarPlay stream resolution.
struct PointerEvent {
    PointerType type{PointerType::Mouse};
    PointerButton button{PointerButton::None};
    bool isDown{false};
    float x{0.0f};
    float y{0.0f};
    uint32_t pointerId{0}; // 0 = primary / single contact
};

// Platform-neutral virtual key codes. The UI layer maps OS VK_* / scancodes to
// this enum; the translator maps this enum to CarPlay knob / button actions.
// Kept deliberately small and extensible.
enum class Key {
    None = 0,

    // Directional / rotary navigation.
    Up,
    Down,
    Left,
    Right,

    // WASD (alternative directional keys, mapped like the arrow keys).
    KeyW,
    KeyA,
    KeyS,
    KeyD,

    // Confirm / cancel.
    Enter,
    Backspace,
    Escape,

    // Media transport.
    PlayPause,
    NextTrack,
    PrevTrack,

    // Assistant / telephony.
    Siri,
    CallAccept,
    CallReject,

    // Volume.
    VolumeUp,
    VolumeDown,

    // Misc.
    Space
};

// A keyboard event. virtualKey is a platform-neutral Key code (see above).
struct KeyEvent {
    Key virtualKey{Key::None};
    bool isDown{true};
};

// A mouse wheel / scroll event.
//
// Supports 4-axis mice (e.g. Logitech Master 3S, G502) that report both a
// vertical and a horizontal wheel. Either axis may be zero for a given event.
//
// Sign convention (UI layer normalizes OS deltas to these):
//   * deltaY > 0 = scroll up,    deltaY < 0 = scroll down
//   * deltaX > 0 = scroll right, deltaX < 0 = scroll left
//
// The UI layer (Partition 8) maps WM_MOUSEWHEEL (vertical, low word) and
// WM_MOUSEHWHEEL (horizontal) into these fields.
struct WheelEvent {
    int32_t deltaY{0};
    int32_t deltaX{0};

    [[nodiscard]] bool IsEmpty() const noexcept { return deltaY == 0 && deltaX == 0; }
};

} // namespace CarPlayStation::Core::Input
