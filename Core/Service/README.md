# Layer 7: Core Service & Facade Layer

## 1. Responsibilities
This module is located at the top of Core (Layer 7). It acts as the central Facade and orchestrator of the entire Core stack. It aggregates capabilities from all underlying subsystems (Layers 1 through 6) and exposes clean, high-cohesion, framework-independent C++ public APIs to the **Application Layer**.

### Core Subcomponents
- **CarPlay Core Service Facade (`CarPlayCoreService`)**:
  - Singleton / factory managing the lifecycles of Hardware, Transport, Protocol, Session, Media, and Input instances.
  - Controls service lifecycles: `StartCore(config)`, `StopCore()`, `ResetSession()`.
- **Core Event Bus (`CoreEventBus`)**:
  - Thread-safe publish-subscribe event mechanism.
  - Emits events to the Application layer: Connection state changes, NowPlaying metadata, album artwork, audio route changes.
- **Public C++ Contract Header (`ICarPlayCoreApi.h`)**:
  - Pure C++ abstract base classes and Plain Old Data transfer objects (DTOs).
  - Contains NO third-party or platform-specific header leaks (e.g. no WinSock, libmpv, or WASAPI headers).

---

## 2. Boundaries & Isolation
- **Downward Aggregation**: Coordinates Layers 1 through 6.
- **Upward Facing**: Exclusively serves the `Application` layer.
- **DEC-002 Compliance**:
  - Encapsulates all Core internals behind `ICarPlayCoreApi`.
  - Swapping the UI framework (e.g., from Win32 to Unreal Engine) requires zero modifications inside `Core/`.

---

## 3. AI Partition & Mocking Guide
- **Full Mock Integration**: Can be constructed with mock implementations of Layers 1-6 while those layers are still under development, validating Application layer interactions early.
- **E2E Lifecycle Testing**: Drives end-to-end simulated session flows (Connect $\rightarrow$ Handshake $\rightarrow$ Stream $\rightarrow$ Input $\rightarrow$ Teardown).
