#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include <functional>
#include <vector>
#include <string_view>

namespace CarPlayStation::Core::Media {

// Standard H.264 / HEVC NAL Unit classification
enum class NalUnitType : uint8_t {
    Unspecified     = 0,
    SliceNonIdr     = 1,
    SliceIdr        = 5,
    SEI             = 6,
    SPS             = 7,
    PPS             = 8,
    AUD             = 9,
    EndOfSequence   = 10,
    EndOfStream     = 11,
    VPS             = 32, // HEVC Video Parameter Set (NAL type 32)
    HevcIdr         = 19, // HEVC IDR_W_RADL
    HevcIdrNoRadl   = 20, // HEVC IDR_N_LP
    HevcCRA         = 21  // HEVC CRA_NUT (Clean Random Access)
};

[[nodiscard]] constexpr std::string_view NalUnitTypeToString(NalUnitType type) noexcept {
    switch (type) {
        case NalUnitType::Unspecified:   return "Unspecified";
        case NalUnitType::SliceNonIdr:   return "SliceNonIdr";
        case NalUnitType::SliceIdr:      return "SliceIdr";
        case NalUnitType::SEI:           return "SEI";
        case NalUnitType::SPS:           return "SPS";
        case NalUnitType::PPS:           return "PPS";
        case NalUnitType::AUD:           return "AUD";
        case NalUnitType::EndOfSequence: return "EndOfSequence";
        case NalUnitType::EndOfStream:   return "EndOfStream";
        case NalUnitType::VPS:           return "VPS";
        case NalUnitType::HevcIdr:       return "HevcIdr";
        case NalUnitType::HevcIdrNoRadl: return "HevcIdrNoRadl";
        case NalUnitType::HevcCRA:       return "HevcCRA";
    }
    return "Unknown";
}

// Extensible hardware and decoder capabilities discovery
struct VideoCapabilities {
    std::vector<Common::VideoCodec> supported_codecs{Common::VideoCodec::H264, Common::VideoCodec::HEVC};
    uint32_t max_width{3840};
    uint32_t max_height{2160};
    uint32_t max_fps{60};
    bool hardware_accelerated{true};
    bool supports_low_latency{true};
};

// Texture delivery callback: delivers D3D11 shared surface handle (or platform texture pointer) without coupling to HWND
using VideoFrameCallback = std::function<void(uintptr_t textureHandle, uint32_t width, uint32_t height, uint64_t ptsUs)>;

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    // Initialize hardware or software decoder for the negotiated stream spec (concrete facts only)
    [[nodiscard]] virtual Common::Status Init(const Common::VideoStreamSpec& spec) = 0;

    // Dynamically update the stream spec (e.g. resolution/fps change after renegotiation or fullscreen toggle)
    [[nodiscard]] virtual Common::Status Reconfigure(const Common::VideoStreamSpec& spec) = 0;

    // Feed raw NAL access unit / elementary stream frame
    [[nodiscard]] virtual Common::Status DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) = 0;

    // Set callback to receive decoded hardware texture frames (D3D11 shared handle)
    virtual void SetFrameCallback(VideoFrameCallback callback) = 0;

    // Query physical decoder capabilities (supported codecs, max resolution/fps, HW acceleration)
    [[nodiscard]] virtual VideoCapabilities QueryCapabilities() const = 0;

    // Return the total number of successfully decoded and presented frames
    [[nodiscard]] virtual uint64_t GetDecodedFrameCount() const noexcept = 0;

    // Flush pending frames
    virtual void Flush() = 0;

    // Release decoder resources
    virtual void Shutdown() = 0;
};

} // namespace CarPlayStation::Core::Media

