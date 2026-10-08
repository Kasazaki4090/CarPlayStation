#pragma once
// MpvVideoPlayer — IVideoDecoder implementation backed by libmpv (mpv's embeddable
// C library). Replaces the in-process MFT decode+render pipeline that deadlocked
// under GPU contention (hardware MFT blocks on the renderer's shared texture;
// software MFT is too slow at 1280x720@60).
//
// Architecture:
//   - libmpv is loaded dynamically (LoadLibrary) from Lib/mpv/libmpv-2.dll.
//   - Decrypted H.264 Annex-B access units are fed to mpv via a custom read-only
//     stream protocol (mpv_stream_cb_add_ro) backed by a bounded ring buffer.
//     This avoids an HTTP server and keeps the data path in-process.
//   - mpv decodes (hardware-accelerated via its own D3D11 VA path, independent of
//     our renderer's D3D11 device, so no GPU contention with our swapchain).
//   - Decoded frames are rendered into a software buffer via the libmpv SW render
//     API (MPV_RENDER_API_TYPE_SW, format "bgr0" = B8G8R8A8), then uploaded into
//     a shared D3D11 texture owned by this class.
//   - The shared texture's HANDLE is passed to the frame callback, which the
//     D3D11Renderer opens via OpenSharedResource and draws into the swapchain.
//     The D2D overlay (HUD) composites on top, unchanged.
//
// Threading:
//   - DecodeAccessUnit (called from the AirPlayVideoReceiver TCP thread) appends
//     to the ring buffer and signals the mpv demuxer's stream-callback read.
//   - A dedicated render thread owns the mpv_render_context (created/destroyed on
//     that thread, as required by libmpv), polls for new frames, renders into the
//     SW buffer, uploads to the shared texture, and fires the frame callback.
//   - Shutdown: Shutdown() signals the render thread to exit, then calls
//     mpv_terminate_destroy (which joins mpv's internal threads) and destroys
//     the D3D11 shared texture. No deadlock: the render thread is the only
//     owner of the render context, and mpv_terminate_destroy is called after
//     the render thread has joined.

#include "Core/Media/IVideoDecoder.h"
#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#endif

// libmpv headers (from Lib/mpv/include/mpv). The DLL is loaded
// dynamically at runtime; only the headers are needed at compile time.
#include "mpv/client.h"
#include "mpv/render.h"
#include "mpv/stream_cb.h"

namespace CarPlayStation::Core::Media {

class MpvVideoPlayer final : public IVideoDecoder {
public:
    MpvVideoPlayer();
    ~MpvVideoPlayer() override;

    MpvVideoPlayer(const MpvVideoPlayer&) = delete;
    MpvVideoPlayer& operator=(const MpvVideoPlayer&) = delete;

    // IVideoDecoder
    [[nodiscard]] Common::Status Init(const Common::VideoStreamSpec& spec) override;
    [[nodiscard]] Common::Status Reconfigure(const Common::VideoStreamSpec& spec) override;
    [[nodiscard]] Common::Status DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) override;
    void SetFrameCallback(VideoFrameCallback callback) override;
    void SetCpuFrameCallback(CpuFrameCallback callback) override;
    void SetSharedDevice(void* device, void* context) override;
    [[nodiscard]] VideoCapabilities QueryCapabilities() const override;
    [[nodiscard]] uint64_t GetDecodedFrameCount() const noexcept override;
    void Flush() override;
    void Shutdown() override;

    // Test / inspection helpers
    [[nodiscard]] bool IsInitialized() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initialized_;
    }
    [[nodiscard]] bool IsHardwareAccelerated() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return hardware_accelerated_;
    }
    [[nodiscard]] uintptr_t GetSharedHandle() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shared_handle_;
    }

