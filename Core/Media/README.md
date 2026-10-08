# Layer 5: Media Streaming Engine (Video & Audio)

## 1. Responsibilities
This module is located at Layer 5 of Core. It manages real-time depacketization, hardware-accelerated video decoding, multi-lane audio mixing and ducking, and microphone audio capture uplink.

### Core Subcomponents
- **Video Pipeline (`AirPlayVideoReceiver`, `AirPlayVideoDemuxer`)**:
  - AirPlay video payload parsing, ChaCha20-Poly1305 frame decryption, NAL unit assembly (SPS/PPS/IDR/P-Frames).
  - Decoder abstraction interface `IVideoDecoder`:
    - libmpv decoder (`MpvVideoPlayer`): feeds decrypted H.264 Annex-B access
      units to mpv over an in-process read-only stream protocol, decodes with
      mpv's own D3D11 hardware VA path, renders through the libmpv SW render
      API, and uploads the frame into a Direct3D 11 shared texture
      (`DEC-009`).
  - Adaptive resolution re-negotiation, jitter buffer, and frame-drop policy under high load.
- **Audio Pipeline (`AirPlayAudioReceiver`, `AirPlayAudioDemuxer`, `AacCodecAdapter`, `WasapiAudioPipeline`)**:
  - Three-Lane Audio Architecture:
    - **Main / Media Lane**: Music and media playback (AAC / PCM).
    - **Alt / Navigation Lane**: Turn-by-turn navigation voice guidance.
    - **Telephony / Siri Lane**: Phone calls and Siri voice interactions.
  - One `AirPlayAudioReceiver` per lane, because each lane has its own UDP data port, its own
    pairing-derived stream key, and its own codec parameters.
  - `AirPlayAudioDemuxer`: RTP depacketization plus ChaCha20-Poly1305 AEAD decryption with the
    RTP header as additional authenticated data. It hands the caller the plaintext AudioUnit and
    stays testable without a codec. Without a key the payload is treated as plaintext.
  - `AacCodecAdapter` / `AacAudioDecoder`: fdk-aac (`Lib/fdk-aac`, `DEC-010`) hidden behind a
    Core-local interface — fdk-aac headers never appear in Core's public headers. Because CarPlay
    sends raw, header-less AAC access units, `BuildAudioSpecificConfig()` builds the explicit
    AudioSpecificConfig (CICP form for AAC-LC, encoder-derived for AAC-ELD) that fdk-aac needs to
    learn the channel configuration.
  - Low-latency WASAPI audio renderer in shared mode, 48 kHz stereo float.
  - The supported downlink formats are advertised through the CCPA `audioFormats` bitmask in the
    RTSP `/info` response (Layer 4): PCM 16 kHz mono, PCM 48 kHz stereo, AAC-ELD 16 kHz mono,
    AAC-ELD 48 kHz, AAC-LC 48 kHz stereo.
  - Ducking: a hard media-lane gain step applied inside `WasapiAudioPipeline` when navigation/voice audio is audible
    or when the phone sends `duckAudio`/`unduckAudio` (see `DEC-008` implementation note).
- **Microphone Uplink Pipeline (`WasapiMicrophoneUplink`, `MicrophoneRtpSender`, `IMicrophoneUplink`)**:
  - WASAPI microphone capture, resampling, and RTP transmission back to the iPhone (Payload Type 100)
    over UDP to the `dataPort` the iPhone advertised for the `input=true` stream.
  - Every uplink packet is sealed with ChaCha20-Poly1305 using the stream's *input* key, for both
    the AAC-ELD and the PCM uplink: the iPhone seals every wireless media packet whatever the codec.
  - The uplink is only opened when the iPhone actually requested it, so a session without a
    microphone request never touches the capture device.

---

## 2. Boundaries & Isolation
- **Downward Dependencies**: WASAPI, Direct3D 11, `Lib/mpv` (libmpv) and `Lib/fdk-aac` (DEC-010). Third-party codec headers stay inside `.cpp` translation units; Core's headers expose only the adapter interfaces.
- **Strictly No UI Thread / HWND Access**: Decoded video frames are delivered via D3D11 shared texture handles or lock-free frame queues to the Application layer; no UI rendering code is invoked here.
- **Public Exported Interfaces**:
  - `IVideoStreamReceiver`: `SetRenderSurface(handle)`, `SetFrameCallback(fn)`, `GetStreamStats()`.
  - `IAudioStreamReceiver`: `SetAudioOutputDevice()`, `SetVolume(lane, val)`.
  - `IMicrophoneController`: `StartVoiceRecording()`, `StopVoiceRecording()`.

---

## 3. AI Partition & Mocking Guide
- **Offline Video Stream Harness**: Feed saved H.264/HEVC byte streams directly into `IVideoDecoder` to benchmark decode latency and D3D11 texture presentation.
- **Ducking Harness**: Synthesize dual-lane audio inputs through `WasapiAudioPipeline` to verify the media-lane gain
  step and its attack/release timing.
