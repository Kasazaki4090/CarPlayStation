# Layer 4: Session & Orchestration Layer (CarPlay & AirPlay)

## 1. Responsibilities
This module is located at Layer 4 of Core. It coordinates the overall CarPlay session lifecycle state machine, manages the AirPlay RTSP control channel, and maintains the device pairing store.

### Core Subcomponents
- **CarPlay Master State Machine (`CarPlaySessionStateMachine`)**:
  - Lifecycle states: `Idle` $\rightarrow$ `BluetoothDiscovered` $\rightarrow$ `iAP2Authenticated` $\rightarrow$ `WifiHandoff` $\rightarrow$ `AirPlayConnecting` $\rightarrow$ `ActiveStreaming` $\rightarrow$ `Paused` $\rightarrow$ `Teardown` $\rightarrow$ `Reconnecting`.
  - Centralized error handling, timeout management, and automatic reconnect scheduler.
- **AirPlay RTSP Server & Parser (`AirPlayRtspServer`, `AirPlayRtspCodec`, `AirPlayRtspTypes`)**:
  - Handles RTSP commands (`OPTIONS`, `ANNOUNCE`, `SETUP`, `RECORD`, `SET_PARAMETER`, `ADD`, `REMOVE`, `TEARDOWN`, `POST /info`, `POST /command`).
  - bplist codec for the `/info` and `/command` bodies. `POST /command` must be answered with the
    42-byte empty bplist response (`{}`, 10 bytes wrapped in the HTTP header) or the iPhone stops
    sending commands.
  - Phase-1 capability advertisement: display `features`, `screen` (`type:110` for the built-in
    CarPlay screen, `dataPort:7100` for the video stream), `audioFormats` (CCPA, including the
    AAC-ELD / PCM options used by the audio path), and the microphone `inputFormats`.
  - Phase-2 stream negotiation (`AirPlayRtspTypes.h`): per-stream `StreamKeys` (output = iPhone
    to accessory, input = accessory to iPhone), `dataPort`, `ipadVendor`/`iphoneVendor`, and the
    `input=true` `type:100` stream that means the iPhone wants our microphone
    (`iphone_uplink_port`).
  - `takeScreen` / `forceKeyFrame` handling on `SET_PARAMETER` and `SETUP`.
  - Host appearance: `AirPlayRtspServer::SetNightMode(bool)` stores the value and writes the
    `setNightMode` command (`params.nightMode`) to every session with an event channel; sessions
    whose event channel is accepted later receive the stored value at that moment (`DEC-013`).
  - Routes negotiated audio/video stream endpoints to Layer 5 (`Media`).
- **Pairing & Accessory Crypto (`Pairing/`)**:
  - Self-contained, dependency-free implementation of the HomeKit-style pairing stack used by
    Wireless CarPlay: `PairSetup` (SRP-6a setup, M1-M6), `PairVerify` (Ed25519 fast re-verify),
    `AuthSetup` (MFi `iAP2` auth session driven by Layer 1 `IMfiSigner`), and `Tlv8` framing.
  - Primitives: `SrpServer` + `BigNum` (multi-precision arithmetic), `Curve25519`, `Ed25519`
    (vendored `ed25519/` C implementation with its own SHA-512), `Sha512`, `Crypto.h`
    (ChaCha20-Poly1305 with AAD, HKDF, control-channel and stream key derivation, `ControlChannel`
    framer). MSVC has no `__uint128_t`, so the Poly1305 128-bit arithmetic is implemented by hand.
  - `Bplist.h`: binary property list decoder, encoder, and a `BplistDumper` used to log what the
    iPhone actually sent.
- **Accessory Identity Store (`AccessoryIdentityStore`)**:
  - Owns the accessory's persistent pairing identity (pairing identifier, Ed25519 long-term
    public key and seed) that `PairSetup`/`PairVerify` sign with.
- **Pairing & Known Device Store (`PairingStore`, `IPairingStore`)**:
  - Persists paired iPhone identifiers, MAC addresses, encryption keys, and session parameters
    (`carplay_pairings.json` at the repository root).
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
- **Crypto Vectors**: `Pairing/` has no external dependencies, so Pair-Setup / Pair-Verify / SRP / Ed25519 / ChaCha20-Poly1305 run against published test vectors in `Tests/` with no network, no MFi chip, and no Apple entitlements.
