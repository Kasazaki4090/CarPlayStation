# Architecture

CarPlayStation is divided into four major parts:

```text
┌───────────────────────────────────────────────┐
│                    UI                         │
│        Win32 / Display / Input / Control      │
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
* Mouse
* Keyboard

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

1. Keep Core independent from UI.
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
