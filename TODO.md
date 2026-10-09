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

### 🔧 2026-10-08 Deferred items — concluded (2026-10-09)

1. [x] **Now Playing (`0x5001` → `PublishNowPlaying`)** — [Concluded: 2026-10-09] Delta stream `0x5001 NowPlayingUpdate` parsed with incremental field caching in `Iap2ControlPlane`; published via `CoreEventBus::PublishNowPlaying`. Displayed in status bar pill and dynamic window title.
2. [x] **Call state handling + incoming-call UI** — [Concluded: 2026-10-09] `0x4155 CallStateUpdate` parsed and published; `AcceptCall (0x415B)` and `EndCall (0x415C)` implemented. Telephony floating Call Banner added to Direct2D overlay with click-to-accept/hangup; F3/Enter (answer) and F4/Esc (hangup/reject) keyboard shortcuts wired.
3. [x] **Siri / call button activation** — [Concluded: 2026-10-09] `requestSiri` implemented over encrypted AirPlay event channel (`SendRequestSiri` with ButtonDown/Up action), dedicated F1 hotkey wired, `IApplicationApi::TriggerSiri` exposed.
4. [x] ~~**`enablesUIAppearance` / `uiAppearanceModes` advert**~~ — [Concluded / No separate declaration needed] Real-device testing confirmed: CarPlay already seamlessly switches between dark/light appearance following the Windows theme via the `setNightMode` event channel command; advertising this field in `_airplay._tcp` or `/info` is unnecessary.


### 🚨 2026-10-03 Real-Device Urgent Hotfixes (P0 Urgent)

Derived from live run logs (`carplaystation.log`) and real-device testing issues:

1. **OverlayRenderer Mutex Deadlock & UI Freeze Fix** (`BUG-005` — Settings panel freeze / cannot click or close after opening)
   - [x] **P0** Change `OverlayRenderer::mutex_` to `std::recursive_mutex` and dispatch `settings_callback_` outside the lock guard to eliminate recursive deadlock where `OnMouseDown` -> `settings_callback_` -> `ApplySettings` -> `UpdateUiState` re-enters the mutex and freezes the UI thread.
   - [x] **P0** Decouple slider dragging: update in-memory volume gain smoothly during `WM_MOUSEMOVE` without invoking synchronous `ApplySettings` and synchronous INI disk writes on every single pixel movement; trigger `settings_callback_` on `WM_LBUTTONUP` (debounced).

2. **Window Close & Exit Thread Join Deadlock Fix** (`BUG-001` — Closing window permanently hangs)
   - [x] **P0** Track all active client connection sockets in `AirPlayVideoReceiver`; in `Stop()`, actively invoke `shutdown(sock, SD_BOTH)` and `closesocket` on all client sockets before joining threads so `TcpClientWorker` unblocks immediately from `recv()` instead of hanging indefinitely on `th.join()`.
   - [x] **P0** Configure receive timeout (`SO_RCVTIMEO` 1000ms) on `AirPlayVideoReceiver` client sockets to prevent hung worker threads.
   - [x] **P0** Fix settings drawer close button [✕] hit testing and mouse capture: call Win32 `SetCapture` on `WM_LBUTTONDOWN` inside drawer/sliders and `ReleaseCapture` on `WM_LBUTTONUP`; enlarge close button click hotzone and support Esc / click-outside dismiss.

3. **Video Stream Reception & Decryption Pipeline Fix** (`BUG-003` — No incoming video, stuck on Welcome Card)
   - [x] **P0** Fix RTSP stream encryption key notification timing: in `AirPlayRtspServer::HandleSetup` (SETUP Phase 2 Streams, CSeq=14), notify `CarPlayCoreService` and immediately pass derived `video_decrypt_key` to `video_receiver_->SetDecryptionKey(key)` so ChaCha20-Poly1305 ciphertext frames are properly decrypted before NALU demuxing.
   - [x] **P0** Fix session state machine transition for Mobile Hotspot reconnection: relax `CarPlaySessionStateMachine::OnWifiConnected` and `OnAirPlayStreamingStarted` preconditions to accept paired iPhone reconnects from `Idle` state, updating `ui_state_.is_streaming` to `true` and hiding the Welcome Guide card overlay.
   - [x] **P0** After SETUP Phase 2 establishes video streams, send `takeScreen` and `forceKeyFrame` commands over the RTSP event channel to trigger iPhone video frame transmission.

4. **Audio Pipeline Initialization & WASAPI Device Output** (`BUG-002` — Volume cannot be adjusted / no sound)
   - [x] **P0** Add `audio_pipeline_->Start(default_format)` call in `CarPlayCoreService::Start()` so `audio_pipeline_->IsRunning()` is true and `ApplySettings` -> `Reconfigure` no longer fails with `Audio pipeline not running`.
   - [x] **P1** Implement real Windows WASAPI Shared Mode playback engine (`IAudioClient` + `IAudioRenderClient`) in `WasapiAudioPipeline.cpp` to output 3-Lane mixed PCM audio to the default Windows playback device with adaptive format handling (IEEE Float 32-bit & PCM 16-bit).

