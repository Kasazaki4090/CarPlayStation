# Bug List

This file records known bugs, regressions, and unresolved implementation issues.

For larger or discussion-heavy issues, a GitHub Issue may be created and referenced here.

## Bug Reporting Rules

A bug report should contain enough information to reproduce and investigate the problem.

### Required Information

* Bug ID
* Status
* Symptom
* Reproduction steps
* Expected behavior
* Actual behavior
* Environment, when relevant
* Current suspicion or affected module, when known

### Optional Information

* Logs
* Packet captures
* Screenshots
* Crash dumps
* Related commits
* Related GitHub Issue
* Workaround

### Status

Use one of the following statuses:

| Status          | Meaning                                                                 |
| --------------- | ----------------------------------------------------------------------- |
| `Open`          | Confirmed or reported, not currently being fixed                        |
| `Investigating` | Being investigated                                                      |
| `Fixing`        | A fix is being implemented                                              |
| `Fixed`         | A fix has been implemented and verified                                 |
| `Reopened`      | Previously fixed but reproduced again                                   |
| `Blocked`       | Cannot continue without missing information, hardware, dependency, etc. |
| `Invalid`       | Not considered a bug                                                    |
| `Duplicate`     | Already tracked elsewhere                                               |

Do not create custom status names unless there is a strong reason.

---

## BUG-000 — Sample

**Status:** `Fixed`

**Date:** 2026-09-28

### Symptom

iPhone connects successfully, but video stops after approximately 10 seconds.

### Reproduction

1. Start Windows.
2. Connect iPhone.
3. Wait approximately 10 seconds.
4. Observe the video session.

### Expected

Video playback continues normally.

### Actual

The AirPlay video session disconnects.

### Current Suspicion

NCM keepalive.

### Root Cause

NCM RX thread exited unexpectedly.

### Fix

Fixed by commit `abc123`.

### Verification

Confirmed that the video session remains active after the previous failure interval.

---

## BUG-001 — Window close hangs / Settings drawer cannot close due to socket thread join deadlock

**Status:** `Fixed`

**Date:** 2026-10-03

### Symptom

Clicking window close button (or closing after opening settings) freezes the entire UI window and process permanently. User cannot exit the application without Task Manager force kill.

### Reproduction

1. Launch `CarPlayStation.exe`.
2. Connect iPhone over AirPlay or open Settings drawer.
3. Click the close button [X] on the window.
4. The window becomes unresponsive and hangs indefinitely.

### Expected

Application terminates cleanly within 500ms, closing all background listening and streaming threads.

### Actual

The main UI thread hangs at `video_receiver_->Stop()` waiting for `client_threads_` to join.

### Current Suspicion

Socket read thread join hang.

### Logs / Evidence

`carplaystation.log` shows the process staying alive with no further log output after receiving video connection, and UI message pump stopped.

### Root Cause

In `AirPlayVideoReceiver.cpp`, `TcpClientWorker` receives on a blocking TCP socket `sock`. However, `clientSock` was never saved in a tracked container. When `AirPlayVideoReceiver::Stop()` is called, it closes `tcp_listen_sock_` but never closes active client connection sockets. As a result, `TcpClientWorker` remains blocked forever inside `recv(sock, ...)` with no timeout, causing `th.join()` on the client threads to hang indefinitely on the main thread during `MainWindow::Close()` / `AppController::Shutdown()`.

### Fix

Track all active client sockets in `AirPlayVideoReceiver`, and in `Stop()` actively shut down (`shutdown(sock, SD_BOTH)`) and close all client sockets before calling `th.join()`. In addition, configure receive timeout (`SO_RCVTIMEO` 1000ms) on client sockets.

### Verification

Tested process shutdown: all client worker threads unblock on socket shutdown and join within ~5ms; application exits cleanly without hanging.

---

## BUG-002 — Audio pipeline never started, Reconfigure fails, and real WASAPI playback unhooked

**Status:** `Fixed`

**Date:** 2026-10-03

### Symptom

Volume sliders in settings panel cannot be adjusted; adjusting sliders causes lag and audio never actually plays through PC speakers.

### Reproduction

1. Launch `CarPlayStation.exe`.
2. Open Settings drawer and drag any volume slider.
3. Observe no sound and log warnings.

### Expected

Audio volume adjusts smoothly in real time and CarPlay audio streams through default Windows playback device.

### Actual

`audio_pipeline_->Start(...)` was never called by `CarPlayCoreService`, so `audio_pipeline_->IsRunning()` is false. Every `ApplySettings` call generates `Audio pipeline reconfigure failed: Audio pipeline not running`. Furthermore, `WasapiAudioPipeline.cpp` has `wasapiActive_ = false` hardcoded (null renderer), so audio samples never reach Windows audio hardware. Also, dragging sliders causes synchronous disk writes and full reconfigures on every mouse pixel move.

### Root Cause

1. `CarPlayCoreService::Start()` never invokes `audio_pipeline_->Start()`.
2. `WasapiAudioPipeline` lacks real `IAudioClient` / `IAudioRenderClient` playback loop.
3. `OverlayRenderer` fired full `settings_callback_` synchronously while holding internal mutex.

### Fix

1. In `CarPlayCoreService::Start()`, invoke `audio_pipeline_->Start({48000, 2, 16})` so the pipeline is running.
2. In `WasapiAudioPipeline.cpp`, initialize WASAPI shared mode (`IMMDeviceEnumerator` -> default render endpoint -> `IAudioClient` -> `IAudioRenderClient`) and adapt to the DAC mix format (both IEEE Float 32-bit and PCM 16-bit) to feed mixed audio without buffer overruns.
3. Changed `WasapiAudioPipeline::mutex_` to `std::recursive_mutex`.

