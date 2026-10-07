# Decisions

This file records important architectural, engineering, and project workflow decisions.

Decisions should document:

* What was decided.
* Why it was decided.
* What alternatives were considered.
* What impact the decision has on the project.

Do not use this file as a TODO list or bug tracker.

---

## Decision Format

Each decision should use the following structure:

```markdown
## DEC-XXX — Decision Title

**Status:** `Accepted`

**Date:** YYYY-MM-DD

### Context

Describe the problem, requirement, or situation that led to the decision.

### Decision

Describe the decision clearly and concretely.

### Alternatives

List relevant alternatives that were considered.

### Reason

Explain why this decision was selected.

### Consequences

Describe the expected effects, limitations, and trade-offs.

### Related

- Related documents
- Related TODO items
- Related bugs
- Related commits
```

### Status

Use only the following statuses:

* `Proposed` — decision is under discussion
* `Accepted` — currently adopted
* `Superseded` — replaced by another decision
* `Rejected` — considered but not adopted

Decisions should not normally be deleted. If a decision becomes invalid, mark it as `Superseded` or `Rejected` and reference the newer decision when applicable.

---

## DEC-001 — AI Project Overview Before Implementation

**Status:** `Accepted`

**Date:** 2026-09-28

### Context

CarPlayStation involves multiple protocol layers, transport mechanisms, platform APIs, and reference implementations.

At the beginning of the project, the exact implementation boundaries and required components are not yet fully known.

Writing a large implementation TODO list manually at this stage would risk:

* missing important components;
* creating tasks based on incorrect assumptions;
* duplicating functionality already present in reference projects;
* choosing incorrect architectural boundaries;
* prematurely fixing implementation details before the protocol behavior is understood.

### Decision

Before starting substantial implementation work, an AI agent should first perform a project-wide overview.

The agent should inspect, at minimum:

```text
PROJECT.md
README.md
ARCHITECTURE.md
DECISIONS.md
BUGS.md
TODO.md
Core/
UI/
Lib/
Reference/
```

The agent should then:

1. Understand the project's goals and constraints.
2. Understand the current architecture.
3. Inspect the available reference implementations.
4. Identify relevant protocols, transports, APIs, and implementation patterns.
5. Identify missing or uncertain components.
6. Separate confirmed facts from assumptions and hypotheses.
7. Identify architectural inconsistencies or missing abstractions.
8. Build or update an appropriate task tree.
9. Add research tasks where implementation details are not yet sufficiently understood.
10. Only then begin implementation of concrete tasks.

The generated task tree should reflect the actual state of the project rather than a predefined theoretical implementation plan.

### Reference Project Analysis

Projects under `Reference/` are research material.

An AI agent should analyze them for:

* protocol behavior;
* packet/data flow;
* state machines;
* transport handling;
* authentication behavior;
* media handling;
* input handling;
* relevant platform interactions;
* useful implementation patterns;
* known limitations and workarounds.

Reference implementations must not automatically become runtime dependencies.

### Task Tree

The task tree should be hierarchical where appropriate.

For example:

```text
CarPlay Receiver
├── Authentication
│   ├── MFi authentication
│   └── Pairing
│
├── Transport
│   ├── Wired
│   │   ├── USB
│   │   └── NCM
│   └── Wireless
│       ├── Wi-Fi
│       └── IP transport
│
├── Protocol
│   ├── iAP2
│   └── CarPlay session
│
├── Media
│   ├── Video
│   ├── Audio
│   └── Microphone
│
└── Input
    ├── Touch
    └── Keyboard / Mouse
```

This example is only a structural example.

The actual task tree must be generated from project requirements and reference analysis.

### Unknowns

When an implementation detail is uncertain, do not silently turn the assumption into an implementation task.

Instead, create a research task.

For example:

```text
[Research] Determine wireless CarPlay discovery and connection sequence.
```

After the behavior is sufficiently understood, convert the research task into implementation tasks.

### Reason

This project is being developed with AI-assisted engineering.

A complete project overview gives the AI enough context to reason about the entire system instead of optimizing for the currently visible file or task.

Reference implementations are especially important because the project involves protocols and behavior that cannot be reliably reconstructed from high-level requirements alone.

Separating research from implementation also prevents speculative assumptions from becoming architecture.

### Consequences

Positive:

* AI agents can understand the project before modifying code.
* TODO.md remains based on actual project knowledge.
* Important missing components are less likely to be overlooked.
* Reference implementations can be systematically analyzed.
* Unknown protocol behavior is explicitly tracked.
* Architectural mistakes can be identified before substantial implementation work.

Trade-offs:

* Initial implementation takes longer because research comes first.
* TODO.md may change substantially after reference analysis.
* Some tasks cannot be defined precisely until protocol behavior is understood.

### Related

* `PROJECT.md`
* `ARCHITECTURE.md`
* `TODO.md`
* `BUGS.md`
* `Reference/`


## DEC-002 — UI Isolation and Application Layer

**Status:** `Accepted`

**Date:** 2026-09-28

### Context

CarPlayStation may use different UI technologies over its lifetime.

The initial implementation uses Win32, while future versions may use other UI frameworks such as Unreal Engine.

Allowing UI code to directly access protocol, transport, media, or platform internals would tightly couple the UI to the implementation and make future UI changes difficult.

It would also allow UI code to gradually become responsible for application logic, resulting in tightly coupled and difficult-to-maintain code.

### Decision

The UI is an independent presentation layer.

UI code must communicate with CarPlayStation through an application layer.

The UI must not directly access lower-level implementation layers.

The dependency direction is:

```text
UI
 ↓
Application
 ↓
Core
 ↓
Platform / Libraries
```

The application layer is responsible for exposing operations and state required by the UI.

Core remains responsible for CarPlay functionality and protocol implementation.

UI is responsible for:

* Windows and UI lifecycle;
* rendering and presentation;
* user interaction;
* displaying application state;
* translating UI events into application requests.

UI is not responsible for:

* CarPlay protocol implementation;
* transport management;
* authentication;
* media decoding;
* audio device implementation;
* USB/NCM implementation;
* direct access to third-party libraries.

### Rules

1. UI must not directly access Core internals.
2. UI must not directly access transport implementations.
3. UI must not directly access protocol implementations.
4. UI must not directly access platform backends.
5. UI must not directly call third-party libraries.
6. UI communicates through application-level interfaces.
7. Application code must not depend on a specific UI framework.
8. Core must not depend on UI code.
9. Replacing Win32 UI with another UI framework should not require rewriting CarPlay protocol code.

### Example

Valid:

```text
Win32 UI
   ↓
Application API
   ↓
CarPlay Core
   ↓
Transport / Platform
```

Also valid:

```text
Unreal Engine UI
   ↓
Application API
   ↓
CarPlay Core
   ↓
Transport / Platform
```

Invalid:

```text
Win32 UI
   ↓
USB / NCM implementation
```