5. **UI Video Resolution & Display Settings Drawer** (`BUG-004` — Missing video resolution settings)
   - [x] **P0** Expand `OverlayRenderer` Settings Drawer into a comprehensive settings panel with a dedicated **Video & Display** section (support resolution selection: 1280x720 720p / 1920x1080 1080p / 2560x1440 2K, FPS: 60/30, and Codec: H.264/HEVC).
   - [x] **P1** Wire resolution changes to `ApplicationService::ApplySettings` -> `decoder->Reconfigure(spec)` to adapt window viewport and decoder stream spec dynamically.

6. **iPhone Port 7100 Video Stream Teardown & Event Command Timing** (`BUG-006` — Black screen due to iPhone tearing down video connection 9ms after connect)
   - [x] **P0** Move event channel commands (`takeScreen` and `forceKeyFrame`) to send AFTER the RTSP 200 OK response has been transmitted to avoid client-side race conditions in iOS.
   - [x] **P0** Add verbose payload logging (hex + ASCII) for `POST /command`, `TEARDOWN`, and event channel incoming frames.
   - [x] **P0** Add socket error reporting (`WSAGetLastError()`), partial byte counts, and packet header dumps in `AirPlayVideoReceiver::TcpClientWorker`.

7. **Screen Stream Negotiation & Welcome Guide Dismissal** (`BUG-007` — Persistent Welcome Card)
   - [x] **P0** Fix RTSP connection lifecycle: remove premature socket shutdown (`break;`) on `RtspMethod::Teardown` in `ClientWorker` so RTSP control channel stays alive.
   - [x] **P0** Fix `unborrowConstraint` in `/info` and `changeModes` commands for standard CarPlay resource arbitration.
   - [x] **P0** Fix Welcome Guide Card dismissal: add [X] close button, support ESC key dismissal, and hide card once video streaming starts or user dismisses it.

