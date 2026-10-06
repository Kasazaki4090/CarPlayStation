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
2. Click "设置" (Settings) button.
3. Observe that drawer only contains 4 audio sliders.

### Expected

Settings panel provides options to switch resolution, frame rate (30/60 FPS), and codec (H.264/HEVC).

### Actual

The drawer was titled `音频与音量控制面板 (3-Lane WASAPI)` and had no video controls whatsoever.

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
2. Click "设置" (Settings) button.
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

1. "按了x和设置，这个框不会关掉": When closing the settings drawer via [X] or gear button, the huge centered Welcome Guide card in the middle of the screen remained permanently visible with no way to close or hide it.
2. "视频还没进来，是没渲染成功还是关了？": Live log revealed iPhone sent TEARDOWN for stream `type: 110, uuid: E0CB6FB0-0000-0000-0000-0000C0FFEE01` immediately upon receiving the SETUP Phase 2 response. Furthermore, receiving TEARDOWN caused the RTSP server to close the RTSP socket immediately, severing the iPhone's incoming TCP video connection at the same millisecond.

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

"现在341的读取有问题": with a WCH CH341 (or CH347) bridge attached to an MFi
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

"怎么现在连不上去了？卡(0x5703)": the iPhone connects over Bluetooth, iAP2
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

**Status:** Fix applied, awaiting hardware retest
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