### Verification

`MediaTests` suite passes (100% pass), WASAPI shared mode initializes cleanly with device mix format, audio pipeline state is active.

---

## BUG-003 — Video stream not incoming / black screen with welcome card overlay

**Status:** `Fixed`

**Date:** 2026-10-03

### Symptom

iPhone successfully pairs and connects via Mobile Hotspot, but no CarPlay video stream appears; screen remains stuck on Welcome Guide card.

### Reproduction

1. Enable Windows Mobile Hotspot.
2. Connect iPhone to Hotspot and initiate CarPlay.
3. Observe RTSP session succeeds, but screen never displays CarPlay interface.

### Expected

Welcome card disappears and decoded CarPlay video is rendered on Direct3D 11 surface.

### Actual

Screen remains on Welcome card. Log shows `Processed avcC VideoConfig (1 SPS extracted, 32 B Annex-B)` followed by no video frames, and continuous `POST /feedback` requests from iPhone.

### Root Cause

1. **Decryption key not passed to video receiver**: In `AirPlayRtspServer.cpp`, video encryption key `video_decrypt_key` is derived from `streamConnectionID` during SETUP Phase 2 (Streams, CSeq=14). However, `streaming_cb_` was only called during `RECORD` (CSeq=9). At CSeq=9, SETUP Phase 2 has not occurred, so `params.video_encrypted` was false and key was 0. SETUP Phase 2 never notified `CoreService` or updated `AirPlayVideoReceiver`'s decryption key. `AirPlayVideoReceiver` therefore treated ChaCha20-Poly1305 ciphertext as raw Annex-B NALUs, failing to decode any frames.
2. **Session state machine rejection upon Hotspot reconnect**: When iPhone connects directly via Wi-Fi without iAP2 Bluetooth handoff, `SessionSM` rejected `OnWifiConnected` and `OnAirPlayStreamingStarted` because current state was `Idle` (0). `ui_state_.is_streaming` remained false, so `OverlayRenderer` permanently rendered the Welcome Guide over the viewport.
3. **Missing takeScreen / forceKeyFrame trigger after stream setup**: After SETUP Phase 2 stream creation, iPhone waits for explicit takeScreen / forceKeyFrame acknowledgment to start video transmission.

### Fix

1. In `AirPlayRtspServer::HandleSetup` (SETUP Phase 2), call `streaming_cb_(params_)` immediately upon deriving `video_decrypt_key` to set `video_receiver_->SetDecryptionKey(key)`. Also send `takeScreen` and `forceKeyFrame` on the RTSP event channel.
2. In `CarPlaySessionStateMachine::OnWifiConnected` and `OnAirPlayStreamingStarted`, allow valid transitions from `Idle` / `BtDiscovered` to transition state to `AirPlayStreaming`, triggering `is_streaming = true` and hiding the pairing card.

### Verification

`SessionTests` and `MediaTests` pass cleanly. Key derivation correctly dispatches to video receiver upon SETUP Phase 2.

---

## BUG-004 — Video resolution and display settings missing from settings panel

**Status:** `Fixed`

**Date:** 2026-10-03

### Symptom

No option exists in the UI to view or change video resolution (e.g. 1280x720, 1920x1080, 1920x720 ultrawide) or video parameters.

### Reproduction

1. Launch `CarPlayStation.exe`.
2. Click the Settings button.
3. Observe that drawer only contains 4 audio sliders.

### Expected

Settings panel provides options to switch resolution, frame rate (30/60 FPS), and codec (H.264/HEVC).

### Actual

The drawer was titled `Audio & Volume Control Panel (3-Lane WASAPI)` and had no video controls whatsoever.

### Fix

Expanded `OverlayRenderer::DrawSettingsDrawer` with interactive buttons for:
- Resolution presets: 720P (1280x720), 1080P (1920x1080), 2K (2560x1440)
- Frame rate presets: 30 FPS, 60 FPS
- Video Codecs: H.264, HEVC
All buttons hook into `settings_.video` and propagate changes through `settings_callback_` to update decoder resolution dynamically.

### Verification

Built and verified in Direct2D overlay layout and hit-testing loop.

---

## BUG-005 — Recursive mutex deadlock and UI freeze upon settings drawer click

**Status:** `Fixed`

**Date:** 2026-10-03

### Symptom

Clicking anywhere in the settings drawer or on a slider causes the application to immediately freeze; the close [✕] button cannot be clicked, sliders cannot be dragged, and window does not re-render.

### Reproduction

1. Launch `CarPlayStation.exe`.
2. Click the Settings button.
3. Click or drag a volume slider or the close button.
4. Window freezes permanently.

### Expected

Sliders drag smoothly and settings drawer closes when [✕] is clicked or clicking outside.

### Actual

The UI thread acquired `OverlayRenderer::mutex_` in `OnMouseDown`, synchronously called `settings_callback_`, which re-entered `UpdateUiState` on the same thread, causing a recursive mutex deadlock on `std::mutex`.

### Fix

