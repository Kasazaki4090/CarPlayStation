#include "Core/Media/AirPlayVideoDemuxer.h"
#include "Core/Common/Logging.h"

namespace CarPlayStation::Core::Media {

namespace {

constexpr uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};

// H.264 NAL unit type is the low 5 bits of the first byte (forbidden_zero,
// nal_ref_id, nal_unit_type).
[[nodiscard]] NalUnitType ClassifyH264(uint8_t firstByte) {
    const uint8_t type = firstByte & 0x1F;
    switch (type) {
        case 1:  return NalUnitType::SliceNonIdr;
        case 5:  return NalUnitType::SliceIdr;
        case 6:  return NalUnitType::SEI;
        case 7:  return NalUnitType::SPS;
        case 8:  return NalUnitType::PPS;
        case 9:  return NalUnitType::AUD;
        case 10: return NalUnitType::EndOfSequence;
        case 11: return NalUnitType::EndOfStream;
        default: return NalUnitType::Unspecified;
    }
}

// HEVC NAL unit type is the low 6 bits of the first byte.
[[nodiscard]] NalUnitType ClassifyHevc(uint8_t firstByte) {
    const uint8_t type = firstByte & 0x3F;
    switch (type) {
        case 19: return NalUnitType::HevcIdr;
        case 20: return NalUnitType::HevcIdrNoRadl;
        case 21: return NalUnitType::HevcCRA;
        case 32: return NalUnitType::VPS;
        case 33: return NalUnitType::SPS;
        case 34: return NalUnitType::PPS;
        case 16: return NalUnitType::SliceNonIdr; // HEVC VCL non-IDR
        case 1:  return NalUnitType::SliceNonIdr;  // HEVC VCL IDR (type 1)
        default: return NalUnitType::Unspecified;
    }
}

} // namespace

NalUnitType AirPlayVideoDemuxer::ClassifyNalu(const uint8_t* nalu, size_t size, Common::VideoCodec codec) {
    if (size == 0 || nalu == nullptr) {
        return NalUnitType::Unspecified;
    }
    (void)nalu;
    if (codec == Common::VideoCodec::HEVC) {
        return ClassifyHevc(nalu[0]);
    }
    return ClassifyH264(nalu[0]);
}

