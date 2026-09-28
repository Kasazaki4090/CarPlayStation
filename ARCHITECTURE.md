# Architecture

CarPlayStation is divided into five major parts:

```text
┌───────────────────────────────────────────────┐
│                    UI                         │
│        Win32 / Display / Input / Control      │
└───────────────────────┬───────────────────────┘
                        │
┌───────────────────────▼───────────────────────┐
│                 Application                   │
│                                               │
│  Application API / State / Commands / Events  │
└───────────────────────┬───────────────────────┘
                        │
┌───────────────────────▼───────────────────────┐
│                    Core                       │
│                                               │
│  CarPlay Session                              │
│  Pairing / Authentication                     │
│  iAP2                                         │
│  Transport                                    │
│  Audio / Video / Input                        │
└───────────────┬───────────────────┬───────────┘
                │                   │
        ┌───────▼──────┐    ┌──────▼────────┐
        │    Wired     │    │   Wireless    │
        │   USB/NCM    │    │   Wi-Fi/IP    │
        └──────────────┘    └───────────────┘

┌───────────────────────────────────────────────┐
│                    Lib                        │
│                                               │
│  System APIs / Media Foundation / WASAPI      │
│  FFmpeg / Other third-party libraries         │
└───────────────────────────────────────────────┘

┌───────────────────────────────────────────────┐
│                 Reference                     │
│                                               │
│  XcertPLay / Carplayd                         │
│  Protocol and behavior reference only         │
└───────────────────────────────────────────────┘
```

## Layer Responsibilities

### Application

The application layer sits between the UI and the Core, as required by `DEC-002`.

It exposes operations and state to the UI and translates UI requests into Core calls.

Responsibilities:

* Session lifecycle operations (connect, disconnect, reconnect)
* Application state and event surface for the UI
* Command handling from the UI toward Core
* Metadata and status presentation data

The application layer must not depend on a specific UI framework, and it must not implement CarPlay protocol logic itself. Protocol logic stays in Core.

### Core

Contains the main CarPlay implementation and protocol logic.

Core should not depend on the UI implementation.

Wired and wireless connections should share the same CarPlay session and protocol layers wherever possible.

### UI

Windows-specific user interface and interaction layer.

Initial target:

* Win32
* Display
* Touch
* Mouse (input modality not confirmed in the CarPlay protocol; see `PROJECT.md` corrections and `TODO.md` sections 1 and 9)
* Keyboard (input modality not confirmed in the CarPlay protocol; see `PROJECT.md` corrections and `TODO.md` sections 1 and 9)

### Lib

Third-party libraries and platform backends used by Core.

Prefer Windows native APIs when practical.

Examples:

* Direct3D
* Media Foundation
* WASAPI
* USB APIs
* FFmpeg

Third-party libraries should not leak unnecessary implementation details into Core.

### Reference

Reference implementations and research material.

* XcertPLay
* Carplayd

These projects are used for protocol, behavior and implementation research only.

They are not runtime dependencies.

## Design Principles

1. Keep Core independent from UI; UI talks to Core only through the application layer.
2. Wired and wireless should converge into the same session layer.
3. Prefer native Windows APIs.
4. Use third-party libraries only when they provide practical value.
5. Keep the runtime and distribution size small.
6. Document major architectural decisions in `DECISIONS.md`.

##  UI Isolation

The UI is an independent presentation layer.

UI code must not directly access low-level implementation details such as:

- USB
- NCM
- Wi-Fi transport
- iAP2 internals
- CarPlay protocol internals
- Authentication internals
- Media decoder internals
- WASAPI
- Direct3D implementation details
- Third-party library APIs

UI may only communicate with the application layer through defined interfaces.

The intended dependency direction is:

UI
 ↓
Application
 ↓
Core
 ↓
Platform / Libraries

UI must never bypass the application layer to access Core internals or lower layers.

The UI implementation may be replaced without requiring changes to the underlying CarPlay protocol implementation.

For example:

Win32 UI
    └── Application API
            └── CarPlay Core

Unreal Engine UI
    └── Application API
            └── CarPlay Core

## Detailed Architecture

Detailed protocol flows, session states, transport behavior, media pipelines and other implementation details should be added here as the project develops.

## Documentation Corrections

### 2026-09-28 — Application layer added

Problem:

* The overview diagram stated "four major parts" and showed only UI, Core, Lib and Reference, with no Application layer.
* This conflicted with `DECISIONS.md` DEC-002, which mandates the dependency direction `UI -> Application -> Core -> Platform / Libraries`.
* The "UI Isolation" section already referenced the application layer, so the document was internally inconsistent with its own diagram.

Change:

* Diagram now shows five parts with an Application layer between UI and Core.
* Added an "Application" responsibilities subsection under "Layer Responsibilities".
* Design Principle 1 now names the application layer explicitly.

This correction only documents the layer DEC-002 already requires. It does not change any project goal. If the Application layer boundary needs to be formalized further, record it as a new decision in `DECISIONS.md`.

### 2026-09-28 — Mouse/Keyboard UI targets annotated

The UI "Initial target" list kept Mouse and Keyboard but annotated them as unconfirmed in the CarPlay protocol, matching the note added to `PROJECT.md` and the research task in `TODO.md`. No goal was removed.
