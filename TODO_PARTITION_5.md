# Temporary TODO: Partition 5 — Core/Media (Media Streaming Pipelines)

> **Notice**: This is a temporary, transient partition-scoped task checklist for Partition 5.
> Once all steps are implemented, verified by tests, and recorded into `result.md`, **this file will be deleted**.

---

## Architecture Reference
- Design Decision: `DEC-008` (Extensible Video Decoders, FFmpeg Auto-Sync Fallback, D3D11 Shared Textures, WASAPI Shared Mode 3-Lane Mixer)
- Directory Lock: `d:\CarPlayStation\Core\Media\` (Tests in `d:\CarPlayStation\Tests\`)

---

## Checklist by Steps

### Step 1: Interface Contracts, Data Models & Mocks (Core/Media) — [Completed]
- [x] **1.1 Video Decoder Contract, Fixed-Aspect Display & Codec Discovery**
  - Extend `IVideoDecoder.h`:
    - Add extensible `VideoCodec` enumeration (`H264`, `HEVC`, `AV1`, `VP9`, `VVC`).
    - Define NALU / Access Unit frame packet structures (`NalUnitType`, SPS/PPS/VPS detection flags, PTS/DTS).
    - Expose `QueryVideoCapabilities()` and discrete resolution `Reconfigure()` specification.
    - Architectural Rule: Forbid window resize/mouse drag from triggering stream renegotiation. Presentation layer handles resize purely via GPU Direct3D 11 scaling (defaults to aspect preservation with black bars Letterboxing/Pillarboxing, or optional Stretch-to-fill). Physical stream resolution changes with iPhone are strictly restricted to explicit Application-layer settings or entering/exiting Borderless Fullscreen / Fullscreen mode.
- [x] **1.2 Audio Pipeline Contract & Format Enums**
  - Extend `IAudioPipeline.h`:
    - Support audiophile formats (32k, 44.1k, 48k, 88.2k, 96k, 176.4k, 192k Hz; 16, 24, 32-bit PCM/Float).
    - Declare `AudioCapabilities` discovery and soundcard `AutoAdaptToMixFormat` toggle.
- [x] **1.3 Microphone Uplink Interface**
  - Create `IMicrophoneUplink.h`:
    - Pure virtual interface for audio capture start/stop, sample rate configuration (16kHz/24kHz mono), and uplink packet callback.
- [x] **1.4 Standalone Mock Implementations**
  - Create `MockVideoDecoder.h`:
    - Track decoded frame count, simulate D3D11 shared texture handle dispatch, verify PTS sequences.
  - Create `MockAudioPipeline.h`:
    - Track 3-lane PCM sample writes, verify lane volume multipliers and ducking attenuation.
  - Create `MockMicrophoneUplink.h`:
    - Mock voice capture stream and test packet injection.

---

### Step 2: Video Demuxer, Media Foundation Decoder & FFmpeg Auto-Sync
- [ ] **2.1 Stream Demuxer & NALU Extractor**
  - Implement `AirPlayVideoDemuxer.h/.cpp`:
    - Parse AirPlay video payloads: 4-byte Big-Endian length-prefixed NALUs to Annex-B (`00 00 00 01`).
    - Support non-standard ultrawide aspect ratios (e.g. `1920x720`, `2560x720`).
    - Identify SPS, PPS, VPS, and IDR keyframes.
    - Implement frame-loss detection and resync to the next IDR keyframe.
- [ ] **2.2 Media Foundation Hardware Decoder & D3D11 Shared Surfaces**
  - Implement `MfVideoDecoder.h/.cpp`:
    - Windows Media Foundation Transform (MFT) hardware decoder for H.264 / HEVC.
    - Enable `CODECAPI_AVLowLatencyMode = TRUE`.
    - Direct3D 11 DXGI Device Manager integration, rendering to D3D11 textures with shared surface handles (`IDXGIResource::GetSharedHandle`).
    - Zero-overhead GPU viewport letterboxing during window dragging; debounced renegotiation trigger.
- [ ] **2.3 Automated FFmpeg Sync (x64 / ARM64)**
  - Implement `cmake/FindOrDownloadFFmpeg.cmake`:
    - Automatically detect host target architecture (x64 vs ARM64).
    - Auto-download latest stable release-shared ZIP (Gyan.dev / BtbN), extract `avcodec`, `avutil`, and headers with local version cache.
    - Configure CMake `POST_BUILD` to copy DLLs to the output directory.
  - Provide `scripts/Update-FFmpeg.ps1` for manual one-click updates.
- [ ] **2.4 FFmpeg Hardware/Software Fallback Decoder**
  - Implement `FFmpegVideoDecoder.h/.cpp`:
    - Dynamic runtime binding (`LoadLibrary` / delay loading) so core builds remain zero-dependency if DLL is not yet present.
    - `d3d11va` hardware decoding (bypasses Windows Store HEVC package requirement) with CPU software fallback.

---

### Step 3: 3-Lane Audio Mixer, Ducking Manager & Microphone Uplink
- [ ] **3.1 Audio Ducking State Machine**
  - Implement `AudioDuckingManager.h/.cpp`:
    - Monitor activity states of `MainMedia`, `AltNavigation`, and `SiriTelephony`.
    - Attack curve: smooth fade from 100% to 20% within 100ms on navigation prompt start.
    - Release curve: smooth restore to 100% over 300ms–500ms after prompt ends.
- [ ] **3.2 Adaptive WASAPI Shared Mode Renderer & Software Mixer**
  - Implement `WasapiAudioPipeline.h/.cpp`:
    - Non-exclusive `AUDCLNT_SHAREMODE_SHARED`, coexisting peacefully with Windows system sounds.
    - `GetMixFormat()` auto-adaptation: query soundcard's native DAC format (up to 192kHz 24/32-bit), bypassing Windows system audio resampling.
    - Internal multi-rate resampler converting incoming iPhone streams to soundcard target rate.
    - 3-Lane software mixer with soft-limiter clipping protection.
- [ ] **3.3 WASAPI Microphone Uplink**
  - Implement `WasapiMicrophoneUplink.h/.cpp`:
    - WASAPI capture client for local microphone input.
    - Resampling to 16kHz / 24kHz mono and packet callback dispatch.

---

### Step 4: Test Suite, Verification & CMake Integration
- [ ] **4.1 Comprehensive Media Tests**
  - Create `Tests/MediaTests.cpp`:
    - Test 1: NALU demuxing, length-prefix conversion, and SPS/PPS parsing.
    - Test 2: Audio ducking gain curve transition and timing.
    - Test 3: 3-lane PCM software mixer mathematical addition and limiter clamping.
    - Test 4: MockVideoDecoder and MockAudioPipeline end-to-end integration.
- [ ] **4.2 Build & CTest Verification**
  - Update `Core/CMakeLists.txt` and `Tests/CMakeLists.txt` (link `mfplat`, `mfuuid`, `d3d11`, `dxgi`).
  - MSVC (`-std=c++20`) 0 warnings / 0 errors.
  - CTest 6/6 test suites pass 100%.