Common::Status AirPlayVideoDemuxer::FeedPayload(Common::ByteSpan payload, uint64_t ptsUs) {
    if (payload.size() < 4) {
        return Common::MakeError(Common::StatusCode::InvalidArgument,
                                 "AirPlay video payload too small (need >= 4 bytes for length prefix)");
    }

    DemuxedFrame frame;
    frame.ptsUs = ptsUs;

    // Snapshot the codec under the lock (used for NALU classification).
    Common::VideoCodec codec;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        codec = stream_codec_;
    }

    // Parse the length-prefixed NALU stream, classify each NALU, and build
    // the Annex-B access unit in a single pass. Parameter sets (VPS/SPS/PPS)
    // are collected separately so they can be ordered before slice data.
    size_t offset = 0;
    bool truncated = false;
    bool hasIdrSlice = false;
    Common::ByteBuffer frameParamSets; // VPS/SPS/PPS from this frame, in stream order
    Common::ByteBuffer sliceData;      // slice / SEI / other NALUs
    while (offset + 4 <= payload.size()) {
        const uint32_t naluLen = Common::Endian::ReadBe32(payload.data() + offset);
        offset += 4;
        if (naluLen == 0 || offset + naluLen > payload.size()) {
            truncated = true;
            break;
        }
        const uint8_t* naluPtr = payload.data() + offset;
        const NalUnitType type = ClassifyNalu(naluPtr, naluLen, codec);

        if (frame.firstNaluType == NalUnitType::Unspecified) {
            frame.firstNaluType = type;
        }
        if (type == NalUnitType::SliceIdr ||
            type == NalUnitType::HevcIdr ||
            type == NalUnitType::HevcIdrNoRadl ||
            type == NalUnitType::HevcCRA) {
            hasIdrSlice = true;
        }

        const bool isParamSet = (type == NalUnitType::VPS ||
                                 type == NalUnitType::SPS ||
                                 type == NalUnitType::PPS);
        Common::ByteBuffer& target = isParamSet ? frameParamSets : sliceData;
        target.insert(target.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
        target.insert(target.end(), naluPtr, naluPtr + naluLen);

        // Store the latest parameter sets in Annex-B form (start-code
        // prefixed) so they can be prepended directly to keyframes.
        switch (type) {
            case NalUnitType::VPS:
                frame.containsVps = true;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_vps_.assign(kAnnexBStartCode, kAnnexBStartCode + 4);
                    latest_vps_.insert(latest_vps_.end(), naluPtr, naluPtr + naluLen);
                }
                break;
            case NalUnitType::SPS:
                frame.containsSps = true;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_sps_.assign(kAnnexBStartCode, kAnnexBStartCode + 4);
                    latest_sps_.insert(latest_sps_.end(), naluPtr, naluPtr + naluLen);
                }
                break;
            case NalUnitType::PPS:
                frame.containsPps = true;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_pps_.assign(kAnnexBStartCode, kAnnexBStartCode + 4);
                    latest_pps_.insert(latest_pps_.end(), naluPtr, naluPtr + naluLen);
                }
                break;
            default:
                break;
        }
        offset += naluLen;
    }

    // Determine keyframe status: a frame is a random-access point if it
    // contains an IDR slice (H.264 type 5 or HEVC 19/20/21), even when
    // parameter sets precede the slice in the payload.
    const bool isIdr = hasIdrSlice;
    frame.isKeyFrame = isIdr;

    // Frame-loss / resync policy:
    //  - If we are awaiting a keyframe and this is not one, drop it.
    //  - If the payload was truncated, drop it and request a resync.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (awaiting_keyframe_ && !isIdr) {
            dropped_frame_count_++;
            CPS_LOG_DEBUG("AirPlayVideoDemuxer", "Dropping non-IDR frame while awaiting keyframe");
            return Common::Status::Ok();
        }
        if (truncated) {
            dropped_frame_count_++;
            awaiting_keyframe_ = true;
            resync_count_++;
            CPS_LOG_WARN("AirPlayVideoDemuxer", "Truncated payload detected; resyncing to next IDR");
            return Common::Status::Ok();
        }
        if (awaiting_keyframe_ && isIdr) {
            awaiting_keyframe_ = false;
        }
    }

    // Assemble the final Annex-B access unit. Parameter sets must precede
    // slice data. For keyframes that do NOT carry their own parameter sets,
    // prepend the latest known VPS/SPS/PPS so the decoder always has complete
    // stream context.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isIdr) {
            Common::ByteBuffer prefix;
            // The stored parameter sets are already Annex-B encoded
            // (start-code prefixed), so append them verbatim.
            auto appendSet = [&](const Common::ByteBuffer& set) {
                prefix.insert(prefix.end(), set.begin(), set.end());
            };
            if (!frame.containsVps && !latest_vps_.empty()) appendSet(latest_vps_);
            if (!frame.containsSps && !latest_sps_.empty()) appendSet(latest_sps_);
            if (!frame.containsPps && !latest_pps_.empty()) appendSet(latest_pps_);
            if (!prefix.empty()) {
                Common::ByteBuffer combined;
                combined.reserve(prefix.size() + frameParamSets.size());
                combined.insert(combined.end(), prefix.begin(), prefix.end());
                combined.insert(combined.end(), frameParamSets.begin(), frameParamSets.end());
                frameParamSets = std::move(combined);
            }
        }
    }
    frame.annexBData.reserve(frameParamSets.size() + sliceData.size());
    frame.annexBData.insert(frame.annexBData.end(), frameParamSets.begin(), frameParamSets.end());
    frame.annexBData.insert(frame.annexBData.end(), sliceData.begin(), sliceData.end());

    delivered_frame_count_++;

    FrameCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = frame_callback_;
    }
    if (cb) {
        cb(frame);
    }
    return Common::Status::Ok();
}

} // namespace CarPlayStation::Core::Media