Invalid:

```text
UI
   ↓
FFmpeg
```

Invalid:

```text
UI
   ↓
iAP2 internals
```

### Reason

The UI is expected to change independently from the underlying CarPlay implementation.

A strict application boundary prevents UI code from becoming a second implementation of the Core and keeps the project maintainable as the UI technology evolves.

### Consequences

Positive:

* UI frameworks can be replaced independently.
* Core remains testable without a UI.
* UI code remains focused on presentation and interaction.
* Protocol and transport implementation details remain isolated.
* Third-party dependencies are prevented from spreading into UI code.

Trade-offs:

* Additional application interfaces must be designed.
* Simple UI operations may require an extra layer of indirection.
* Application state must be explicitly exposed to the UI.

### Related

* `ARCHITECTURE.md`
* `PROJECT.md`
* `TODO.md`


## DEC-003 — Wireless-First Priority, MFi Authentication Strategy, and Input Mapping

**Status:** `Accepted`

**Date:** 2026-09-28

### Context

1. **Wired vs. Wireless Viability on PC:**
   - On typical x86/Windows PC hardware, the Intel xDCI (USB Device Controller Interface) is heavily constrained by BIOS/EC and firmware implementations. Windows lacks general native APIs/drivers to switch the host port into a USB Device / Gadget role (iAP2 + NCM composite device).
   - In contrast, Wireless CarPlay uses standard PC Wi-Fi (SoftAP/Hotspot) and Bluetooth (RFCOMM/SDP), which can be implemented natively on Windows 10/11.

2. **MFi Authentication Interface:**
   - iAP2 authentication requires communicating with an Apple MFi authentication coprocessor (or an authorized signing service).
   - On Windows, this needs flexible backend options:
     - Local hardware: USB-to-I2C bridge chips (such as WCH CH341, CP2112, FT232H, etc.) interfacing directly with an MFi chip over I2C.
     - Network daemon / proxy: A configurable TCP/HTTP socket endpoint (e.g. `MfiDaemonEndpoint`, configurable host:port, defaulting to `127.0.0.1:9000`) providing remote signing / MFi emulation or bridge capabilities.

3. **Input Modality Clarification:**
   - CarPlay native protocol does not define direct PC raw mouse pointer / QWERTY keyboard streams.
   - Mouse interaction can be directly translated to standard CarPlay Touch / Multi-touch events:
     - Left-click press / release → Touch Down / Touch Up (Tap)
     - Left-click hold & move → Touch Move (Drag / Pan / Swipe)
     - Right-click → CarPlay Back / Home button action
   - Keyboard interaction maps to vehicle rotary knob / directional navigation buttons:
     - Arrow keys (Up, Down, Left, Right) → Focus move / rotary knob steps
     - Enter → Select / Confirm (Knob push)
     - Backspace / Esc → Back / Return action
     - Media / Function keys → Siri, Play/Pause, Next/Prev, Call buttons

### Decision

1. **Wireless-First Target:**
   - Wireless CarPlay is designated as the primary, mandatory, and first implementation target (P0).
   - Native PC wired USB device-mode is deprioritized (blocked by xDCI / BIOS limitations). Any wired support will rely on external bridge hardware (dongle / MCU) and is deferred.

2. **Pluggable MFi Authentication Backends:**
   - Support hardware USB-to-I2C adapter interface (CH341 and equivalent bridge chips) for local MFi coprocessors.
   - Support network daemon interface over configurable endpoint / proxy address (defaulting to `127.0.0.1:9000`) for remote signing / daemon services.

3. **Mouse & Keyboard Input Mapping:**
   - Mouse input will be mapped to Touch events (left click = tap, drag = swipe, right click = back).
   - Keyboard input will be mapped to vehicle knob / directional and media navigation controls.

### Consequences

Positive:
- Removes the blocker of PC xDCI device mode from the primary development path.
- Clear development roadmap focused on wireless CarPlay (Bluetooth RFCOMM + Wi-Fi SoftAP + AirPlay).
- Pluggable MFi architecture enables testing with either physical CH341+MFi hardware or a configurable network proxy daemon (`127.0.0.1:9000`).
- Provides intuitive PC desktop control using standard mouse and keyboard without protocol violations.

### Related

* `PROJECT.md`
* `ARCHITECTURE.md`
* `TODO.md`


## DEC-004 — Configuration Layering: Core Gets Facts, Application Owns Preferences

**Status:** `Accepted`

**Date:** 2026-09-28

### Context

After the initial C++ skeleton and the "fully configurable" pass (recorded in `result.md`, record 2), user-tunable knobs had leaked into Core:

* `Core/Common/Config.h::VideoConfig` mixed UI concepts (`WindowDisplayMode`, resolution presets, codec `"Auto"` preference) with concrete stream parameters.
* `AudioConfig` mixed the negotiated output format (sample rate / channels / bit depth) with runtime controls (per-lane volumes, ducking ratio) and an advisory bitrate request.
* `ICarPlaySession::Start(config)` / `UpdateConfig(config)` accepted a monolithic config blob even though session media parameters are negotiated with the iPhone at runtime, not user-configured.

The low-level layer should only consume facts (negotiated protocol values and environment endpoints), while everything mutable by the user belongs to the Application/UI boundary.

### Decision

1. **Core configuration is split into narrow, fact-only structs** (`Core/Common/Config.h`):
   * `VideoStreamSpec` — concrete negotiated codec (`H264` / `HEVC`, no `"Auto"`), width, height, fps.
   * `AudioOutputFormat` — concrete sample rate, channels, bit depth (no volumes, no bitrate).
   * `DeviceIdentity` — device name / manufacturer / model advertised in the handshake.
   * `NetworkEndpoints` — MFi daemon endpoint, RTSP bind address and ports, AirPlay audio/video ports, SoftAP SSID/password/gateway, hardware-MFi flag. Consumed by Transport/Hardware only.
   * `CoreEnvironmentConfig` = `DeviceIdentity` + `NetworkEndpoints`. This is the only config Core accepts at init/update.

2. **User preferences live in the Application layer** (`Application/AppSettings.h`):
   * `VideoPreferences` — resolution preset (incl. `Auto`/`Custom`), codec preference (incl. `Auto`), fps, `WindowDisplayMode`.
   * `AudioPreferences` — preferred sample rate, advisory bitrate request, per-lane volumes, ducking ratio.
   * `DevicePreferences` — user-editable identity strings.

3. **Resolution happens at the boundary**: the Application layer translates preferences into concrete Core parameters (e.g. preset → width/height, codec preference + negotiation result → final codec) before calling into Core. Core never sees presets, `"Auto"` values, or window modes.

