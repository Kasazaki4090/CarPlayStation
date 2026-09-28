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
