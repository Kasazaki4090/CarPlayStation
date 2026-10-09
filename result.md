# CarPlayStation — Phase Results and Fix Summary

## 1. Core Milestones

- **Wireless CarPlay Full Handshake Link Operational**:
  - **iAP2 Link & Dual-Generation MFi Authentication**: Supports generation 2 and 3 (MFi 3.0 SHA-256) hardware certificate probing and dynamic signature verification; passes full bi-directional negotiation from `0xAA00`, `0xAA02`, `0x1D00` (StartIdentification) to `0x1D02` (IdentifyAccepted), seamlessly guiding iPhone to join the Wi-Fi hotspot.
  - **AirPlay 2 RTSP Protocol Chain Fully Landed**: Successfully passes `POST /pair-setup` (M1~M6), `POST /pair-verify`, `POST /auth-setup`, `SETUP Phase 1`, `GET /info`, `RECORD`, and `SETUP Phase 2 (Streams)`.
  - **Real Event and Timing Channels (Event / Timing Ports)**: Dynamically binds and allocates ports; starts accurate background NTP timing response; successfully establishes encrypted Event Channel during `RECORD` phase and receives iPhone 200 OK acknowledgment.
  - **Active Screen Focus Preemption**: Issues `requestUI`, `takeScreen (changeModes)`, and `forceKeyFrame` immediately after establishing the Event Channel, guiding iOS to start screen projection and forcing keyframe emission.

---

## 2. Video Pipeline Fixes

### 2.1 SETUP Phase 2 Key Synchronization Timing Fix
- **Root Cause**: Previously, the Core service triggered stream configuration only upon `RECORD`, but the iPhone's `streamConnectionID` and video ChaCha20 decryption key are derived and persisted during `SETUP Phase 2` (after `RECORD`). Consequently, `AirPlayVideoReceiver` did not receive the decryption key and parsed encrypted ciphertext as plaintext NALUs, skipping all frames due to abnormal lengths.
- **Fix**: Immediately upon deriving stream keys in `AirPlayRtspServer` during `SETUP Phase 2`, trigger `streaming_cb_` to push the 32-byte video decryption key in real time to `AirPlayVideoReceiver::SetDecryptionKey()`, activating ChaCha20 decryption.

### 2.2 Windows Media Foundation Hardware Decoder Overhaul (`MfVideoDecoder`)
- **MFT Creation Upgrade**: Retired legacy COM `CoCreateInstance` (unsupported for direct instantiation) and adopted system standard `MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER)` to dynamically enumerate and activate hardware-accelerated, ultra-low-latency H.264 decoders.
- **MFT Output Buffer Container Allocation**: Handled `MFT_OUTPUT_STREAM_PROVIDES_SAMPLES = NO` MFT behavior by dynamically allocating and attaching `IMFMediaBuffer` based on `GetOutputStreamInfo.cbSize`, completely resolving empty-sample `MF_E_BUFFERTOOSMALL` failures.
- **Direct3D 11 Shared Texture and Color Space Adaptation**:
  - Upgraded shared texture format to `DXGI_FORMAT_B8G8R8A8_UNORM` with explicit `D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET`.
  - Implemented high-speed NV12 -> BGRA memory conversion (~1ms per frame), uploading directly to D3D11 shared texture via `UpdateSubresource` for renderer consumption.

### 2.3 Direct AVCC to Annex-B Passthrough (Zero Frame Loss)
- **Opcode 1 (VideoConfig)**: Extracts SPS and PPS, wraps into Annex-B headers (`00 00 00 01`), and feeds directly to decoder for specification preset, avoiding demuxer false-drops due to missing IDRs.
- **Opcode 0 (VideoFrame)**: ChaCha20-Poly1305 decrypted frames are converted directly to Annex-B stream and fed to the decoder, eliminating legacy demuxer drop locks.

### 2.4 ChaCha20 Nonce Reconnection Counter Reset
- Resets `frame_counter_` to 0 upon receiving a new TCP video connection or session key update, avoiding nonce drift that triggered repeated `ChaCha20 decrypt failed` warnings.

### 2.5 Application Layer Late Frame Consumer Binding
- In `ApplicationService::StartCarPlay()`, immediately attaches the UI-registered `frame_consumer_` after starting Core and creating `video_decoder_`, allowing D3D11 shared textures to trigger `D3D11Renderer::ConsumeFrame()`.

