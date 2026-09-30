# TODO

This file tracks planned work for CarPlayStation.

TODO items should be derived from:

1. `PROJECT.md` — project goals and scope
2. `ARCHITECTURE.md` — current architecture
3. `DECISIONS.md` — accepted design decisions
4. `BUGS.md` — known problems and regressions
5. `Reference/` — protocol and implementation research
6. Current source code — implemented and missing functionality

Do not blindly add tasks based on assumptions.

## Rules

### 1. Keep tasks actionable

Prefer:

```text
- [ ] Implement USB transport initialization
```

over:

```text
- [ ] USB
```

### 2. Do not duplicate BUGS.md

Known bugs belong in `BUGS.md`.

A TODO may reference a bug when fixing it requires planned development work.

### 3. Do not treat research assumptions as implementation requirements

If something is not yet understood, record it as a research task first:

```text
- [ ] Investigate wireless CarPlay discovery flow
```

rather than prematurely defining an implementation.

### 4. Keep priorities visible

Use the following priority levels:

* `P0` — Required for the basic receiver to work
* `P1` — Required for first usable release
* `P2` — Important improvement
* `P3` — Optional / future work

Example:

```text
- [ ] P0 — Implement USB transport
- [ ] P0 — Implement iAP2 session
- [ ] P1 — Implement touch input
```

### 5. Remove completed work

Completed implementation should normally disappear from TODO rather than becoming a historical checklist.

Historical information belongs in:

* Git history
* `DECISIONS.md`
* `BUGS.md`

---

## Current TODO

Project-wide review completed on 2026-09-28 per `DEC-001`, updated per `DEC-003`.

Current repository state at the time of writing:

- No first-party source code exists. `Core/`, `UI/Win32/` and `Lib/` contain only README files.
- `Reference/carplayd` (Rust, Raspberry Pi wired head unit) and `Reference/xcertplay` (Kotlin/Java,
  Android head unit) are git submodules and are the only protocol material.
- `BUGS.md` contains only the `BUG-000` template example; no real defects are recorded yet.
- Platform & Transport Strategy (`DEC-003`): Native PC wired USB device mode is constrained by Intel xDCI
  hardware and BIOS/EC implementations on Windows PCs. Therefore, **Wireless CarPlay is the primary and mandatory
  P0 project focus**. Wired CarPlay is deprioritized and deferred to external hardware bridge solutions.
- MFi Authentication Strategy (`DEC-003`): Pluggable signer interfaces supporting USB-to-I2C chips (CH341, etc.)
  for local physical chips, and configurable network proxy / daemon socket endpoint (defaulting to `127.0.0.1:9000`)
  for remote/emulated signing.
- Input Strategy (`DEC-003`): Mouse is mapped directly to touch events (left-click tap/drag, right-click back);
  keyboard is mapped to vehicle rotary knob / directional controls (arrow keys, Enter, Backspace/Esc, hotkeys).

Tasks below are ordered so research precedes implementation. `[Research]` tasks must be closed with a
written conclusion in `ARCHITECTURE.md` or a new `DECISIONS.md` entry before the dependent
implementation tasks are started.

### 1. Architectural and strategic decisions

- [x] P0 [Concluded: DEC-003] **Wired xDCI vs. Wireless Priority**: Native PC USB device mode is unviable due to Intel xDCI BIOS/EC constraints. **Wireless CarPlay is the primary, mandatory milestone (P0)**.
- [x] P0 [Concluded: DEC-003] **MFi Authentication Interface**: Adopt pluggable signer backends supporting both local USB-to-I2C bridge chips (CH341 / CP2112 / FT232H) and configurable network daemon / proxy endpoint (default `127.0.0.1:9000`).
- [x] P0 [Concluded: DEC-003] **Input Modality Translation**: Mouse maps to Touch (tap, drag, right-click back); Keyboard maps to vehicle rotary/directional controls (arrow keys, Enter, Backspace/Esc, media/Siri hotkeys).
- [ ] P2 [Research] If wired CarPlay support is required later, evaluate external bridge MCU/SBC hardware (Pi Zero, RP2350, ESP32-S3 OTG, Carlinkit-style dongle) attached over USB/serial/Ethernet.

### 2. Architecture and project foundation

- [ ] P0 Define the application layer required by `DEC-002`: its directory, its ownership of state/commands/events, and the interface contract between Win32 UI and Core.
- [ ] P0 Fix the Core layer structure and directory layout, based on the reference component split: transport, iAP2 link/session, CarPlay session, media, input, pairing store.
- [ ] P0 Choose and document the C++ baseline: language standard, compiler/SDK, build system (CMake or MSVC solution), static analysis, unit-test framework, and the "no extra runtime" packaging rule from `PROJECT.md`.
- [ ] P1 Define the threading and data-flow model for the media path (capture/decode/render/audio) and the UI/Core hand-off, so decoder callbacks never enter UI code.
- [ ] P1 Define an error-handling and logging policy consistent with `CONTRIBUTING.md` rule 13 (no swallowed errors), including a session trace format usable for protocol debugging.
- [ ] P2 Update `README.md` so the feature checklist reflects real status instead of the placeholder "New Folder" state.

