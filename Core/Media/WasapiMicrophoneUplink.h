#pragma once

#include "Core/Media/IMicrophoneUplink.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <thread>

namespace CarPlayStation::Core::Media {

// ============================================================================
// WasapiMicrophoneUplink (DEC-008, Partition 5 Step 3.3)
//
// WASAPI capture client for local microphone input. Captures at the negotiated
// rate, resamples to 16kHz / 24kHz mono, and dispatches uplink packets via
// the UplinkAudioCallback.
//
// The resampling + packetization logic is pure and platform-independent
// (testable headless). The WASAPI COM capture backend is guarded by _WIN32 and
// degrades to a "null" capture source when no microphone is present, so the
// uplink can be driven by injected frames in tests.
// ============================================================================
class WasapiMicrophoneUplink final : public IMicrophoneUplink {
public:
    WasapiMicrophoneUplink() = default;
    ~WasapiMicrophoneUplink() override { Shutdown(); }

    // --- IMicrophoneUplink ---
    [[nodiscard]] Common::Status Start(const Common::AudioOutputFormat& format) override;
    void SetUplinkCallback(UplinkAudioCallback callback) override;
    [[nodiscard]] Common::Status Stop() override;
    [[nodiscard]] bool IsCapturing() const noexcept override;
    [[nodiscard]] MicrophoneCapabilities QueryCapabilities() const override;
    void SetInputGain(float gain) override;
    [[nodiscard]] uint64_t GetCapturedSampleCount() const noexcept override;
    void Shutdown() override;

    // --- Resampling / packetization (platform-independent, testable) ---
    // Resample `src` (srcCount float32 samples at srcRate) to `dstRate` mono
    // and dispatch as uplink packets of `packetSamples` samples each.
    void ProcessCapturedSamples(const float* src, size_t srcCount,
                                uint32_t srcRate, uint32_t dstRate,
                                size_t packetSamples, uint64_t timestampUs);

    // --- WASAPI capture backend ---
    // Opens the default eCapture endpoint in shared mode at the device mix
    // format and drains it on its own thread. When no capture device exists
    // (headless / test runs) Start() falls back to the null capture path, so
    // the uplink can still be driven by injected frames.
    bool StartCaptureBackend();
    void StopCaptureBackend();
    void CaptureLoop();
    void LogCaptureStats();

    // --- Inspection / test helpers ---
    [[nodiscard]] bool IsWasapiActive() const noexcept { return wasapiActive_.load(); }
    [[nodiscard]] uint32_t GetPacketDispatchCount() const noexcept { return packetDispatchCount_.load(); }
    [[nodiscard]] uint32_t GetCallbackDispatchCount() const noexcept { return callbackDispatchCount_.load(); }
    [[nodiscard]] float GetInputGain() const { return inputGain_.load(); }
    [[nodiscard]] Common::AudioOutputFormat GetCurrentFormat() const;
    [[nodiscard]] uint32_t GetLastPacketSamples() const noexcept { return lastPacketSamples_.load(); }
    void SetCapabilities(const MicrophoneCapabilities& caps);

private:
    mutable std::mutex mutex_;
    // Guards the capture backend handles separately from mutex_: the capture
    // thread takes mutex_ briefly (to read the uplink callback), so Stop()
    // must be able to join the thread without holding it.
    std::mutex backendMutex_;
    bool capturing_{false};
    Common::AudioOutputFormat format_{};
    MicrophoneCapabilities capabilities_{};
    UplinkAudioCallback uplinkCallback_{};
    std::atomic<float> inputGain_{1.0f};
    std::atomic<uint64_t> capturedSampleCount_{0};
    std::atomic<uint32_t> packetDispatchCount_{0};
    std::atomic<uint32_t> callbackDispatchCount_{0};
    std::atomic<uint32_t> lastPacketSamples_{0};
    std::atomic<bool> wasapiActive_{false};
    void* wasapiCapture_{nullptr};   // IAudioClient* (opaque; COM under _WIN32)
    void* wasapiCaptureClient_{nullptr}; // IAudioCaptureClient*
    void* wasapiEvent_{nullptr};     // HANDLE (capture event)
    uint32_t captureRate_{0};        // device mix rate the endpoint was opened at
    uint16_t captureChannels_{0};    // device mix channel count
    bool captureIsFloat_{false};
    uint16_t captureBits_{0};
    size_t capturePacketSamples_{0}; // resampled samples per dispatched packet
    std::thread captureThread_;
    std::atomic<bool> captureStop_{false};
    std::atomic<uint64_t> captureDrainCount_{0};
    uint64_t statsClockMs_{0};
    uint64_t lastCapturedSamples_{0};
};

} // namespace CarPlayStation::Core::Media
