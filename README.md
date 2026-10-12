# CarPlayStation

[English](README.md) | [简体中文](README_zh.md)

A Windows CarPlay receiver.

> [!NOTE]  
> **Language Notice:** The application UI and setup wizard are currently available in **Simplified Chinese only**. Multi-language support (including English) is planned for upcoming releases.

CarPlayStation turns a PC into the head unit of a car. It advertises itself as a
CarPlay accessory, pairs with an iPhone over Bluetooth (RFCOMM) and Wi-Fi
(AirPlay), runs the full CarPlay session — iAP2 identification, MFi
authentication, pairing, RTSP setup, stream keys — and then renders the
encrypted H.264 video and AAC audio locally while sending touch, pointer and
remote-control input back to the phone.

Everything is native C++ (C++17) on Win32: no .NET, no Electron, no extra
runtime to install, and no media framework beyond libmpv for video decoding and
fdk-aac for audio decoding.

## Status

Implementation in progress. The Core protocol stack (MFi auth, pairing, iAP2,
transport, session), the media pipeline, and the UI / Application layers are in
place and a wireless CarPlay session reaches the desktop end to end.

Features:

- [ ] Wired CarPlay (USB / NCM) — Not viable directly on Windows x86 due to Intel xDCI hardware controller being locked to Host mode by PC OEM BIOS firmware without device-mode peripheral stack (see [Intel xDCI Driver](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-atom-processor-for-smartphone-and-tablet-usb-otg-xdci-driver.html)); deferred to future Windows on ARM64 UDC or external bridge hardware (RP2350/Linux SBC, `DEC-003`)
- [x] Wireless CarPlay (Bluetooth RFCOMM + Wi-Fi hotspot, mDNS/DNS-SD)
- [x] MFi authentication — three interchangeable backends (see below)
- [x] Pairing / identity — `pair-setup` (SRP), `pair-verify`, `auth-setup`,
      persistent accessory identities (`carplay_pairings.json`)
- [x] AirPlay RTSP setup phase 1 / info / record / setup phase 2 (stream keys)
- [x] Video — ChaCha20-Poly1305 frame decryption, libmpv decode, D3D11
      shared-texture render (see `DEC-009`)
- [x] Audio — three-lane WASAPI shared-mode pipeline, AAC via fdk-aac
      (see `DEC-010`)
- [x] Microphone uplink — WASAPI capture, sealed and sent as RTP (PT 100)
- [x] Siri activation — triggers `requestSiri` over encrypted AirPlay event channel (`F1`)
- [x] Telephony & Call handling — iAP2 `0x4155 CallStateUpdate` session tracking, native in-stream CarPlay call UI, `0x415B AcceptCall` (`F3`) and `0x415C EndCall` (`F4`)
- [x] Now Playing — delta metadata stream (`0x5001 NowPlayingUpdate`) with incremental field caching, HUD pill display, and dynamic window title
- [x] Touch / Input — Win32 input router, single- and multi-touch HID
- [x] Day/night theme sync — follows the Windows light/dark theme locally
      (HUD, letterbox, DWM caption) and sends the CarPlay `setNightMode`
      event command to the phone (see `DEC-012` / `DEC-013`)
- [x] First-run setup wizard and persisted UI preferences

## Architecture

Five parts, top to bottom:

```text
UI            Win32 window, D3D11/D2D overlay, input router, setup wizard
Application   Settings / commands / events / MFi signer factory (DEC-002)
Core          7-layer CarPlay stack: hardware & MFi, transport, iAP2,
              session & pairing, media, input, service facade
Lib           System APIs / WASAPI (platform APIs), libmpv, fdk-aac,
              WCH and FTDI USB-to-I2C driver libraries
Reference     xcertplay / carplayd / diplay / remote-mfi-for-xcertplay
              (protocol reference only, never linked)
```

`Core/` is itself a strict seven-layer stack with downward-only dependencies,
from Layer 1 (hardware / MFi) up to Layer 7 (the service facade
`ICarPlayCoreApi.h`). Each layer has its own README with boundaries, a file
inventory, and an "AI Partition & Mocking Guide" for isolated development.

See [ARCHITECTURE.md](./ARCHITECTURE.md) for the full layer responsibilities,
[Core/README.md](./Core/README.md) for the layer topology, and
[DECISIONS.md](./DECISIONS.md) for the design decisions (DEC-xxx).

## MFi Authentication Backends

CarPlay requires an Apple-licensed MFi coprocessor to sign the authentication
challenge. CarPlayStation can satisfy that requirement in three ways, chosen in
the first-run wizard or in the settings drawer (`F2`):

