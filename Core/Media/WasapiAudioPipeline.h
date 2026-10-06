#pragma once

#include "Core/Media/IAudioPipeline.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <chrono>

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
    void SetTargetDuckingRatio(float ratio) override;
    void SetDuckingActive(bool active) override;
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

    // Drop `frames` frames (frames * channels samples) from the front of every
    // lane's mix queue. The render pump calls this once it has mixed a chunk
    // out to the endpoint, so the queues stay aligned frame-by-frame.
    void ConsumeLaneSamplesLocked(AudioLane lane, size_t frames);

    // Number of frames currently queued for a lane (mix queue depth).
    [[nodiscard]] size_t GetQueuedFrameCount(AudioLane lane) const;

    // Disable the WASAPI render pump (tests only). When disabled, the mix
    // queues are only touched by the caller, so the FIFO semantics can be
    // exercised deterministically on a machine that has an audio device.
    void SetRenderPumpEnabled(bool enabled) { pumpEnabled_ = enabled; }

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
    [[nodiscard]] float GetDuckingRatio() const noexcept { return targetDuckingRatio_.load(); }
    [[nodiscard]] float GetEffectiveDuckingRatio() const noexcept { return duckingRatio_.load(); }
    [[nodiscard]] bool IsDuckingActive() const noexcept { return isDucked_.load(); }
    [[nodiscard]] uint32_t GetReconfigureCount() const;
    [[nodiscard]] uint32_t GetLaneStopCount(AudioLane lane) const;
    [[nodiscard]] uint32_t GetLimiterClampCount() const noexcept { return limiterClampCount_.load(); }
    [[nodiscard]] uint64_t GetDroppedFrameCount() const noexcept { return droppedFrames_.load(); }
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
    // Dynamic voice-activity ducking helpers (called with mutex_ held).
    void UpdateDuckingStateLocked();
    // Decide-and-apply in one step, with mutex_ held. carplayd's Ducker keeps
    // its lock across the set_volume() side effect on purpose: deciding the
    // state and applying the gain must not interleave, or the two duck
    // triggers can apply their gains in reverse order and the reported state
    // stops matching what is actually audible.
    void ApplyDuckingGainLocked();

    // Voice-activity gate for the AltNavigation / SiriTelephony lanes. The
    // navigation stream stays open for the whole session and keeps sending
    // silence between prompts, so "the lane is carrying data" is not the
    // trigger - only an *audible* chunk is. carplayd measures this as an RMS of
    // 50 on the 16-bit full-scale range (duck.rs SILENCE_RMS_THRESHOLD); real
    // speech measures in the thousands, silence padding far below 50. We mix in
    // float32, so the same gate is 50/32767.
    static constexpr float kVoiceAudibleRms{50.0f / 32767.0f};
    // carplayd's NAV_RESTORE_AFTER_MS: how long after the last audible
    // navigation chunk the media lane is restored.
    static constexpr uint32_t kDuckingHangoverMs{1500};
    // Safety net for a duckAudio whose unduckAudio never arrives. Timed out
    // independently of the local detector so a stuck one cannot hold the media
    // lane quiet for the rest of the session. Kept well above a real prompt: the
    // phone's own duck windows in our logs run up to ~8.2 s for a long guidance
    // sequence, and cutting one short would bring the music back mid-sentence.
    static constexpr uint32_t kRtspDuckingSafetyMs{12000};

    std::atomic<float> targetDuckingRatio_{0.20f};
    std::atomic<bool> isDucked_{false};
    // The gain actually applied to MainMedia. Always either
    // targetDuckingRatio_ (ducked) or 1.0 (not ducked) - a hard step, matching
    // the reference, which applies DUCK_GAIN/UNDUCK_GAIN with no fade.
    std::atomic<float> duckingRatio_{1.0f};

    std::chrono::steady_clock::time_point lastVoiceActivityTime_{};
    bool voiceDuckingActive_{false};
    bool rtspDuckingActive_{false};
    std::chrono::steady_clock::time_point rtspDuckingStartTime_{};
    // Highest chunk RMS seen per lane since the last stats window, so the
    // audibility gate can be checked against what the phone really sends.
    float voiceLanePeakRms_[3]{};
    std::unordered_map<AudioLane, float> laneVolumes_;
    std::unordered_map<AudioLane, uint32_t> laneStopCounts_;
    std::atomic<uint64_t> renderedSampleCount_{0};
    uint32_t reconfigureCount_{0};

    // 3-lane mix accumulation queues (float32, interleaved). These double as
    // the playback FIFO: the render pump consumes them from the front at the
    // device clock rate.
    std::deque<float> laneBuffers_[3]{};
    // Hard cap on queued audio per lane (frames), so a stalled endpoint or a
    // burst from the phone cannot grow the queue without bound.
    uint32_t maxQueuedFrames_{96000};
    std::atomic<uint64_t> droppedFrames_{0};

    // Soft-limiter state.
    float limiterThreshold_{0.95f};
    std::atomic<uint32_t> limiterClampCount_{0};

    // WASAPI render pump. The endpoint is drained on the device clock; without
    // this thread the only thing that ever called GetBuffer/RenderMixed was an
    // arriving RTP packet, so anything that did not fit into the endpoint's
    // free space was silently thrown away (choppy playback).
    void PumpLoop();
    // Deepest queue across the lanes: the number of frames the mixer can
    // produce in one pass. Lanes with less data contribute silence.
    [[nodiscard]] size_t MixAvailableFramesLocked() const;
    void LogPumpStats();

    std::thread pumpThread_;
    std::atomic<bool> pumpStop_{false};
    bool pumpEnabled_{true};
    uint32_t mixRate_{48000};
    // Frames that must sit in the mix queues before the pump starts feeding the
    // endpoint, and again after the queues run dry. Without this the pump writes
    // whatever has just arrived, so any scheduling jitter becomes a gap in the
    // output (the "choppy playback" symptom).
    uint32_t prebufferFrames_{0};
    // Pump diagnostics (pump thread only).
    uint64_t pumpWrites_{0};
    uint64_t pumpFramesWritten_{0};
    uint64_t pumpUnderruns_{0};
    uint64_t peakQueuedFrames_{0};
    uint64_t maxPaddingFrames_{0};
    uint64_t statsClockMs_{0};
    uint64_t lastPumpFrames_{0};

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