### 3. Protocol: iAP2

- [ ] P0 [Research] Produce a written iAP2 message model for CarPlay from `carplayd` (`crates/iap2-core`: link, message, session, state, spec, mfi) and `xcertplay` (`shared/.../iap2`), separating verified wire behaviour from reference-code assumptions.
- [x] P0 [Concluded: Partition 3] Implement iAP2 framing and serialization/deserialization for the control and data links, with round-trip tests built from recorded frames. (Iap2LinkEngine + TlvBuilder)
- [x] P0 [Concluded: Partition 3] Implement accessory identification and wireless session start/stop (handling Wi-Fi/BT declare flags). (BuildIdentifyInfo + Iap2SessionManager)
- [x] P0 Implement pluggable MFi authentication backend for USB-to-I2C bridge chips (CH341, CH347, FTDI, etc.) for local hardware MFi coprocessors (DEC-003, DEC-007).
- [x] P0 Implement pluggable MFi authentication backend over configurable network daemon / proxy endpoint (`127.0.0.1:9000` / TCP RPC) for remote signing / daemon services (DEC-003, DEC-005).
- [x] P1 [Concluded: Partition 3] Implement the `IdentificationRejected` retry behaviour (carplayd doc 23) once a reject can be reproduced. (First-reject → RetryIdentify, second-reject → Abort)
- [ ] P1 Establish a single source of truth for message ids, directions, parameters and enums, mirroring `crates/iap2-core/src/spec.rs`, and a generator or checked-in table derived from Apple's authoritative spec.
- [ ] P2 Implement notification/transcript handling for accessory information and vehicle identity data.

### 4. Transport: wireless CarPlay (Primary Target — P0)

- [x] P0 [Concluded: Partition 2] Document the full wireless discovery and connection sequence (Bluetooth discoverability and pairing, SDP, RFCOMM, iAP2 handshake, Wi-Fi handoff, mDNS, AirPlay session start) from carplayd docs 19/21/22 and xcertplay.
- [x] P0 [Concluded: Partition 2] Select the Windows Bluetooth stack API for pairing, SDP lookup and RFCOMM, and confirm it works without additional runtimes on Windows 10/11 (AF_BTH, BTHPROTO_RFCOMM).
- [x] P0 [Concluded: Partition 2] Select the Windows access-point mechanism for the Wi-Fi handoff (SoftAP/hotspot/native wifi) and verify 5 GHz operation and concurrent client connectivity on typical PC NICs.
- [x] P0 Implement Bluetooth pairing and SDP record discovery/registration for the iAP2 service (BtSdpHelper).
- [x] P0 Implement RFCOMM transport and the iAP2-over-RFCOMM handshake (BtRfcommTransport).
- [x] P0 Implement the Wi-Fi handoff, AP startup, mDNS responder (WifiSoftApManager, MdnsAdvertiser).
- [ ] P1 Implement a pairing and known-device store with reconnect for previously paired iPhones.
- [ ] P2 Implement dashboard / instrument-cluster template handling for wireless-capable vehicles.

### 5. Transport: wired CarPlay (Secondary / External Bridge — P2)

- [ ] P2 Evaluate external bridge MCU/SBC transport (Pi Zero, RP2350, ESP32-S3, USB dongle) for wired CarPlay if physical USB connection is desired.
- [ ] P2 Implement bridge communication protocol (USB serial/CDC-NCM/IP tunnel) between Windows host and external bridge hardware.

### 6. CarPlay session layer

- [x] P0 [Concluded: Partition 4] Implement the CarPlay session state machine: start, availability, active/paused, teardown, reconnect, and a single place where transport failures are converted into session events (`CarPlaySessionStateMachine`).
- [x] P0 [Concluded: Partition 4] Implement the CarPlay control plane required for a first working session (display, input and media negotiation via `AirPlayRtspCodec` and `AirPlayRtspServer`).
- [x] P0 [Concluded: Partition 4] Implement pairing store with atomic disk persistence and fast reconnect for previously paired iPhones (`PairingStore`).
- [ ] P1 Implement the remaining session planes already studied in carplayd doc 23 (session start/availability, vehicle data, location, destination) and mark which are unit-tested but not yet device-exercised.
- [ ] P2 Implement Modern Vehicle Data Config (VDC) over the AirPlay iAP channel (researched, unimplemented in carplayd).

### 7. Media: video

