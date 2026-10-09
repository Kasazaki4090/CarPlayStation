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
| `SetupWizard` | First-run flow and persistence. Safety disclaimer modal, MFi backend wizard (local file / USB-to-I2C hardware / Remote MFi HTTP), and the INI read/write for all of it. Both modals are laid out in 96-DPI design units and scaled to the parent monitor's DPI (controls, system UI font and window size), because the process is Per-Monitor V2 aware and Windows does not stretch them. |
| `Win32InputRouter` | Translates `WM_POINTER*`, mouse, wheel and keyboard messages into the pure data structs consumed by `Core/Input/IInputEventSink`. Multi-contact pointers are read from pointer messages, not from synthesized mouse messages. |
| `SystemPermissionHelper` | Reads the Windows Microphone and Location capability consent from the registry and can open the matching Settings page. |
| `ThemeHelper` | Reads the Windows app light/dark setting and builds the colour palette used by the HUD, the letterbox and the window frame. |

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

### `[Video]` / `[Audio]`

The settings drawer (`F2`) writes these on change (debounced, and flushed on
exit), and they are read back **before the window is created**, so the window
opens at the saved size. Changing them takes effect on the next start.

| Key | Meaning |
| :--- | :--- |
| `ResolutionPreset` | `0`=720p `1`=1080p `2`=1440p `3`=2160p `4`=Auto (desktop) `5`=Custom |
| `CustomWidth` / `CustomHeight` | Frame size used with `ResolutionPreset=5` |
| `Fps` | `30` or `60` |
| `PreferredCodec` | `0`=H.264 `1`=HEVC (H.265) `2`=Auto |
| `DisplayMode` | `0`=Windowed `1`=Borderless fullscreen |
| `MediaVolume` / `NavigationVolume` / `TelephonyVolume` | Per-lane volume, whole percent `0`–`100` |

Audio has no other keys. The output sample rate always follows the Windows
default render endpoint (shared mode / mix format), the encode bitrate is fixed
at 320 kbps, and ducking is driven by the sender — so a `SampleRate`,
`BitrateKbps` or `DuckingRatio` left over from an older version is ignored.

Hand-editing the file is supported, which is how you try a resolution the drawer
does not offer — write `ResolutionPreset=5` plus the size you want:

```ini
[Video]
ResolutionPreset=5
CustomWidth=1920
CustomHeight=720
PreferredCodec=1
```

Odd sizes are accepted and rounded up to whole even pixels (`1921x721` becomes
`1922x722`); anything outside 320–4096 on either axis is treated as a typo,
logged as `Ignoring out-of-range custom resolution`, and falls back to 720p. The
renderer aspect-fits the stream, so non-standard aspect ratios letterbox rather
than distort.

`PreferredCodec` is a request, not a decision. Choosing HEVC makes the receiver
announce it during AirPlay discovery (`hevcInfo = {}` in the `/info` reply plus
`hevc` in the phase‑1 `enabledFeatures` list) — the phone only sends H.265 when
both halves are present. The phone then picks the codec, and the first
`VideoConfig` packet tells us which one it chose; if that differs from what we
asked for, the receiver logs `Sender codec differs from the configured spec` and
reconfigures the decoder to match the actual bitstream, so a mismatch degrades
gracefully instead of producing a black window. Watch `SETUP Phase 1 response
body keys=…, enabledFeatures=on, hevc=on` and the `MpvVideoPlayer DIAG` line's
`video-codec=` / `decodedSize=` fields to confirm which codec is really flowing.

## Appearance (Light / Dark)

`ThemeHelper` reads `AppsUseLightTheme` under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` (falling back to
`HKLM`, then to dark) and turns it into a palette of named roles — text, panel,
accent, status colours, slider colours and the letterbox colour. There is no in-app
override and nothing is written to the INI: the UI follows whatever Windows is set
to, which is also what the caption does.

* `OverlayRenderer` creates its Direct2D brushes from the palette and repaints them
  in place (`ID2D1SolidColorBrush::SetColor`) when the theme changes, so no render
  target has to be rebuilt.
* `D3D11Renderer::SetLetterboxColor()` takes the letterbox colour from the palette;
  the swapchain is cleared with it instead of hardcoded black.
* `MainWindow::ApplyTheme()` asks DWM for a light or dark caption
  (`DWMWA_USE_IMMERSIVE_DARK_MODE`) and paints `WM_ERASEBKGND` with the matching
  background brush.
* A change is picked up from `WM_SETTINGCHANGE` / `WM_DWMCOLORIZATIONCOLORCHANGED`,
  and `AppController::ApplyCurrentTheme()` re-reads the registry once a second as a
  fallback, because the broadcast is not always delivered.

The dark palette is exactly the set of colours the HUD used before, so a system left
on dark mode looks unchanged.

### Telling the phone

The same `AppController::ApplyCurrentTheme()` call also pushes the appearance to the
iPhone: it calls `IApplicationApi::SetHostNightMode(night)`, which passes through
`ICarPlayCoreApi::SetHostNightMode()` into `AirPlayRtspServer::SetNightMode()`. The
RTSP server remembers the value and writes an AirPlay event-channel command

```
POST /command  ->  { "type": "setNightMode", "params": { "nightMode": true|false } }
```

to every session that has an event channel. A session that is not connected yet, or
whose event channel has not been accepted yet, receives the stored value as soon as
the channel appears, so the phone is never left with a stale appearance.

The window is built before the protocol stack starts, so this call usually arrives
before `AirPlayRtspServer` exists. `CarPlayCoreService` therefore keeps the requested
value itself and replays it into the RTSP server right after creating it
(`Replayed remembered host night mode …` in the log). Without that second store the
startup push would be dropped and the phone would keep its own appearance.

Each theme transition logs two lines: `UI theme switched to …` followed by
`CarPlay night mode requested=…, commandWritten=…`. `commandWritten=1` only proves
the frame was queued on the event channel — whether the phone actually re-colours its
CarPlay UI depends on the iOS version and cannot be verified from the host. Nothing
here changes the video pixels we decode: the stream is whatever the phone decided to
send. See `DEC-013`.

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
and the HID descriptor that was chosen, mDNS join/announce activity, the
WASAPI pipeline format, and every `UI theme switched to light|dark` transition —
which is the first place to look when a phone does not connect or the HUD looks
washed out.

## AI Partition & Mocking Guide

* `AppController::Step()` runs one message-processing and frame iteration, so
  the whole UI can be driven headlessly from a test.
* `headless = true` suppresses interactive popups; the disclaimer and the MFi
  wizard are accepted programmatically.
* Rendering is verified by asserting on the presentation rectangles and overlay
  hit-test rects, not by pixel comparison.
* `ThemeHelper` is pure Win32 registry access plus plain structs, so it can be
  asserted on directly (`Tests/UiTests.cpp` checks that the dark palette reproduces
  the historical colours and that both palettes stay translucent and in range).