4. **Interface changes:**
   * `IVideoDecoder::Init / Reconfigure` take `VideoStreamSpec`.
   * `IAudioPipeline::Start / Reconfigure` take `AudioOutputFormat`; volume and ducking become explicit runtime controls (`SetLaneVolume`, new `SetDuckingRatio`).
   * `ICarPlaySession::Start()` takes no config; media parameters flow from negotiation. `UpdateConfig` removed.
   * `ICarPlayCoreApi::Initialize / UpdateEnvironment` take `CoreEnvironmentConfig`.
   * `IApplicationApi::StartCarPlay / ApplySettings` take `AppSettings`; `AppUiState.active_settings` is an `AppSettings` snapshot, and width/height fields report the actual negotiated resolution.

### Alternatives

* Keep one monolithic config struct everywhere — rejected: forces every Core subsystem to depend on knobs it does not use and lets UI concepts (window mode) reach the low level.
* Put preferences in a separate `Core/Common` header but still pass them through session/decoder interfaces — rejected: interface signatures would still leak mutability into Core; the resolution step must be explicit at the boundary.

### Reason

Negotiated media parameters are protocol facts that change with each handshake, and network endpoints are environment facts that can change while running. Both are legitimately "mutable" inside Core but for different reasons than user preferences. Presets, `"Auto"` modes, window display mode, volumes and ducking ratios have no place in protocol or codec code: they exist to be edited by the user at runtime.

### Consequences

* Core interfaces now accept only concrete facts; adding a new UI knob cannot force changes into Core as long as it resolves to an existing fact.
* The Application layer owns one explicit resolution step (preferences → concrete parameters), making negotiation and preference logic testable in isolation.
* Per-lane volume / ducking became first-class audio pipeline controls instead of fields in a config blob, matching how they are actually used at runtime.

### Related

* `ARCHITECTURE.md`
* `result.md` (record 3)
* `Core/Common/Config.h`, `Application/AppSettings.h`


## DEC-005 — MFi Authentication Layering: Endpoints are User Preferences, Capabilities belong to Hardware Layer

**Status:** `Accepted`

**Date:** 2026-09-29

### Context

During the configuration layering refinement (DEC-004), the placement of MFi authentication parameters was further analyzed:
* MFi authentication operates at the hardware and cryptographic coprocessor layer (`Core/Hardware`), which is an internal capability not directly operated by users during normal session operation.
* However, the choice of MFi backend (local physical USB-to-I2C chip vs remote/local TCP signing daemon) and the TCP daemon network endpoint address (host:port) are user-configurable preferences.
* Hardcoding or keeping daemon endpoints inside Core configuration structs would leak user-configuration policies into the HAL.

### Decision

1. **MFi Authentication Interface Stays in `Core/Hardware`**:
   - `Core/Hardware/IMfiSigner.h` provides pure virtual authentication capabilities (`GetCertificate`, `GenerateSignature`, `GetChipInfo`, `IsReady`).
   - Core and Protocol modules depend strictly on `IMfiSigner` abstractions and never hardcode daemon URLs, IP addresses, or bus addresses.

2. **MFi Backend Selection & Daemon Endpoint Belong to `Application` Layer**:
   - `Application/AppSettings.h :: MFiPreferences` holds `use_hardware_backend` (bool) and `daemon_endpoint` (`IpEndpoint`, default `127.0.0.1:9000`).
   - The Application layer is responsible for instantiating the appropriate `IMfiSigner` implementation based on user preferences and injecting it into Core.

3. **Core Environment Config Excludes MFi Endpoints**:
   - `CoreEnvironmentConfig` in `Core/Common/Config.h` contains strictly receiver environment facts (RTSP bind address/port, SoftAP credentials, device identity).

### Reason

MFi cryptographic signing is a low-level capability, but deciding where the signer lives (physical USB hardware vs network daemon) is a deployment/user setting. Separating capability (Core/Hardware) from endpoint configuration (Application) preserves total isolation and allows seamless switching between physical hardware and network proxies without altering protocol or HAL code.

### Consequences

* Core HAL remains completely agnostic to where the daemon is hosted.
* Application layer can hot-switch between hardware and network backends via `ApplySettings()`.
* Unit testing of upper layers (Protocol/Session) can inject mock signers without network or hardware dependencies.

### Related

* `HANDOFF_MFI_LAYERING.md`
* `DEC-003`, `DEC-004`
* `Application/AppSettings.h`, `Core/Hardware/IMfiSigner.h`, `Core/Common/Config.h`


## DEC-006 — Local File-Based MFi Identity Signer (Zero-Hardware & Zero-Network MFi v3 Authentication)

**Status:** `Accepted`

**Date:** 2026-09-29

### Context

1. **Hardware and Network Limitations:**
   - Physical USB-to-I2C coprocessors (CH341 + Apple MFi chip) require specialized hardware modules and driver installations.
   - Remote TCP signing daemons (`NetworkMfiClient`) require a secondary device or background daemon service (such as Raspberry Pi / CCPA `mfi_oracle`).
   - Many desktop environments benefit from a fully self-contained receiver running on a single PC without external hardware or network dependencies.

2. **Reference Discovery (`Reference/diplay`):**
   - Analysis of `Reference/diplay` (`LocalMfiAuthenticationClient.kt`) demonstrates that Apple MFi 3.0 (v3) authentication utilizes ECDSA over NIST P-256 (secp256r1) with a 32-byte challenge digest and standard X.509/PKCS#7 accessory certificates.
   - A local software signer loading PKCS#8 private keys (`identity.pk8` or raw/PEM format) and X.509/PKCS#7 certificate files (`certificate.p7b` or `.der`) can compute valid Apple MFi v3 ECDSA signatures directly.

### Decision

1. **Implement `FileMfiSigner` (or `LocalMfiSigner`) in `Core/Hardware`**:
   - Implements the pure virtual `IMfiSigner` contract.
   - Uses Windows native **CNG (Cryptography Next Generation / `bcrypt.dll`)** with `BCRYPT_ECDSA_P256_ALGORITHM` to perform raw ECDSA signing (`BCryptSignHash`), outputting raw 64-byte `(R || S)` signatures directly matching Apple MFi v3 on-wire expectations without introducing external crypto libraries.
   - Loads accessory certificate bytes (`FetchCertificate()`) directly from the configured certificate file (e.g. `.p7b` / `.der`).
   - Performs startup self-validation (signing a test challenge and verifying with the certificate public key) to fail fast on corrupted or mismatched keypairs.

2. **Application Layer Owns Key/Certificate Paths**:
   - `Application/AppSettings.h :: MFiPreferences` supports `MFiBackendType::LocalFile` and configurable file paths for `certificate_path` and `private_key_path`.
   - Allows users to drop in custom certificate and private key files.

### Reason

This allows CarPlayStation to run completely standalone on Windows PCs without physical MFi hardware or network daemons, while retaining 100% interface compatibility with the pluggable `IMfiSigner` architecture.

### Consequences

Positive:
- Zero external hardware required.
- Zero network dependencies for authentication.
- Seamless certificate replacement and testing.
- Uses Windows native CNG API (zero third-party crypto library dependencies).