private:
    // libmpv stream-callback read (called on mpv's demuxer thread; blocks until
    // data is available or EOF). Must NOT call any libmpv API (deadlock).
    static int64_t StreamReadCallback(void* cookie, char* buf, uint64_t nbytes);
    // libmpv stream-callback open (called once when mpv opens the stream).
    static int StreamOpenCallback(void* user_data, char* uri, mpv_stream_cb_info* info);
    // libmpv stream-callback close (called when the stream is closed).
    static void StreamCloseCallback(void* cookie);

    // Render thread main loop.
    void RenderThreadMain();

    // Init() body with mutex_ already held (shared by Init and Reconfigure).
    [[nodiscard]] Common::Status InitLocked(const Common::VideoStreamSpec& spec);
    // Shutdown() body with mutex_ already held (shared by Shutdown and Reconfigure).
    void StopLocked();

    // D3D11 shared texture setup for handing decoded frames to the renderer.
    [[nodiscard]] Common::Status ConfigureD3D11SharedSurfaces();
    void ReleaseD3D11SharedSurfaces();

    // Upload the SW-rendered frame into the shared D3D11 texture.
    void UploadFrameToSharedTexture(const uint8_t* bgraData, uint32_t width, uint32_t height);

    // libmpv function pointers (loaded dynamically). Signatures mirror the
    // real mpv C API (see Lib/mpv/include/mpv/*.h).
    struct MpvApi {
        // client.h
        mpv_handle* (*mpv_create)();
        int (*mpv_initialize)(mpv_handle*);
        void (*mpv_terminate_destroy)(mpv_handle*);
        void (*mpv_destroy)(mpv_handle*);
        int (*mpv_set_option_string)(mpv_handle*, const char*, const char*);
        int (*mpv_command_string)(mpv_handle*, const char*);
        int (*mpv_stream_cb_add_ro)(mpv_handle*, const char*, void*,
                                     mpv_stream_cb_open_ro_fn);
        char* (*mpv_get_property_string)(mpv_handle*, const char*);
        void (*mpv_free)(void*);
        int (*mpv_request_log_messages)(mpv_handle*, const char*);
        mpv_event* (*mpv_wait_event)(mpv_handle*, double);
        // render.h
        int (*mpv_render_context_create)(mpv_render_context**, mpv_handle*,
                                         const mpv_render_param*);
        void (*mpv_render_context_free)(mpv_render_context*);
        void (*mpv_render_context_set_update_callback)(mpv_render_context*,
                                                       void (*)(void*), void*);
        uint64_t (*mpv_render_context_update)(mpv_render_context*);
        int (*mpv_render_context_render)(mpv_render_context*, const mpv_render_param*);
        void (*mpv_render_context_report_swap)(mpv_render_context*);
        bool loaded{false};
    };

    [[nodiscard]] bool LoadMpvLibrary();
    void UnloadMpvLibrary();

    // Ring buffer for the stream callback (bounded circular FIFO under stream_mutex_).
    // When full, the oldest data is dropped to maintain live streaming.
    static constexpr size_t kRingBufferSize = 8 * 1024 * 1024; // 8 MB
    std::vector<uint8_t> ring_buffer_;
    size_t ring_head_{0};
    size_t ring_size_{0};
    std::atomic<bool> stream_eof_{false};
    std::atomic<bool> stream_error_{false};
    std::mutex stream_mutex_;
    std::condition_variable stream_cv_;

    // mpv state
    MpvApi api_;
    mpv_handle* mpv_{nullptr};
    mpv_render_context* render_ctx_{nullptr};
    std::thread render_thread_;
    std::atomic<bool> render_thread_running_{false};
    std::atomic<bool> new_frame_available_{false};
    std::mutex render_mutex_;
    std::condition_variable render_cv_;
    std::atomic<bool> shutdown_requested_{false};

    // D3D11 shared texture
#ifdef _WIN32
    ID3D11Device* d3d_device_{nullptr};
    ID3D11DeviceContext* d3d_context_{nullptr};
    IDXGIDevice* dxgi_device_{nullptr};
    IDXGIResource* shared_resource_{nullptr};
    ID3D11Texture2D* shared_texture_{nullptr};
    IDXGIKeyedMutex* shared_keyed_mutex_{nullptr};
    uintptr_t shared_handle_{0};
    // When true, d3d_device_/d3d_context_ were injected by the UI layer via
    // SetSharedDevice (the renderer's device) and must NOT be released here.
    bool owns_d3d_device_{false};
    // Injected presentation device (set before Init via SetSharedDevice).
    ID3D11Device* injected_device_{nullptr};
    ID3D11DeviceContext* injected_context_{nullptr};
    // Deferred context created on the injected device for the writer thread.
    // The renderer's immediate context is owned by the UI thread (Render/Present);
    // sharing it across threads deadlocks the driver, so the writer uses its own
    // deferred context on the same device.
    ID3D11DeviceContext* d3d_deferred_context_{nullptr};
#endif

    // Software render buffer (64-byte aligned, B8G8R8A8)
    std::vector<uint8_t> sw_buffer_;
    size_t sw_stride_{0};

    // CPU shared frame buffer (the presentation path). The render thread
    // writes decoded BGRA pixels here (under cpu_frame_buffer_.mutex) and the
    // renderer copies the latest frame out and uploads it to a local texture.
    CpuFrameBuffer cpu_frame_buffer_;

    // Frame callback
    std::mutex callback_mutex_;
    VideoFrameCallback frame_callback_;
    CpuFrameCallback cpu_frame_callback_;

    // Stats
    std::atomic<uint64_t> decoded_frames_{0};
    std::atomic<uint64_t> dropped_frames_{0};
    std::atomic<bool> initialized_{false};
    bool hardware_accelerated_{false};
    Common::VideoStreamSpec spec_;
    mutable std::mutex mutex_;
};

} // namespace CarPlayStation::Core::Media
