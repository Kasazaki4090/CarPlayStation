#pragma once

#include "Core/Media/IAudioPipeline.h"
#include <mutex>
#include <unordered_map>
#include <atomic>

namespace CarPlayStation::Core::Media {

class MockAudioPipeline final : public IAudioPipeline {
public:
    MockAudioPipeline() {
        lane_volumes_[AudioLane::MainMedia] = 1.0f;
        lane_volumes_[AudioLane::AltNavigation] = 1.0f;
        lane_volumes_[AudioLane::SiriTelephony] = 1.0f;
    }
    ~MockAudioPipeline() override {
        Shutdown();
    }

    [[nodiscard]] Common::Status Start(const Common::AudioOutputFormat& format) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (simulate_start_failure_) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Simulated audio start failure");
        }
        format_ = format;
        running_ = true;
        return Common::Status::Ok();
    }

    [[nodiscard]] Common::Status Reconfigure(const Common::AudioOutputFormat& format) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Audio pipeline not running");
        }
        format_ = format;
        reconfigure_count_++;
        return Common::Status::Ok();
    }

    [[nodiscard]] Common::Status WriteAudio(AudioLane lane, Common::ByteSpan pcmData) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Audio pipeline not running");
        }
        if (simulate_write_failure_) {
            return Common::MakeError(Common::StatusCode::IoError, "Simulated audio write failure");
        }

        lane_written_bytes_[lane] += pcmData.size();
        lane_write_calls_[lane]++;

        size_t bytesPerSample = (format_.bit_depth / 8) * format_.channels;
        if (bytesPerSample > 0) {
            uint64_t samples = pcmData.size() / bytesPerSample;
            rendered_sample_count_ += samples;
        }
        return Common::Status::Ok();
    }

    void SetLaneVolume(AudioLane lane, float volume) override {
        std::lock_guard<std::mutex> lock(mutex_);
        lane_volumes_[lane] = volume;
    }

    void SetDuckingRatio(float ratio) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ducking_ratio_ = ratio;
    }

    [[nodiscard]] AudioCapabilities QueryCapabilities() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return capabilities_;
    }

    [[nodiscard]] uint64_t GetRenderedSampleCount() const noexcept override {
        return rendered_sample_count_.load();
    }

    void StopLane(AudioLane lane) override {
        std::lock_guard<std::mutex> lock(mutex_);
        lane_stop_counts_[lane]++;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }

    // Test inspection helpers
    [[nodiscard]] bool IsRunning() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    [[nodiscard]] Common::AudioOutputFormat GetCurrentFormat() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return format_;
    }

    [[nodiscard]] float GetLaneVolume(AudioLane lane) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lane_volumes_.find(lane);
        return it != lane_volumes_.end() ? it->second : 0.0f;
    }

    [[nodiscard]] float GetDuckingRatio() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ducking_ratio_;
    }

    [[nodiscard]] size_t GetLaneWrittenBytes(AudioLane lane) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lane_written_bytes_.find(lane);
        return it != lane_written_bytes_.end() ? it->second : 0;
    }

    [[nodiscard]] uint32_t GetLaneWriteCalls(AudioLane lane) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lane_write_calls_.find(lane);
        return it != lane_write_calls_.end() ? it->second : 0;
    }

    [[nodiscard]] uint32_t GetLaneStopCount(AudioLane lane) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lane_stop_counts_.find(lane);
        return it != lane_stop_counts_.end() ? it->second : 0;
    }

    [[nodiscard]] uint32_t GetReconfigureCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reconfigure_count_;
    }

    void SetSimulateStartFailure(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        simulate_start_failure_ = fail;
    }

    void SetSimulateWriteFailure(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        simulate_write_failure_ = fail;
    }

    void SetCapabilities(const AudioCapabilities& caps) {
        std::lock_guard<std::mutex> lock(mutex_);
        capabilities_ = caps;
    }

private:
    mutable std::mutex mutex_;
    bool running_{false};
    Common::AudioOutputFormat format_{};
    AudioCapabilities capabilities_{};
    float ducking_ratio_{1.0f};
    std::unordered_map<AudioLane, float> lane_volumes_;
    std::unordered_map<AudioLane, size_t> lane_written_bytes_;
    std::unordered_map<AudioLane, uint32_t> lane_write_calls_;
    std::unordered_map<AudioLane, uint32_t> lane_stop_counts_;
    std::atomic<uint64_t> rendered_sample_count_{0};
    uint32_t reconfigure_count_{0};
    bool simulate_start_failure_{false};
    bool simulate_write_failure_{false};
};

} // namespace CarPlayStation::Core::Media
