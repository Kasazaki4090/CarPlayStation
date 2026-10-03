# CarPlayStation

A Windows CarPlay receiver.

CarPlayStation implements a CarPlay receiver on Windows: it pairs with an iPhone
over Bluetooth (MFi) and AirPlay, carries the CarPlay session over RFCOMM /
Wi-Fi, and renders the decrypted H.264 video stream and audio to a local window
while forwarding touch / input back to the phone.

## Status

Implementation in progress. The Core protocol stack (pairing, iAP2, transport,
session) and the UI / Application layers are in place; the video path is
currently driven by libmpv (see `Lib/`).

Features:

- [x] Wired CarPlay (USB / NCM)
- [x] Wireless CarPlay (Wi-Fi / IP)
- [x] Audio (WASAPI shared-mode pipeline + microphone uplink)
- [x] Video (libmpv decode + D3D11 shared-texture render)
- [x] Touch / Input (Win32 input router)

## Architecture

Five layers, top to bottom:

```text
UI            Win32 window, D3D11/D2D rendering, input
Application   Application API / state / commands / events (DEC-002)
Core          CarPlay session, pairing/auth, iAP2, transport, audio/video/input
Lib           System APIs, Media Foundation, WASAPI, FFmpeg, libmpv
Reference     xcertplay / carplayd / DiPlay (protocol reference only)
```

See [ARCHITECTURE.md](./ARCHITECTURE.md) for the full layer responsibilities and
[DECISIONS.md](./DECISIONS.md) for the design decisions (DEC-xxx).

## Directory Layout

```text
Core/          CarPlayStation's own implementation (protocol, media, transport)
Application/   Application layer between UI and Core
UI/Win32/      Win32 window, D3D11/D2D renderer, input router
Lib/           External libraries (FFmpeg, libmpv) — see Lib/Readme.md
Reference/     Protocol reference projects (git submodules)
Tests/         Unit / hardware tests
cmake/         CMake helper modules (e.g. FindOrDownloadFFmpeg)
scripts/       Maintenance scripts (e.g. Update-FFmpeg.ps1)
```

## Building

Prerequisites:

- Windows 10/11 (x64)
- Visual Studio 2022 Build Tools (C++ workload)
- CMake 3.20+
- A Bluetooth radio (for MFi pairing)

The external libraries under `Lib/` (FFmpeg, libmpv) are vendored locally and
are not committed to git. Obtain them as described in [Lib/Readme.md](./Lib/Readme.md).

Build (x64, Release):

```bat
build.bat
```

or manually:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d D:\CarPlayStation\build-release
cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ..
nmake
```

The executable is produced at `build-release/UI/Win32/CarPlayStation.exe`.

## Running

Run the executable. The app opens a window, starts the CarPlay core (MFi
signer, RFCOMM server, AirPlay RTSP, video/audio receivers), and waits for an
iPhone to connect.

```bat
build-release\UI\Win32\CarPlayStation.exe
```

## Documentation

- [ARCHITECTURE.md](./ARCHITECTURE.md) — layer responsibilities and data flow
- [DECISIONS.md](./DECISIONS.md) — design decisions (DEC-xxx)
- [PROJECT.md](./PROJECT.md) — project overview
- [TODO.md](./TODO.md) — open work
- [BUGS.md](./BUGS.md) — known issues
- [CONTRIBUTING.md](./CONTRIBUTING.md) — contribution guidelines
- [Lib/Readme.md](./Lib/Readme.md) — external library records and policy

## License

See [LICENSE](./LICENSE).
