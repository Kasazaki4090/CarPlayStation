#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include <cstdint>
#include <functional>
#include <mutex>
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

// CPU shared frame buffer: the decoder's render thread writes decoded BGRA
// pixels into this buffer (guarded by its mutex) and bumps a frame counter;
// the presentation thread (renderer) copies the latest frame out and uploads
// it to a local D3D11 texture on its own immediate context. This avoids all
// cross-context / cross-device GPU synchronization (keyed mutex, deferred
// contexts, shared textures) that is fragile and driver-dependent.
struct CpuFrameBuffer {
    std::mutex mutex;
    std::vector<uint8_t> data;   // BGRA (B8G8R8A8), tightly packed (stride == width*4)
    uint32_t width{0};
    uint32_t height{0};
    uint64_t frame_counter{0};   // bumped on every new frame
};

// CPU frame delivery callback: hands the shared CPU frame buffer to the
// presentation layer. The renderer copies the latest frame under the buffer's
// mutex and uploads it to a local texture.
using CpuFrameCallback = std::function<void(CpuFrameBuffer* buffer)>;

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

    // Optional: set a callback that receives the shared CPU frame buffer.
    // Decoders that present via a CPU shared buffer (e.g. MpvVideoPlayer)
    // override this; the default is a no-op.
    virtual void SetCpuFrameCallback(CpuFrameCallback callback) { (void)callback; }

    // Query physical decoder capabilities (supported codecs, max resolution/fps, HW acceleration)
    [[nodiscard]] virtual VideoCapabilities QueryCapabilities() const = 0;

    // Return the total number of successfully decoded and presented frames
    [[nodiscard]] virtual uint64_t GetDecodedFrameCount() const noexcept = 0;

    // Flush pending frames
    virtual void Flush() = 0;

    // Release decoder resources
    virtual void Shutdown() = 0;

    // Optional: register a callback invoked when the decoder detects a stall
    // (e.g. its input buffer stays full) and needs the upstream demuxer to drop
    // to the next IDR keyframe. Default is a no-op; decoders that support
    // self-recovery override this.
    virtual void SetResyncCallback(std::function<void()> callback) { (void)callback; }

    // Optional: inject the presentation device so the decoder's shared texture is
    // created on the SAME D3D11 device the renderer samples from. Cross-device
    // shared textures in the same process are not coherent, so the reader would
    // never see the writer's updates. Must be called before Init(). Default is a
    // no-op; decoders that create their own D3D11 device override this.
    virtual void SetSharedDevice(void* device, void* context) { (void)device; (void)context; }

    // Whether the decoder currently has hardware acceleration active
    [[nodiscard]] virtual bool IsHardwareAccelerated() const { return false; }
};

} // namespace CarPlayStation::Core::Media

