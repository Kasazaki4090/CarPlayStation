# Layer 6: Input & Navigation Layer

## 1. Responsibilities
This module is located at Layer 6 of Core. It translates PC desktop input events (mouse, keyboard, touchscreen) into Apple CarPlay touch gestures and automotive rotary knob / button packets.

### Core Subcomponents
- **Mouse-to-Touch Translator (`MouseTouchTranslator`)**:
  - Left-Click Press / Release $\rightarrow$ Touch Down / Touch Up (single-point tap).
  - Left-Click Drag $\rightarrow$ Touch Move (pan, swipe, scroll).
  - Right-Click $\rightarrow$ Vehicle Back or Home button action.
  - Coordinate Normalization: Scales and maps Win32 window client coordinates to CarPlay stream resolution.
- **Keyboard-to-Vehicle-Control Translator (`KeyboardKnobTranslator`)**:
  - Arrow Keys (Up/Down/Left/Right) $\rightarrow$ Rotary knob step rotation and directional focus movement.
  - Enter $\rightarrow$ Rotary knob push / select confirm.
  - Backspace / Escape $\rightarrow$ Back / Return action.
  - Media & Function Hotkeys $\rightarrow$ Play/Pause, Next/Prev Track, Siri activation, Call answer/hangup.
- **Multi-Touch Injector (`MultiTouchInjector`)**:
  - Passes native Windows pointer / multi-touch points through when running on touchscreen hardware.
  - Two contact slots maximum: the touchscreen HID descriptor advertised in `/info` declares two
    contacts only when `Core/Hardware/TouchscreenCapabilities` reports a display-attached
    digitizer with two or more simultaneous contacts. On mouse / touchpad / pen-only hosts the
    single-contact descriptor is used, so the report format never lies about what the host can
    produce.
  - Contacts are read from `WM_POINTER*` messages, never from the synthesized mouse messages a
    touchpad produces, which is why a touchpad stays single-point.

---

## 2. Boundaries & Isolation
- **Downward Dependencies**: Input transmission channels provided by `Layer 4 (Session)`.
- **Public Exported Interfaces**:
  - `IInputEventSink`:
    - `OnPointerEvent(type, x, y, pointerId)`
    - `OnKeyEvent(virtualKey, isDown)`
    - `OnWheelEvent(delta)`
- **No Win32 Message Pump Coupling**: The Application / UI layer parses OS messages into pure data structs before feeding them to `IInputEventSink`.

---

## 3. AI Partition & Mocking Guide
- **Gesture Unit Tests**: Feed simulated mouse coordinate paths (e.g. (100,100) -> (100,500)) and verify generated CarPlay HID/Touch packets and normalized floating-point coordinates.
- **Key Mapping Tests**: Verify all registered keyboard hotkeys map to the correct automotive HID usages.
