# Layer 1: Hardware Abstraction Layer (HAL) & MFi Bridges

## 1. Responsibilities
This module is located at the lowest layer of Core (Layer 1). It directly interfaces with physical hardware, peripheral driver chips, and background service daemons to provide raw I/O channels and hardware abstractions for upper layers (`Transport` and `Protocol`).

### Core Subcomponents
- **Universal I2C Bus Abstraction (`II2cAdapter`)**:
  - Pure virtual interface (`Core/Hardware/II2cAdapter.h`) abstracting physical I2C read/write/transaction and GPIO reset pulse operations.
  - Implementations:
    - `Ch341I2cAdapter`: Dynamically binds to `CH341DLL.DLL` (with `Ch341Protocol.h` stream frame encoder).
    - `Ch347I2cAdapter`: Dynamically binds to `CH347DLL.DLL` for high-speed 480Mbps hardware I2C engine.
    - `FtdiI2cAdapter`: Dynamically binds to `libMPSSE.dll` / `ftd2xx.dll` for FT232H / FT2232H MPSSE I2C.
    - `MockI2cAdapter`: Pure in-memory virtual MFi coprocessor simulation for 100% deterministic unit testing.
- **Universal Hardware MFi Signer (`I2cMfiSigner`)**:
  - Implements `IMfiSigner` by driving an Apple MFi authentication coprocessor (CP 2.0C / 3.0) over any `II2cAdapter`.
  - Encapsulates register interaction (0x02, 0x05, 0x10, 0x11, 0x12, 0x20, 0x21, 0x30, 0x31..), windowed certificate retrieval, challenge signing, and exponential retry recovery.
- **Local File MFi Signer (`FileMfiSigner`)**:
  - Pure offline software signer (DEC-006) loading external X.509/PKCS#7 certificate and PKCS#8 private key, computing ECDSA P-256 signatures via native Windows CNG (`bcrypt.dll`).
- **MFi Network Daemon Client (`NetworkMfiClient`)**:
  - Configurable TCP client connecting to a remote/local MFi signing daemon/proxy address (configured and injected by the Application layer via `Application::MFiPreferences::daemon_endpoint`; Core/Lib does not hardcode daemon addresses).
  - Dispatches MFi challenges (Challenge / Digest) to the signing daemon service and retrieves certificates and cryptographic signatures.
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
