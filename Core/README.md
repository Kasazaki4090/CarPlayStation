# CarPlayStation Core

`Core/` contains the complete CarPlay receiver protocol stack and engine implementation, positioned between the underlying platform/hardware interfaces and the upper Application / UI layers.

Drawing inspiration from the OSI 7-layer model and real-world CarPlay receiver protocol engineering, the Core is structured into 7 decoupled layers (Layer 1 to Layer 7). Each layer has strictly defined boundaries, dependency isolation rules, and standalone mocking/testing capabilities.

---

## Layered Architecture Topology

```text
┌────────────────────────────────────────────────────────────────────────┐
│                        UI (Win32 / Unreal Engine)                      │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ (DEC-002 UI Isolation Boundary)
┌───────────────────────────────────▼────────────────────────────────────┐
│                            Application Layer                           │
│              (State Manager / Commands / UI Presentation Data)         │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ (ICarPlayCoreApi.h)
╔═══════════════════════════════════▼════════════════════════════════════╗
║                          CarPlayStation Core                           ║
║                                                                        ║
║  ┌──────────────────────────────────────────────────────────────────┐  ║
║  │ Layer 7: Service (Core Facade, Lifecycle, Public C++ API & Bus)  │  ║
║  └──────────────────────────────────┬───────────────────────────────┘  ║
║                                     │                                  ║
║         ┌───────────────────────────┴───────────────────────────┐      ║
║         ▼                                                       ▼      ║
║  ┌─────────────────────────────┐         ┌──────────────────────────┐  ║
║  │ Layer 5: Media Streaming    │         │ Layer 6: Input           │  ║
║  │ - Video: libmpv D3D11       │         │ - Mouse -> Touch/Drag    │  ║
║  │ - Audio: 3-Lane WASAPI Duck │         │ - Keyboard -> Knob/Nav   │  ║
║  │ - Mic: WASAPI AAC/PCM       │         │ - Multi-Touch Injection  │  ║
║  └──────────────┬──────────────┘         └────────────┬─────────────┘  ║
║                 │                                     │                ║
║                 └───────────────────┬─────────────────┘                ║
║                                     ▼                                  ║
║  ┌──────────────────────────────────────────────────────────────────┐  ║
║  │ Layer 4: Session (CarPlay State Machine, AirPlay RTSP, Pairing)  │  ║
║  └──────────────────────────────────┬───────────────────────────────┘  ║
║                                     │                                  ║
║  ┌──────────────────────────────────▼───────────────────────────────┐  ║
║  │  Layer 3: Protocol (iAP2 Framing, Packet Engine, Control Plane)  │  ║
║  └──────────────────────────────────┬───────────────────────────────┘  ║
║                                     │                                  ║
║  ┌──────────────────────────────────▼───────────────────────────────┐  ║
║  │ Layer 2: Transport (Bluetooth RFCOMM/SDP, Wi-Fi SoftAP, mDNS)    │  ║
║  └──────────────────────────────────┬───────────────────────────────┘  ║
║                                     │                                  ║
║  ┌──────────────────────────────────▼───────────────────────────────┐  ║
║  │    Layer 1: Hardware & MFi (File / USB-I2C / Remote MFi HTTP)    │  ║
║  └──────────────────────────────────────────────────────────────────┘  ║
╚════════════════════════════════════════════════════════════════════════╝
                                    │
┌───────────────────────────────────▼────────────────────────────────────┐
│                    Platform / System APIs / Libraries                  │
│      (Windows SDK, Direct3D 11, WASAPI, WinSock, libmpv, fdk-aac)      │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Layer Breakdown & Directory Index

| Layer | Directory | Primary Responsibilities | Key Interfaces & Components |
| :--- | :--- | :--- | :--- |
| **Layer 1** | [`Hardware/`](file:///d:/CarPlayStation/Core/Hardware/README.md) | Hardware Abstraction & MFi Bridges | `IMfiSigner` (local file / CH341-CH347-FTDI USB-to-I2C coprocessor / Remote MFi HTTP client), `II2cAdapter`, `TouchscreenCapabilities` |
| **Layer 2** | [`Transport/`](file:///d:/CarPlayStation/Core/Transport/README.md) | Link Layer & Wireless Channels | `BtRfcommTransport`, `WifiSoftApManager`, `MdnsAdvertiser` + `MdnsPacketBuilder` |
| **Layer 3** | [`Protocol/`](file:///d:/CarPlayStation/Core/Protocol/README.md) | iAP2 Framing & Message Codecs | `Iap2LinkEngine` (`0xFF 0x5A`), `Iap2SessionManager`, Message Catalog, `Iap2WirelessControlPlane` |
| **Layer 4** | [`Session/`](file:///d:/CarPlayStation/Core/Session/README.md) | CarPlay & AirPlay Orchestration | `CarPlaySessionStateMachine`, `AirPlayRtspServer`, `PairingStore`, `Pairing/` (Pair-Setup, Pair-Verify, SRP, Ed25519, bplist) |
| **Layer 5** | [`Media/`](file:///d:/CarPlayStation/Core/Media/README.md) | Low-latency Audio/Video Pipelines | `IVideoDecoder` (libmpv + D3D11), `AirPlayAudioReceiver` + fdk-aac, 3-Lane WASAPI Ducking, `MicrophoneRtpSender` |
| **Layer 6** | [`Input/`](file:///d:/CarPlayStation/Core/Input/README.md) | Input Translation & Gesture Mapping | `MouseTouchTranslator` (Tap/Drag/Back), `KeyboardKnobTranslator`, `MultiTouchInjector` |
| **Layer 7** | [`Service/`](file:///d:/CarPlayStation/Core/Service/README.md) | Core Facade & Public C++ Contracts | `CarPlayCoreService`, `CoreEventBus`, `ICarPlayCoreApi.h` |

---

## Engineering Rules

1. **Unidirectional Dependency Rule**:
   - Dependencies strictly flow downward: `Layer 7` $\rightarrow$ `Layer 6/5` $\rightarrow$ `Layer 4` $\rightarrow$ `Layer 3` $\rightarrow$ `Layer 2` $\rightarrow$ `Layer 1`.
   - Lower layers must never include or call higher layers.
2. **Interface-Based Isolation**:
   - Cross-layer interactions use abstract pure virtual interfaces (e.g. `IMfiSigner`, `ITransportChannel`, `IVideoDecoder`), enabling seamless mocking and independent testing.
3. **UI Isolation (DEC-002)**:
   - Core internals never reference Windows UI constructs (`HWND`, UI message loops). Video frames are handed off as D3D11 shared texture handles or frame buffers.
4. **Multi-AI Collaborative Safety**:
   - Every module can be developed by independent AI agents concurrently using dedicated partition prompts and mocks. Each layer README ends with an "AI Partition & Mocking Guide" section describing how to isolate that layer.