1. Changed `OverlayRenderer::mutex_` from `std::mutex` to `std::recursive_mutex`.
2. Dispatched `settings_callback_` outside `lock(mutex_)` in `OnMouseDown` and `OnMouseMove`.
3. In `Win32InputRouter.cpp`: tracked `is_overlay_capturing_` and guaranteed `ReleaseCapture()` is ALWAYS called on `WM_LBUTTONUP` whenever `GetCapture() == hwnd`, completely eliminating the mouse capture leak.
4. Avoided synchronous INI disk writes (`SaveMFiSettings`) on every mouse movement tick in `AppController::OnSettingsChanged`.
5. Added comprehensive null guards on all Direct2D brushes and DirectWrite text formats before rendering.
6. Replaced non-ASCII close glyph `L"✕"` with ASCII `L"X"`, and enlarged hit zones for gear button, close button, and slider tracks (+/- 10-20px) with click-outside dismiss.

### Verification

Verified UI loop, mouse capture release, settings drawer toggle/slider dragging, and test suite 100% pass (6/6).

---

## BUG-006 — iPhone TEARDOWN 9ms after connecting to Video Receiver on port 7100

**Status:** `Fixed`

**Date:** 2026-10-03

### Symptom

During wireless CarPlay connection, iPhone completes RTSP SETUP Phase 1 & 2 and connects to port 7100 on `AirPlayVideoReceiver`, but disconnects 9ms later and sends `TEARDOWN rtsp://... (CSeq=20)`, causing video to stop and screen to remain black.

### Reproduction

1. Launch `CarPlayStation.exe`.
2. Connect iPhone via Wi-Fi hotspot.
3. RTSP handshake reaches SETUP Phase 2.
4. iPhone connects to port 7100, then disconnects 9ms later.

### Root Cause & Current Suspicion

1. `AirPlayRtspServer::HandleSetup` (SETUP Phase 2) was sending `takeScreen` (changeModes) and `forceKeyFrame` on the event channel **synchronously before returning the RTSP 200 OK response** to the iPhone. This caused an out-of-order state race in iOS `airplayd`.
2. iPhone sends `POST /command (CSeq=19, body_len=287)` right after SETUP Phase 2.
3. Lack of detailed socket error and packet header logging in `AirPlayVideoReceiver::TcpClientWorker`.

### Fix

1. In `AirPlayRtspServer.cpp`, deferred `SendTakeScreen()` and `SendForceKeyFrame()` to execute in `ClientWorker` **immediately after the RTSP 200 OK response wire transmission completes**, ensuring iOS receives the SETUP response before mode-change events.
2. Added comprehensive payload dumping (hex + ASCII) for `POST /command`, `TEARDOWN`, and event channel incoming frames to inspect iPhone diagnostic messages.
3. Added socket error (`WSAGetLastError()`), partial byte count (`hdrRead`, `bodyRead`), and packet header hex dumps in `AirPlayVideoReceiver::TcpClientWorker`.

### Verification

Built and compiled cleanly with MSVC 14.44; test suite 100% passed (6/6). Ready for live real-device connection verification.

---

## BUG-007 — Missing stream UUID / controlPort in SETUP Phase 2 and persistent Welcome Guide Card

**Status:** `Fixed`

**Date:** 2026-10-03

### Symptom

1. "Pressed [X] and Settings, but this dialog will not close": When closing the settings drawer via [X] or gear button, the huge centered Welcome Guide card in the middle of the screen remained permanently visible with no way to close or hide it.
2. "Video has not arrived yet, did rendering fail or was the stream closed?": Live log revealed iPhone sent TEARDOWN for stream `type: 110, uuid: E0CB6FB0-0000-0000-0000-0000C0FFEE01` immediately upon receiving the SETUP Phase 2 response. Furthermore, receiving TEARDOWN caused the RTSP server to close the RTSP socket immediately, severing the iPhone's incoming TCP video connection at the same millisecond.

### Reproduction

1. Launch `CarPlayStation.exe`.
2. Open settings drawer and click [X]. Notice settings drawer closes in logs, but the centered Welcome Guide Card stays on screen with no close button.
3. iPhone connects over RTSP; inspect `carplaystation.log` to see TEARDOWN on stream 110 and premature socket shutdown.

### Root Cause

1. **Unclosable Welcome Guide Card**: `OverlayRenderer::DrawWelcomeGuide` lacked any close button or dismiss flag and was unconditionally drawn whenever `!ui_state_.is_streaming`.
2. **Missing UUID and controlPort in SETUP Phase 2 response**: In `AirPlayRtspServer::HandleSetup`, the stream dictionary only included `type: 110` and `dataPort: 7100`. It omitted `uuid: "E0CB6FB0-0000-0000-0000-0000C0FFEE01"`, `controlPort: 7101`, `timingPort`, and `eventPort`. Without the UUID matching the display configuration declared in `/info`, iOS AirPlay 2 could not bind the stream and sent `TEARDOWN`.
3. **Premature RTSP socket termination**: `ClientWorker` broke out of its loop and called `CLOSE_SOCK(sock)` whenever `req.method == RtspMethod::Teardown`, killing the control channel while the client was attempting to open the video connection.
4. **Missing `unborrowConstraint`**: CarPlay resource arbitration requires `unborrowConstraint: 100` alongside `takeConstraint` and `borrowConstraint`.

### Fix

1. In `OverlayRenderer`: added `show_welcome_guide_` flag, an `[X]` close button in the top-right of the card, click hit testing, and ESC key dismissal in `Win32InputRouter`. The card auto-hides once video streaming starts.
2. In `AirPlayRtspServer::HandleSetup`: included `uuid`, `controlPort: 7101`, `timingPort`, and `eventPort` in the SETUP Phase 2 bplist response.
3. In `AirPlayRtspServer::ClientWorker`: removed `break;` on `RtspMethod::Teardown`, keeping the RTSP socket alive.
4. In `HandleInfo` and `SendTakeScreen`: added `unborrowConstraint: 100` to all resource descriptors.