| Backend | What it uses | When |
| --- | --- | --- |
| Local file | Imported certificate + private key, signed in software | Development without hardware |
| USB-to-I2C hardware | An MFi authentication coprocessor (CP 2.0C / 3.0) on I2C behind a WCH CH341 / CH347 or FTDI D2XX bridge | Real accessory behaviour, both MFi 2.x and 3.0 (SHA-256) certificates |
| Remote MFi HTTP | An HTTP service that performs the signing (see `Reference/remote-mfi-for-xcertplay`, `DEC-011`) | Sharing one accessory certificate across machines |

The Core never hardcodes a file path, a driver, or a server address: the
Application layer builds the right `Core::Hardware::IMfiSigner` from
`AppSettings` (see `DEC-004`).

## Directory Layout

```text
Core/          CarPlayStation's own implementation (7 layers) — see Core/README.md
Application/   Application layer between UI and Core (settings, signer factory)
UI/Win32/      Win32 window, D3D11/D2D renderer, input router, setup wizard
UI/Qml/        QML / Qt Quick modern UI layer (components, themes, video surface)
Lib/           External libraries — see Lib/Readme.md
Reference/     Protocol reference projects (git submodules)
Tests/         Per-layer unit / replay / hardware tests
scripts/       Helper scripts (launch, firewall rules)
```

## Building

Prerequisites:

- Windows 10/11 (x64)
- Visual Studio 2022 Build Tools (C++ workload)
- CMake 3.20+
- Git (for the submodules)
- A Bluetooth radio and a Wi-Fi adapter (for wireless CarPlay)

Fetch the reference submodules and the pinned fdk-aac submodule:

```bat
git submodule update --init --recursive
```

The remaining third-party packages under `Lib/` (libmpv, the WCH CH341/CH347
driver package, the FTDI D2XX + libMPSSE package) are vendored locally and are
not committed to git. What to download, which version, and where to place it is
recorded in [Lib/Readme.md](./Lib/Readme.md).

Build (x64, Release):

```bat
build.bat
```

or manually:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d D:\CarPlayStation\build-release
cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ..
nmake
```

The executable is produced at `build-release/UI/Win32/CarPlayStation.exe`.

Run the test suites from the build directory:

```bat
ctest --output-on-failure
```

Each layer has its own suite (`CommonTests`, `HardwareTests`,
`TransportTests`, `ProtocolTests`, `ProtocolReplayTests`, `SessionTests`,
`MediaTests`, `InputTests`, `ServiceTests`, `UiTests`). They run against mock
adapters and recorded protocol traffic, so no iPhone, USB-I2C bridge, or
microphone is needed to run them.

## Running

The easiest way is the launcher script:

```powershell
.\scripts\Run-CarPlayStation.ps1            # start the UI
.\scripts\Run-CarPlayStation.ps1 -Headless  # core only, no window
.\scripts\Run-CarPlayStation.ps1 -Debug     # prefer the build-msvc output
```

or start the executable directly:

```bat
build-release\UI\Win32\CarPlayStation.exe
```

First run: the app shows a safety disclaimer, then a wizard that picks the MFi
backend (local file / USB-to-I2C hardware / remote MFi HTTP) and the video and
audio options. Preferences are saved in `carplaystation_ui.ini` next to the
executable when the folder is writable, otherwise in
`%APPDATA%\CarPlayStation\`. See [UI/Win32/README.md](./UI/Win32/README.md)
for every key.

Wireless CarPlay: start the app, then on the iPhone open *Settings → CarPlay*
and select *CarPlayStation*. The app advertises `_airplay._tcp` and
`_carplay-ctrl._tcp` over mDNS, accepts the Bluetooth RFCOMM identification,
brings the phone onto the Windows hotspot, and continues over Wi-Fi. If the
phone never appears, inbound mDNS and RTSP are almost certainly blocked — the
hotspot interface is treated as a *Public* network profile. Fix it once from an
elevated PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\Set-UpFirewall.ps1
```

#### Wi-Fi Hotspot Requirements (Crucial)

When configuring the Windows Mobile Hotspot (*Settings → Network & internet → Mobile hotspot*), the following settings are strictly required:

1. **Network band MUST be set to 5 GHz:**  
   CarPlay audio/video streaming is extremely latency-sensitive. The 2.4 GHz band shares radio spectrum with Bluetooth, causing severe packet collisions and handshake timeouts. Always edit your hotspot and explicitly select **5 GHz** (do NOT choose 2.4 GHz or "Any available band").
