# Libraries

This directory contains external libraries used by CarPlayStation.

Libraries in this directory are external dependencies and are not part of the project's own implementation.

## Rules

When adding a library:

1. Create a dedicated directory under `Lib/`.
2. Document the library in this file.
3. Record the exact version, release, tag, or commit used.
4. Record the original source.
5. Record the license.
6. Describe why the library is required.
7. Prefer dependencies that do not introduce unnecessary runtime requirements.
8. Prefer Windows native APIs when they provide the required functionality.
9. Keep external library APIs isolated from `Core` where practical.
10. Do not modify third-party source unless necessary. Document significant modifications.

## Library Record

Each library should use the following format:

```markdown
### Library Name

**Purpose:**  
What the library is used for.

**Version:**  
Release, tag, or commit.

**Source:**  
Official project/source.

**License:**  
License of the version used.

**Reason:**  
Why the library is required.

**Integration:**  
How CarPlayStation uses the library.

**Notes:**  
Important build, runtime, compatibility, or licensing information.
```

## Current Libraries

### FFmpeg

**Purpose:**
Fallback media decoding and processing.

**Version:**
9.0.2.

- x64: GyanD/codexffmpeg release build, `full_build-shared` (zip; includes headers + import libs).
- arm64: tordona/ffmpeg-win-arm64 release build, `full-shared` (7z; runtime-only, `bin/` DLLs, no headers or import libs).

**Source:**
Official FFmpeg project (prebuilt release binaries).

**License:**
To be documented according to the exact build and enabled components.

**Reason:**
Provides a fallback when Windows native media decoding cannot provide the required codec or compatibility.

**Integration:**
FFmpeg should be used behind CarPlayStation-owned interfaces where practical.

Native Windows media APIs should be preferred when they provide the required functionality.

Preferred video decoding order:

1. Native Windows hardware decoding.
2. FFmpeg hardware-accelerated decoding.
3. FFmpeg software decoding when necessary.

**Notes:**
- The x64 and arm64 builds come from different upstream sources but target the same FFmpeg version (9.0.2), so the runtime library versions match (e.g. `avcodec-63.dll`, `avutil-61.dll`).
- The arm64 (tordona) build is runtime-only: it ships `bin/` DLLs but no `include/` headers or `.lib` import libraries. This is sufficient because `Core/Media/FFmpegVideoDecoder.cpp` binds to FFmpeg at runtime via `LoadLibrary`/`GetProcAddress` and never includes FFmpeg headers or links import libraries.
- The CMake configure step (`cmake/FindOrDownloadFFmpeg.cmake`) and the manual script (`scripts/Update-FFmpeg.ps1`) both auto-select the correct source and archive format per architecture.
- The exact enabled components, hardware acceleration backends, redistribution requirements, and package size will be documented when integrated.

### mpv (libmpv)

**Purpose:**
Video decoding and rendering for the CarPlay video stream.

**Version:**
`v0.41.0-1092-g3186d369f` (mpv development build, x64 Windows).

**Source:**
Official mpv project (prebuilt `mpv-dev` archive, `mpv-dev.7z`).

**License:**
The client API (`client.h`, `render.h`, `render_gl.h`, `stream_cb.h`) is ISC.
The mpv core is GPLv2+ by default, or LGPLv2+ when built with `-Dgpl=false`.
The prebuilt `libmpv-2.dll` in this directory is a GPL build; redistribution
follows the GPL terms.

**Reason:**
The in-process Media Foundation (MFT) decode + D3D11 render pipeline deadlocked
under GPU contention (the hardware MFT blocks on the renderer's shared texture,
and software MFT is too slow at 1280x720@60). libmpv decodes with its own
hardware-accelerated D3D11 VA path, independent of the renderer's D3D11 device,
so it avoids that contention.

**Integration:**
Used behind the `IVideoDecoder` interface via `Core/Media/MpvVideoPlayer.cpp`.
The DLL is loaded dynamically at runtime with `LoadLibrary("libmpv-2.dll")`
(resolved from the executable directory); only the C headers are needed at
compile time. Decrypted H.264 Annex-B access units are fed to mpv through a
custom read-only stream protocol (`mpv_stream_cb_add_ro`) backed by a bounded
ring buffer, keeping the data path in-process (no HTTP server). Decoded frames
are rendered into a software buffer via the libmpv SW render API
(`MPV_RENDER_API_TYPE_SW`, format `bgr0` = B8G8R8A8) and uploaded into a
D3D11 shared texture consumed by the UI renderer.

**Notes:**
- The CMake build (`Core/CMakeLists.txt`) adds `Lib/mpv/include` to the include
  path and copies `Lib/mpv/libmpv-2.dll` next to the executable at build time.
- `libmpv-2.dll` (~115 MB) and the `mpv-dev.7z` archive are not committed to
  git (see `.gitignore`); they are vendored locally under `Lib/mpv/`.
- The mpv render API only exposes `MPV_RENDER_API_TYPE_OPENGL` and
  `MPV_RENDER_API_TYPE_SW` (no native D3D11), so the SW render path is used and
  the frame is uploaded to a D3D11 shared texture by CarPlayStation.

## Dependency Policy

`Core/` contains CarPlayStation's own implementation.

`Lib/` contains external libraries.

Windows system APIs and frameworks are not copied into `Lib/`. They are treated as platform APIs.

The project should avoid allowing external library implementation details to unnecessarily spread throughout `Core/`.

For example:

```text
Core
 │
 └── Video Decoder Interface
          │
          ├── Windows Media Foundation
          │
          └── FFmpeg
```

The purpose of this separation is to keep CarPlayStation's own implementation independent from any particular external library where practical.