Trade-offs:
- Requires valid Apple accessory certificates and corresponding P-256 private keys provided by the user.

### Related

- `DEC-003`, `DEC-004`, `DEC-005`
- `Core/Hardware/IMfiSigner.h`
- `Core/Hardware/FileMfiSigner.h`
- `Application/AppSettings.h`


## DEC-007 — Universal I2C Adapter Abstraction (`II2cAdapter`) and Multi-Chip Hardware MFi Bridge (`I2cMfiSigner`, CH341, CH347, FTDI)

**Status:** `Accepted`

**Date:** 2026-09-29

### Context

1. **Hardware Bridge Coupling:**
   - Previous designs (Step 2 of Partition 1) targeted a single specific USB-to-I2C bridge silicon: WCH CH341.
   - However, in practical embedded and custom automotive PC setups, USB-to-I2C bridge chips vary widely:
     - WCH CH341 (cost-effective, ubiquitous USB 1.1 Full Speed).
     - WCH CH347 (modern high-speed USB 2.0 480Mbps with hardware I2C controller engine).
     - FTDI FT232H / FT2232H / FT4232H (industry standard MPSSE engine).
     - Linux `/dev/i2c-N` native buses, Silicon Labs CP2112, etc.
   - The Apple MFi authentication coprocessor (CP 2.0C / 3.0) protocol itself is completely independent of the USB-I2C bridge. It communicates via standard I2C (7-bit address 0x10 or 0x11) using fixed register addresses (0x02, 0x05, 0x10, 0x11, 0x12, 0x20, 0x21, 0x30, 0x31..).
   - Tightly coupling MFi register orchestration directly to CH341 driver calls violates Single Responsibility and Adapter patterns, requiring rewriting MFi logic for every new bridge chip.

### Decision

1. **Introduce `II2cAdapter` as a Universal Hardware Bus Abstraction (`Core/Hardware/II2cAdapter.h`)**:
   - Pure abstract interface for physical I2C operations:
     - `Write(address7Bit, data)`
     - `Read(address7Bit, length)`
     - `Transaction(address7Bit, writeData, readLength)` (atomic write-then-restart-read)
     - `PulseReset(pinIndex, duration, driveHigh)` (hardware GPIO reset line control)
     - `SetSpeed(I2cClockSpeed)`
     - `ProbeDevice(address7Bit)`

2. **Implement Universal `I2cMfiSigner` (`Core/Hardware/I2cMfiSigner.h/.cpp`)**:
   - Implements `IMfiSigner` by composing any `std::shared_ptr<II2cAdapter>`.
   - Encapsulates pure Apple MFi coprocessor register protocol:
     - Certificate extraction: 0x30 length read followed by 128-byte window reads (0x31..0x3X).
     - Challenge signing: writes 0x20 length and 0x21 payload, writes 0x01 to 0x10 to start computation, polls 0x10 for 0x10 completion status, reads 0x11 length and 0x12 signature response.
     - Chip diagnostics: reads 0x02 protocol major.
     - Exponential backoff & retry mechanism to tolerate coprocessor wake-up NACKs.

3. **Pluggable Multi-Chip Adapter Implementations**:
   - `Ch341I2cAdapter`: Dynamically binds to Windows vendor `CH341DLL.DLL` (`CH341StreamI2C`, `CH341SetOutput`).
   - `Ch341Protocol.h`: Standalone CH341 I2C/UIO stream packet frame encoder (pure C++20, enables headless testing and cross-platform stream creation).
   - `Ch347I2cAdapter`: Dynamically binds to WCH high-speed `CH347DLL.DLL` (`CH347StreamI2C`, `CH347I2C_Init`).
   - `FtdiI2cAdapter`: Dynamically binds to FTDI `libMPSSE.dll` / `ftd2xx.dll` (MPSSE I2C engine).
   - `MockI2cAdapter`: Pure in-memory virtual MFi coprocessor simulation for 100% deterministic, offline automated unit tests.

4. **Application Layer Preferences (`Application/AppSettings.h`)**:
   - Added `HardwareI2cPreferences` to `MFiPreferences` supporting `HardwareI2cDriverType` (Auto, CH341, CH347, FTDI, Mock), configurable device index, slave address (0x10 / 0x11), and bus speed.

### Reason

Separating the generic I2C bus transport from MFi coprocessor register semantics makes the architecture pluggable, future-proof, and immediately portable across any USB-I2C silicon or operating system, while keeping testability strictly decoupled from physical hardware availability.

### Consequences

Positive:
- Users can plug in CH341, CH347, or FTDI adapters interchangeably.
- MFi coprocessor state machine logic is written and tested once.
- Deterministic, headless CI testing via `MockI2cAdapter`.
- Zero compiler link errors when vendor DLLs are not present (safe dynamic runtime binding).

### Related

- `DEC-003`, `DEC-004`, `DEC-005`
- `Core/Hardware/II2cAdapter.h`
- `Core/Hardware/I2cMfiSigner.h`
- `Core/Hardware/Ch341Protocol.h`
- `Core/Hardware/Ch341I2cAdapter.h`
- `Core/Hardware/Ch347I2cAdapter.h`
- `Core/Hardware/FtdiI2cAdapter.h`
- `Core/Hardware/MockI2cAdapter.h`
- `Application/AppSettings.h`

---

## DEC-008 — Media Streaming Pipeline Architecture: Extensible Video Decoders (MF + Dynamic FFmpeg Fallback), Direct3D 11 Shared Textures, and App-Internal 3-Lane WASAPI Shared Mode Audio Ducking

**Status:** `Partially Superseded` — the video decoder strategy in section 1 and the FFmpeg package sync in section 2 were replaced by `DEC-009`. The Direct3D 11 shared-texture presentation, display scaling rules, and audio design remain in force.

### Context

CarPlay video and audio pipelines on Windows face several platform-specific challenges and forward-compatibility considerations:

1. **Future Video Codecs (AV1, VP9, VVC)**:
   - While AirPlay 2 video is currently negotiated as H.264 or HEVC (H.265), modern Apple silicon (A17 Pro / M3 and newer) provides hardware AV1 decode. Future protocol extensions may introduce AV1 (AV01), VP9, or H.266 (VVC).
   - The video decoder interface must decouple codec identification from implementation, supporting extensible codec discovery and plugin registration.

2. **Windows HEVC Store Extension Barrier & FFmpeg Fallback**:
   - Clean retail/OEM installations of Windows 10/11 do not include the HEVC Media Foundation Transform (MFT) by default (it is a paid $0.99 extension in the Microsoft Store). Querying `CLSID_CMSH265DecoderMFT` directly fails with `MF_E_NOT_FOUND`.
   - To guarantee out-of-the-box HEVC playback without requiring users to purchase store packages, the receiver must support an FFmpeg hardware/software fallback (`d3d11va` / CPU).
   - Native 32-bit (x86) support is deprecated; the project strictly targets **x64** and **ARM64** (e.g. Snapdragon Windows devices).
   - FFmpeg libraries (`avcodec`, `avutil`, `swscale`) should have an automated build-time mechanism to acquire the latest stable release (e.g. via CMake / GitHub releases) with zero manual downloading, and be loaded dynamically/optionally so the core build never breaks if FFmpeg binaries are temporarily absent.

