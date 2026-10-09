# UI Layer (QML / Qt Quick)

This directory contains the modernized QML / Qt Quick user interface architecture for CarPlayStation. It is designed to provide a fluid, hardware-accelerated, touch-friendly UI layer that will eventually succeed the native Direct2D/Direct3D 11 Win32 UI (`UI/Win32/`).

Like `UI/Win32/`, this layer is strictly decoupled from the internal Core protocol engine. It interfaces with CarPlayStation exclusively through the Application facade (`Application::IApplicationApi`).

---

## Directory Structure

```text
UI/Qml/
├── Main.qml                        # Root ApplicationWindow and HUD overlay container
├── README.md                       # Architecture, component inventory, and integration guide
├── theme/
│   └── Theme.qml                   # Color palette, metrics, fonts, dark/light theme sync
└── components/
    ├── CarPlayVideoSurface.qml     # Video viewport item with aspect-ratio letterboxing
    ├── StatusPill.qml              # Header status pill showing link state and Now Playing song
    ├── CallBanner.qml              # Incoming & active telephony banner with accept/reject buttons
    ├── SettingsDrawer.qml          # Slide-out drawer for volume mixer, video resolution & codec
    ├── WelcomeCard.qml             # Startup pairing guidance card with dismiss hotzone
    ├── LogPanel.qml                # Collapsible in-app diagnostic console
    ├── IconButton.qml              # Reusable responsive icon action button
    └── VolumeSlider.qml            # Custom styled volume slider with lane labels
```

---

## Architecture & Decoupling Model

The QML UI layer mirrors the architectural principles defined in [`DEC-002`](../../DECISIONS.md):

1. **No Core Direct Dependencies**: QML components and their C++ QObject backend must never include headers from `Core/`. All data flows through `Application::IApplicationApi` and `Application::AppSettings`.
2. **C++ Bridge (`QmlAppController` / `QmlCarPlayBridge`)**:
   - Exposes reactive `Q_PROPERTY` attributes for UI bindings:
     - `isStreaming`, `statusText`, `songTitle`, `artistName`
     - `callState`, `callerName`, `callerNumber`
     - `isNightMode`, `mediaVolume`, `navigationVolume`, `telephonyVolume`
   - Maps QML signals to API commands:
     - `triggerSiri()`, `acceptCall()`, `endCall()`
     - `applySettings()`, `setNightMode()`
   - Forwards input events from QML pointer handlers (`PointerHandler`, `MultiPointTouchArea`, `Keys`) to `Core::Input::IInputEventSink`.

---

## Component Inventory

| Component | Responsibility |
| :--- | :--- |
| `Main.qml` | Top-level window hosting keyboard shortcuts (`F1` Siri, `F2` Settings, `F3` Answer, `F4` Hangup, `Ctrl+F4` Log, `F11` Fullscreen, `Esc` Dismiss), video viewport, and overlay hierarchy. |
| `theme/Theme.qml` | Centralized design tokens (colors, radii, spacing, typography). Follows system dark/light mode dynamically. |
| `CarPlayVideoSurface.qml` | Manages letterbox / pillarbox scaling to maintain pixel aspect ratios across diverse display geometries. Serves as the host for the hardware video texture. |
| `StatusPill.qml` | Pill indicator displaying connection health, status pulses, and real-time Now Playing metadata. |
| `CallBanner.qml` | Floating call banner for incoming and active calls (`0x4155 CallStateUpdate`) with one-touch answer/reject buttons. |
| `SettingsDrawer.qml` | Interactive settings panel with sliders for the 3-Lane WASAPI audio mixer (`MainMedia`, `AltNavigation`, `SiriTelephony`), and selectors for video resolution (720p / 1080p / 1440p), frame rate (30 / 60 FPS), and codec (H.264 / HEVC). |
| `WelcomeCard.qml` | Pairing guide presented before streaming begins, showing accessory identity and wireless setup instructions. |
| `LogPanel.qml` | Real-time rolling diagnostic console for runtime protocol and pipeline inspection. |

---

## Video Rendering Pipeline in QML

To integrate CarPlay's hardware-decoded video into Qt Quick without CPU copy overhead:

1. **Direct3D 11 / DXGI Shared Texture (`QSGSimpleTextureNode`)**:
   - `ApplicationService` supplies a D3D11 shared texture handle from `MpvVideoPlayer`.
   - When Qt Quick runs with the Direct3D 11 rendering backend (standard on Windows), `QQuickWindow::createTextureFromNativeObject` binds the D3D11 texture directly into a scene graph node (`QQuickItem::updatePaintNode`), achieving zero-copy GPU presentation.
2. **libmpv Render Context (`mpv_render_context`)**:
   - Alternatively, use `libmpv`'s native OpenGL or D3D11 render context integration with `QQuickFramebufferObject`.

---

## Implementation Roadmap

- [x] Initial scaffold: folder structure, theme tokens, and foundation QML components created.
- [ ] Implement C++ QObject bridge (`QmlAppController` / `QmlBridge`) binding `Application::IApplicationApi` to Qt Quick properties and signals.
- [ ] Implement zero-copy hardware texture renderer (`CarPlayVideoNode` / `QQuickItem`).
- [ ] Add touch coordinate normalization and multi-touch forwarding to `IInputEventSink`.
- [ ] Integrate CMake build target (`CarPlayStation_Qml`) with optional `ENABLE_QML` switch.