---

## 3. Build & Automated Verification Status

- **Release Build**:
  - MSVC 2022 x64 Release builds 100% cleanly (`CarPlayCore.lib`, `CarPlayUI.lib`, `CarPlayStation.exe`).
- **Unit Test Suites**:
  - `CarPlayStation_MediaTests.exe`: All passed (including MfVideoDecoder D3D11 BGRA texture creation and initialization).
  - `CarPlayStation_SessionTests.exe`: All passed (including RTSP state machine, codec, and lifecycle).

---

## 4. Subsequent Task Planning (UI Layer Overhaul)

1. **Win32 Window Lifecycle & Render Driving**:
   - Refine main render loop and framerate synchronization (VSync presentation).
   - Direct3D 11 SwapChain and Direct2D interop overlay (status bar, floating buttons, menus).
2. **Multi-resolution & Adaptive Scaling**:
   - Letterbox / Pillarbox aspect-ratio preservation and fullscreen interaction across display aspect ratios and DPI settings.
3. **Input Event Forwarding**:
   - Wire mouse/touch clicks and map coordinates back to iPhone via iAP2 / HID touch channel.

---

## Task Execution Logs

---

### Record [6]: Video Pipeline Latency, Window Aspect Locking, Display Descriptor Dimensions Correction, and Decoder Consolidation (Core/Media, UI/Win32)

* **Timestamp**: `2026-10-04 21:20:00 (UTC+8)`
* **Agent**: `GitHub Copilot (Copilot SDK in VS Code)`
* **Background & Implementation**:
  Testing on hardware (iPhone 16 Pro, 1280x720@60 H.264) reported "video stuttering / high input-to-feedback latency", followed by "stretched display aspect ratio". Completed four tasks in order: "Reduce Latency -> Lock Window -> Fix Aspect Ratio -> Streamline Dependencies":

  1. **Step 1 Input-to-Display Latency Optimization (`Core/Media/AirPlayVideoReceiver.cpp`, `Core/Media/MpvVideoPlayer.cpp`, `UI/Win32/AppController.cpp`, `Core/Session/Pairing/Crypto.h`)**:
     - Fixed compilation break in `AirPlayVideoReceiver.cpp`, restoring video receive link.
     - Cached BCrypt algorithm/key providers for ChaCha20-Poly1305, avoiding per-frame provider recreation.
     - Removed per-frame heap allocations and redundant `cpu_staging_` full-frame copies; unused shared texture upload paths are bypassed.
     - Removed unconditional `::Sleep(1)` in `AppController::Step()` (a source of 1ms jitter in the main loop).
     - Result: Average framerate increased from ~20 fps to 30–64 fps, reaching sustained 60 fps intervals.
  2. **Step 2 Window Size Locking (`UI/Win32/MainWindow.h/.cpp`)**:
     - Locked window size per user request: disabled manual resizing and maximize; fixed client area to negotiated resolution to eliminate distorted aspect ratio perception.
  3. **Step 3 Display Descriptor Physical Dimensions Correction (`Core/Session/AirPlayRtspServer.cpp`)**:
     - Pixel measurements showed CarPlay icons at 121x180 (stretched vertically by 1.50x), matching `widthPhysical/heightPhysical = 240/90 = 2.667` vs `1280/720 = 1.778`.
     - Root Cause: `widthPhysical`/`heightPhysical` (mm) in CarPlay `/info` dictates layout aspect ratio before encoding; mismatched physical aspect ratios squash the image by `physical_aspect / pixel_aspect`.
     - Correction rule (consistent with `Reference/xcertplay`): `heightPhysical = round(widthPhysical * heightPixels / widthPixels)` (240 -> 135 in this case). Icons re-measured at 121x121 (1.00 aspect ratio).
  4. **Step 4 Decoder Consolidation to libmpv Single Backend (`Core/CMakeLists.txt`, `Core/Media/`, `cmake/`, `scripts/`, `Lib/`)**:
     - Recorded `DEC-009`, superseding `DEC-008` Media Foundation + FFmpeg fallback.
     - Removed `Core/Media/MfVideoDecoder.{h,cpp}`, `Core/Media/FFmpegVideoDecoder.{h,cpp}`, `cmake/FindOrDownloadFFmpeg.cmake`, `scripts/Update-FFmpeg.ps1`, and local `Lib/ffmpeg/` (386 MB).
     - Retained D3D11/DXGI for shared texture delivery; `IVideoDecoder` interface boundary preserved.
     - Updated `Tests/MediaTests.cpp` fallback tests for `MpvVideoPlayer` (clean failure without `libmpv-2.dll`).