3. **Presentation Pipeline: Direct3D 11 Shared Textures vs. EVR / madVR**:
   - EVR (Enhanced Video Renderer) is legacy DirectShow/MF technology with heavy topology state machines and high latency. madVR is an external heavy filter with significant render queue delays.
   - Modern Windows video presentation should output decoded video frames directly into `ID3D11Texture2D` (NV12 format) and share them with the UI presentation window via `IDXGIResource::GetSharedHandle`, achieving sub-frame latency (0–1 frame) and zero CPU/GPU memory copy.

4. **Audio Management: WASAPI Shared Mode vs. Audio Hijacking / Exclusive Mode**:
   - High-fidelity audiophile software (e.g. foobar2000) often uses WASAPI Exclusive Mode or ASIO, which monopolizes sound cards and mutes all other system sounds (browsers, notifications, games). This is unacceptable for an in-car desktop environment.
   - Windows Audio Endpoint Volume (system mixer) treats the entire application as a single audio session and cannot natively arbitrate internal stream priority.
   - Therefore, CarPlayStation must use **WASAPI Shared Mode (`AUDCLNT_SHAREMODE_SHARED`)** to seamlessly coexist with all other desktop audio, while implementing an **application-internal 3-Lane Software Audio Mixer** (`MainMedia`, `AltNavigation`, `SiriTelephony`) with an automatic `AudioDuckingManager` providing smooth attack/release volume attenuation.

### Decision

1. **Extensible Codec & Decoder Architecture (`Core/Media/IVideoDecoder.h`)**:
   - Support `VideoCodec` enumeration with forward-compatible entries (`H264`, `HEVC`, `AV1`, `VP9`, `VVC`).
   - Implement `MfVideoDecoder` as primary hardware decoder for H.264/HEVC using Windows Media Foundation Transforms with `CODECAPI_AVLowLatencyMode = TRUE`.
   - Implement `FFmpegVideoDecoder` as fallback decoder using FFmpeg `d3d11va` hardware decoding (bypassing Microsoft Store HEVC licensing) and CPU fallback.
   - Implement dynamic runtime binding (`LoadLibrary` / delay loading) so core builds remain 100% self-contained even before external DLLs are placed.

2. **Automated FFmpeg Package Sync (x64 & ARM64)**:
   - Provide a CMake synchronization script (`cmake/FindOrDownloadFFmpeg.cmake`) and maintenance helper (`scripts/Update-FFmpeg.ps1`) targeting official latest stable releases (Gyan.dev / BtbN `FFmpeg-Builds`).
   - Automatically detect host target architecture (`x64` vs `winarm64`), download stripped shared release packages, cache version tags to prevent repeated network delays during incremental compilation, and configure CMake `POST_BUILD` to deploy DLLs alongside `CarPlayStation.exe`.

3. **Direct3D 11 Display Scaling (No Resize Renegotiation) & Fullscreen Resolution Jumps**:
   - Window Resizing Isolation: When the user resizes or drags window borders (regardless of window dimensions or aspect ratio), the underlying physical video stream resolution NEVER renegotiates with the iPhone. The presentation layer handles this purely at the Direct3D 11 rendering level based on user preference:
     - **Aspect Ratio with Black Bars (Default)**: Automatically preserves native aspect ratio and fills excess borders with black bars (Letterboxing / Pillarboxing).
     - **Stretch to Fill (Optional)**: Directly scales the quad/viewport across the client area without aspect preservation.
   - Strict Trigger for Physical Stream Resolution Jumps:
     - Physical video renegotiation with the iPhone is strictly restricted to:
       1. Explicit resolution selection by the user in Application settings (e.g. 720p, 1080p, 2K);
       2. Entering/exiting **Borderless Fullscreen / Fullscreen mode**, where the stream renegotiates to match the physical display's native resolution and refresh rate.
     - Ordinary window maximization or manual resizing never triggers stream renegotiation.

4. **Adaptive WASAPI Shared Mode Audio Engine (`GetMixFormat`)**:
   - Single system-level audio stream using **WASAPI Shared Mode** (`AUDCLNT_SHAREMODE_SHARED`), guaranteeing zero device monopolization.
   - Hardware Self-Adaptive Mode: Utilizes `IAudioClient::GetMixFormat` to automatically query and match the soundcard's native hardware mix format (supporting audiophile soundcards up to 24-bit / 32-bit Float, 44.1k / 48k / 88.2k / 96k / 176.4k / 192kHz), eliminating Windows system-level destructive resampling.
   - Internal Resampler & Formats: High-precision internal resampler converts incoming iPhone audio streams (typically 44.1kHz or 48kHz 16-bit) to the target DAC format.
   - Application-internal software mixer routing:
     - `MainMedia` (Music, Podcasts)
     - `AltNavigation` (Turn-by-turn prompts)
     - `SiriTelephony` (Voice calls, Siri)
   - `AudioDuckingManager` providing smooth linear/exponential gain ducking:
     - Ducking Attack: Attenuates `MainMedia` to 20%~25% within 100ms when navigation/telephony starts.
     - Ducking Release: Smoothly restores `MainMedia` to 100% over 300ms–500ms after navigation prompts finish.
     - Soft-limiter protection preventing clipping when all 3 lanes output concurrently.


### Consequences

Positive:
- No user friction: plays HEVC even without the $0.99 Microsoft Store HEVC package.
- Automated dependency management: developers and users do not need to manually hunt for FFmpeg DLLs.
- Sub-frame display latency with zero UI thread coupling.
- No audio monopolization: CarPlay audio plays harmoniously with other PC apps, with clean automotive ducking.
- Future-proof for AV1/VP9 if Apple expands CarPlay specifications.

### Related

- `DEC-002`, `DEC-003`, `DEC-004`
- `Core/Media/IVideoDecoder.h`
- `Core/Media/IAudioPipeline.h`
- `Core/Media/IMicrophoneUplink.h`
- `Core/Media/AirPlayVideoDemuxer.h`
- `Core/Media/AudioDuckingManager.h`
- `Core/Media/WasapiAudioPipeline.h`






---

## DEC-009 — Video Decoding Strategy: libmpv Only (Media Foundation and FFmpeg Decoders Removed)

**Status:** `Accepted`

**Date:** 2026-10-04

### Context

`DEC-008` specified a Media Foundation (MFT) decoder as the primary path with a dynamic FFmpeg fallback. Both were implemented (`Core/Media/MfVideoDecoder.*`, `Core/Media/FFmpegVideoDecoder.*`) and later replaced in practice by `Core/Media/MpvVideoPlayer.*`, which drives libmpv in-process.

