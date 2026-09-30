#pragma once

#include "Core/Media/IMicrophoneUplink.h"
#include <mutex>
#include <atomic>

namespace CarPlayStation::Core::Media {

class MockMicrophoneUplink final : public IMicrophoneUplink {
public:
    MockMicrophoneUplink() = default;
    ~MockMicrophoneUplink() override {
        Shutdown();
    }

    [[nodiscard]] Common::Status Start(const Common::AudioOutputFormat& format) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (simulate_start_failure_) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Simulated mic start failure");
        }
        format_ = format;
        capturing_ = true;
        return Common::Status::Ok();
    }

    void SetUplinkCallback(UplinkAudioCallback callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        uplink_callback_ = std::move(callback);
    }

    [[nodiscard]] Common::Status Stop() override {
        std::lock_guard<std::mutex> lock(mutex_);
        capturing_ = false;
        return Common::Status::Ok();
    }

    [[nodiscard]] bool IsCapturing() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return capturing_;
    }

    [[nodiscard]] MicrophoneCapabilities QueryCapabilities() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return capabilities_;
    }

    void SetInputGain(float gain) override {
        std::lock_guard<std::mutex> lock(mutex_);
        input_gain_ = gain;
    }

    [[nodiscard]] uint64_t GetCapturedSampleCount() const noexcept override {
        return captured_sample_count_.load();
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        capturing_ = false;
    }

    // Test simulation & inspection helpers
    void InjectAudioData(Common::ByteSpan pcmData, uint64_t timestampUs) {
        UplinkAudioCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!capturing_) return;

            size_t bytesPerSample = (format_.bit_depth / 8) * format_.channels;
            if (bytesPerSample > 0) {
                captured_sample_count_ += (pcmData.size() / bytesPerSample);
            }
            cb = uplink_callback_;
            callback_dispatch_count_++;
        }
        if (cb) {
            cb(pcmData, timestampUs);
        }
    }

    [[nodiscard]] Common::AudioOutputFormat GetCurrentFormat() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return format_;
    }

    [[nodiscard]] float GetInputGain() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return input_gain_;
    }

    [[nodiscard]] uint32_t GetCallbackDispatchCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return callback_dispatch_count_;
    }

    void SetSimulateStartFailure(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        simulate_start_failure_ = fail;
    }

    void SetCapabilities(const MicrophoneCapabilities& caps) {
        std::lock_guard<std::mutex> lock(mutex_);
        capabilities_ = caps;
    }

private:
    mutable std::mutex mutex_;
    bool capturing_{false};
    Common::AudioOutputFormat format_{};
    MicrophoneCapabilities capabilities_{};
    UplinkAudioCallback uplink_callback_{};
    float input_gain_{1.0f};
    std::atomic<uint64_t> captured_sample_count_{0};
    uint32_t callback_dispatch_count_{0};
    bool simulate_start_failure_{false};
};

} // namespace CarPlayStation::Core::Media