* **Build & Test Metrics**:
  - Build environment: MSVC 2022 Build Tools (`vcvars64`) + CMake `NMake Makefiles`, Release, `build-release/`. Full reconfigure executed due to source list changes.
  - Diagnostics: 0 errors; binary produced at `build-release/UI/Win32/CarPlayStation.exe`.
  - Automated tests: CTest 6/6 suites passed 100% (CommonTests, HardwareTests, TransportTests, ProtocolTests, SessionTests, MediaTests, total time 2.99s).
  - Hardware validation: Icon aspect ratio corrected from 1.50 to ~1.00; framerate sustained at 60 fps.
* **Key Defects & Insights**:
  1. `Squashed display aspect ratio`: Not a rendering layer scaling bug, but contradictory display descriptors in the protocol layer (`widthPhysical`/`heightPhysical` must match pixel ratio; hardcoded 240x90 in `Reference/ncm_carplayd` is a potential defect and must not be treated as authoritative).
  2. `Crash 0xC000041D at startup`: Missing header dependencies in `compiler_depend.make` caused ODR/layout mismatch.
  3. `Low framerate log samples`: Mostly idle screens where iPhone halts encoding during static frames, rather than decoding stalls.
  4. `PrintWindow(hwnd, hdc, 2)` hangs indefinitely on D3D11 flip-model swap chains. Screenshots should use DPI-aware `Graphics.CopyFromScreen` + `GetWindowRect`.
  5. Unexplained observations (retained for future tracking): Decoder `decoded=` count is approximately double the receiver's `total frames=`; periodic `Unknown packet opcode: 5` once per second; `latencyMs` in SETUP Phase 2 stream dict is unread.
* **Completed Cleanup**:
  - Removed `Lib/ffmpeg/` (386 MB), `_wer_dump/`, obsolete logs in `build-release`, `Testing/Temporary/*.log`, and `TODO_missing.md`.
  - Deduplicated `.gitignore` and removed invalid entries (`/Lib/ffmpeg`, duplicate `/Tests`, deleted `/cmake`, `/_wer_dump`).
  - Synchronized documentation across modules for libmpv single backend; recorded `DEC-009` and annotated superseded scope in `DEC-008`.
  - Registered 4 previously untracked test targets (`Tests/InputTests.cpp`, `Tests/ProtocolReplayTests.cpp`, `Tests/ServiceTests.cpp`, `Tests/UiTests.cpp`), expanding test suites from 6 to 10 (all 10 passed in 3.39s).

---

### Record [7]: Windows Theme Synchronization, Theme Switch Crash Fix, and Complete TODO Audit (UI/Win32, Application, Core/Session, Documentation)

* **Timestamp**: `2026-10-08 22:15:53 (UTC+8)`
* **Agent**: `GitHub Copilot (Copilot SDK in VS Code)`
* **Background & Implementation**:
  User requested host and CarPlay appearance synchronization with Windows dark/light mode, then reported crashes on theme switch. Fixed crash and performed complete status audit of `TODO.md`:
  1. **Host-side Theme Following (DEC-012)**: `ThemeHelper` reads registry `Themes\Personalize`; `MainWindow::ApplyTheme` updates palette and titlebar via `DwmSetWindowAttribute`; `OverlayRenderer::ApplyTheme` updates HUD colors; `WM_DWMCOLORIZATIONCOLORCHANGED` and `WM_SETTINGCHANGE` drive dynamic switching; `AppController::ApplyCurrentTheme` synchronizes letterbox color and HUD.
  2. **Phone-side Night Mode (DEC-013)**: `MainWindow` forwards raw signals -> `AppController` determines night mode -> `ApplicationService::SetHostNightMode` -> `CarPlayCoreService::SetHostNightMode` -> `AirPlayRtspServer::SetNightMode` sends `setNightMode` over Event Channel (deduplicated; re-sent upon reconnect).
  3. **Hardware Evidence (`carplaystation.log`)**: Confirmed `Sent event command: setNightMode (nightMode=false/true), success=true`, with video sustained at 58 fps and audio rendering uninterrupted.
  4. **Crash Fix**: `WM_SETTINGCHANGE` handler previously passed `wParam` to `_wcsicmp` instead of `lParam`; `wParam` is a number (0 for theme switch), causing an access violation dereferencing null. Fixed by reading `lParam` in `MainWindow.cpp`.
  5. **Documentation Synchronization**: Added DEC-012/DEC-013 to `DECISIONS.md`; updated module READMEs and `TODO.md`.
