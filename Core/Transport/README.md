# Layer 2: Transport & Link Layer

## 1. Responsibilities
This module is located at Layer 2 of Core. It manages physical communication channels and transport links (Wireless Bluetooth / Wi-Fi SoftAP and external hardware bridge links), exposing reliable byte streams and packet transport to upper layers.

### Core Subcomponents
- **Bluetooth RFCOMM & SDP (`BtRfcommTransport`)**:
  - Windows Bluetooth stack interaction via WinSock (`AF_BTH`) / Windows Bluetooth APIs.
  - Registers and advertises iAP2 SDP service records (UUID: `00000000-deca-fade-deca-deafdecacafe`, etc.).
  - Manages RFCOMM channel listening, data I/O, MTU negotiation, and connection lifecycles.
- **Wi-Fi SoftAP & Handoff (`WifiSoftApManager`)**:
  - Windows Mobile Hotspot / SoftAP lifecycle management (SSID generation, dynamic WPA2-PSK credential negotiation).
  - Handles Wi-Fi handoff sequencing and IP address coordination.
- **mDNS / Bonjour Service Advertiser (`MdnsAdvertiser`)**:
  - Publishes `_airplay._tcp`, `_raop._tcp`, and `_carplay._tcp` mDNS service discovery records on the local subnet.
- **(Optional/Fallback) Hardware Bridge Transport (`BridgeTransport`)**:
  - Serial / CDC-NCM / TCP passthrough channel for external MCU / RP2350 wired bridge devices.

---

## 2. Boundaries & Isolation
- **Downward Dependencies Only**: `Layer 1 (Hardware)` and Windows networking APIs (WinSock / Native Wifi).
- **Public Exported Interfaces**:
  - `ITransportChannel`: Abstract stream interface (`AsyncSend(data)`, `SetDataCallback(fn)`, `Disconnect()`, `GetState()`).
  - `IWifiHandoffController`: `StartAccessPoint()`, `GetNetworkCredentials()`, `StopAccessPoint()`.
- **Strictly No Upward Dependencies**: Unaware of iAP2 message semantics; contains no CarPlay session state machine or media decoding logic.

---

## 3. AI Partition & Mocking Guide
- **Mocking**: Use loopback sockets or in-memory ring buffers to implement `MockTransportChannel` for testing iAP2 packet framing.
- **Test Focus**: Binary SDP record construction, RFCOMM disconnect/reconnect state transitions, SoftAP state monitoring.
