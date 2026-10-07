# UI Layer (Win32)

This directory contains the Windows user interface: the window, the video
presentation surface, the HUD overlay, the first-run setup flow, and the input
router. It is the only layer in the repository that is allowed to know about
Win32, Direct3D 11, or Direct2D.

The UI talks to the rest of the program exclusively through the Application
layer (`Application::IApplicationApi`). It never reaches into Core.

## Component Overview

| File | Responsibility |
| :--- | :--- |
| `main.cpp` | Entry point. Builds `ApplicationService`, then hands control to `AppController`. Supports a `--headless` mode for automated runs. |
| `AppController` | Owns and wires every UI component to `IApplicationApi`: window, renderer, overlay, wizard, input router. Exposes `Initialize()`, `Run()`, `Step()` (single frame, used by tests) and `Shutdown()`. |
| `MainWindow` | Window class, DPI handling, resize, swapchain host, message pump. |
| `D3D11Renderer` | Direct3D 11 device and DXGI swapchain. Presents the frame that libmpv rendered into a shared texture (`DEC-009`); no decoding happens here. |
| `VideoAdaptationHelper` | Maps the negotiated video stream onto the client area (letterbox / aspect handling) and keeps the presentation rect in sync with window resizes. |
| `OverlayRenderer` | Direct2D / DirectWrite HUD drawn on top of the video: status pill, watermark, settings drawer (`F2`), log panel (`F4`), and the welcome / pairing guide. Mouse input is consumed by the HUD first and only forwarded to CarPlay when it falls outside the overlay. |
| `SetupWizard` | First-run flow and persistence. Safety disclaimer modal, MFi backend wizard (local file / USB-to-I2C hardware / Remote MFi HTTP), and the INI read/write for all of it. |
| `Win32InputRouter` | Translates `WM_POINTER*`, mouse, wheel and keyboard messages into the pure data structs consumed by `Core/Input/IInputEventSink`. Multi-contact pointers are read from pointer messages, not from synthesized mouse messages. |
| `SystemPermissionHelper` | Reads the Windows Microphone and Location capability consent from the registry and can open the matching Settings page. |

## Settings Persistence

`SetupWizard` stores user preferences in `carplaystation_ui.ini`:

* Portable mode: next to the executable, when that directory is writable.
* Otherwise: `%APPDATA%\CarPlayStation\carplaystation_ui.ini`.

Sections cover the video / audio / device / MFi preferences from
`Application::AppSettings` (including `RemoteHost`, `RemotePort` and
`RemoteToken` for the Remote MFi backend) and a `[Device]` section holding the
last recorded iPhone `BluetoothMac` / `WifiMac` so a paired phone does not have
to be re-paired. Only keys that are actually present in the INI override the
compiled-in defaults, so a fresh install keeps the documented defaults. A legacy
`BackendType=1` (the removed TCP signing daemon) is migrated to `RemoteHttp` on
read.

## Boundaries & Isolation

The UI must not access directly:

* CarPlay protocol internals
* Transport implementations
* Media decoder implementations
* USB / I2C / MFi hardware
* Third-party libraries (`libmpv`, `fdk-aac`, vendor driver DLLs)

Everything crosses through `IApplicationApi` and the DTOs in `Application/`.
The UI is replaceable: another toolkit could be dropped in without touching Core.

## Diagnostics

The app writes a rolling log next to the executable (`carplaystation.log`). It
records which MFi backend was resolved, the touchscreen capability probe result
and the HID descriptor that was chosen, mDNS join/announce activity, and the
WASAPI pipeline format — which is the first place to look when a phone does not
connect.

## AI Partition & Mocking Guide

* `AppController::Step()` runs one message-processing and frame iteration, so
  the whole UI can be driven headlessly from a test.
* `headless = true` suppresses interactive popups; the disclaimer and the MFi
  wizard are accepted programmatically.
* Rendering is verified by asserting on the presentation rectangles and overlay
  hit-test rects, not by pixel comparison.