* **Build & Test Metrics**:
  - Clean full build succeeded with 0 errors.
  - `CarPlayStation_UiTests.exe`: 8/8 passed (theme parsing, HUD palette, letterbox color, message handling, `setNightMode` deduplication and reset).
  - `CarPlayStation_SessionTests.exe`: All passed (including `setNightMode` parameter encoding).
  - Real device test: Multiple theme switches succeeded without crashes; `setNightMode` logged `success=true`.
* **Key Defects & Insights**:
  1. `WM_SETTINGCHANGE` section string resides in `lParam`, not `wParam` (`MainWindow.cpp`). Tests should dispatch Windows messages directly instead of calling internal methods.
  2. Night mode command must use Event Channel (`stream=11`), not RTSP `SET_PARAMETER` (which returns error 355).
  3. Residual check: Advertised `_airplay._tcp` TXT and `/info` omit `enablesUIAppearance` / `uiAppearanceModes`, but real hardware accepts `setNightMode` cleanly.
  4. PowerShell variable interpolation risk: Unquoted `$content` truncated files; all file editing must use dedicated editing tools.
* **Completed Cleanup**:
  - Removed temporary build logs; `git status` clean with only expected modifications; no new dependencies added.

---

### Record [8]: Dead-Code Cleanup (AudioDuckingManager / VehicleNightModeEvent) and Startup Dialog DPI Scaling (Core/Media, Core/Service, UI/Win32, Documentation)

* **Timestamp**: `2026-10-09 00:40:00 (UTC+8)`
* **Agent**: `GitHub Copilot (Copilot SDK in VS Code)`
* **Background & Implementation**:
  Cleaned verified dead code and fixed tiny unclickable startup dialogs on high-DPI displays:
  1. **Removed `Core/Media/AudioDuckingManager`**: Components, headers, CMake entries, and unit test `TestAudioDuckingManager()` removed. Actual ducking is handled internally by `WasapiAudioPipeline`.
  2. **Removed `CoreEventBus::VehicleNightModeEvent` / `PublishVehicleNightMode`**: Night mode flows directly through `IApplicationApi::SetHostNightMode` -> `AirPlayRtspServer::SetNightMode` (DEC-013).
  3. **Startup Dialog DPI Scaling**: Added scaling helpers (`DialogDpi`, `Scaled`, `CreateScaledUiFont`, `CreateScaledChild`, `ScaledWindowSize`) to `UI/Win32/SetupWizard.cpp` to properly scale coordinates and fonts for Per-Monitor V2 DPI awareness.
  4. **Documentation & Backlog**: Updated `TODO.md` and module READMEs.
* **Build & Test Metrics**:
  - Full build succeeded with 0 errors.
  - CTest 10/10 test suites passed 100% (16.27s).
* **Key Defects & Insights**:
  1. Modifying `CoreEventBus.h` requires rebuilding all dependent compilation units to avoid `LNK2019`.
  2. Per-Monitor V2 dialogs created with absolute pixel dimensions must manually calculate scaling via `GetDpiForWindow`.
  3. Dead code determination must be grounded in hard evidence (0 callers and 0 subscribers).

---

## 5. Architectural Completeness Audit (Against `TODO.md` and Source Code)

### 5.1 Implemented Items

