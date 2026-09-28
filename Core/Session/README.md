# Layer 4: Session & Orchestration Layer (CarPlay & AirPlay)

## 1. Responsibilities
This module is located at Layer 4 of Core. It coordinates the overall CarPlay session lifecycle state machine, manages the AirPlay RTSP control channel, and maintains the device pairing store.

### Core Subcomponents
- **CarPlay Master State Machine (`CarPlaySessionStateMachine`)**:
  - Lifecycle states: `Idle` $\rightarrow$ `BluetoothDiscovered` $\rightarrow$ `iAP2Authenticated` $\rightarrow$ `WifiHandoff` $\rightarrow$ `AirPlayConnecting` $\rightarrow$ `ActiveStreaming` $\rightarrow$ `Paused` $\rightarrow$ `Teardown` $\rightarrow$ `Reconnecting`.
  - Centralized error handling, timeout management, and automatic reconnect scheduler.
- **AirPlay RTSP Server & Parser (`AirPlayRtspServer`)**:
  - Handles RTSP commands (`OPTIONS`, `ANNOUNCE`, `SETUP`, `RECORD`, `SET_PARAMETER`, `TEARDOWN`).
  - Negotiates stream parameters (display resolution, FPS, audio formats, screen capabilities, FairPlay / Pair-Setup flow).
  - Routes negotiated audio/video stream endpoints to Layer 5 (`Media`).
- **Pairing & Known Device Store (`PairingStore`)**:
  - Persists paired iPhone identifiers, MAC addresses, encryption keys, and session parameters.
  - Supports silent, fast reconnection for previously paired devices.

---

## 2. Boundaries & Isolation
- **Downward Dependencies**: `Layer 2 (Transport)` and `Layer 3 (Protocol / iAP2)`.
- **Public Exported Interfaces**:
  - `ICarPlaySessionController`: `StartListening()`, `StopSession()`, `GetSessionState()`, `GetDeviceInfo()`.
  - `ISessionEventDispatcher`: Dispatches session state changes, disconnections, and capability updates to upper layers.
- **Inter-Layer Coordination**: Passes negotiated video/audio ports to Layer 5 (`Media`) and binds input stream channels to Layer 6 (`Input`).

---

## 3. AI Partition & Mocking Guide
- **State Machine Simulation**: Drive the state machine with simulated iAP2 events and RTSP messages to test error recovery (e.g. Wi-Fi drops, authentication timeouts, client aborts).