- [x] P0 [Concluded: DEC-008] Architecture decision concluded: forward-compatible codec discovery (H264/HEVC/AV1/VP9/VVC), Direct3D 11 shared texture handles (EVR/madVR rejected), and automated FFmpeg `d3d11va`/CPU fallback.
- [x] P0 [Concluded: Partition 5] Implement H.264 depacketization and Media Foundation decoding with Direct3D11 presentation. (`AirPlayVideoDemuxer` + `MfVideoDecoder`)
- [x] P1 [Concluded: Partition 5] Implement HEVC decoding and dynamic FFmpeg fallback with automated build sync. (`FFmpegVideoDecoder` + `FindOrDownloadFFmpeg.cmake`)
- [ ] P1 Implement codec capability negotiation and runtime selection between H.264 and HEVC.
- [ ] P1 Implement the display-mode selector described in `PROJECT.md`.
- [ ] P2 Measure decode and presentation latency and define the frame-drop policy under load.

### 8. Media: audio and microphone

- [x] P0 [Concluded: DEC-008] Architecture decision concluded: non-exclusive WASAPI Shared Mode (`AUDCLNT_SHAREMODE_SHARED`), internal 3-lane software mixer (`MainMedia`, `AltNavigation`, `SiriTelephony`), and `AudioDuckingManager`.
- [x] P0 [Concluded: Partition 5] Implement audio rendering over WASAPI (Shared Mode) with device selection, format negotiation and hot-plug recovery. (`WasapiAudioPipeline` 3-lane mixer + soft-limiter + multi-rate resampler)
- [x] P1 [Concluded: Partition 5] Implement ducking and lane swapping so navigation and calls behave correctly against media playback. (`AudioDuckingManager` attack/release gain curve)
- [ ] P1 Implement microphone capture, resampling and encoding for the uplink path (carplayd doc 16), including Siri and call activation. (`WasapiMicrophoneUplink` resample to 16/24kHz mono + packet dispatch; encoding/Siri/call activation still pending)
- [ ] P2 Implement multichannel audio and automatic volume control, listed as limited in Phase 1.

### 9. Input

- [ ] P0 Implement Win32 mouse-to-touch input translation:
  - Left click press / release → Touch Down / Touch Up (Tap)
  - Left click hold + motion → Touch Move (Pan / Swipe / Drag)
  - Right click → CarPlay Back / Home action
- [ ] P1 Implement Win32 keyboard-to-vehicle-navigation mapping:
  - Arrow keys (Up/Down/Left/Right) → Directional / rotary knob focus navigation
  - Enter → Select / Confirm (Knob push)
  - Backspace / Escape → Back / Cancel action
  - Media & Function keys → Play/Pause, Next/Prev Track, Siri activate, Call answer/hangup
- [ ] P1 Implement native multi-touch injection for touchscreens (pointer coordinate scaling and multi-finger touch messages).
- [ ] P1 Implement media control buttons (play/pause, next, previous) and verify against carplayd doc 18.
- [ ] P1 [Research] Determine the protocol behaviour for Siri, navigation-back and call-control buttons; carplayd marks these as still needing research.

### 10. Win32 UI and application interface

- [ ] P0 Define the application-level interface the UI consumes: connection state, session state, metadata, and commands such as connect, disconnect, mute, track control and display-mode change.
- [ ] P1 Implement the Win32 application window, lifecycle, DPI handling and the rendering surface, with no direct access to Core internals.
- [ ] P1 Implement status and pairing UI: device state, connection errors, pairing prompts and basic settings.
- [ ] P2 Implement Now Playing and navigation metadata display from the session layer (carplayd doc 17).
- [ ] P2 Implement system integration: autostart option, tray icon, single-instance behaviour and clean shutdown.

### 11. Verification and test infrastructure

- [ ] P0 Build a protocol test harness that replays recorded iAP2/CarPlay frames against Core without a device or UI.
- [ ] P1 Create a mock iPhone accessory simulator sufficient to drive identification, session start and media negotiation in CI.
- [ ] P1 Add a Windows build to CI that compiles Core and the Win32 UI and runs unit tests.
- [ ] P2 Add device-level regression scripts for the manual test matrix: wired (bridge), wireless, reconnect, sleep/resume, codec switch.

### 12. Documentation follow-ups from this review

- [ ] P0 Record the outcome of every section 1 research task as a `DECISIONS.md` entry before implementation begins.
- [ ] P1 Add the detailed protocol flows, session states, transport behaviour and media pipeline sections promised at the end of `ARCHITECTURE.md`.
- [ ] P2 Reconcile `PROJECT.md` Phase 1 goals with what the platform research proves is achievable on Windows, without silently narrowing scope.

---

## AI Project Review

When an AI agent starts work on the project, it should first review:

```text
PROJECT.md
ARCHITECTURE.md
DECISIONS.md
BUGS.md
TODO.md
Reference/
Core/
UI/
Lib/
```

After reviewing the project, the AI should:

1. Identify missing implementation areas.
2. Identify assumptions that require verification.
3. Identify missing research tasks.
4. Identify architectural inconsistencies.
5. Update `TODO.md` with concrete tasks.
6. Avoid modifying project goals without documenting the decision in `DECISIONS.md`.
