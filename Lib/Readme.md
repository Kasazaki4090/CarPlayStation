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
