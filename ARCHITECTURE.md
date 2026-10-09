# Architecture

CarPlayStation is divided into five major parts:

```text
┌───────────────────────────────────────────────┐
│                    UI                         │
│        Win32 / Display / Input / Control      │
└───────────────────────┬───────────────────────┘
                        │
┌───────────────────────▼───────────────────────┐
│                 Application                   │
│                                               │
│  Application API / State / Commands / Events  │
└───────────────────────┬───────────────────────┘
                        │
┌───────────────────────▼───────────────────────┐
│                    Core                       │
│                                               │
│  CarPlay Session                              │
│  Pairing / Authentication                     │
│  iAP2                                         │
│  Transport                                    │
│  Audio / Video / Input                        │
└───────────────┬───────────────────┬───────────┘
                │                   │
        ┌───────▼──────┐    ┌──────▼────────┐
        │    Wired     │    │   Wireless    │
        │   USB/NCM    │    │   Wi-Fi/IP    │
        └──────────────┘    └───────────────┘

┌───────────────────────────────────────────────┐
│                    Lib                        │
│                                               │
│  System APIs / WASAPI (platform APIs)         │
│  libmpv (third-party video decoding)          │
│  fdk-aac (third-party audio decoding)         │
│  WCH / FTDI USB-I2C driver libraries          │
└───────────────────────────────────────────────┘

┌───────────────────────────────────────────────┐
│                 Reference                     │
│                                               │
│  XcertPLay / Carplayd / DiPlay                │
│  remote-mfi-for-xcertplay                     │
│  Protocol and behavior reference only         │
└───────────────────────────────────────────────┘
```

## Layer Responsibilities

### Application

The application layer sits between the UI and the Core, as required by `DEC-002`.

It exposes operations and state to the UI and translates UI requests into Core calls.

Responsibilities:

* Session lifecycle operations (connect, disconnect, reconnect)
* Application state and event surface for the UI
* Command handling from the UI toward Core
* Metadata and status presentation data

The application layer must not depend on a specific UI framework, and it must not implement CarPlay protocol logic itself. Protocol logic stays in Core.

### Core

Contains the main CarPlay implementation and protocol logic.

Core should not depend on the UI implementation.

Wired and wireless connections should share the same CarPlay session and protocol layers wherever possible.

### UI

Windows-specific user interface and interaction layer.

Initial target:

* Win32
* Display
* Touch
* Mouse (input modality not confirmed in the CarPlay protocol; see `PROJECT.md` corrections and `TODO.md` sections 1 and 9)
* Keyboard (input modality not confirmed in the CarPlay protocol; see `PROJECT.md` corrections and `TODO.md` sections 1 and 9)

### Lib

Third-party libraries and platform backends used by Core.

Prefer Windows native APIs when practical.

Examples:

* Direct3D
* WASAPI
* USB APIs
* libmpv (video decoding, see `DEC-009`)
* fdk-aac (audio decoding, see `DEC-010`)
* WCH CH341/CH347 and FTDI D2XX driver libraries (MFi authentication over USB-I2C, see `DEC-011`)

Third-party libraries should not leak unnecessary implementation details into Core.

### Reference

Reference implementations and research material.

* XcertPLay
* Carplayd
* DiPlay
* remote-mfi-for-xcertplay

These projects are used for protocol, behavior and implementation research only.

They are not runtime dependencies.

## Design Principles

1. Keep Core independent from UI; UI talks to Core only through the application layer.
2. Wired and wireless should converge into the same session layer.
3. Prefer native Windows APIs.
4. Use third-party libraries only when they provide practical value.
5. Keep the runtime and distribution size small.
6. Document major architectural decisions in `DECISIONS.md`.

##  UI Isolation

The UI is an independent presentation layer.

UI code must not directly access low-level implementation details such as:

- USB
- NCM
- Wi-Fi transport
- iAP2 internals
- CarPlay protocol internals
- Authentication internals
- Media decoder internals
- WASAPI
- Direct3D implementation details
- Third-party library APIs

UI may only communicate with the application layer through defined interfaces.

The intended dependency direction is:

UI
 ↓
Application
 ↓
Core
 ↓
Platform / Libraries

UI must never bypass the application layer to access Core internals or lower layers.

The UI implementation may be replaced without requiring changes to the underlying CarPlay protocol implementation.

For example:

Win32 UI
    └── Application API
            └── CarPlay Core

Unreal Engine UI
    └── Application API
            └── CarPlay Core

## Detailed Architecture