2. **Security / Encryption type (Must be WPA2 or WPA2/WPA3 hybrid):**  
   - **Windows 11:** Must be set to **WPA2/WPA3-Personal** or **WPA2-Personal** (⚠️ **Pure WPA3 is NOT supported** by the CarPlay Wi-Fi provisioning handshake and will fail!).  
   - **Windows 10:** Must be set to **WPA2-Personal (AES)**.  
   - ⚠️ Open (unsecured) networks and pure WPA3 are rejected by iOS. Password must be 8–63 characters.

The app writes a rolling log next to the executable (`carplaystation.log`), and
`Ctrl + F4` opens the in-app log panel.

## Input

The CarPlay window forwards local input to the phone. Touch and mouse are sent
as HID touch reports on the touchscreen device; the keyboard and wheel have no
knob device on CarPlay, so they are turned into synthetic swipe gestures on the
same touchscreen device.

| Local input | Sent to the phone |
| --- | --- |
| Left mouse / touch drag | Touch / scroll |
| Right mouse, `Backspace` | Back button |
| `↑` `↓` `←` `→`, `W` `A` `S` `D` | Swipe up / down / left / right |
| Mouse wheel | Swipe down / up (scroll) |
| `Enter` | Tap at the centre of the screen |
| `Esc` | Back button |
| `Space`, media keys | Play-pause / next / previous |
| `F1` | Siri activation (`requestSiri` button event) |
| `F2` | Settings drawer (local only) |
| `F3` | Accept incoming call (`0x415B AcceptCall`) |
| `F4` | Reject / End active call (`0x415C EndCall`) |
| `Ctrl + F4` | In-app log panel (local only, when the overlay is up) |
| `F11` | Toggle fullscreen (local only) |

### Multi-touch

There is nothing to configure. At startup the app asks Windows what pointer
devices exist: if a display digitizer reports two or more simultaneous contacts
(Surface, all-in-one touch panels), the touchscreen HID descriptor advertised in
`/info` declares two contacts and touch reports carry two slots. On a machine
with only a mouse, touchpad or pen, the single-contact descriptor is used. The
startup log records which mode was chosen and why.

Two-finger input is read from `WM_POINTER*` messages, not from the synthesized
mouse messages a touchpad produces, which is why a touchpad stays single-point.

## Documentation

Root documents:

- [ARCHITECTURE.md](./ARCHITECTURE.md) — layer responsibilities, data flow, media pipeline latency/resync, and wired xDCI analysis
- [docs/IAP2_SPECIFICATION.md](./docs/IAP2_SPECIFICATION.md) — authoritative iAP2 framing, TLV format, and message dictionary
- [DECISIONS.md](./DECISIONS.md) — design decisions (DEC-xxx)
- [PROJECT.md](./PROJECT.md) — goals, scope, requirements
- [TODO.md](./TODO.md) — open work and roadmap status
- [BUGS.md](./BUGS.md) — known issues (BUG-xxx)
- [CONTRIBUTING.md](./CONTRIBUTING.md) — contribution guidelines
- [Lib/Readme.md](./Lib/Readme.md) — external library records and policy
- [result.md](./result.md) — phase progress and fix log

Layer and module documents:

- [Core/README.md](./Core/README.md) — the seven-layer Core topology
- [Core/Hardware](./Core/Hardware/README.md) — MFi signers, USB-to-I2C bridges, touch capability probe
- [Core/Transport](./Core/Transport/README.md) — RFCOMM, IP/NCM, mDNS/DNS-SD
- [Core/Protocol](./Core/Protocol/README.md) — iAP2 framing, packet engine, wireless control plane
- [Core/Session](./Core/Session/README.md) — CarPlay state machine, AirPlay RTSP, pairing crypto
- [Core/Media](./Core/Media/README.md) — video and audio receivers, codecs, WASAPI pipelines
- [Core/Input](./Core/Input/README.md) — HID input and multi-touch injection
- [Core/Service](./Core/Service/README.md) — the `ICarPlayCoreApi` facade
- [Application/README.md](./Application/README.md) — settings model and signer factory
- [UI/Win32/README.md](./UI/Win32/README.md) — window, renderer, overlay, setup wizard, INI keys
- [UI/Qml/README.md](./UI/Qml/README.md) — QML / Qt Quick architecture, components, and rendering model

Every layer README ends with an "AI Partition & Mocking Guide": which interfaces
to mock so the layer can be developed and tested in isolation.

## License

See [LICENSE](./LICENSE).