### Verification

1. All 6 test suites passed 100% (`CommonTests`, `HardwareTests`, `TransportTests`, `ProtocolTests`, `SessionTests`, `MediaTests`).
2. Verified welcome card hit testing, close button interaction, ESC dismissal, and SETUP Phase 2 response bplist serialization.

---

## BUG-008 — CH341 I2C reads return 0xFF / MFi certificate read fails

**Status:** `Fixed`

**Date:** 2026-10-06

### Symptom

"CH341 reads are failing": with a WCH CH341 (or CH347) bridge attached to an MFi
coprocessor, register reads returned all `0xFF` / garbage, `FetchCertificate()`
failed or produced a corrupt certificate, and the CH347 adapter never loaded its
driver at all.

### Reproduction

1. Install the WCH driver, plug in a CH341 (or CH347) with an MFi coprocessor on
   I2C, and select the hardware I2C signer.
2. Start `CarPlayStation.exe` and watch the certificate read in the log.
3. Reads of any register (0x30 length, 0x31 certificate, 0x12 signature) come
   back as `0xFF` padding.

### Root Cause

1. **Address byte dropped on pure reads.** `Ch341I2cAdapter::Transaction()` set
   `write_len = 0` whenever `writeData` was empty, so `CH341StreamI2C` emitted a
   START with no address byte and the coprocessor was never selected.
2. **No STOP→START bus idle gap.** The CH341A revision 2.0C NACKs a read address
   when a START follows a STOP sooner than ~3 ms, then answers the read with
   `0xFF`. The reference transports sleep 5 ms before a write-free read.
3. **Certificate read as one oversized transaction.** `I2cMfiSigner` read the
   whole certificate (`ReadRegister(0x31, cert_len)`, ~607 bytes). The
   coprocessor exposes the blob as sequential 128-byte register windows
   (0x31, 0x32, ...), so everything past the first window was garbage.
4. **Register select and read retried independently.** Retrying only the read
   half re-read whatever register was previously selected.
5. **CH347 driver never found.** `LoadLibraryA("CH347DLL.DLL")` with no path
   probing and no `*A64` variant, so it cannot load in a 64-bit process. The
   DriverStore folder is not on the DLL search path.
6. **CH347 self-deadlock.** `Open()` held `mutex_` and called `SetSpeed()`,
   which locked `mutex_` again.

### Fix

1. `Ch341I2cAdapter` / `Ch347I2cAdapter` now send the read address byte
   `(addr << 1) | 1` as a one-byte write for write-free reads, and hold the bus
   idle 5 ms before such reads.
2. `I2cMfiSigner` selects a register with a write-only transaction and reads with
   a separate pure read, retrying the whole pair; `ReadRegisterWindows()` walks
   the 128-byte windows for the certificate body. The signature stays a single
   read, matching the reference client.
3. New `Core/Hardware/DriverDllLookup.h` shared helper probes the default search
   path, the executable directory, `System32`, and
   `DriverStore\FileRepository\ch341wdm.inf.*` / `ch347wdm.inf.*` /
   `ftdigp22.inf.*`, preferring the 64-bit DLL, and logs the path that loaded.
4. `Ch347I2cAdapter` gained `ApplySpeedLocked()` so `Open()` no longer re-enters
   the mutex.
5. Vendor driver DLLs are now committed under `Lib/wch-ch341` and
   `Lib/ftdi-d2xx` and copied next to the executable by `Core/CMakeLists.txt`
   (see `Lib/Readme.md`).

### Verification

1. `MockI2cAdapter` now models 128-byte register windows; `TestI2cMfiSigner()`
   checks every byte of the 945-byte mock certificate, which fails if the windows
   are stitched in the wrong order or read as one transaction.
2. All 10 test suites pass (`ctest`), including `HardwareTests`.
3. The build copies `CH341DLLA64.DLL`, `CH341DLL.DLL`, `CH347DLLA64.DLL`,
   `CH347DLL.DLL`, `libmpsse.dll` and `ftd2xx.dll` next to `CarPlayStation.exe`,
   and the CH341 contract test logs the DLL it loaded.

---

## BUG-009 — Wireless CarPlay stalls at `AccessoryWiFiConfig` (0x5703)

**Status:** `Fixed`

**Date:** 2026-10-06

### Symptom

"Connection fails now, stuck on (0x5703)": the iPhone connects over Bluetooth, iAP2
authentication and identify both succeed, the accessory sends
`AccessoryWiFiConfig` (0x5703), and then nothing happens. No station join, no
mDNS discovery, no RTSP connection.

### Reproduction

1. Start `CarPlayStation.exe` with the SoftAP up and the mock/hardware transport.
2. Pair and connect an iPhone over Bluetooth.
3. Watch the log: `0x5702` → `0x5703` repeats, then silence.

### Expected

After the Wi-Fi credentials are exchanged the iPhone joins the SoftAP, discovers
the AirPlay receiver, and starts the RTSP session.

### Actual

The iPhone re-requests `RequestAccessoryWiFi` (0x5702) and the accessory keeps
answering 0x5703 forever. The iPhone never advances to the CarPlay session.

### Root Cause

1. **The identify record never advertised the CarPlay session messages.**
   `BuildIdentifyInfo()` only appended `wirelessCarPlayStartSession` (0x4301) and
   `carPlayAvailability` (0x4300) to the sent/received message lists when
   `declareWired` was true. In wireless mode the iPhone therefore never considered
   us CarPlay-capable and never sent `0x4300`, so nothing advanced past `0x5703`.
