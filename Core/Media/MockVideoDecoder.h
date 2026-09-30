#pragma once

#include "Core/Media/IVideoDecoder.h"
#include <mutex>
#include <vector>

namespace CarPlayStation::Core::Media {

class MockVideoDecoder final : public IVideoDecoder {
public:
    MockVideoDecoder() = default;
    ~MockVideoDecoder() override {
        Shutdown();
    }

    [[nodiscard]] Common::Status Init(const Common::VideoStreamSpec& spec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (simulate_init_failure_) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Simulated init failure");
        }
        current_spec_ = spec;
        initialized_ = true;
        return Common::Status::Ok();
    }

    [[nodiscard]] Common::Status Reconfigure(const Common::VideoStreamSpec& spec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
        }
        current_spec_ = spec;
        reconfigure_count_++;
        return Common::Status::Ok();
    }

    [[nodiscard]] Common::Status DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
        }
        if (simulate_decode_failure_) {
            return Common::MakeError(Common::StatusCode::MediaPipelineError, "Simulated decode error");
        }

        decoded_frame_count_++;
        total_decoded_bytes_ += nalData.size();
        last_pts_us_ = ptsUs;
        pts_history_.push_back(ptsUs);

        if (frame_callback_) {
            frame_callback_(simulated_texture_handle_, current_spec_.width, current_spec_.height, ptsUs);
        }
        return Common::Status::Ok();
    }

    void SetFrameCallback(VideoFrameCallback callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_callback_ = std::move(callback);
    }

    [[nodiscard]] VideoCapabilities QueryCapabilities() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return capabilities_;
    }

    [[nodiscard]] uint64_t GetDecodedFrameCount() const noexcept override {
        return decoded_frame_count_.load();
    }

    void Flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        flush_count_++;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = false;
    }

    // Test inspection helpers
    [[nodiscard]] bool IsInitialized() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initialized_;
    }

    [[nodiscard]] Common::VideoStreamSpec GetCurrentSpec() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_spec_;
    }

    [[nodiscard]] uint64_t GetTotalDecodedBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_decoded_bytes_;
    }

    [[nodiscard]] uint64_t GetLastPtsUs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_pts_us_;
    }

    [[nodiscard]] std::vector<uint64_t> GetPtsHistory() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pts_history_;
    }

    [[nodiscard]] uint32_t GetReconfigureCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reconfigure_count_;
    }

    [[nodiscard]] uint32_t GetFlushCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return flush_count_;
    }

    void SetSimulatedTextureHandle(uintptr_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        simulated_texture_handle_ = handle;
    }

    void SetSimulateInitFailure(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        simulate_init_failure_ = fail;
    }

    void SetSimulateDecodeFailure(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        simulate_decode_failure_ = fail;
    }

    void SetCapabilities(const VideoCapabilities& caps) {
        std::lock_guard<std::mutex> lock(mutex_);
        capabilities_ = caps;
    }

private:
    mutable std::mutex mutex_;
    bool initialized_{false};
    Common::VideoStreamSpec current_spec_{};
    VideoCapabilities capabilities_{};
    VideoFrameCallback frame_callback_{};
    std::atomic<uint64_t> decoded_frame_count_{0};
    uint64_t total_decoded_bytes_{0};
    uint64_t last_pts_us_{0};
    std::vector<uint64_t> pts_history_{};
    uint32_t reconfigure_count_{0};
    uint32_t flush_count_{0};
    uintptr_t simulated_texture_handle_{0xDEADBEEF};
    bool simulate_init_failure_{false};
    bool simulate_decode_failure_{false};
};

} // namespace CarPlayStation::Core::Media