Measured behaviour on the real device (iPhone 16 Pro, 1280x720@60 H.264):

1. **In-process MFT decode deadlocks under GPU contention.** The hardware H.264 MFT blocks on the renderer's shared texture; the software MFT cannot sustain 720p60.
2. **FFmpeg `d3d11va` had the same contention problem**, because it would have to share the decoder device with the presentation device.
3. **libmpv decodes on its own D3D11 VA device**, independent of the renderer, and its own threading absorbs the jitter. After switching to it, the session holds 60 fps for long stretches.
4. Maintaining three decoder backends costs build complexity (an FFmpeg download module, a maintenance script, ~386 MB of vendored binaries) and test surface, for code paths that are no longer used.

### Decision

1. Keep **libmpv (`MpvVideoPlayer`) as the only video decoder backend** behind `IVideoDecoder`.
2. Delete `Core/Media/MfVideoDecoder.{h,cpp}` and `Core/Media/FFmpegVideoDecoder.{h,cpp}`, the FFmpeg acquisition module `cmake/FindOrDownloadFFmpeg.cmake`, the maintenance script `scripts/Update-FFmpeg.ps1`, and the vendored `Lib/ffmpeg/` tree.
3. `Core` no longer links `mfplat` or `mfuuid`. It keeps `d3d11`/`dxgi` only for the shared-texture surface handed to the UI renderer.
4. `IVideoDecoder` stays the abstraction boundary. A future backend (native MFT, NVDEC, etc.) can be re-added behind it without touching the session or UI layers.

### Alternatives

- **Keep MF as primary, libmpv as fallback:** rejected — the MF path is the one that deadlocked, and keeping it means keeping the failure mode in the shipping binary.
- **Keep FFmpeg as a codec-coverage fallback (HEVC without the Store extension):** rejected for now — libmpv bundles its own decoders, so it already covers the codecs FFmpeg was added for. Re-adding a backend later is cheap because the interface is unchanged.
- **Vendor FFmpeg but leave it unreferenced:** rejected — dead third-party weight with no code path to exercise it.

### Reason

One decoder that demonstrably holds 720p60 on the target hardware is worth more than three that do not. Removing the unused backends deletes a third-party dependency, its download/sync machinery, and its test surface, without weakening the abstraction that would let another backend return.

### Consequences

Positive:
- No FFmpeg download step, no `Lib/ffmpeg` tree, no MF decoder code to maintain.
- Smaller build graph and shorter configure step; fewer runtime DLLs next to the executable.
- The single remaining path is the one that is actually tested against a real iPhone.

Negative / limitations:
- The HEVC-without-Store-extension argument no longer applies to a native path; codec coverage now follows whatever the vendored libmpv build supports.
- Redistribution now follows libmpv's GPL terms for the shipped `libmpv-2.dll` (see `Lib/Readme.md`).
- `Tests/MediaTests.cpp` now exercises graceful degradation of `MpvVideoPlayer` only (missing `libmpv-2.dll` must fail cleanly, not crash).

### Related

- `DEC-008` (superseded sections 1 and 2)
- `Core/Media/IVideoDecoder.h`
- `Core/Media/MpvVideoPlayer.h`
- `Lib/Readme.md` (mpv library record)
- `Tests/MediaTests.cpp`
- Commits `ab9d3f0` (MpvVideoPlayer), and the cleanup commit that removed MF/FFmpeg

---

## DEC-010 — CarPlay audio: RTSP phase-2 negotiation, fdk-aac, and a sealed microphone uplink

**Status:** `Accepted`

**Date:** 2026-10-04

### Context

`CarPlayStation` decoded and presented video only. Every audio stream in a real session was produced by the
streamer side of the reference implementation: the iPhone announces its audio formats, the receiver negotiates
per-stream audio parameters over RTSP, and the C process pushed decoded media audio to the speaker while feeding
the car microphone back to the phone. With no audio path at all, music, navigation guidance, Siri responses and
the telephony uplink were all missing, and a session that reached `STREAMING` was still unusable as a head unit.

`DEC-002` reserved `Core/Media` for audio decoding and the microphone uplink, and `DEC-009` had just removed the
FFmpeg backend, so no codec library capable of AAC was left in the tree. The authoritative behaviour was taken
from the vendored `Reference/carplayd` implementation (`rtsp/server.rs`, `rtsp/handlers.rs`, `session.rs`,
`audio/receiver.rs`, `audio/encoder.rs`, `stream.rs`) and cross-checked against the capture in
`docs/carplay-core-protocol-and-wire-analysis.md` plus the live `c_flow_chatty.log` SETUP bodies.

### Decision

1. **The RTSP server owns phase-2 audio negotiation.** `AirPlayRtspServer::HandleSetup` now parses the bplist
   body of a phase-2 SETUP and:
   - routes media and voice by the `audioType` string, not by `type` (`media` -> media stream, anything else ->
     voice stream);
   - binds one UDP data port per stream and answers it as `dataPort`;
   - binds a `controlPort` **only** for `type=102`, which is the only stream the iPhone sends RTCP SR for;
   - accepts `input` as a boolean or an integer, and treats the newest `type=100` SETUP as superseding any
     previous one;
   - keeps the legacy branch for `audio?streamType=100` SETUPs that carry a `Transport:` header, which are
     answered with `server_port`/`control_port` instead of a bplist body.
   The negotiated parameters are published on `NegotiatedMediaParams` (`audio_streams`, `mic_*`).
2. **The `/info` advert is the wireless receiver set.** `audioFormats` advertises the eight CCPA entries the C
   process advertises (PCM 16k mono, PCM 48k stereo, AAC-LC 44.1k/48k stereo, AAC-ELD 44.1k/48k stereo,
   AAC-ELD 16k/24k mono). `CarPlayStation` is a wireless receiver, so the wired-only rows are not offered.
3. **Audio lives in `Core/Media`.** Four new units:
   - `AirPlayAudioDemuxer` — RTP header, AEAD nonce placement, ChaCha20-Poly1305 open, length validation;
   - `AirPlayAudioReceiver` — one instance per stream/lane, decode into PCM, hand the PCM to the pipeline;
   - `AacCodecAdapter` — the only file that includes fdk-aac; pimpl with opaque handles on both sides;
   - `MicrophoneRtpSender` — WASAPI float capture to AAC-ELD frames to a sealed RTP packet on the phone's
     microphone port.
   `CarPlayCoreService` wires them up when the RTSP server reports `STREAMING` and tears them down before the
   session subsystems.
4. **fdk-aac is a pinned git submodule.** `Lib/fdk-aac` tracks `https://github.com/mstorsjo/fdk-aac` at the
   official tag `v2.0.3`, is added with `add_subdirectory(... EXCLUDE_FROM_ALL)`, built as a static library, and
   linked `PUBLIC` into `CarPlayCore`. The same tag was first vendored as a plain copy and verified byte-identical
   to upstream (modulo `core.autocrlf`) before being replaced by the submodule.