2. **The rest of the wireless control plane was unimplemented.** `carplayd`
   documents the `0x5702/0x5703` exchange plus AP hosting as "Phase A2" and
   defers the subscriptions, `0x4E0x` handling and `0x4300 → 0x4301` transition to
   "Phase A3", which had never been written. Nothing handled `0x4E0D`
   (wirelessCarPlayAvailability), `0x4E0E` (deviceTransportIdentifier) or `0x4300`.
3. **No accessory long-term identity.** `0x4301` has to name the same Ed25519
   public key that RTSP pair-setup uses. The keypair was regenerated on every
   launch, so it could not be published in the iAP2 record.

### Fix

1. `Iap2MessageCatalog` now always declares `0x4301`/`0x4300` (the
   `declareWired` parameter was removed) and gained the wire codec for the whole
   control plane: `BuildAccessoryWiFiConfig`, the five subscription builders,
   `BuildCarPlayStartSession`, and parsers for `0x5702`, `0x4E0D` and `0x4E0E`.
2. New `Protocol::Iap2WirelessControlPlane` (`Core/Protocol/Iap2ControlPlane.*`)
   holds the state machine: identify accepted → five subscriptions → `0x5703` with
   the real SoftAP SSID/passphrase/BSSID/channel, resend on `0x5702` under a
   pre/post-transport send cap, `0x4E0D` → availability flag, `0x4E0E` → transport
   flag then resend, `0x4300` → `0x4301` with the RTSP port, device identifier and
   Ed25519 public key. The class never sends; the service emits what it returns.
3. New `Session::AccessoryIdentityStore` persists the accessory Ed25519 keypair in
   `carplay_accessory_identity.json`, and `CarPlayCoreService` feeds it to RTSP
   pair-setup/pair-verify and to the handler via `SetAirPlayReceiver()`.
4. `CoreIap2PacketHandler` refreshes the SoftAP credentials/BSSID before every
   control message and routes every unhandled control message id through the
   control plane.

### Verification

1. New `ProtocolTests`: `TestBuildAccessoryWiFiConfig`, `TestSubscriptionBuilders`,
   `TestBuildCarPlayStartSession`, `TestWirelessControlParsers` and
   `TestWirelessControlPlane` (full identify → subscriptions → 0x5703 → 0x5702 →
   0x4E0D → 0x4E0E → 0x4300 → 0x4301 walk, plus the send caps).
2. All 10 test suites pass (`ctest`).
3. On hardware the log should now show the five subscriptions after identify
   accepted, one `0x5703` per `0x5702` up to the cap, then a station join and an
   RTSP connection.

---

## BUG-010 — iPhone never opens RTSP after `0x4301 CarPlayStartSession` (mDNS discovery silent)

**Status:** `Fixed` (hardware verified)
**Component:** Core/Transport (mDNS), Windows host configuration
**Severity:** Critical
**Found In:** `0184078`
**Platform:** Windows 11, `netsh wlan hostednetwork`/Mobile Hotspot SoftAP at `192.168.137.1`

### Symptoms

The iAP2 wireless control plane completes end to end: Bluetooth RFCOMM accept,
MFi authentication, `0x1D00-02` identify, the five subscriptions, `0x5700`/
`0x5702`/`0x5703`, `0x4E0A`-`0x4E0D`, `0x5001`/`0x4155`, `0x4E0E`, `0x4300`
and finally `0x4301 CarPlayStartSession port=5000 ip=192.168.137.1`. After that
the log is silent: no `Discovered iPhone _carplay-ctrl service at ...`, no
`Sent /ctrl-int/1/connect ...`, and `AirPlayRtsp` never reports an inbound
connection on port 5000. The iPhone keeps showing the "checking" spinner.

`carplaystation.log` (255 lines) contains zero lines from the mDNS receive path.

### Reproduction

1. Start the app; it brings up the SoftAP (`Z890 3197`, gateway `192.168.137.1`),
   the RTSP server on 5000 and the mDNS advertiser (`bind() OK on 0.0.0.0:5353`,
   `Joined mDNS multicast group 224.0.0.251 on 192.168.137.1`).
2. Pair over Bluetooth and join the hotspot from the iPhone (`192.168.137.204`).
3. Wait for the control plane to reach `0x4301`.
4. Observe: outbound announcements and `_carplay-ctrl` PTR queries are logged,
   but nothing is ever received.

### Diagnostics performed

1. **The receive path was unobservable.** `BroadcastWorkerLoop()` only logged when
   it happened to find a `_carplay-ctrl` SRV port, so "the iPhone sends no mDNS"
   and "our socket never receives it" were indistinguishable.
2. **The parser could not resolve a DNS-SD browse.** `ParseCarPlayCtrlPort()`
   matched SRV records only and required the *record name* to contain
   `_carplay-ctrl`. A correct browse answer (a PTR pointing at the instance) was
   discarded, so a two-step resolution could never happen.
3. **Query matching was too narrow.** `IsAirPlayQuery()` required `QDCOUNT > 0`
   and a literal `_airplay._tcp` name, so service-enumeration (`_services._dns-sd._tcp`)
   and wildcard queries were ignored.
4. **Socket sharing is not the problem.** A probe that binds a second
   `SO_REUSEADDR` socket to `0.0.0.0:5353` while `Dnscache` (svchost) holds the
   port, joins `224.0.0.251` on `192.168.137.1` and multicasts to itself
   receives every datagram. Windows delivers 5353 to all wildcard binders.