| Item | Evidence |
| --- | --- |
| README Feature checklist reflects reality | `README.md` features updated with real implementation status |
| Codec capability negotiation and runtime selection H.264/HEVC | `VideoCodec` enum (`Core/Common/Config.h`), `advertise_hevc`, `hevcInfo` in `/info`, `hevc` in `enabledFeatures`, HEVC NAL classification in `AirPlayVideoDemuxer`, and hardware logs confirming HEVC 1920x1080@60 |
| Display mode selector described in PROJECT.md | `MainWindow::SetDisplayMode` Windowed <-> BorderlessFullscreen, presets in `AppSettings.h` (720p/1080p/1440p/2160p/Auto/Custom/Car1280x480/Car1920x720 and 30/60 fps) |
| Microphone uplink | `MicrophoneRtpConfig`, `MicrophoneRtpSender`, RTSP negotiation on ports 5002/5003. Verified on real device: voice input captured via PC microphone |
| Single instance and clean exit | Mutex in `MainWindow.cpp`; shutdown deadlocks fixed in BUG-001 / BUG-005 |
| 3-Lane volume and automatic ducking | `SetLaneVolume` 3 lanes + `voiceDuckingActive_` / `rtspDuckingActive_` ducking in `WasapiAudioPipeline.cpp` |

### 5.2 Concluded Items (2026-10-09 Update)

| Item | Evidence |
| --- | --- |
| Now Playing metadata display | `0x5001 NowPlayingUpdate` parsed with incremental field caching in `Iap2ControlPlane`; published via `CoreEventBus::PublishNowPlaying`; displayed in status bar pill and dynamic window title |
| Telephony & Call handling | `0x4155 CallStateUpdate` parsed and published; `AcceptCall (0x415B)` and `EndCall (0x415C)` implemented. Floating HUD Call Banner with click-to-accept/hangup; F3/Enter (answer) and F4/Esc (hangup/reject) hotkeys wired |
| Siri button activation | `requestSiri` implemented over encrypted Event Channel (`SendRequestSiri` with ButtonDown/Up action), dedicated F1 hotkey wired, `IApplicationApi::TriggerSiri` exposed |
| iAP2 specification & SSOT | Authoritative TLV definitions, framing models, and parameter catalog documented in `docs/IAP2_SPECIFICATION.md` |
| Low latency & frame-drop resync policy | Documented in `ARCHITECTURE.md`: libmpv low latency configuration (`untimed=yes`, `cache=no`, `demuxer-max-bytes=128KiB`) and WASAPI event pump 100ms cushion with underrun self-healing |
| Wired CarPlay explanation | Documented in `ARCHITECTURE.md`: Intel xDCI hardware controller feasibility vs OEM BIOS Host-only lock, and roadmap for ARM64 UDC / external bridge hardware |

### 5.3 Deprioritized / Deferred / Removed Items

| Item | Status & Notes |
| --- | --- |
| Multichannel audio output | Removed (3-lane shared WASAPI output retained) |
| System integration (Tray icon, autostart) | Removed (single-instance mutex retained) |
| Secondary display selector | Removed |
| Picture-in-Picture (PIP) / Mini-window mode | Deferred |
| Internationalization (i18n) | Deferred |
| CI automation pipeline | Deferred |
| Mock iPhone accessory simulator | Deferred |
| Extended session planes / CarPlay Ultra (VDC) | Deferred |
| Wired CarPlay direct on Windows x86 | Not viable due to Intel xDCI BIOS/EC locking controller to Host-only mode; deferred to ARM64 UDC or external hardware bridge |

### 5.4 Audio Ducking Real Implementation Details

Ducking is implemented in `Core/Media/WasapiAudioPipeline.cpp` with two independent trigger sources combined with boolean OR:

| Trigger Source | Asserted | Released |
| --- | --- | --- |
| `voiceDuckingActive_` (local voice ducking) | Asserted when voice lanes (`SiriTelephony` / `AltNavigation`) receive audible PCM chunks (`WasapiAudioPipeline.cpp:423-424`) | Released automatically after silence exceeds `kDuckingHangoverMs` |
| `rtspDuckingActive_` (phone-requested ducking) | Asserted on RTSP event `duckAudio` (`AirPlayRtspServer.cpp:1834`) -> `CarPlayCoreService::SetDuckingActive` (`:898`) -> `SetDuckingActive(true)` | Released on `unduckAudio` (`:1836` / `:915`), or after `kRtspDuckingSafetyMs` timeout by safety net |

`UpdateDuckingStateLocked` merges both into a single `ducking_active_`. `ApplyDuckingGainLocked` applies a hard step matching `DUCK_GAIN` / `UNDUCK_GAIN`. Target ratio is hot-updated from settings HUD via `SetTargetDuckingRatio`. Tests in `Tests/MediaTests.cpp:185,522-585`.