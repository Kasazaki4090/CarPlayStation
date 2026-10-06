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

### fdk-aac (Fraunhofer FDK AAC Codec Library)

**Purpose:**
AAC decoding for the CarPlay audio downlink (AAC-LC for media, AAC-ELD for
Siri/telephony/alert) and AAC-ELD encoding for the microphone uplink.

**Version:**
`v2.0.3` (git tag, commit `716f4394641d53f0d79c9ddac3fa93b03a49f278`).

**Source:**
Fraunhofer IIS FDK AAC Codec Library. Mirrored with official git tags at
`https://github.com/mstorsjo/fdk-aac`, which is the URL used for the submodule.

**License:**
"Software License for The Fraunhofer FDK AAC Codec Library for Android"
(`NOTICE` in the repository). A permissive BSD-3-clause-style grant with a
separate patent grant; note the upstream `METADATA` flags it as
`BY_EXCEPTION_ONLY` for Google-internal third-party use, and MPEG-H / HE-AAC
patent royalties may apply to end products depending on region and use.

**Reason:**
CarPlay's audio downlink is AAC (LC for `media`, ELD for the low-latency voice
streams) and the microphone uplink the iPhone asks for is AAC-ELD 16 kHz mono.
Windows has no public system encoder that produces AAC-ELD, and Media Foundation
cannot emit the raw ELD access units plus the AudioSpecificConfig the CarPlay
RTP payload requires. fdk-aac is the same library the reference implementations
(carplayd, DiPlay) use, so the codec behaviour matches what iPhones expect.

**Integration:**
Added as a git submodule at `Lib/fdk-aac` (see `.gitmodules`) and built from
source as a static library by the top-level `CMakeLists.txt`
(`add_subdirectory(Lib/fdk-aac EXCLUDE_FROM_ALL)` with `BUILD_SHARED_LIBS OFF`),
so there is no extra runtime DLL. Third-party sources are compiled at `/W3`
instead of the project's `/W4`. `CarPlayCore` links `FDK-AAC::fdk-aac` and the
only file that includes an fdk-aac header is `Core/Media/AacCodecAdapter.cpp`,
which exposes a pimpl facade (`AacAudioDecoder` / `AacAudioEncoder`) to the rest
of `Core` (rule 9).

**Notes:**
- The submodule is pinned to the `v2.0.3` tag; run
  `git submodule update --init --recursive` after cloning.
- No third-party source is modified.
- `AacAudioEncoder::audio_specific_config()` returns a reference into encoder
  state that `Close()` clears; callers must copy it before closing.
- The decoder is opened with an explicit `AudioSpecificConfig` built from the
  CarPlay `audioFormat` bitmask; `TT_MP4_RAW` is the right transport type for
  the ELD access units CarPlay sends.

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

### WCH CH341 / CH347 USB-to-I2C driver package (wch-ch341)

**Purpose:**
User-mode driver DLLs for the WCH CH341 and CH347 USB-to-I2C/SPI/UART bridges,
used by `Core/Hardware/Ch341I2cAdapter.cpp` and `Core/Hardware/Ch347I2cAdapter.cpp`
to talk to the Apple MFi authentication coprocessor.

**Version:**
Taken from the installed Windows driver package
`ch341wdm.inf_amd64_ad95b9e9450660c1` (`DriverVer = 04/14/2025, 2.6.2025.04`).
`CH341DLLA64.DLL` / `CH341DLL.DLL` report file version 1.10;
`CH347DLLA64.DLL` / `CH347DLL.DLL` report file version 1.50.

**Source:**
WCH (Nanjing Qinheng Microelectronics) official driver package, as installed by
WCH's driver installer into
`C:\Windows\System32\DriverStore\FileRepository\ch341wdm.inf_amd64_*`. The same
DLLs are distributed from `https://www.wch-ic.com/downloads/CH341PAR_HTML.html`
(CH341) and `https://www.wch-ic.com/downloads/CH347PAR_HTML.html` (CH347).

**License:**
WCH's freeware driver/EVAL distribution terms. Unmodified binary redistribution
is permitted; no source is shipped and no source is modified.

**Reason:**
The adapters call the vendor API (`CH341OpenDevice`, `CH341SetStream`,
`CH341WriteI2CData`, `CH347I2C_Init`, `CH347I2C_Write` / `CH347I2C_Read`, ...)
through `LoadLibrary`. The DriverStore folder that holds these DLLs is not on
the default DLL search path, and a machine with only the kernel driver installed
still has no import library to link against, so the DLLs are vendored and copied
next to the executable.