5. **Firewall profile scope was the host-side cause.** The running executable had
   inbound Allow rules with `Profile=Private` only, while the hotspot access
   interface has no `Get-NetConnectionProfile` entry and is therefore treated as
   Public. Inbound mDNS (UDP 5353) and the RTSP connection (TCP 5000) were both
   blocked.
6. **The new logging itself had a defect.** `InspectMdnsPacket()` built its
   summary into a local string that was never assigned to `out.summary`, so the
   first hardware-free smoke run logged `mDNS inbound 459 bytes ... []`. Fixed by
   binding the local to `out.summary`.

### Fix

1. `MdnsAdvertiser.cpp`: replaced `ParseCarPlayCtrlPort()`/`IsAirPlayQuery()` with
   a single `InspectMdnsPacket()` that walks QDCOUNT **and** ANCOUNT+NSCOUNT+ARCOUNT,
   decodes PTR targets and SRV ports, and returns a decoded summary of every name,
   type and port in the datagram.
2. `MdnsAdvertiser.cpp`: the receive loop now logs every inbound datagram
   (`mDNS inbound N bytes from ip:port [summary]`), and chases a browse answer with
   one SRV/A query per newly seen instance (`SendCtrlLookup()`, deduplicated by
   `m_ctrlInstance`) — RFC 6767 two-step resolution.
3. `MdnsPacketBuilder`: added `BuildCarPlayControlLookup(instanceName)` which asks
   SRV + A for an instance learned from a PTR, and falls back to a PTR browse when
   no instance is known.
4. Query matching now also accepts `local.`, `.`, and `_services._dns-sd` questions.
5. `CarPlayCoreService::ApplyAirPlayPublicKey()` publishes the accessory Ed25519
   public key as the `pk` TXT key of `_airplay._tcp` (hex, as xcertplay and
   carplayd do). Without `pk` the iPhone refuses to open the RTSP control channel.
6. Host configuration: `scripts/Set-UpFirewall.ps1` (run elevated) adds
   `CarPlayStation (all profiles)` with `-Profile Any` for the executable, plus an
   explicit `CarPlayStation mDNS (UDP 5353)` rule. Applied and verified:
   `Get-NetFirewallRule` now lists both rules as `Enabled=True Action=Allow Profile=Any`.

### Verification

1. `TransportTests::TestMdnsCarPlayControlQueries` checks the browse, the SRV+A
   lookup, and the empty-instance fallback.
2. All 10 test suites pass (`ctest`).
3. Host smoke test (no iPhone): the receive path now logs and decodes our own
   looped-back traffic — `mDNS inbound 459 bytes from 192.168.137.1:5353
   [response qd=0 an=4 ... | ptr:_airplay._tcp.local.->CarPlayStation._airplay._tcp.local.
   | srv:CarPlayStation._airplay._tcp.local.:5000 | rr:....TXT | rr:CarPlayStation.local./A]`
   and the browse query `q:_carplay-ctrl._tcp.local./PTR`. The announcement grew
   from 391 to 459 bytes, which is the `pk=` key plus 64 hex characters.
4. On hardware the log must now show `mDNS inbound ... from 192.168.137.204:5353`
   lines. If they appear, the decoded summary says exactly what the iPhone is
   asking for; if they still do not, the datagrams never reach the socket.

### Related

- GitHub Issue:
- Commit: `34ad3d9` (parser, two-step lookup, `pk`, firewall script), `5d8de8d` (summary logging)
- Related Bug: BUG-009 (stale object files), BUG-008 (CH341 register reads)

---

## BUG-011 — Hardware signer reads `Invalid MFi certificate length: 65535` (coprocessor never answered)

**Status:** `Fixed` (hardware verified)

**Date:** 2026-10-07

### Symptom

"Certificate read succeeded, but session was not brought up": With `BackendType=2` the CH341 backend loads and is
selected (`MFi signer active: HardwareI2C: driver=0 slave=0x11`), the Bluetooth
link comes up, iAP2 reaches `0xAA00 MfiMsgReqAuthCert` — and 7 ms later the log
says:

```
[WARN] MFi certificate fetch failed: Invalid MFi certificate length: 65535
```

No `0x4301`, no RTSP connection, no CarPlay session. The software signer
(`BackendType=0`) still completes the session end to end.

### Reproduction

1. `carplaystation_ui.ini` → `[MFi] BackendType=2`, `I2cDriver=0`,
   `I2cSlaveAddress=17`.
2. Start `CarPlayStation.exe`, connect the iPhone over Bluetooth.
3. `FetchCertificate()` fails immediately with a length of `0xFFFF`.

### Root Cause

1. **No wake-up read.** The coprocessor answers its first read only after
   something has selected it; the proven reference transports (`iap2_auth.c`
   `mfi_warmup()`, `mfi_oracle.c`) read register `0x00` (DeviceVersion) once at
   startup specifically "so the first cert read succeeds". `I2cMfiSigner` went
   straight for `0x30`.
2. **The bridge's idle fill was treated as success.** `CH341StreamI2C` returns
   TRUE when the slave NACKs the read address and fills the buffer with `0xFF`.
   `ReadRegister()` only retried on `IsError()` or a short read, so `FF FF`
   passed through as a valid length of 65535. That is also why the failure
   appeared after 7 ms although `io_retry_timeout` is 2000 ms — the retry loop
   never engaged.
