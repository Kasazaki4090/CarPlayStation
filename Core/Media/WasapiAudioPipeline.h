#pragma once

#include "Core/Media/IAudioPipeline.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace CarPlayStation::Core::Media {

// ============================================================================
// WasapiAudioPipeline (DEC-008, Partition 5 Step 3.2)
//
// Adaptive WASAPI *shared-mode* renderer with an internal 3-lane software
// mixer:
//
//   * Non-exclusive AUDCLNT_SHAREMODE_SHARED so it coexists peacefully with
//     Windows system sounds.
//   * GetMixFormat() auto-adaptation: the pipeline queries the soundcard's
//     native DAC mix format (up to 192kHz 24/32-bit) and bypasses the Windows
//     system audio resampler by rendering at the native rate.
//   * An internal multi-rate resampler converts incoming iPhone streams to the
//     soundcard target rate.
//   * A 3-lane software mixer (MainMedia / AltNavigation / SiriTelephony) with
//     a soft-limiter clipping guard.
//
// The mixer and resampler are pure, platform-independent and unit-testable.
// The WASAPI COM backend is guarded by _WIN32/_MSC_VER and degrades to a
// "null" renderer (which only counts rendered samples) when no audio device
// is present, so the pipeline is fully testable headless.
// ============================================================================
class WasapiAudioPipeline final : public IAudioPipeline {
public:
    WasapiAudioPipeline() = default;
    ~WasapiAudioPipeline() override { Shutdown(); }

    // --- IAudioPipeline ---
    [[nodiscard]] Common::Status Start(const Common::AudioOutputFormat& format) override;
    [[nodiscard]] Common::Status Reconfigure(const Common::AudioOutputFormat& format) override;
    [[nodiscard]] Common::Status WriteAudio(AudioLane lane, Common::ByteSpan pcmData) override;
    void SetLaneVolume(AudioLane lane, float volume) override;
    void SetDuckingRatio(float ratio) override;
    [[nodiscard]] AudioCapabilities QueryCapabilities() const override;
    [[nodiscard]] uint64_t GetRenderedSampleCount() const noexcept override;
    void StopLane(AudioLane lane) override;
    void Shutdown() override;

    // --- Software mixer (platform-independent, testable) ---
    // Feed float32 samples (interleaved, `sampleCount` total) for a lane.
    // Applies the lane volume and, for MainMedia, the ducking ratio, then
    // accumulates into the lane's mix buffer.
    void PushLaneSamples(AudioLane lane, const float* samples, size_t sampleCount);

    // Unlocked variant; callers must already hold mutex_.
    void PushLaneSamplesLocked(AudioLane lane, const float* samples, size_t sampleCount);

    // Sum all three lanes element-wise and apply the soft limiter; returns the
    // mixed float32 output (interleaved, `sampleCount` total).
    [[nodiscard]] std::vector<float> RenderMixed(size_t sampleCount);

    // Clear the per-lane accumulation buffers.
    void ClearMixBuffers();

    // Soft-limiter threshold (linear, 0.0-1.0). Output is bounded by this.
    void SetLimiterThreshold(float threshold) { limiterThreshold_ = threshold; }
    [[nodiscard]] float GetLimiterThreshold() const noexcept { return limiterThreshold_; }

    // --- Resampler (platform-independent, testable) ---
    // Linear-interpolation multi-rate resampler. `src` is `srcCount` float32
    // samples at `srcRate`; returns `dstCount` samples at `dstRate`.
    [[nodiscard]] static std::vector<float> ResampleLinear(const float* src, size_t srcCount,
                                                           uint32_t srcRate, uint32_t dstRate,
                                                           size_t dstCount);

    // --- Inspection / test helpers ---
    [[nodiscard]] bool IsRunning() const noexcept { return running_; }
    [[nodiscard]] bool IsWasapiActive() const noexcept { return wasapiActive_.load(); }
    [[nodiscard]] float GetLaneVolume(AudioLane lane) const;
    [[nodiscard]] float GetDuckingRatio() const noexcept { return duckingRatio_.load(); }
    [[nodiscard]] uint32_t GetReconfigureCount() const;
    [[nodiscard]] uint32_t GetLaneStopCount(AudioLane lane) const;
    [[nodiscard]] uint32_t GetLimiterClampCount() const noexcept { return limiterClampCount_.load(); }
    [[nodiscard]] Common::AudioOutputFormat GetCurrentFormat() const;
    void SetCapabilities(const AudioCapabilities& caps);

private:
    // Convert a raw PCM byte span (per the current format) to float32 samples.
    [[nodiscard]] std::vector<float> PcmToFloat(Common::ByteSpan pcm) const;
    // Apply the soft limiter to a sample; returns the clamped value and reports
    // whether clamping occurred.
    [[nodiscard]] float ApplySoftLimiter(float x, bool& clamped) const;

    mutable std::recursive_mutex mutex_;
    bool running_{false};
    Common::AudioOutputFormat format_{};
    AudioCapabilities capabilities_{};
    std::atomic<float> duckingRatio_{1.0f};
    std::unordered_map<AudioLane, float> laneVolumes_;
    std::unordered_map<AudioLane, uint32_t> laneStopCounts_;
    std::atomic<uint64_t> renderedSampleCount_{0};
    uint32_t reconfigureCount_{0};

    // 3-lane mix accumulation buffers (float32, interleaved).
    std::vector<float> laneBuffers_[3]{};

    // Soft-limiter state.
    float limiterThreshold_{0.95f};
    std::atomic<uint32_t> limiterClampCount_{0};

    // WASAPI backend state (guarded).
    std::atomic<bool> wasapiActive_{false};
    void* wasapiClient_{nullptr};   // IAudioClient* (opaque; COM under _WIN32)
    void* wasapiRender_{nullptr};   // IAudioRenderClient*
    void* wasapiEvent_{nullptr};    // HANDLE (event)
    uint16_t mixChannels_{2};
    uint16_t mixBitsPerSample_{32};
    bool mixIsFloat_{false};
};

} // namespace CarPlayStation::Core::Media
