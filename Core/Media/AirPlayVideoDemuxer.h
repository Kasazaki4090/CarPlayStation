#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include "Core/Media/IVideoDecoder.h"
#include <functional>
#include <mutex>
#include <vector>

namespace CarPlayStation::Core::Media {

// A fully assembled access unit (frame) ready for the decoder.
// annexBData is Annex-B encoded (00 00 00 01 start codes) and, for keyframes,
// is prefixed with the latest known VPS/SPS/PPS parameter sets so the decoder
// always has complete stream context.
struct DemuxedFrame {
    Common::ByteBuffer annexBData{};
    uint64_t ptsUs{0};
    bool isKeyFrame{false};
    bool containsVps{false};
    bool containsSps{false};
    bool containsPps{false};
    NalUnitType firstNaluType{NalUnitType::Unspecified};
};

// AirPlay video payload demuxer.
//
// AirPlay H.264/HEVC video payloads carry NAL units as a sequence of
// 4-byte Big-Endian length-prefixed NALUs (STAP-A style), NOT Annex-B.
// This demuxer:
//   * parses the length-prefixed NALU stream and re-encodes it to Annex-B
//     (00 00 00 01 start codes) for the decoder,
//   * identifies SPS / PPS / VPS parameter sets and IDR keyframes,
//   * supports non-standard ultrawide aspect ratios (1920x720, 2560x720, ...)
//     by carrying the stream geometry through to the decoder spec,
//   * detects frame loss (truncated / parameter-set-only payloads) and
//     resynchronizes to the next IDR keyframe.
class AirPlayVideoDemuxer {
public:
    using FrameCallback = std::function<void(const DemuxedFrame&)>;

    AirPlayVideoDemuxer() = default;
    ~AirPlayVideoDemuxer() = default;

    AirPlayVideoDemuxer(const AirPlayVideoDemuxer&) = delete;
    AirPlayVideoDemuxer& operator=(const AirPlayVideoDemuxer&) = delete;

    // Configure the expected stream geometry (e.g. 1920x720 ultrawide) and
    // codec. Purely informational for the demuxer; the decoder is configured
    // via IVideoDecoder::Init/Reconfigure with the same spec.
    void SetStreamGeometry(uint32_t width, uint32_t height, Common::VideoCodec codec = Common::VideoCodec::H264) {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_width_ = width;
        stream_height_ = height;
        stream_codec_ = codec;
    }

    // Feed one raw AirPlay video payload (4-byte BE length-prefixed NALUs).
    // On success the assembled access unit is delivered to the frame callback.
    [[nodiscard]] Common::Status FeedPayload(Common::ByteSpan payload, uint64_t ptsUs);

    // Set the callback that receives completed access units.
    void SetFrameCallback(FrameCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_callback_ = std::move(callback);
    }

    // Explicitly request a resync: all non-IDR frames are dropped until the
    // next IDR keyframe arrives.
    void RequestResync() {
        std::lock_guard<std::mutex> lock(mutex_);
        awaiting_keyframe_ = true;
    }

    // Latest parameter sets observed in the stream (Annex-B encoded).
    [[nodiscard]] Common::ByteBuffer GetLatestSps() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_sps_;
    }
    [[nodiscard]] Common::ByteBuffer GetLatestPps() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_pps_;
    }
    [[nodiscard]] Common::ByteBuffer GetLatestVps() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_vps_;
    }

    // Stream statistics
    [[nodiscard]] uint64_t GetDroppedFrameCount() const noexcept { return dropped_frame_count_.load(); }
    [[nodiscard]] uint64_t GetResyncCount() const noexcept { return resync_count_.load(); }
    [[nodiscard]] uint64_t GetDeliveredFrameCount() const noexcept { return delivered_frame_count_.load(); }
    [[nodiscard]] uint32_t GetStreamWidth() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stream_width_;
    }
    [[nodiscard]] uint32_t GetStreamHeight() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stream_height_;
    }

    // Reset all state (parameter sets, counters, resync flag).
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_sps_.clear();
        latest_pps_.clear();
        latest_vps_.clear();
        awaiting_keyframe_ = false;
        dropped_frame_count_ = 0;
        resync_count_ = 0;
        delivered_frame_count_ = 0;
    }

private:
    // Classify the first NALU byte of a NAL unit (H.264 or HEVC).
    [[nodiscard]] static NalUnitType ClassifyNalu(const uint8_t* nalu, size_t size, Common::VideoCodec codec);

    mutable std::mutex mutex_;
    FrameCallback frame_callback_{};
    Common::ByteBuffer latest_sps_{};
    Common::ByteBuffer latest_pps_{};
    Common::ByteBuffer latest_vps_{};
    bool awaiting_keyframe_{false};
    uint32_t stream_width_{1920};
    uint32_t stream_height_{1080};
    Common::VideoCodec stream_codec_{Common::VideoCodec::H264};
    std::atomic<uint64_t> dropped_frame_count_{0};
    std::atomic<uint64_t> resync_count_{0};
    std::atomic<uint64_t> delivered_frame_count_{0};
};

} // namespace CarPlayStation::Core::Media
