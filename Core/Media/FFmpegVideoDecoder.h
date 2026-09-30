#pragma once

#include "Core/Media/IVideoDecoder.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include <mutex>
#include <atomic>
#include <memory>
#include <string>

// Opaque FFmpeg types (forward-declared so the public surface stays
// lightweight and the core build remains zero-dependency when the FFmpeg
// runtime is absent). The .cpp resolves the real symbols via dynamic
// LoadLibrary / dlopen binding.
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVCodec;
struct AVBufferRef;
struct SwsContext;

namespace CarPlayStation::Core::Media {

// FFmpeg hardware / software fallback video decoder (H.264 / HEVC).
//
//  * Dynamic runtime binding (LoadLibrary on Windows / dlopen elsewhere) so
//    the core build stays zero-dependency when the FFmpeg DLL is not yet
//    present. If the runtime cannot be loaded, Init() reports NotSupported
//    and the pipeline falls back to the Media Foundation hardware decoder.
//  * d3d11va hardware decoding (bypasses the Windows Store HEVC package
//    requirement) with automatic CPU software fallback when no HW device is
//    available.
//  * Decoded frames are delivered to the presentation layer through the
//    IVideoDecoder::VideoFrameCallback.
class FFmpegVideoDecoder final : public IVideoDecoder {
public:
    FFmpegVideoDecoder();
    ~FFmpegVideoDecoder() override;

    FFmpegVideoDecoder(const FFmpegVideoDecoder&) = delete;
    FFmpegVideoDecoder& operator=(const FFmpegVideoDecoder&) = delete;

    [[nodiscard]] Common::Status Init(const Common::VideoStreamSpec& spec) override;
    [[nodiscard]] Common::Status Reconfigure(const Common::VideoStreamSpec& spec) override;
    [[nodiscard]] Common::Status DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) override;
    void SetFrameCallback(VideoFrameCallback callback) override;
    [[nodiscard]] VideoCapabilities QueryCapabilities() const override;
    [[nodiscard]] uint64_t GetDecodedFrameCount() const noexcept override;
    void Flush() override;
    void Shutdown() override;

    // Explicitly request the hardware (d3d11va) or software decode path.
    // When false, the decoder uses the CPU software path unconditionally.
    void SetPreferHardwareDecode(bool prefer) {
        std::lock_guard<std::mutex> lock(mutex_);
        prefer_hw_ = prefer;
    }

    // Test / inspection helpers
    [[nodiscard]] bool IsInitialized() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initialized_;
    }
    [[nodiscard]] bool IsHardwareAccelerated() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return hardware_accelerated_;
    }
    [[nodiscard]] bool IsRuntimeLoaded() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return runtime_loaded_;
    }
    [[nodiscard]] const std::string& GetRuntimeError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return runtime_error_;
    }

private:
    [[nodiscard]] bool LoadRuntime();
    void UnloadRuntime();
    [[nodiscard]] Common::Status OpenCodec();
    [[nodiscard]] Common::Status TryHardwareDevice();
    [[nodiscard]] Common::Status DecodeFrame(Common::ByteSpan nalData, uint64_t ptsUs);

    mutable std::mutex mutex_;
    bool initialized_{false};
    bool runtime_loaded_{false};
    bool hardware_accelerated_{false};
    bool prefer_hw_{true};
    std::string runtime_error_;
    Common::VideoStreamSpec current_spec_{};
    VideoFrameCallback frame_callback_{};
    std::atomic<uint64_t> decoded_frame_count_{0};

    // Opaque FFmpeg handles (owned by this class; freed in Shutdown).
    void* codec_ctx_{nullptr};
    void* hw_device_ctx_{nullptr};
    void* hw_frames_ctx_{nullptr};
    void* sws_ctx_{nullptr};
    void* codec_{nullptr};
    void* runtime_handle_{nullptr};
};

} // namespace CarPlayStation::Core::Media
