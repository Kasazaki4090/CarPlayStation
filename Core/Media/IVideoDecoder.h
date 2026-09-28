#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include <functional>

namespace CarPlayStation::Core::Media {

// Texture delivery callback: delivers D3D11 shared surface handle (or platform texture pointer) without coupling to HWND
using VideoFrameCallback = std::function<void(uintptr_t textureHandle, uint32_t width, uint32_t height, uint64_t ptsUs)>;

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    // Initialize hardware or software decoder for the negotiated stream spec (concrete facts only)
    [[nodiscard]] virtual Common::Status Init(const Common::VideoStreamSpec& spec) = 0;

    // Dynamically update the stream spec (e.g. resolution change after renegotiation)
    [[nodiscard]] virtual Common::Status Reconfigure(const Common::VideoStreamSpec& spec) = 0;

    // Feed raw NAL access unit / elementary stream frame
    [[nodiscard]] virtual Common::Status DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) = 0;

    // Set callback to receive decoded hardware texture frames
    virtual void SetFrameCallback(VideoFrameCallback callback) = 0;

    // Flush pending frames
    virtual void Flush() = 0;

    // Release decoder resources
    virtual void Shutdown() = 0;
};

} // namespace CarPlayStation::Core::Media
