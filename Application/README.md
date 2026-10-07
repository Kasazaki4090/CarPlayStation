# Application Layer

`Application/` serves as the intermediary layer connecting `UI/` and `Core/`, strictly adhering to the `DEC-002` architectural decision.

## Responsibilities
1. **Application State Management**: Tracks global session lifecycles, active connected iPhone information, NowPlaying metadata, and playback status.
2. **Command Relay & Dispatch**: Receives user actions from the UI (connect, disconnect, resolution change, mute, input events), validates them, and invokes the public Core C++ interface (`ICarPlayCoreApi`).
3. **Event Observation**: Subscribes to Core protocol and media events, updates UI view models, and notifies the UI layer for re-rendering. Runtime video facts (the resolution/fps actually advertised and the codec actually on the wire) are read back through `ICarPlayCoreApi::GetActiveVideoStreamSpec()` and stamped into `AppUiState`, so the HUD shows what is really streaming rather than what was configured.
4. **Preference Model (`AppSettings.h`)**: The single container for everything the user may tune at runtime — `VideoPreferences` (resolution preset 720p/1080p/2K/4K/Auto/Custom, codec preference, fps, window display mode), `AudioPreferences` (the three per-lane volumes only — the output sample rate follows the Windows default endpoint's mix format, the encode bitrate is fixed at 320 kbps, and ducking is protocol-driven), `DevicePreferences` (advertised name, manufacturer, model) and `MFiPreferences`. Presets are resolved into concrete `Core::Common::VideoStreamSpec` / `AudioOutputFormat` values here; Core only ever receives facts, never user intent (`DEC-004`).
5. **MFi Backend Resolution (`MfiSignerFactory`)**: Turns `MFiPreferences::backend_type` into a concrete `Core::Hardware::IMfiSigner`:
   - `LocalFile` — offline software signer with an imported certificate and private key (`DEC-006`).
   - `HardwareI2C` — a physical USB-to-I2C bridge driving an MFi coprocessor; `HardwareI2cDriverType` selects CH341 / CH347 / FTDI, or `Auto` probes them in that order (`DEC-003`, `DEC-007`).
   - `RemoteHttp` — the Remote MFi HTTP JSON service (`DEC-011`), host / port / bearer token from `RemoteMfiPreferences` (default port 8972).
   The former value `1` (binary TCP signing daemon) was removed; configs that still say `BackendType=1` are migrated to `RemoteHttp` when the INI is read. `Describe()` returns a human-readable name of the resolved backend for the UI.

## Isolation Rules (DEC-002)
- **Upward Facing**: Exposes high-level business interfaces and Data Transfer Objects (DTOs) to the UI. Completely agnostic of the specific UI framework (compatible with Win32, Unreal Engine, etc.).
- **Downward Calling**: Interacts exclusively with `Core/Service/ICarPlayCoreApi.h`. Strictly forbidden from accessing private internal headers of `Core/Protocol`, `Core/Transport`, etc.
- **Preference Ownership**: The UI edits `AppSettings`; this layer converts them into Core parameters and constructs the hardware/daemon-backed signers. Core never hardcodes a file path, service address, or driver name.

## Files
| File | Responsibility |
| :--- | :--- |
| `IApplicationApi.h` | The only interface the UI is allowed to use. |
| `ApplicationService` | Implementation: session commands, state aggregation, Core event subscription, `AppUiState` publication. |
| `AppSettings.h` | User-tunable preference model (`DEC-004`). |
| `MfiSignerFactory` | Preference → `IMfiSigner` resolution (`DEC-003`/`DEC-006`/`DEC-007`/`DEC-011`). |
