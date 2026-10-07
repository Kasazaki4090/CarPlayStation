# CarPlayStation

A Windows-based CarPlay receiver.

## Project Goal

Turn a Windows PC into a CarPlay receiver:

```text
iPhone
  ↓
CarPlay
  ↓
Windows
  ↓
Display / touch / keyboard and mouse / audio
```

## Phase 1 Goals

- MFi authentication
- Wired CarPlay
- Wireless CarPlay
- iAP2
- USB / NCM
- Video output
- Microphone input
- Touch input
- Keyboard and mouse input (input modality not confirmed in reference implementations; see TODO sections 1 and 9)

### Features That May Be Limited in Phase 1

The following features may be incomplete or have limited functionality in the initial release:

- Siri
- Phone calls
- Multichannel audio
- Automatic volume control
- A game-style display mode selector, including:
  - Windowed, borderless-windowed, and fullscreen modes
  - Resolutions from 720p to 2160p
  - Automatic resolution selection
  - Frame rates from 30 to 60 FPS

### Out of Scope

- GUI platforms other than Win32

## Platform and Technologies

- Windows 10 / 11
- C++
- Direct3D
- WASAPI
- USB
- libmpv (video decoding, see `DEC-009`)

## Requirements

- Keep the application footprint as small as possible.
- Run on Windows 10 / 11 without requiring additional runtimes such as .NET.
- Decode video with libmpv, which uses its own hardware-accelerated D3D11 VA
  path. The earlier Media Foundation (MFT) decoder and FFmpeg fallback were
  removed; see `DEC-009`.

## Reference Projects

The following projects may be consulted for protocol, behavior, and implementation references. They are not runtime dependencies and must not be called directly:

- XcertPLay
- Carplayd
- DiPlay
- remote-mfi-for-xcertplay

## Documentation Corrections

### 2026-09-28 — Keyboard and mouse input flagged for verification

Problem:

* `PROJECT.md` lists "Keyboard and mouse input" as a Phase 1 goal, and `ARCHITECTURE.md` lists "Mouse" and "Keyboard" under the UI initial target.
* The reference implementations under `Reference/` only demonstrate touch input plus media/Siri/telephony buttons. No keyboard or mouse channel is confirmed in the CarPlay protocol as implemented there.
* `TODO.md` tracks this as a P1 research task (sections 1 and 9).

Action:

* The goal is kept but explicitly marked as unconfirmed and linked to the research task, rather than silently removed or assumed.
* Per `DEC-001` and `TODO.md` rule 6, the scope will only be changed after the research concludes and a decision is recorded in `DECISIONS.md`.