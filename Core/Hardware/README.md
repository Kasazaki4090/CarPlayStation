# Layer 1: Hardware Abstraction Layer (HAL) & MFi Bridges

## 1. Responsibilities
This module is located at the lowest layer of Core (Layer 1). It directly interfaces with physical hardware, peripheral driver chips, and background service daemons to provide raw I/O channels and hardware abstractions for upper layers (`Transport` and `Protocol`).

### Core Subcomponents
- **MFi Hardware I2C Bridge (`MfiHardwareBridge`)**:
  - Supports USB-to-I2C driver interfaces for chips such as WCH CH341, Silicon Labs CP2112, FTDI FT232H, etc.
  - Handles I2C read/write transactions, device addressing (0x22/0x20), and bus timing with an onboard/external Apple MFi authentication coprocessor (CP3.0 / 2.0C).
- **MFi Network Daemon Client (`MfiNetworkClient`)**:
  - Configurable TCP/RPC client supporting arbitrary proxy endpoints (host:port or URI, e.g. configurable via `MfiDaemonEndpoint` with default `127.0.0.1:9000`).
  - Dispatches MFi challenges (Challenge / Digest) to a remote/local signing daemon service and retrieves certificates and cryptographic signatures.
- **Radio & Adapter Helpers (`RadioAdapterHelper`)**:
  - Windows Bluetooth Radio status inquiry and power state queries.
  - Wi-Fi adapter HostedNetwork / SoftAP capability detection and 5GHz band support checks.

---

## 2. Boundaries & Isolation
- **Downward Dependencies Only**: Windows native hardware APIs (WinUSB / WinSock / Wlan API) or vendor driver libraries (e.g., CH341 DLL).
- **Strictly No Upward Dependencies**: Must NOT import `Protocol`, `Session`, `Media`, or `UI` logic.
- **Public Exported Interfaces**:
  - `IMfiSigner`: Abstract pure virtual interface providing `GetCertificate()`, `GenerateSignature(challenge, out_sig)`, and status queries.
  - `IRadioState`: Interface for querying Bluetooth / Wi-Fi hardware adapter readiness.

---

## 3. AI Partition & Mocking Guide
- **Mocking**: Implement `MockMfiSigner` to return pre-recorded test certificates and dummy signatures, enabling upper layers (Protocol/iAP2) to develop and test without physical MFi hardware or network daemons.
- **Unit Testing**: Create standalone tests validating CH341 I2C frame timing and socket packet round-trips against arbitrary test endpoints.
