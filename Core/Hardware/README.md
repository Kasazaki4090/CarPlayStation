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
- **Remote MFi HTTP Client (`RemoteMfiHttpClient`)**:
  - Implements `IMfiSigner` against the Remote MFi HTTP JSON API (DEC-011): `GET /mfi/certificate`, `POST /mfi/sign`, `POST /mfi/reset`, `GET /healthz`.
  - Raw Winsock HTTP/1.1 with `Connection: close`; no TLS (set `use_tls` and every call returns `NotSupported`), no third-party HTTP library.
  - Certificates are cached after the first fetch (`InvalidateCache()` drops it), connect failures and `503` are retried per `Options::max_retries`, and HTTP status codes map onto `Common::StatusCode`.
  - Host, port and optional bearer token come from `Application::MFiPreferences::remote_mfi` (`RemoteMfiPreferences`); nothing is hardcoded in Core.
- **Radio & Adapter Helpers (`RadioAdapterHelper`)**:
  - Windows Bluetooth Radio status inquiry and power state queries.
  - Wi-Fi adapter HostedNetwork / SoftAP capability detection and 5GHz band support checks.
- **Touchscreen Capability Probe (`TouchscreenCapabilities`)**:
  - Reads the host's pointer devices once at startup and reports the maximum number of
    simultaneous contacts a display-attached digitizer can produce.
  - CarPlay has no knob device, so the contact count in the touchscreen HID descriptor advertised
    in `/info` has to match the hardware; this probe is what decides between the one-contact and
    two-contact descriptors. Touchpads are deliberately excluded (their input arrives as
    `PT_TOUCHPAD`, which the UI pointer router ignores).
- **I2C Bus Correctness (`062320e`, `7c73c88`, `822600b` / BUG-012)**:
  - The coprocessor is woken before it is read, and bus-idle padding (`0xFF` runs) is rejected
    rather than returned as register data.
  - Register reads are one combined write-then-read I2C transaction instead of two separate
    transfers, which is what the CH341/CH347/FTDI engines actually require.
  - `II2cAdapter::ProbeDevice()` is NACK-aware: a zero-length write that the slave NACKs is no
    longer reported as a device present, so the startup bus sweep can distinguish "wrong address"
    from "nothing on the bus". A reset pulse is only logged when the driver confirmed one.

---

## 2. Boundaries & Isolation
- **Downward Dependencies Only**: Windows native hardware APIs (WinUSB / WinSock / Wlan API) or vendor driver libraries (e.g., CH341 DLL).
- **Strictly No Upward Dependencies**: Must NOT import `Protocol`, `Session`, `Media`, or `UI` logic.
- **Public Exported Interfaces**:
  - `IMfiSigner`: Abstract pure virtual interface providing `GetCertificate()`, `GenerateSignature(challenge, out_sig)`, and status queries.
  - `II2cAdapter`: `Read()`, `Write()`, `WriteRead()`, `ProbeDevice()`, `PulseReset()`.
  - `IRadioState`: Interface for querying Bluetooth / Wi-Fi hardware adapter readiness.
  - `DetectTouchscreenCapabilities()` / `DetectMaxTouchContacts()`: Free functions consumed by Layer 4/6 to size the touchscreen HID descriptor.

---

## 3. AI Partition & Mocking Guide
- **Mocking**: Implement `MockMfiSigner` to return pre-recorded test certificates and dummy signatures, enabling upper layers (Protocol/iAP2) to develop and test without physical MFi hardware or network daemons.
- **Unit Testing**: Create standalone tests validating CH341 I2C frame timing and socket packet round-trips against arbitrary test endpoints.
