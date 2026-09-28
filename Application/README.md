# Application Layer

`Application/` serves as the intermediary layer connecting `UI/` and `Core/`, strictly adhering to the `DEC-002` architectural decision.

## Responsibilities
1. **Application State Management**: Tracks global session lifecycles, active connected iPhone information, NowPlaying metadata, and playback status.
2. **Command Relay & Dispatch**: Receives user actions from the UI (connect, disconnect, resolution change, mute, input events), validates them, and invokes the public Core C++ interface (`ICarPlayCoreApi`).
3. **Event Observation**: Subscribes to Core protocol and media events, updates UI view models, and notifies the UI layer for re-rendering.

## Isolation Rules (DEC-002)
- **Upward Facing**: Exposes high-level business interfaces and Data Transfer Objects (DTOs) to the UI. Completely agnostic of the specific UI framework (compatible with Win32, Unreal Engine, etc.).
- **Downward Calling**: Interacts exclusively with `Core/Service/ICarPlayCoreApi.h`. Strictly forbidden from accessing private internal headers of `Core/Protocol`, `Core/Transport`, etc.