5. **The AES-CTR PCM path stays.** Format `0x8000` (PCM S16 BE 48k stereo, AES-CTR) is still demuxed and played,
   so a receiver that negotiates PCM instead of AAC keeps working.

### Alternatives

- **Media Foundation (`IMFTransform`)**: rejected. It is already the reason `Lib/` had to carry extra backends
  before `DEC-009`, it cannot be relied on to *encode* AAC-ELD at 16 kHz mono (the microphone uplink requirement),
   and feeding it a raw `AudioSpecificConfig` is awkward. It would also leave the encode and decode paths in two
   different libraries.
- **Re-add FFmpeg (`avcodec`)**: rejected. `DEC-009` removed it deliberately, and pulling back the whole
  framework for one codec family is far more third-party weight than AAC needs.
- **System codecs / "let the phone send PCM"**: rejected. The iPhone picks the format from what the receiver
  advertises; advertising only PCM loses the bitrate and latency behaviour of a real head unit, and there is no
  system encoder for the uplink.
- **Opus**: rejected. It is not part of the CCPA `audioFormats` set the iPhone will accept here.
- **Vendor fdk-aac as a plain directory** (like the old FFmpeg copy): considered and briefly implemented; the
  submodule was chosen instead so the pin is a real upstream tag, the diff against upstream is always checkable,
  and the 439-file tree does not enter our own history.

### Reason

AAC-LC (media) and AAC-ELD (Siri, telephony, alert tones, microphone uplink) have to come from one library, and
fdk-aac is what the reference implementation uses, so the parameter sets, the `AudioSpecificConfig` handling and
the frame sizes are already known good. It builds with CMake in the existing tree, needs no runtime DLL, and its
surface can be confined to one adapter file. Pinning a tag rather than a tarball keeps the license text, the
changelog and the upstream diff available to anyone auditing the third-party list.

### Consequences

Positive:
- Media audio, Siri/telephony audio and the microphone uplink all work end to end in the same process as video.
- The negotiation is verified against hardware-confirmed SETUP bodies, and the AEAD/ASC handling is covered by
  unit tests (`TestAudioFormatBitmask`, `TestAirPlayAudioDemuxerRoundTrip`, `TestAirPlayAudioDemuxerPlainPcm`,
  `TestAacEldCodecRoundTrip`, `TestMicrophoneRtpSenderAacEld`).
- `git submodule update --init --recursive` is the only extra step for a new clone.

Negative / limitations:
- fdk-aac is Fraunhofer's "FDK AAC Codec Library for Android" licence, which upstream metadata marks
  `BY_EXCEPTION_ONLY`; that has to stay in the third-party list before any distribution (see `Lib/Readme.md`).
- No hardware offload: audio is software decoded, which is irrelevant at these bitrates but rules out reuse of a
  hardware codec path.
- `Core/Media` already depends on `Core/Session/Pairing/Crypto.h`, so `Core/Session` must not include
  `Core/Media`. The audio codec enum is therefore duplicated in `AirPlayRtspTypes.h` and mapped with a
  `static_cast` in the service layer; a static_assert in `MediaTests` pins the two enumerants together.
- `AacAudioDecoder`/`AacAudioEncoder` hand out a reference into their own state for the `AudioSpecificConfig`;
  callers must copy it before `Close()` (a dangling reference silently produced an empty ASC in the first version
  of the round-trip test).
- Changing a widely included header such as `AirPlayRtspTypes.h` needs a full rebuild: stale object files with a
  different `NegotiatedMediaParams` layout crashed `SessionTests` and `ServiceTests` in destructors far from the
  change.

### Related

- `DEC-002` (layering, `Core/Media` owns audio), `DEC-008`, `DEC-009` (why no other codec backend is present)
- `Core/Session/AirPlayRtspServer.cpp`, `Core/Session/AirPlayRtspTypes.h`
- `Core/Media/AirPlayAudioDemuxer.h`, `Core/Media/AirPlayAudioReceiver.h`, `Core/Media/AacCodecAdapter.h`,
  `Core/Media/MicrophoneRtpSender.h`, `Core/Service/CarPlayCoreService.cpp`
- `Lib/Readme.md` (fdk-aac library record), `.gitmodules`
- `TODO.md` section 8

---

## DEC-011 — MFi over HTTP: a Remote MFi backend and user-entered service endpoints

**Status:** `Accepted`

**Date:** 2026-10-07

### Context

`DEC-003` gave MFi authentication three backends: a physical USB-to-I2C coprocessor, a local certificate/key pair
(`DEC-006`), and a TCP signing daemon. On this machine the hardware path never worked: after vendoring the WCH and
FTDI driver DLLs into `Lib/`, fixing the combined-transaction reads (`7c73c88`) and adding a NACK-aware startup
self-probe (`822600b`, `BUG-012`), every tool in the chain — the app, a standalone `CH341_GetDeviceId`/`ReadI2C`
probe, and the vendor demo — reported an empty bus for the same board that the vendor demo had been expected to
see. The PCB is open source and already fabricated, so the board cannot be changed to satisfy the driver stack, and
the iPhone kept failing authentication with "check the accessory" regardless of which register sequence was tried.

The vendored `Reference/remote-mfi-for-xcertplay` project solves the same problem from the other side: it is a Go
service that owns the coprocessor (or a certificate) and exposes it as a small HTTP JSON API, already used by
`xcertplay`. Talking to that service gives a working signing path without any USB/I2C stack in this process, and it
also works for a service hosted on another machine on the LAN.

The existing endpoint plumbing was also inadequate for that: `MFiPreferences::daemon_endpoint` was editable only in
the `.ini`, and the setup wizard offered radio buttons with no way to type a host or port, so a remote service
required hand-editing a config file.

### Decision

1. **A fourth backend, `MFiBackendType::RemoteHttp`, implemented as `Core::Hardware::RemoteMfiHttpClient`.**
   It implements `IMfiSigner` against the Remote MFi HTTP API contract:
   - `GetCertificate()` -> `GET /mfi/certificate`, which returns `{"type":"mfi","protocolMajor":N,"certificate":"<base64>"}`;
   - `SignChallenge()` -> `POST /mfi/sign` with `{"challenge":"<base64>","requestId":"<uuid v4>"}`;
   - `ResetChip()` -> `POST /mfi/reset` with the body exactly `{}`;
   - `GetHealth()` -> `GET /healthz`, which is never authenticated.
   Errors are the service's flat `{"detail":"..."}` bodies, mapped by status code: `400`->`InvalidArgument`,
   `401`->`AuthenticationFailed`, `404`->`NotFound`, `503`->`DeviceUnavailable`, `500`->`HardwareError`, anything
   else ->`NetworkError`. The `detail` string is carried into the `Status` message.
