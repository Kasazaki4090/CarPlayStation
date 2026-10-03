#pragma once

#include "Core/Media/IVideoDecoder.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include <mutex>
#include <atomic>
#include <memory>

// Windows COM / Media Foundation / D3D11 forward declarations.
// The .cpp includes the full headers; the header only needs the opaque types
// so the public surface stays lightweight.
struct IMFTransform;
struct IMFMediaBuffer;
struct IMFMediaType;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGIDevice;
struct IDXGIResource;
struct ID3D11Texture2D;

namespace CarPlayStation::Core::Media {

// Windows Media Foundation hardware video decoder (H.264 / HEVC).
//
//  * Uses the Media Foundation Transform (MFT) hardware decoder with
//    CODECAPI_AVLowLatencyMode = TRUE for low-latency presentation.
//  * Renders decoded frames to D3D11 textures and exposes the shared surface
//    handle (IDXGIResource::GetSharedHandle) to the presentation layer via
//    the IVideoDecoder::VideoFrameCallback.
//  * Zero-overhead GPU viewport letterboxing during window dragging: the
//    decoder never triggers stream renegotiation on resize; the presentation
//    layer scales the D3D11 texture with aspect preservation (letterbox /
//    pillarbox) or stretch-to-fill.
//  * Debounced renegotiation trigger: Reconfigure() is only honored when the
//    stream spec actually changes (resolution / codec / fps), and is debounced
//    to avoid rapid-fire renegotiation during window drag.
class MfVideoDecoder final : public IVideoDecoder {
public:
    MfVideoDecoder();
    ~MfVideoDecoder() override;

    MfVideoDecoder(const MfVideoDecoder&) = delete;
    MfVideoDecoder& operator=(const MfVideoDecoder&) = delete;

    [[nodiscard]] Common::Status Init(const Common::VideoStreamSpec& spec) override;
    [[nodiscard]] Common::Status Reconfigure(const Common::VideoStreamSpec& spec) override;
    [[nodiscard]] Common::Status DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) override;
    void SetFrameCallback(VideoFrameCallback callback) override;
    [[nodiscard]] VideoCapabilities QueryCapabilities() const override;
    [[nodiscard]] uint64_t GetDecodedFrameCount() const noexcept override;
    void Flush() override;
    void Shutdown() override;

    // Debounce window (ms) for Reconfigure() calls. Rapid calls within this
    // window are coalesced into a single renegotiation.
    void SetReconfigureDebounceMs(uint32_t ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        reconfigure_debounce_ms_ = ms;
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
    [[nodiscard]] uint32_t GetReconfigureCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reconfigure_count_;
    }
    [[nodiscard]] uint32_t GetDebouncedReconfigureCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return debounced_reconfigure_count_;
    }
    [[nodiscard]] uintptr_t GetSharedHandle() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shared_handle_;
    }

private:
    [[nodiscard]] Common::Status CreateMft(const Common::VideoStreamSpec& spec);
    [[nodiscard]] Common::Status ConfigureD3D11SharedSurfaces();
    [[nodiscard]] Common::Status ProcessInput(Common::ByteSpan nalData, uint64_t ptsUs);
    [[nodiscard]] Common::Status DrainOutput();
    void ReleaseComPointers();

    mutable std::mutex mutex_;
    bool initialized_{false};
    bool hardware_accelerated_{false};
    Common::VideoStreamSpec current_spec_{};
    VideoFrameCallback frame_callback_{};
    std::atomic<uint64_t> decoded_frame_count_{0};
    uint32_t reconfigure_count_{0};
    uint32_t debounced_reconfigure_count_{0};
    uint32_t reconfigure_debounce_ms_{100};
    uint64_t last_reconfigure_tick_ms_{0};

    // COM pointers (owned by this class; released in Shutdown).
    IMFTransform* mft_{nullptr};
    ID3D11Device* d3d_device_{nullptr};
    ID3D11DeviceContext* d3d_context_{nullptr};
    IDXGIDevice* dxgi_device_{nullptr};
    IDXGIResource* shared_resource_{nullptr};
    ID3D11Texture2D* shared_texture_{nullptr};
    uintptr_t shared_handle_{0};
    uint32_t output_buffer_size_{0};
    std::vector<uint8_t> bgra_buffer_;
};

} // namespace CarPlayStation::Core::Media