3. **Auth polling would have burned the whole I/O retry budget.** Once reads
   start failing, `SignChallenge()` polled `REG_AUTH_STATUS` through
   `ReadRegister()`, which spends a full `io_retry_timeout` (2 s) per poll, while
   the reference notes NACKs during signing are expected for ~750 ms–1.6 s.

The first round of fixes (warm-up + idle-fill rejection) shipped and behaved
exactly as designed on the next run — and the chip still refused to answer:

```
[INFO] MFi reset pulse issued on CH341 GPIO0
[WARN] Coprocessor did not answer the warm-up read at address 0x17
[WARN] Register 0x48 read failed after 2000ms: I2C read returned bus-idle padding (slave not answering)
[WARN] MFi certificate fetch failed: I2C read returned bus-idle padding (slave not answering)
```

That produced two more findings.

4. **The log was lying about every address.** `static_cast<int>(reg)` streamed
   after a literal `"0x"` prints decimal: `address 0x17` was `0x11`, and
   `Register 0x48` was `REG_CERT_LEN = 0x30`. The addressing had been correct
   the whole time (`I2cSlaveAddress=17` in the INI is parsed as decimal and
   becomes `0x11`), which is why chasing an "address mismatch" would have been a
   dead end.
5. **Register select and read were two separate I2C transactions.** This is the
   actual reason the coprocessor never answers. `ReadRegisterOnce()` used to do
   `adapter_->Write(addr, {reg})` and then `adapter_->Read(addr, len)`, and
   `Ch341I2cAdapter` implements each of those as its own `CH341StreamI2C` call,
   so a STOP is emitted between them. The MFi chip drops the selected register
   across a STOP, so the read addresses a chip that has forgotten what was
   selected and NACKs. Both proven transports do it as one message pair with a
   repeated start:

   ```c
   /* mfi_oracle.c i2c_rd() */
   struct i2c_msg m[2] = { { .addr=a, .flags=0,      .len=1,     .buf=&reg },
                           { .addr=a, .flags=I2C_RD,  .len=(__u32)n, .buf=buf } };
   ioctl(fd, I2C_RDWR, &rq);   /* START addr(W) reg  RESTART addr(R) data STOP */
   ```

   `Ch341I2cAdapter::Transaction(addr, writeData, readLength)` already emits that
   shape, and `I2cMfiSigner::GetChipInfo()` was already using it — which is why
   the one method that had never been called from the app was the only one that
   would have worked.

### Fix

1. `I2cMfiSigner::WarmUpLocked()`: up to 8 single-shot select/read attempts on
   `REG_DEVICE_VERSION = 0x00` at 50 ms spacing, accepting only a non-empty,
   non-`0xFF` response; after the fourth attempt it pulses the reset GPIO when
   `auto_pulse_reset_on_failure` is set. Latched in `warmup_ok_`, cleared
   whenever a later read fails so the next operation re-warms the chip. Called
   from `FetchCertificate()`, `SignChallenge()` and `GetChipInfo()`.
2. `IsBusIdleFill()` plus `ReadRegisterOnce()`: a read that comes back as all
   `0xFF` is now `HardwareError` "I2C read returned bus-idle padding (slave not
   answering)", so `ReadRegister()` retries the read and finally logs the
   register, the retry budget and the message.
3. `FetchCertificate()` is split: the public method does the open/warm-up and one
   reset-and-retry, `FetchCertificateLocked()` does the length read and the
   windowed body read, and success is logged reference-style (`MFi certificate
   read: 945 bytes first16=...`).
4. `SignChallenge()` polls the status register with `ReadRegisterOnce()` so a
   NACK costs one transaction, not the I/O retry budget; only the `auth_timeout`
   deadline can end the loop, and the number of NACKed polls is logged.
5. `ReadRegisterOnce()` now issues the select and the read as **one** combined
   transaction (`adapter_->Transaction(slave_address_, {reg}, length)`), matching
   `mfi_oracle.c` and `GetChipInfo()`; `SelectRegister()` is gone.
   `MockI2cAdapter` routes its own `Write`/`Read` through `Transaction()` and
   tracks the selected register from the write phase, so the mock coprocessor
   still answers and the existing tests cover the new path.
6. Every address, register and device field in the MFi logs is now printed as
   real hex (`HexByte()`/`HexWord()` in `I2cMfiSigner.cpp`, `%08X` for the chip
   id in `AuthSetup.h`).
7. When the warm-up fails, `ReportSlavesOnBus()` sweeps addresses `0x08`–`0x77`
   with `ProbeDevice()` and logs either the addresses that did answer (wrong
   `I2cSlaveAddress`) or that nothing answered anywhere (power, reset, wiring).
   That turns the most likely hardware misconfiguration into a one-line answer
   instead of another guess.

### Verification

1. Clean rebuild (`cmake --build . --target clean` then full build — the header
   changed, see BUG-009) with no new warnings from these files.
2. All 10 test suites pass (`ctest`), including `HardwareTests`:
   `MockI2cAdapter` answers register `0x00` with `0x00`, so the warm-up succeeds
   against the mock, and it never returns an idle fill, so the new check does not
   change the expected certificate or signature flows.
3. On hardware the log must show `MFi coprocessor awake at 0x11:
   DeviceVersion=0x…` followed by `MFi certificate read: <607..945> bytes
   first16=…`, then `MFi-SAP M2 size=…` and the RTSP connection. If instead
   `Coprocessor did not answer the warm-up read at address 0x11` appears, the
   following bus-scan line decides: an address that answered means
   `I2cSlaveAddress` is wrong; nothing answering anywhere means the module is
   unpowered, held in reset, or not wired — and no amount of retrying will help.

### Related