2. **Raw Winsock HTTP/1.1, no TLS, no new third-party library.** The client speaks `HTTP/1.1` with
   `Connection: close`, reads until the peer closes (or `Content-Length` is satisfied), and rejects oversized
   responses at 2 MiB and decoded payloads above 65525 bytes, matching the service's own limits. `ws2_32` is already
   linked by the network daemon client, so this adds no dependency. `Options::use_tls` defaults to false and, if set,
   every call returns `NotSupported` rather than silently sending plaintext to a TLS endpoint.
3. **The endpoint is typed by the user, never hardcoded.** `Application::RemoteMfiPreferences{host,port,bearer_token}`
   is persisted in the `.ini` (`RemoteHost`, `RemotePort`, `RemoteToken`) and edited in the MFi setup wizard.
   The default port is the service's own default listen port, 8972. The wizard validates the host as a bare name or
   `host:port`, the port as `1..65535`, and rejects a host containing a URL scheme.
4. **The wizard offers one network option, not two.** The TCP signing daemon and the Remote MFi HTTP service are the
   same thing from the user's point of view — a signing service reached over the network — so the first-run wizard
   shows a single "网络 MFi 认证服务" radio with one shared address/port/token field set and a nested protocol
   sub-selector (HTTP JSON API by default, binary TCP daemon still selectable). The sub-selector only decides which
   `MFiBackendType` the wizard writes back (`RemoteHttp` or `NetworkDaemon`) and which settings struct the typed
   endpoint is persisted into; the enum, factory, `.ini` keys and tests are unchanged. Switching protocol refills an
   empty or default port field with that protocol's default (8972 / 9000) and hides the bearer-token field, which the
   binary protocol has no use for.
5. **`protocolMajor` is mapped onto the existing chip-identity fields.** `MFiChipInfo` has no protocol field, and
   `Core/Session/Pairing/AuthSetup.h` selects the v2 vs v3 digest from `chip_id` bits 16-23 or from substrings of
   `firmware_version`. So `protocolMajor:2` becomes `chip_id = 0x00020000` / `firmware_version = "remote-mfi 2.0"`,
   `3` becomes `0x00030000` / `"remote-mfi 3.0"`, and an unknown value leaves both at the neutral `"remote-mfi"`,
   which lets the certificate-driven heuristic decide. `FetchCertificate()` also sets `MFiChipInfo::present = true`,
   which the startup self-probe in `ApplicationService` gates its logging on.
6. **Certificates are cached, connect failures and `503` are retried.** One `GET /mfi/certificate` per session until
   `ResetChip()` or `InvalidateCache()` drops it. `RequestWithRetry` retries only connect failures and `503` (the
   service holds a chip gate for up to eight seconds and answers `503` while it does), `max_retries + 1` attempts
   with `retry_delay` between them; `400`/`401`/`404` are returned immediately. A fresh `requestId` is generated per
   sign, because the service caches signatures per id for sixty seconds and rejects a reused id with a different
   challenge.

### Alternatives

- **An HTTP mode inside `NetworkMfiClient`**: rejected. The daemon client speaks a binary framed protocol
  (`op | length BE | payload`) with its own framing and framing-level errors; the HTTP service has neither frames nor
  a persistent connection. Sharing one class would mean two transport state machines in one socket loop.
- **`WinHTTP` / `winhttp.lib`**: rejected. It would add a link dependency and a second HTTP stack for four GETs and
  two POSTs, and the response handling (bounded reads, flat error bodies) is small enough to keep explicit.
- **TLS via Schannel**: deferred, not rejected. The reference service listens plaintext by default and is expected on
  a LAN; adding Schannel now would be speculative work with no endpoint to test against. The `use_tls` option and its
  `NotSupported` result are the placeholder, so a future implementation cannot silently downgrade.
- **Fixing the I2C path instead**: attempted at length across `BUG-011`/`BUG-012` and the driver vendoring, and left
  parked in a stash. The hardware path stays in the tree and selectable; it is simply no longer the only route to a
  signed challenge.
- **Patching `xcertplay`'s own client instead of writing ours**: rejected. `xcertplay` is a reference submodule we do
  not own, and its client is Rust.

### Reason

The HTTP contract is small, already documented in the reference repository (`docs/01-api-contract.md`), and already
proven against a real iPhone by another project. Implementing it costs one file, no new dependency, and no hardware;
and because the endpoint is user-entered, the same build can sign against a service on this machine, on the LAN, or
on a future host. Keeping it a separate `IMfiSigner` implementation means the iAP2 `StartAuth` path is unchanged —
the backend choice stays a factory decision (`Application::MfiSignerFactory`) exactly as `DEC-003` arranged it.

### Consequences

Positive:
- A session can authenticate without any USB/I2C hardware in the loop, which unblocks every iAP2 `StartAuth` test on
  this machine.
- Host, port and token are configured in the GUI, so the daemon and remote backends are usable without editing
  `carplay_config.ini` by hand.
- The backend is covered by unit tests against an in-process HTTP stub server
  (`TestRemoteMfiHttpClientSuccess`, `TestRemoteMfiHttpClientErrors`, `TestRemoteMfiHttpClientHealthAndReset`).

Negative / limitations:
- Plaintext HTTP: the bearer token and the certificate cross the network unencrypted. Until TLS is implemented, the
  service should be on the same LAN as the receiver.
- The base64 and JSON handling is a hand-written scanner, not a parser. It handles the service's own output shapes
  (flat objects, no nested structures in the fields we read) and reports `ProtocolError` otherwise; it is not a
  general JSON implementation.
- `MFiChipInfo::chip_id` for this backend is synthetic. Anything that expects a real coprocessor serial number will
  have to be told that the remote backend cannot provide one.
- The I2C hardware path is still unresolved and remains stashed; choosing `HardwareI2C` on this machine still fails.

### Related

- `DEC-003` (MFi backend selection and the factory), `DEC-006` (the offline file signer), `DEC-007` (I2C hardware path)
- `Core/Hardware/RemoteMfiHttpClient.h`, `Core/Hardware/RemoteMfiHttpClient.cpp`, `Core/Hardware/IMfiSigner.h`
- `Application/AppSettings.h` (`MFiBackendType::RemoteHttp`, `RemoteMfiPreferences`), `Application/MfiSignerFactory.cpp`,
  `UI/Win32/SetupWizard.cpp` (`LoadMFiSettings`/`SaveMFiSettings` read and write the `.ini` keys)
- `Core/Session/Pairing/AuthSetup.h` (why `protocolMajor` maps to `chip_id`/`firmware_version`)
- `Tests/HardwareTests.cpp` (`MockHttpMfiServer` and the three remote-MFi tests)
- `Reference/remote-mfi-for-xcertplay/docs/01-api-contract.md`, `internal/httpapi/handlers.go`
- `Core/Hardware/README.md` (backend list), `.gitmodules`
