# Layer 3: Protocol & Framing Layer (iAP2)

## 1. Responsibilities
This module is located at Layer 3 of Core. It handles Apple iAP2 (iPod Accessory Protocol v2) link-layer framing, multi-session multiplexing, and structured message encoding/decoding.

### Core Subcomponents
- **iAP2 Link Packet Engine (`Iap2LinkEngine`)**:
  - Framing: Packet Start (`0xFF 0x5A`), Header, Payload, CheckByte checksum validation.
  - Link Control Frames: SYN (`0x80`), ACK (`0x40`), EAK (`0x20`), RST (`0x10`), SLP (`0x08`).
  - Sliding window sequence numbering, retransmission timers, and link synchronization handshake.
- **iAP2 Session Multiplexer (`Iap2SessionManager`)**:
  - Control Session (Session ID 1).
  - External Accessory / Data Sessions (Session ID 2+).
- **iAP2 Message Catalog & Serialization (`Iap2MessageCatalog`)**:
  - Authentication Messages: `AuthenticationStart`, `AuthenticationChallenge`, `AuthenticationResponse`, `AuthenticationFailed`, `AuthenticationSucceeded`.
  - Wireless Handshake Messages: `RequestWiFiCredentials`, `WiFiCredentials`, `StartWirelessCarPlay`.
  - Accessory Declarations: `IdentificationInformation`, `PowerStateUpdate`, `VehicleStatus`.
- **MFi Authentication Coordinator (`MfiAuthCoordinator`)**:
  - Connects to Layer 1 `IMfiSigner` interface to request signatures when receiving iPhone challenge packets.
- **Wireless Control Plane (`Iap2WirelessControlPlane`, `Iap2ControlPlane.h`)**:
  - The control-session conversation that runs after `IdentifyAccepted`: subscriptions, Wi-Fi
    credentials, and the CarPlay start-session exchange.
  - Message ids: `0x5702` / `0x5703` (Wi-Fi device info / configuration), `0x4300` / `0x4301`
    (CarPlay availability / start session), `0x4E0E`.
  - Send caps mirror the reference control loop: `kMaxPreTransportWiFiSends = 5` before the
    transport notification, `kMaxPostTransportWiFiSends = 2` after it.
  - Pure state machine: it never writes to the link itself, it returns the `OutgoingMessage`
    list the caller must emit, so the whole wireless handoff is testable without Bluetooth.
  - Carries `WiFiConfig` (SSID, passphrase, security type, channel, BSSID) and `SessionParams`
    (AirPlay port 5000, our IPv4 address, device identifier, accessory Ed25519 LTPK as
    lowercase hex, `sourceVersion` `320.17`). Channel and BSSID may be patched in later,
    because the WinRT hotspot API does not report them up front.

---

## 2. Boundaries & Isolation
- **Downward Dependencies Only**: `Layer 1 (IMfiSigner)` and `Layer 2 (ITransportChannel)`.
- **Public Exported Interfaces**:
  - `IIap2Engine`: `StartLink()`, `SendControlMessage(msg)`, `RegisterMessageCallback(id, fn)`.
  - `IIap2SessionObserver`: Listens for Link Established, Auth Succeeded, Wi-Fi Credentials Received events.
- **Strictly No Upward Dependencies**: Independent of media decoding or UI rendering.

---

## 3. AI Partition & Mocking Guide
- **Offline Packet Replay**: Feed recorded iAP2 hex/pcap streams directly into `Iap2LinkEngine` for deterministic framing verification.
- **Zero Hardware Coupling**: All `0xFF 0x5A` frame parsing and message serialization tests run purely in-memory.
- **Control Plane Replay**: Drive `Iap2WirelessControlPlane::OnControlMessage()` with recorded control-session bodies and assert on the returned `OutgoingMessage` list (including the Wi-Fi send caps).