### 1. Transport & Session Lifecycle Flow
```text
[Bluetooth RFCOMM]
   │  (AF_BTH channel 1, Wireless iAP2 SDP record)
   ▼
[iAP2 Link Setup (0xFF 0x5A)]
   │  SYN -> ACK/SYN -> ACK -> DETECT -> StartIdentification (0x1D00)
   ▼
[MFi Challenge / Certificate Exchange]
   │  ReqAuthCert (0xAA00) -> Cert (0xAA01) -> ReqAuthChallenge (0xAA02) -> Response (0xAA03)
   ▼
[Accessory Identification (0x1D01) & Subscriptions]
   │  IdentifyAccepted (0x1D02) -> Subscriptions (0x5000, 0x4154, 0x4157, 0x5200, 0xAE00)
   ▼
[Wi-Fi SoftAP Handoff (0x5702 / 0x5703)]
   │  Accessory provides SSID & WPA2 passphrase -> iPhone associates to Windows Hotspot
   ▼
[mDNS / DNS-SD & AirPlay Control Connect]
   │  Accessory advertises _airplay._tcp & _carplay-ctrl._tcp -> Dials iPhone ephemeral control port
   ▼
[AirPlay RTSP Handshake]
   │  POST /pair-setup / pair-verify (SRP / Ed25519) -> OPTIONS -> /info -> SETUP phase 1 & 2 -> RECORD
   ▼
[Media Streaming & Multiplexed Input]
   ├─ TCP 7100: Encrypted H.264/HEVC Video Stream -> libmpv -> D3D11 Shared Texture Render
   ├─ UDP 7000/7001/7002: 3-Lane Audio Downlink (Media/Nav/Siri) -> fdk-aac -> WASAPI Shared Mode Pump
   ├─ UDP Uplink: PC Microphone Capture -> Opus/AAC-ELD Encrypt -> RTP -> iPhone
   └─ Encrypted Event Channel: Touch/HID reports, requestSiri, setNightMode, Call Controls
```

### 2. Media Pipeline & Clock Resynchronization
* **Video Pipeline**: TCP 7100 receives 128-byte header frames containing AVCC length-prefixed NALUs. Frames are decrypted via ChaCha20-Poly1305 with per-connection monotonic counter, converted to Annex-B (`00 00 00 01`), and pushed to libmpv's custom stream buffer (`carplay://video`). Libmpv runs with `untimed=yes`, `cache=no`, and `video-latency-hacks=yes` to guarantee ultra-low display latency (<50ms).
* **Audio Pipeline**: 3 independent software mix lanes (`MainMedia`, `AltNavigation`, `SiriTelephony`) render over a non-exclusive WASAPI Shared Mode device. Jitter and clock drift between iPhone RTP packets and host soundcards are absorbed using bounded playback FIFOs and pre-buffered event-driven WASAPI render pumps.

### 3. Wired CarPlay & Platform Feasibility Note (x86/x64 vs ARM64)
* **The xHCI Dual-Role / Device Mode Reality on x86/x64**:
  * Intel architecture specifications define the xDCI (Synopsys DesignWare USB 3.0 Device Controller Interface, [Intel Developer Zone](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-atom-processor-for-smartphone-and-tablet-usb-otg-xdci-driver.html)) capable of USB Dual-Role / OTG operation in silicon.
  * However, on consumer PC motherboards and laptops (x86/x64), **OEM BIOS/UEFI firmware universally disables or hides the xDCI Device Mode option**, forcing the USB controller into exclusive xHCI Host Mode. Windows on x86/x64 also lacks native inbox USB Function/Device controller drivers for host-to-host Type-C cables.
  * Consequently, **native direct USB-to-USB wired CarPlay is not feasible on x86/x64 Windows PCs**. Direct USB Device Mode support in the Windows ecosystem is realistic only on **Windows on ARM64** devices (e.g. Snapdragon X Elite / QC chips with exposed UDC/Dual-Role controllers) or via external bridge hardware (e.g. RP2350 / Linux SBC running NCM/CDC composite).

---

## Documentation Corrections

### 2026-09-28 — Application layer added

Problem:

* The overview diagram stated "four major parts" and showed only UI, Core, Lib and Reference, with no Application layer.
* This conflicted with `DECISIONS.md` DEC-002, which mandates the dependency direction `UI -> Application -> Core -> Platform / Libraries`.
* The "UI Isolation" section already referenced the application layer, so the document was internally inconsistent with its own diagram.

Change:

* Diagram now shows five parts with an Application layer between UI and Core.
* Added an "Application" responsibilities subsection under "Layer Responsibilities".
* Design Principle 1 now names the application layer explicitly.

This correction only documents the layer DEC-002 already requires. It does not change any project goal. If the Application layer boundary needs to be formalized further, record it as a new decision in `DECISIONS.md`.

### 2026-09-28 — Mouse/Keyboard UI targets annotated

The UI "Initial target" list kept Mouse and Keyboard but annotated them as unconfirmed in the CarPlay protocol, matching the note added to `PROJECT.md` and the research task in `TODO.md`. No goal was removed.

## Configuration Layering (2026-09-28, DEC-004)

Configuration is split by ownership, per `DECISIONS.md` DEC-004:

* **Core** (`Core/Common/Config.h`) accepts only facts:
  * `VideoStreamSpec` — negotiated codec / width / height / fps (concrete values, no `"Auto"`).
  * `AudioOutputFormat` — sample rate / channels / bit depth.
  * `DeviceIdentity` + `NetworkEndpoints`, combined as `CoreEnvironmentConfig`.
* **Application** (`Application/AppSettings.h`) owns all user-tunable preferences: resolution presets, codec preference, window display mode, per-lane volumes, device identity strings. The output sample rate is not a preference — the audio pipeline follows the OS default render endpoint's mix format — and the encode bitrate and ducking are fixed/protocol-driven.

The Application layer resolves preferences into concrete Core parameters at the boundary (e.g. `Auto` preset → actual dimensions). Runtime controls such as lane volume and ducking ratio are exposed directly on `IAudioPipeline` (`SetLaneVolume`, `SetDuckingRatio`) instead of living in a config blob; only the lane volumes are driven from the settings UI.
