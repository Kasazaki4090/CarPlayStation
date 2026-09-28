# Core

`Core/` contains the complete CarPlayStation implementation stack.

The Core is responsible for the functionality between the underlying platform/hardware interfaces and the application layer.

The intended architecture is layered, inspired by the concept of the OSI model.

Conceptually:

```text
Hardware
    ↓
Link
    ↓
Network
    ↓
Transport
    ↓
Session
    ↓
Media / Services
    ↓
Application
```

The exact layer boundaries and directory structure are not fixed yet.

They should be determined after analyzing the CarPlay protocol, reference implementations, and actual implementation requirements.

This file Will be rewrited after directory structure are changed.

## Rules

* Core contains CarPlayStation's own implementation.
* Core must not depend on UI implementations.
* Core must not depend on a specific UI framework.
* Lower layers should not depend on higher layers.
* UI communicates with Core through the application layer.
* Platform and third-party implementations should be isolated behind appropriate interfaces where practical.

The initial project review should determine the final layer structure before substantial implementation begins.