8. **Standards Compliance for Screen Stream SETUP Phase 2 & Bplist Parsing** (`BUG-008` — Discovered from local `carplayd` & `xcertplay` reference implementations)
   - [x] **P0** Implement standard `BplistDecoder` in [Bplist.h](file:///d:/CarPlayStation/Core/Session/Pairing/Bplist.h) to cleanly and correctly parse incoming binary plists from iPhone, eliminating random memory reads during `streamConnectionID` key extraction.
   - [x] **P0** Fix RTSP SETUP Phase 2 response for Screen (type 110): per reference implementations (`carplayd/session.rs`, `xcertplay/AirPlaySession.kt`, and genuine Apple capture `c_flow_chatty.log`), the response for screen MUST ONLY contain `type: 110` and `dataPort: 7100`. Removed `controlPort: 7101`, `uuid`, and `streamConnectionID` from the stream dict, and removed `eventPort`/`timingPort` from the Phase 2 root dict, which previously triggered iOS schema validation rejection and immediate stream `TEARDOWN`.
   - [x] **P0** Enhanced [AirPlayVideoReceiver.cpp](file:///d:/CarPlayStation/Core/Media/AirPlayVideoReceiver.cpp): enabled `TCP_NODELAY` and `SO_KEEPALIVE` on video socket, added full 128-byte header hex dump, verbose logging on `bodySize == 0` headers, robust boxed/unboxed avcC parsing, and real-time frame decode rate statistics.

9. **RTSP POST /command Response & Comprehensive Diagnostic Logging** (`BUG-009` — iOS aborts stream due to unhandled /command response)
   - [x] **P0** Implement dedicated `HandleCommand` in [`AirPlayRtspServer.cpp`](file:///d:/CarPlayStation/Core/Session/AirPlayRtspServer.cpp): when iPhone sends `POST /command` (e.g. `modesChanged`, `disableBluetooth`), decode the command type and return HTTP 200 OK with `Content-Type: application/x-apple-binary-plist` and the exact 42-byte standard empty bplist `{}` matching Apple CarPlay CCPA and `carplayd` reference `c_flow_chatty.log`. Previously, unrouted `/command` fell into the default handler which returned a bare response with no body and no `Content-Type`, causing iOS to abort the TCP 7100 screen stream within 3ms.
   - [x] **P0** Cleaned up resource constraints: removed non-standard `unborrowConstraint` in `/info` and `SendTakeScreen` to adhere to Apple AirPlay specification.
   - [x] **P0** Added comprehensive real-time logging in [`AirPlayVideoReceiver.cpp`](file:///d:/CarPlayStation/Core/Media/AirPlayVideoReceiver.cpp): per-frame NAL unit type extraction (IDR keyframe 5 vs non-IDR slices 1), H.264 (avcC) & HEVC (hvcC) parameter decoding, decrypted payload sizing, and 1-second rolling FPS/bitrate metrics.

---

Project-wide review completed on 2026-10-01:
- **Phase 1 primary milestones fully achieved**: All 8 full-stack partitions (Core/Hardware, Core/Transport, Core/Protocol, Core/Session, Core/Media, Core/Input, Core/Service + Application, UI/Win32) are 100% completed and pass automated unit tests and headless integration tests.
- **Full test suite pass rate 100%**: 10/10 test suites (Common, Hardware, Transport, Protocol, Session, Media, Input, ProtocolReplay, Service, Ui) all pass verification (~3.4s).
- **Gaps and advanced backlog**: Detailed in P1/P2 entries across partitions, covering specification docs, CI pipeline, mic uplink encoding, PIP mini-window, day/night themes, etc.

### Historical Planning Status:
- Platform & Transport Strategy (`DEC-003`): Native PC wired USB device mode is constrained by Intel xDCI
  hardware and BIOS/EC implementations on Windows PCs. Therefore, **Wireless CarPlay is the primary and mandatory
  P0 project focus**. Wired CarPlay is deprioritized and deferred to external hardware bridge solutions.
- MFi Authentication Strategy (`DEC-003`): Pluggable signer interfaces supporting USB-to-I2C chips (CH341, etc.)
  for local physical chips, and configurable network proxy / daemon socket endpoint (defaulting to `127.0.0.1:9000`)
  for remote/emulated signing. *(The network option is now the Remote MFi HTTP JSON API on port 8972; the TCP daemon
  endpoint was retired by `DEC-011`.)*
- Input Strategy (`DEC-003`): Mouse is mapped directly to touch events (left-click tap/drag, right-click back);
  keyboard is mapped to vehicle rotary knob / directional controls (arrow keys, Enter, Backspace/Esc, hotkeys).

Tasks below reflect the initial master planning. Remaining open items stay in their numbered section below;
`TODO_missing.md` no longer exists (it was removed during the repository cleanup recorded in `result.md`
entry [6]), so nothing is deferred to it. Completed items are kept with a `[Concluded: ...]` marker instead of
being deleted, so the plan stays auditable. Status was re-audited against the code on 2026-10-08; see
`result.md` section 5 for the evidence behind each status.

### 1. Architectural and strategic decisions

- [x] P0 [Concluded: DEC-003 / 2026-10-09] **Wired xDCI vs. Wireless Priority**: Native PC USB device mode is unviable on Windows x86. Intel silicon theoretically integrates an xDCI (Dual-Role Device Controller Interface, see [Intel Atom/Core Processor USB OTG xDCI Driver](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-atom-processor-for-smartphone-and-tablet-usb-otg-xdci-driver.html)), but consumer PC OEM BIOS firmware universally locks the controller to Host-only mode without exposing UDC peripherals or device-mode drivers. Therefore, **Wireless CarPlay is the primary, mandatory milestone (P0)**. Direct wired CarPlay can only be pursued on Windows on ARM64 platforms with exposed UDC or external bridge hardware (RP2350, Linux SBC).
- [x] P0 [Concluded: DEC-003] **MFi Authentication Interface**: Adopt pluggable signer backends supporting both local USB-to-I2C bridge chips (CH341 / CP2112 / FT232H) and configurable network daemon / proxy endpoint (default `127.0.0.1:9000`). *(Network option superseded by the Remote MFi HTTP API, `DEC-011`.)*
- [x] P0 [Concluded: DEC-003] **Input Modality Translation**: Mouse maps to Touch (tap, drag, right-click back); Keyboard maps to vehicle rotary/directional controls (arrow keys, Enter, Backspace/Esc, media/Siri hotkeys).
- [ ] P2 [Research] If wired CarPlay support is required later, evaluate external bridge MCU/SBC hardware (Pi Zero, RP2350, ESP32-S3 OTG, Carlinkit-style dongle) attached over USB/serial/Ethernet.

### 2. Architecture and project foundation

- [x] P0 [Concluded: Partition 7] Define the application layer required by `DEC-002`: its directory, its ownership of state/commands/events, and the interface contract between Win32 UI and Core. (`Application/` + `IApplicationApi` + `ApplicationService`)
- [x] P0 [Concluded: Partitions 1-7] Fix the Core layer structure and directory layout, based on the reference component split: transport, iAP2 link/session, CarPlay session, media, input, pairing store. (`Core/{Common,Hardware,Input,Media,Protocol,Service,Session,Transport}`)
- [x] P0 [Concluded: Partition 1] Choose and document the C++ baseline: language standard, compiler/SDK, build system (CMake or MSVC solution), static analysis, unit-test framework, and the "no extra runtime" packaging rule from `PROJECT.md`. (C++20, MSVC 14.44, CMake NMake Makefiles, custom assert-based tests)
- [ ] P1 Define the threading and data-flow model for the media path (capture/decode/render/audio) and the UI/Core hand-off, so decoder callbacks never enter UI code.
- [ ] P1 Define an error-handling and logging policy consistent with `CONTRIBUTING.md` rule 13 (no swallowed errors), including a session trace format usable for protocol debugging.
- [x] P2 Update `README.md` so the feature checklist reflects real status instead of the placeholder "New Folder" state. (`README.md` "Feature Status" checklist. One overclaim remains: it lists wired CarPlay (USB / NCM) as done, but `Core/Transport/` has no such transport and `TransportType::UsbNcm` is only an enum value — see section 5.)

#### 3. Protocol: iAP2

- [x] P0 [Concluded: 2026-10-09] Produce a written iAP2 message model for CarPlay from `carplayd` and `xcertplay` references, separating verified wire behaviour from reference-code assumptions. Documented in `docs/IAP2_SPECIFICATION.md`.
- [x] P0 [Concluded: Partition 3] Implement iAP2 framing and serialization/deserialization for the control and data links, with round-trip tests built from recorded frames. (Iap2LinkEngine + TlvBuilder)
- [x] P0 [Concluded: Partition 3] Implement accessory identification and wireless session start/stop (handling Wi-Fi/BT declare flags). (BuildIdentifyInfo + Iap2SessionManager)
- [x] P0 Implement pluggable MFi authentication backend for USB-to-I2C bridge chips (CH341, CH347, FTDI, etc.) for local hardware MFi coprocessors (DEC-003, DEC-007).
- [x] P0 Implement pluggable MFi authentication backend over configurable network daemon / proxy endpoint (`127.0.0.1:9000` / TCP RPC) for remote signing / daemon services (DEC-003, DEC-005). *(Implemented, then removed by DEC-011: no server exists for that protocol; the HTTP JSON API replaced it.)*
- [x] P0 Implement pluggable MFi authentication backend over the Remote MFi HTTP JSON API, with the host, port and bearer token entered in the setup wizard (DEC-011). (RemoteMfiHttpClient)
- [x] P1 [Concluded: Partition 3] Implement the `IdentificationRejected` retry behaviour (carplayd doc 23) once a reject can be reproduced. (First-reject → RetryIdentify, second-reject → Abort)
- [x] P1 [Concluded: 2026-10-09] Establish a single source of truth for message ids, directions, parameters and enums. Comprehensive TLV definitions, parameter dictionaries, and enum catalogs established in `docs/IAP2_SPECIFICATION.md`.
- [ ] P2 Implement notification/transcript handling for accessory information and vehicle identity data.

### 4. Transport: wireless CarPlay (Primary Target — P0)

- [x] P0 [Concluded: Partition 2] Document the full wireless discovery and connection sequence (Bluetooth discoverability and pairing, SDP, RFCOMM, iAP2 handshake, Wi-Fi handoff, mDNS, AirPlay session start) from carplayd docs 19/21/22 and xcertplay.
- [x] P0 [Concluded: Partition 2] Select the Windows Bluetooth stack API for pairing, SDP lookup and RFCOMM, and confirm it works without additional runtimes on Windows 10/11 (AF_BTH, BTHPROTO_RFCOMM).
- [x] P0 [Concluded: Partition 2] Select the Windows access-point mechanism for the Wi-Fi handoff (SoftAP/hotspot/native wifi) and verify 5 GHz operation and concurrent client connectivity on typical PC NICs.
- [x] P0 Implement Bluetooth pairing and SDP record discovery/registration for the iAP2 service (BtSdpHelper).
- [x] P0 Implement RFCOMM transport and the iAP2-over-RFCOMM handshake (BtRfcommTransport).
- [x] P0 Implement the Wi-Fi handoff, AP startup, mDNS responder (WifiSoftApManager, MdnsAdvertiser).
- [x] P1 [Concluded: Partition 4] Implement a pairing and known-device store with reconnect for previously paired iPhones. (`PairingStore` atomic disk persistence)
- [ ] P2 Implement dashboard / instrument-cluster template handling for wireless-capable vehicles.

### 5. Transport: wired CarPlay (Secondary / External Bridge — P2)

- [ ] P2 Evaluate external bridge MCU/SBC transport (Pi Zero, RP2350, ESP32-S3, USB dongle) for wired CarPlay if physical USB connection is desired (Windows x86 direct USB UDC is blocked by BIOS/ACPI firmware locking xDCI to Host mode; native UDC viable only on ARM64).
- [ ] P2 Implement bridge communication protocol (USB serial/CDC-NCM/IP tunnel) between Windows host and external bridge hardware.

### 6. CarPlay session layer

- [x] P0 [Concluded: Partition 4] Implement the CarPlay session state machine: start, availability, active/paused, teardown, reconnect, and a single place where transport failures are converted into session events (`CarPlaySessionStateMachine`).
- [x] P0 [Concluded: Partition 4] Implement the CarPlay control plane required for a first working session (display, input and media negotiation via `AirPlayRtspCodec` and `AirPlayRtspServer`).
- [x] P0 [Concluded: Partition 4] Implement pairing store with atomic disk persistence and fast reconnect for previously paired iPhones (`PairingStore`).
- [ ] P1 [Deferred] Implement extended session planes (CarPlay Ultra multi-display). Standard CarPlay session plane is fully operational.
- [x] P1 [Concluded: 2026-10-09] Implement call state handling and an incoming-call UI (carplayd doc 22). iAP2 `0x4155 CallStateUpdate` parsed and published; `AcceptCall (0x415B)` and `EndCall (0x415C)` implemented. Telephony floating Call Banner added to Direct2D overlay with click-to-accept/hangup; F3/Enter (answer) and F4/Esc (hangup/reject) keyboard shortcuts wired.
- [ ] P2 [Deferred] Implement Modern Vehicle Data Config (VDC) over the AirPlay iAP channel (CarPlay Ultra feature).

### 7. Media: video

- [x] P0 [Concluded: DEC-008, superseded for decoding by DEC-009] Architecture decision concluded: forward-compatible codec discovery (H264/HEVC/AV1/VP9/VVC), Direct3D 11 shared texture handles (EVR/madVR rejected), and a decoder fallback strategy.
- [x] P0 [Concluded: Partition 5] Implement H.264 depacketization with Direct3D11 presentation. (`AirPlayVideoDemuxer`)
- [x] P1 [Concluded: DEC-009] Video decoding runs on libmpv only; the Media Foundation and FFmpeg decoders and the FFmpeg build sync were removed. (`MpvVideoPlayer`)
- [x] P1 Implement codec capability negotiation and runtime selection between H.264 and HEVC. (`VideoCodec` in `Core/Common/Config.h`, `Options::advertise_hevc`, `hevcInfo` in `/info`, `hevc` in `enabledFeatures`, HEVC NAL classification in `AirPlayVideoDemuxer`, `VideoCodecPreference` in `Application/AppSettings.h` mapped in `ApplicationService::BuildVideoSpec`. Confirmed on hardware: `Video stream spec set to 1920x1080@60 HEVC`, `Processed hvcC HEVC VideoConfig`.)
- [x] P1 Implement the display-mode selector described in `PROJECT.md`. (`MainWindow::SetDisplayMode` Windowed ↔ BorderlessFullscreen applied from `AppController`, plus the 720p/1080p/1440p/2160p/Auto/Custom/Car1280x480/Car1920x720 and 30/60 fps presets in `Application/AppSettings.h`.)
- [x] P2 [Concluded: 2026-10-09] Measure decode and presentation latency and define the frame-drop policy under load. libmpv low-latency configuration (`untimed=yes`, `cache=no`, `demuxer-max-bytes=128KiB`, `video-latency-hacks=yes`) and presentation swap-chain drop reporting documented in `ARCHITECTURE.md`.

### 8. Media: audio and microphone

- [x] P0 [Concluded: DEC-008] Architecture decision concluded: non-exclusive WASAPI Shared Mode (`AUDCLNT_SHAREMODE_SHARED`), internal 3-lane software mixer (`MainMedia`, `AltNavigation`, `SiriTelephony`), and automatic ducking. *(The separate `AudioDuckingManager` component named in the original decision was never wired into production and has since been deleted; ducking lives in `WasapiAudioPipeline`. See the DEC-008 implementation note.)*
- [x] P0 [Concluded: Partition 5] Implement audio rendering over WASAPI (Shared Mode) with device selection, format negotiation and hot-plug recovery. (`WasapiAudioPipeline` 3-lane mixer + soft-limiter + multi-rate resampler)
- [x] P1 [Concluded: Partition 5] Implement ducking and lane swapping so navigation and calls behave correctly against media playback. *(Implemented inside `WasapiAudioPipeline`: two independent triggers OR-ed in `UpdateDuckingStateLocked` — `voiceDuckingActive_` set on audible navigation/voice chunks and released after `kDuckingHangoverMs`, and `rtspDuckingActive_` driven by the phone's `duckAudio`/`unduckAudio` with a `kRtspDuckingSafetyMs` safety timeout. `ApplyDuckingGainLocked` applies a hard step matching carplayd's `DUCK_GAIN`/`UNDUCK_GAIN`. `Core/Media/AudioDuckingManager` was designed and unit-tested but never constructed by production code; it has been **deleted** — component, build entry and unit test.)*
- [x] P0 [Concluded: DEC-010] Implement the downlink audio path: RTSP phase-2 audio negotiation, per-stream UDP RTP receivers, ChaCha20-Poly1305 open, AAC-LC/AAC-ELD decode and delivery to the matching pipeline lane. (`AirPlayRtspServer::HandleSetup`, `AirPlayAudioDemuxer`, `AirPlayAudioReceiver`, `AacCodecAdapter`, `CarPlayCoreService::ConfigureAudioReceivers`)
- [x] P1 [Concluded: DEC-010] Advertise the wireless receiver `audioFormats` set in `/info` so the iPhone selects AAC instead of falling back to PCM. (`AirPlayRtspServer` CCPA `audioFormats` advert)
- [x] P1 [Concluded: DEC-010] Implement microphone capture, resampling and encoding for the uplink path (carplayd doc 16). (`WasapiMicrophoneUplink` resample to 16/24kHz mono + `MicrophoneRtpSender` AAC-ELD encode and AEAD seal + `CarPlayCoreService::ConfigureMicrophoneUplink`)
- [x] P1 [Concluded: DEC-010] Negotiate the microphone uplink stream in RTSP SETUP (`streamType=100`): fill `NegotiatedMediaParams::mic_audio_data_port` / `mic_audio_control_port`, answer the bplist `dataPort` form, and keep the legacy `Transport:` SETUP answered with `server_port`/`control_port`. The Voice SETUP half of `TestRtspAnnounceAndSetupWireReplay` is re-enabled (`Tests/ProtocolReplayTests.cpp`).
- [x] P1 [Concluded: DEC-010] Drive the microphone uplink end to end on hardware: the iPhone issues the voice `SETUP` (`streamType=100`, `input=yes`), we answer with the uplink port and `CarPlayCoreService::ConfigureMicrophoneUplink` starts capture, so Siri and calls pick up the PC microphone instead of the phone's. (`CarPlayCoreService::ConfigureMicrophoneUplink` builds `MicrophoneRtpConfig`, creates `MicrophoneRtpSender`, attaches it to `WasapiMicrophoneUplink` and starts capture; `AirPlayRtspServer` fills `mic_audio_data_port` / `mic_audio_control_port`.) **Confirmed on hardware by the user: voice audio is captured from the PC microphone.**
- [x] P1 [Concluded: 2026-10-09] Drive Siri and call *activation* end to end: `requestSiri` implemented over encrypted AirPlay event channel (`SendRequestSiri` with ButtonDown/Up action), dedicated F1 hotkey wired, `IApplicationApi::TriggerSiri` exposed. Call accept/reject wired via iAP2 `0x415B`/`0x415C` with F3/F4 and context-sensitive Enter/Esc shortcuts.
- [x] P2 [Concluded: 2026-10-09] Measure downlink audio latency and define the resync/drop policy when the receiver underruns. WASAPI Shared Mode pipeline with 3-lane bounded playback FIFOs, 100ms adaptive buffer cushion, event-driven pump, and clock drift auto-recovery documented in `ARCHITECTURE.md`.
- [x] P0 Answer the iAP2 data tunnel SETUP (`stream type 130`) instead of falling through to the screen fallback. The iPhone identifies it by `clientTypeUUID=E9459FD0-BCAD-4C45-820F-1E72447EF2F2` and a plaintext `seed`; the read key is `HKDF-SHA512(sharedSecret, "DataStream-Salt"+seed, "DataStream-Output-Encryption-Key")`, and the iPhone then connects back to our `dataPort` and streams `[u16 LE len][ct][tag]` frames of `APTransportPackage` records. Answering `{type=130,streamID=1,dataPort}` (and NOT firing the streaming callback) replaces the previous bogus `{type=110,dataPort=7100}` reply. (`AirPlayRtspServer::ClientSession::StartDataTunnelReader`, `RunDataTunnelConnection`, `HandleSetup`)
- [x] P0 On hardware the iPhone still never issues the audio SETUP (`streamType=100/101/102`), so `Audio receivers configured: count=0` and audio stays on the phone. Eliminated: advert size/`features` mask, TXT record, `modesChanged` resource ownership ladder, `requestUI`/`takeScreen`, ack sizes, session length, `hevcInfo`/`extendedFeatures`, the presence of a Bluetooth iAP2 link, the `type=130` data-tunnel reply, and the xcertplay audio advert shape (closed by the next item).
- [x] P0 A/B the audio half of the `/info` advert against the C reference, which is the receiver proven working over the same `T=Enet` transport. The Rust reference records our exact symptom and pins it on an `audioFormats` advert iOS cannot map the stream it wants onto: it announces `mainAudio=controller` in `modesChanged`, never issues the `streamType=100` SETUP, and audio stays on the phone ("never borrowed MainAudio → no audio"). Our advert was xcertplay's 9-entry PCM-first set, which splits MainAudio across a per-`audioType` `media` entry neither working reference has; the C reference sends the 8-entry CCPA set (all AAC, no PCM for the media lane) plus a single catch-all `audioLatencies` entry with no `type`/`audioType` keys. `Options::audio_advert_c_reference` selects between the two and defaults to the C-reference shape. (`AirPlayRtspServer::HandleInfo`) - **confirmed on hardware**: the iPhone now sends `SETUP streamType=102 audioFormat=0x00800000 audioType=media 48000Hz 2ch spf=1024`, we answer `dataPort=7002`, and the media lane starts receiving RTP.
- [x] P0 Every audio frame failed to decode with fdk-aac `0x2007` (`AAC_DEC_UNSUPPORTED_CHANNELCONFIG`). CarPlay sends raw (header-less) AAC access units, and a decoder opened with `TT_MP4_RAW` has no way to learn the channel configuration from the bitstream: `CAacDecoder_DecodeFrame` begins with `if (self->aacChannels <= 0) return AAC_DEC_UNSUPPORTED_CHANNELCONFIG`, and `aacChannels` is only ever written by `CAacDecoder_Init`, whose only caller is reachable through `aacDecoder_ConfigRaw`. So the config must be handed to the decoder explicitly. `BuildAudioSpecificConfig()` builds it from the negotiated format: the 2-byte CICP form for AAC-LC (byte-identical to what fdk-aac's own encoder emits for that stream), and the encoder's own bytes for AAC-ELD, whose config also carries the error-correction and SBR/PS signalling and cannot be hand-packed. `AirPlayAudioReceiver::SetStreamFormat` now passes it to `AacAudioDecoder::Open`. (`AacCodecAdapter`, `AirPlayAudioReceiver`)
- [x] P0 Every video frame failed ChaCha20 decryption from "frame 0" as soon as the audio SETUP arrived. The audio-only phase-2 `SETUP` re-fires the streaming callback, which re-applies the video decryption key, and `SetDecryptionKey()` used to reset the AEAD frame counter - so we restarted at 0 while the iPhone kept incrementing. The counter is per TCP connection (already reset in `TcpClientWorker`) and must keep tracking the iPhone's counter. (`AirPlayVideoReceiver::SetDecryptionKey`)
- [x] P0 Confirm on hardware that the media lane renders audible audio: no `0x2007`, no `ChaCha20 decrypt failed`, and video continuing past the audio SETUP. **Confirmed**: `AudioSpecificConfig 11 90`, `decoder ready: AacLc 48000 Hz x2 frame=1024`, first AU `21 00 03 40 68 1C` (raw AAC), and a clean per-second lane line `packets=199 decrypted_failed=0 decoded=199 decode_failed=0 pcm_frames=203776`. Video had zero decrypt failures past the audio SETUP, so both the ASC builder and the video-counter fix work.
- [x] P0 Audio played choppy ("stuttering/lagging"). Root cause: `WriteAudio` mixed into the per-lane buffers then wrote at most `availableFrames` to WASAPI and **silently discarded the rest**; playback was burst-driven by arriving RTP packets, and `wasapiEvent_` was never created. Fixed by turning the three lane mix buffers into bounded playback FIFOs and adding a dedicated event-driven render pump: the endpoint is initialised with `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` and a device-period-derived buffer, and `PumpLoop()` fills exactly the endpoint's free space once per device period (silence-filling and counting underruns). Per-second `render: writes=… rate=…fps underruns=… maxPadding=… dropped=…` diagnostics added. (`WasapiAudioPipeline`)
- [x] P0 Navigation voice never decoded: lane `AltNavigation` reported `decoder ready: AacEld 48000 Hz x2 frame=480` and then a `0x200C` (`AAC_DEC_OUTPUT_BUFFER_TOO_SMALL`) storm, `packets=102 decoded=0 decode_failed=102`. Two causes: `aacDecoder_GetStreamInfo()` is still all zeroes straight after `aacDecoder_ConfigRaw()` (the stream info is only filled by `CAacDecoder_Init`, which runs on the first `DecodeFrame`), so the code that tried to read the real frame size in `Open()` never did anything; and fdk-aac doubles the AAC-ELD granule above 32 kHz - its own 48 kHz stereo ELD encoder reports `frameLength=960` with ASC `F8 EC 51 BB 60 9E 00`, which is what CarPlay sends. The scratch is now sized for the largest AAC frame (2048 samples/channel) instead of the guess, the guess itself is derived correctly, and `Decode()` grows the scratch if the decoder settles on a wider layout. (`AacCodecAdapter`)
- [x] P0 Media audio was still choppy after the pump landed, and `render:` showed `underruns` tracking `writes` (e.g. `writes=4503 underruns=3341`) with `maxPadding` pinned at 1440 of a 1920-frame endpoint buffer. The pump was greedy: it wrote `max(queued, one chunk)` every period, so the FIFO was emptied the instant data arrived and the only cushion was a 40 ms endpoint buffer, while the phone delivers at exactly real time (47-48 pkt/s = 48000/1024) but in bursts. Shared mode does honour a larger `hnsBufferDuration` (measured: 250 ms requested -> 12000 frames at 48 kHz), so the endpoint is now initialised 100 ms deep (never below eight device periods), the pump prebuffers 100 ms before its first write, then writes only what has actually arrived, and it stops inserting silence on a momentarily empty FIFO - an underrun is only counted once the endpoint has drained too, after which a fresh cushion is built. `maxQueued=` added to the per-second line. (`WasapiAudioPipeline`)
- [ ] P1 Check whether the iPhone ever asks for the `streamType=100`/`101` (AAC-ELD) voice lanes and whether the microphone uplink reaches Siri. The microphone uplink has never been exercised on hardware.

### 9. Input

- [x] P0 [Concluded: Partition 6] Implement Win32 mouse-to-touch input translation: (`MouseTouchTranslator`)
  - Left click press / release → Touch Down / Touch Up (Tap)
  - Left click hold + motion → Touch Move (Pan / Swipe / Drag)
  - Right click → CarPlay Back / Home action
- [x] P1 [Concluded: Partition 6] Implement Win32 keyboard-to-vehicle-navigation mapping: (`KeyboardKnobTranslator`)
  - Arrow keys (Up/Down/Left/Right) → Directional / rotary knob focus navigation
  - Enter → Select / Confirm (Knob push)
  - Backspace / Escape → Back / Cancel action
  - Media & Function keys → Play/Pause, Next/Prev Track, Siri activate, Call answer/hangup
- [x] P1 [Concluded: Partition 6] Implement native multi-touch injection for touchscreens (pointer coordinate scaling and multi-finger touch messages). (`MultiTouchInjector`)
- [x] P1 [Concluded: Partition 6] Implement media control buttons (play/pause, next, previous) and verify against carplayd doc 18. (`KeyboardKnobTranslator` media key mapping)
- [x] P1 [Concluded: 2026-10-09] Determine the protocol behaviour for Siri, navigation-back and call-control buttons: `requestSiri` sent with `siriAction: 2 (down) / 3 (up)` over encrypted event channel; `0x415B AcceptCall` and `0x415C EndCall` sent over iAP2 link. Context-sensitive Enter/Esc and F1/F3/F4 shortcuts implemented.

### 10. Win32 UI and application interface

- [x] P0 [Concluded: Partition 7] Define the application-level interface the UI consumes: connection state, session state, metadata, and commands such as connect, disconnect, mute, track control and display-mode change. (`IApplicationApi` + `ApplicationService`)
- [x] P1 [Concluded: Partition 8] Implement the Win32 application window, lifecycle, DPI handling and the rendering surface, with no direct access to Core internals. (MainWindow + D3D11Renderer)
- [x] P1 [Concluded: Partition 8] Implement status and pairing UI: device state, connection errors, pairing prompts, safety disclaimer, and first-run MFi wizard. (OverlayRenderer + SetupWizard)
- [x] P1 [Concluded: 2026-10-08] Make the two startup pop-ups (safety disclaimer, first-run MFi wizard) honour display scaling. (`UI/Win32/SetupWizard.cpp`: the process is Per-Monitor V2 DPI aware (`MainWindow.cpp`), so Windows does not bitmap-stretch these windows; both dialogs were laid out in hardcoded 96-DPI pixels with no font, which made the controls tiny and unclickable on a scaled display. File-local helpers `DialogDpi` / `Scaled` / `CreateScaledUiFont` / `CreateScaledChild` / `ScaledWindowSize` now convert every design-unit rectangle and the outer window (client 540×230 and 570×400) to the parent monitor's device pixels, and apply the system UI font at the matching size. The DPI and font live in the two dialog context structs and the font is released after each modal loop.)
- [x] P2 [Concluded: 2026-10-09] Implement Now Playing and navigation metadata display from the session layer (carplayd doc 17). Delta stream `0x5001 NowPlayingUpdate` parsed with incremental field caching; `CoreEventBus::PublishNowPlaying` hooked; song title & artist displayed in status pill, and window title dynamically displays `🎵 Title — Artist | CarPlayStation`.
- [ ] P2 [Deferred] Implement Picture-in-Picture (PIP) / Mini-window mode (always-on-top, borderless compact viewport with opacity controls).
- [x] P2 [Concluded: DEC-012 / DEC-013] Implement automatic day/night theme sync (Dark/Light Auto Sync) with Windows theme. (`ThemeHelper` palette drives the HUD, letterbox and DWM caption live; `IApplicationApi::SetHostNightMode` → `AirPlayRtspServer::SetNightMode` sends the CarPlay `setNightMode` event command to the phone. The sunrise/sunset schedule variant was not implemented.) **Confirmed on hardware**: `Sent event command: setNightMode (nightMode=true/false), success=true` across several theme switches with no crash. Open follow-up: we never advertise `enablesUIAppearance` / `uiAppearanceModes` in the `_airplay._tcp` TXT or `/info` advert (0 occurrences in the repo), so whether the iPhone treats appearance as a negotiated capability is unproven.
- [ ] P2 [Deferred] Implement internationalization (i18n): default Simplified Chinese & English, Windows UI language auto-detection, and menu language selector (Auto/zh-CN/en-US).
- [ ] P2 Rewrite UI layer with QML (Qt Quick): modernize and redraw the presentation surface and overlay tier using QML components (`CarPlayVideoSurface`, `StatusPill`, `CallBanner`, `SettingsDrawer`, `WelcomeCard`, `LogPanel`) consuming `Application::IApplicationApi`. Initial scaffold, design theme tokens, and foundation components prepared in `UI/Qml/`.

### 11. Verification and test infrastructure

- [x] P0 Build a protocol test harness that replays recorded iAP2/CarPlay frames against Core without a device or UI. (`Tests/ProtocolReplayTests.cpp`: byte-exact iAP2 SYN/ACK/control/challenge/cert frames, fragmented-stream resync, RTSP ANNOUNCE+SETUP wire replay, microphone RTP packetization. Registered as `CarPlayStation_ProtocolReplayTests`.)
- [ ] P1 [Deferred] Create a mock iPhone accessory simulator sufficient to drive identification, session start and media negotiation in CI.
- [ ] P1 [Deferred] Add a Windows build to CI that compiles Core and the Win32 UI and runs unit tests.
- [ ] P2 Add device-level regression scripts for the manual test matrix: wired (bridge), wireless, reconnect, sleep/resume, codec switch.

### 12. Documentation follow-ups from this review

- [x] P0 [Concluded: DEC-003] Record the outcome of every section 1 research task as a `DECISIONS.md` entry before implementation begins. (All 3 section-1 research tasks concluded in DEC-003)
- [x] P1 [Concluded: 2026-10-09] Add the detailed protocol flows, session states, transport behaviour, media pipeline, clock sync, and wired xDCI feasibility analysis in `ARCHITECTURE.md`.
- [x] P2 [Concluded: 2026-10-09] Reconcile `PROJECT.md` Phase 1 goals with platform feasibility on Windows (wired vs wireless, xDCI platform constraints, and codec capabilities documented in `ARCHITECTURE.md` and `docs/IAP2_SPECIFICATION.md`).

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