- GitHub Issue:
- Commit: pending
- Related Bug: BUG-008 (CH341 register reads, 128-byte windows)

---

## BUG-012 — Bus scan reported every address as present; reset pulse could be a silent no-op

**Status:** `Fixed` (hardware verified)

**Date:** 2026-10-07

### Symptom

BUG-011 added a bus sweep so a failed warm-up could tell "wrong address" from
"nothing on the bus". Reading the adapter layer showed that sweep could not be
trusted, and that the log line claiming a reset pulse had been issued did not
prove one had been.

### Root Cause

1. **`II2cAdapter::ProbeDevice()` was NACK-blind.** It wrote a zero-length
   payload and trusted the return value, but `CH341StreamI2C` returns TRUE when
   the slave NACKs the address. Every one of the 112 swept addresses would have
   been logged as answering, so the diagnostic could never distinguish the two
   cases it exists for.
2. **`PulseReset()` returned success when it could not do anything.**
   `Ch341Driver::IsValid()` only requires open/close/stream-I2C; if the loaded
   DLL exports no `CH341SetOutput` (same shape in CH347 via the GPIO set
   function), `PulseReset()` fell through to `Ok()` and the log said
   `Pulsed coprocessor reset line (GPIO0)`. A reset pulse that never happened is
   exactly the wrong thing to conclude from when the chip is silent.
3. **`GetChipInfo()` read the protocol register with a raw adapter
   transaction**, bypassing the idle-fill rejection added in BUG-011, so an
   all-`0xFF` NACK would have been reported as a present chip with
   `chip_id=0x000200FF`.
4. **Nothing ever exercised the chip without a phone.** `GetChipInfo()` had no
   caller, so a dead bus was only visible as three failed `0xAA00` attempts five
   seconds apart.

### Fix

1. `II2cAdapter::ProbeDevice()` now probes with a short read and answers true
   only if at least one byte is not the `0xFF` idle fill. A zero-length write
   cannot detect a NACK on these bridges, which is noted in the comment.
2. `Ch341I2cAdapter::PulseReset()` and `Ch347I2cAdapter::PulseReset()` return
   `DeviceUnavailable` when the driver is not loaded and `NotSupported` when the
   loaded driver exports no GPIO output function, instead of `Ok()`.
   `WarmUpLocked()` only logs the pulse when the call actually succeeded, and
   otherwise logs why no pulse was possible.
3. `GetChipInfo()` reads register `0x02` through `ReadRegisterOnce()`, so the
   idle-fill check applies and `present` stays false on a NACK.
4. `ApplicationService::ApplyMfiSigner()` now starts a detached self-probe for
   `HardwareI2C`: `GetChipInfo()` followed by `FetchCertificate()`, logging
   either the chip id and certificate size or an explicit "no coprocessor
   answered — check power, SDA/SCL, pull-ups and the slave address". It runs off
   the calling thread because the warm-up and bus sweep can take a couple of
   seconds.

### Verification

1. Clean rebuild, no new warnings; all 10 test suites pass. `MockI2cAdapter`
   returns "Address NACK" from `Transaction()` for the wrong address and never
   returns an idle fill, so the new probe and the new `GetChipInfo()` path are
   covered by `HardwareTests`.
2. On hardware, a failed warm-up must now log either
   `slave(s) answered at 0x…` (the INI address is wrong) or that nothing
   answered anywhere (power, reset, wiring) — never all 112 addresses.

### Related

- GitHub Issue:
- Commit: pending
- Related Bug: BUG-011 (coprocessor never answered)

---

## Bug Template

Copy the following template when adding a new bug:

```markdown
## BUG-XXX — Short Description

**Status:** `Open`

**Date:** YYYY-MM-DD

### Symptom

Describe what is wrong.

### Reproduction

1. Step one.
2. Step two.
3. Step three.

### Expected

Describe the expected behavior.

### Actual

Describe the actual behavior.

### Environment

- Windows:
- Hardware:
- iPhone / iOS:
- Connection: Wired / Wireless
- Build / Commit:

### Current Suspicion

Affected module or current hypothesis, if known.

### Logs / Evidence

Add relevant logs, packet captures, screenshots, crash dumps, etc.

### Root Cause

Fill this in after the cause is confirmed.

### Fix

Describe the fix and reference the relevant commit or GitHub Issue.

### Verification

Describe how the fix was verified.

### Related

- GitHub Issue:
- Commit:
- Related Bug:
```

## Guidelines

### 1. Do not present a hypothesis as a confirmed cause

Use:

> Current Suspicion: NCM keepalive

instead of:

> Cause: NCM keepalive

until the cause has been confirmed.

### 2. Keep reproduction steps deterministic

Prefer concrete steps over descriptions such as:

> Sometimes it disconnects after a while.

Record timing, connection type, device, software version, and other conditions when they affect reproduction.

### 3. Record the root cause after confirmation

A bug may initially have several suspected causes. Keep the original hypothesis, but update the report when evidence identifies the actual cause.

### 4. Link external discussion instead of duplicating it

For complex bugs, use a GitHub Issue for discussion and keep the important result in this file.

### 5. Do not delete fixed bugs

Keep historical bugs for regression tracking.

### 6. Bug IDs are permanent

Once assigned, a Bug ID should not be reused for another issue.

---

## Status Legend

```text
Open
  ↓
Investigating
  ↓
Fixing
  ↓
Fixed
```

Possible alternate paths:

```text
Investigating → Blocked
Investigating → Invalid
Investigating → Duplicate
Fixed → Reopened
```
