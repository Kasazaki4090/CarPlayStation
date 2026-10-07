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
- **mDNS / DNS-SD Advertiser (`MdnsAdvertiser`)**:
  - Self-contained native implementation (WinSock multicast on `224.0.0.251:5353`), no Bonjour dependency.
  - Publishes `_airplay._tcp` and answers Wireless CarPlay discovery on `_carplay-ctrl._tcp`.
  - Full inbound packet inspection: RFC 1035 name decoding with compression pointers, plus both
    halves of the DNS-SD lookup (a browse `PTR` answer yields the instance name, which is then
    resolved with a follow-up `SRV`/`A` query in `SendCtrlLookup()`).
  - `DialCarPlayControl()` opens the iPhone's ephemeral control port once discovered; the dialed
    target cache is cleared by `ClearDialedTargets()` at the start of every session so
    reconnection re-resolves the new port.
  - `AnnounceTo(targetIp)` sends a unicast announcement to the connected iPhone in addition to
    the periodic multicast broadcast.
  - `EnableSimulatedMode()` / `GetBroadcastCount()` / `GetLastBroadcastPacket()` are the offline
    test hooks used by the unit tests.
- **mDNS Packet Codec (`MdnsPacketBuilder`)**:
  - Pure byte-level DNS record encoding (header, question, `A`/`PTR`/`SRV`/`TXT` answers) and the
    matching decoder helpers. No sockets, so record layout is verified in-memory.
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
- **Mocking**: Use loopback sockets or in-memory ring buffers to implement `MockTransportChannel` for testing iAP2 packet framing. `MockMdnsAdvertiser` and `MockWifiSoftApManager` replace the network side entirely.
- **Test Focus**: Binary SDP record construction, RFCOMM disconnect/reconnect state transitions, SoftAP state monitoring, mDNS record encoding and browse/SRV answer parsing.
- **Firewall**: Wireless CarPlay needs inbound access on the hotspot interface. `scripts/Set-UpFirewall.ps1` adds the program-scoped allow rule plus an explicit UDP 5353 rule; the app itself never touches firewall configuration.
