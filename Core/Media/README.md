# Layer 5: Media Streaming Engine (Video & Audio)

## 1. Responsibilities
This module is located at Layer 5 of Core. It manages real-time depacketization, hardware-accelerated video decoding, multi-lane audio mixing and ducking, and microphone audio capture uplink.

### Core Subcomponents
- **Video Pipeline (`VideoPipeline`)**:
  - AirPlay video payload parsing, NAL unit assembly (SPS/PPS/IDR/P-Frames).
  - Decoder abstraction interface `IVideoDecoder`:
    - libmpv decoder (`MpvVideoPlayer`): feeds decrypted H.264 Annex-B access
      units to mpv over an in-process read-only stream protocol, decodes with
      mpv's own D3D11 hardware VA path, renders through the libmpv SW render
      API, and uploads the frame into a Direct3D 11 shared texture
      (`DEC-009`).
  - Adaptive resolution re-negotiation, jitter buffer, and frame-drop policy under high load.
- **Audio Pipeline (`AudioPipeline`)**:
  - Three-Lane Audio Architecture:
    - **Main / Media Lane**: Music and media playback (AAC / PCM).
    - **Alt / Navigation Lane**: Turn-by-turn navigation voice guidance.
    - **Telephony / Siri Lane**: Phone calls and Siri voice interactions.
  - Low-latency WASAPI audio renderer.
  - Audio Ducking Manager (`AudioDuckingManager`): Smooth volume attenuation on media audio when navigation or phone calls become active.
- **Microphone Uplink Pipeline (`MicrophoneUplink`)**:
  - WASAPI microphone capture, noise suppression, and resampling.
  - AAC-ELD / OPUS / PCM encoding and uplink RTP transmission back to the iPhone.

---

## 2. Boundaries & Isolation
- **Downward Dependencies**: WASAPI, Direct3D 11, and `Lib/mpv` (libmpv).
- **Strictly No UI Thread / HWND Access**: Decoded video frames are delivered via D3D11 shared texture handles or lock-free frame queues to the Application layer; no UI rendering code is invoked here.
- **Public Exported Interfaces**:
  - `IVideoStreamReceiver`: `SetRenderSurface(handle)`, `SetFrameCallback(fn)`, `GetStreamStats()`.
  - `IAudioStreamReceiver`: `SetAudioOutputDevice()`, `SetVolume(lane, val)`.
  - `IMicrophoneController`: `StartVoiceRecording()`, `StopVoiceRecording()`.

---

## 3. AI Partition & Mocking Guide
- **Offline Video Stream Harness**: Feed saved H.264/HEVC byte streams directly into `IVideoDecoder` to benchmark decode latency and D3D11 texture presentation.
- **Audio Ducking Harness**: Synthesize dual-lane audio inputs to verify ducking attack/release curves.