**Integration:**
`Core/CMakeLists.txt` copies `CH341DLLA64.DLL`, `CH341DLL.DLL`,
`CH347DLLA64.DLL` and `CH347DLLA64.DLL` next to `CarPlayStation` at build time.
The adapters resolve them through `Core/Hardware/DriverDllLookup.h`, which probes
the default search path, the executable directory, `System32`, and any
`DriverStore\FileRepository\ch341wdm.inf.*` / `ch347wdm.inf.*` folder, preferring
the `*A64` build on x64 (a 32-bit DLL cannot be loaded into a 64-bit process).
No header is needed: the functions are resolved with `GetProcAddress`.

**Notes:**
- `CH341W64.SYS`, `CH341WDM.CAT` and `ch341wdm.inf` are kept only so the exact
  driver package this build was validated against is recorded; they are not used
  at runtime (the kernel driver is installed by WCH's installer).
- The 32-bit and 64-bit DLLs have different names (`CH341DLL.DLL` vs
  `CH341DLLA64.DLL`), so both are copied and the adapter picks the loadable one.

### FTDI D2XX + libMPSSE (ftdi-d2xx)

**Purpose:**
User-mode driver and MPSSE bit-bang library used by
`Core/Hardware/FtdiI2cAdapter.cpp` to drive an FTDI AM-series part
(FT232H/FT2232H/FT4232H) as an I2C master for the MFi coprocessor.

**Version:**
libMPSSE `v1.0.7` (official Windows package `LibMPSSE_1.0.7.zip`, released
2024-08). `ftd2xx.dll` `3.2.21.1` (product 2.12.36.20), taken from the D2XX
driver already installed on this machine (`System32` / `SysWOW64`).

**Source:**
FTDI. `https://www.ftdichip.cn/Support/SoftwareExamples/MPSSE/LibMPSSE_1.0.7.zip`
(the nested `Windows/libmpsse-windows-1.0.7.zip` inside it holds the Windows
binaries and headers). Driver pages:
`https://www.ftdichip.cn/Drivers/D2XX.htm` (D2XX) and
`https://www.ftdichip.cn/Drivers/VCP.htm` (VCP).

**License:**
FTDI's binary-only driver/library licence (see `ReadMe.txt` from the libMPSSE
package). Binaries are unmodified.

**Reason:**
**D2XX was chosen over VCP.** VCP (`ftcserco.dll` / `ftser2k.dll`) re-presents
the FTDI channel as a virtual COM port; it exposes no MPSSE GPIO or I2C API, so
it cannot bit-bang SCL/SDA. D2XX ships `ftd2xx.dll`, which is what libMPSSE's
`I2C_GetNumDevices` / `I2C_OpenPort` / `I2C_Read` / `I2C_Write` are built on,
so D2XX + libMPSSE is the only option that supports the `II2cAdapter` contract.

**Integration:**
`libmpsse.dll` is loaded dynamically by `FtdiI2cAdapter` through
`Core/Hardware/DriverDllLookup.h` (probing the executable directory, `System32`,
and `DriverStore\FileRepository\ftdigp22.inf.*` / `ftd2xx.inf.*`).
`Core/CMakeLists.txt` copies the architecture-matched `libmpsse.dll` and
`ftd2xx.dll` (`x64` or `Win32`) next to `CarPlayStation`. `include/` is kept for
reference only; nothing in `Core` includes an FTDI header (rule 9).

**Notes:**
- `libmpsse.dll` links against `ftd2xx.dll` at runtime, so **both** must sit next
  to the executable; the adapter logs a warning naming the two files when loading
  fails.
- libMPSSE 1.0.7 fixed the very slow `I2C_DeviceRead` / `I2C_DeviceWrite` path
  (it now uses `FAST_TRANSFER_BITS`); older 1.0.x releases are unusable for MFi
  certificate reads.
- The package ships `libmpsse.dll`, not the `libMPSSE-I2C.dll` name used by some
  Linux/older builds; the adapter accepts either.
- The D2XX *driver installer* was not vendored (the download 404s from this
  network); install FTDI's CDM driver normally, or rely on the vendored runtime
  DLLs for a machine that already has the kernel driver.

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
          └── libmpv
```

The purpose of this separation is to keep CarPlayStation's own implementation independent from any particular external library where practical.

Earlier decoders behind this interface (a Media Foundation MFT decoder and an
FFmpeg fallback) were removed; see `DEC-009` in `DECISIONS.md`.
