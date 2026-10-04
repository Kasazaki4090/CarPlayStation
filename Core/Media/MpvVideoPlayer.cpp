#include "Core/Media/MpvVideoPlayer.h"
#include "Core/Common/Logging.h"

#ifdef _MSC_VER
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstring>
#include <algorithm>

// libmpv headers (from Lib/mpv/include/mpv).
#include "mpv/client.h"
#include "mpv/render.h"
#include "mpv/stream_cb.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace CarPlayStation::Core::Media {

namespace {

// Path to the libmpv DLL, relative to the working directory
// (build-release/UI/Win32). The DLL is copied there by the build.
const char* kMpvDllPath = "libmpv-2.dll";

// Custom stream protocol name.
const char* kStreamProtocol = "carplay";
const char* kStreamUri = "carplay://carplay-video";

// 64-byte alignment for the SW render buffer (SIMD fast path).
constexpr size_t kSwAlign = 64;

[[nodiscard]] size_t AlignUp(size_t v, size_t align) {
    return (v + align - 1) / align * align;
}

} // namespace

MpvVideoPlayer::MpvVideoPlayer() = default;

MpvVideoPlayer::~MpvVideoPlayer() {
    Shutdown();
}

bool MpvVideoPlayer::LoadMpvLibrary() {
    if (api_.loaded) {
        return true;
    }
    HMODULE dll = ::LoadLibraryA(kMpvDllPath);
    if (!dll) {
        // Fall back to the system PATH (in case the DLL is on PATH).
        dll = ::LoadLibraryA("libmpv-2.dll");
        if (!dll) {
            CPS_LOG_ERROR("MpvVideoPlayer", "Failed to load libmpv-2.dll (error=",
                          std::to_string(::GetLastError()), ")");
            return false;
        }
    }

    auto get = [&](const char* name) -> void* {
        return reinterpret_cast<void*>(::GetProcAddress(dll, name));
    };

    api_.mpv_create = reinterpret_cast<decltype(api_.mpv_create)>(get("mpv_create"));
    api_.mpv_initialize = reinterpret_cast<decltype(api_.mpv_initialize)>(get("mpv_initialize"));
    api_.mpv_terminate_destroy = reinterpret_cast<decltype(api_.mpv_terminate_destroy)>(get("mpv_terminate_destroy"));
    api_.mpv_destroy = reinterpret_cast<decltype(api_.mpv_destroy)>(get("mpv_destroy"));
    api_.mpv_set_option_string = reinterpret_cast<decltype(api_.mpv_set_option_string)>(get("mpv_set_option_string"));
    api_.mpv_command_string = reinterpret_cast<decltype(api_.mpv_command_string)>(get("mpv_command_string"));
    api_.mpv_stream_cb_add_ro = reinterpret_cast<decltype(api_.mpv_stream_cb_add_ro)>(get("mpv_stream_cb_add_ro"));
    api_.mpv_get_property_string = reinterpret_cast<decltype(api_.mpv_get_property_string)>(get("mpv_get_property_string"));
    api_.mpv_free = reinterpret_cast<decltype(api_.mpv_free)>(get("mpv_free"));
    api_.mpv_render_context_create = reinterpret_cast<decltype(api_.mpv_render_context_create)>(get("mpv_render_context_create"));
    api_.mpv_render_context_free = reinterpret_cast<decltype(api_.mpv_render_context_free)>(get("mpv_render_context_free"));
    api_.mpv_render_context_set_update_callback =
        reinterpret_cast<decltype(api_.mpv_render_context_set_update_callback)>(get("mpv_render_context_set_update_callback"));
    api_.mpv_render_context_update = reinterpret_cast<decltype(api_.mpv_render_context_update)>(get("mpv_render_context_update"));
    api_.mpv_render_context_render = reinterpret_cast<decltype(api_.mpv_render_context_render)>(get("mpv_render_context_render"));
    api_.mpv_render_context_report_swap = reinterpret_cast<decltype(api_.mpv_render_context_report_swap)>(get("mpv_render_context_report_swap"));

    // Verify all symbols resolved.
    if (!api_.mpv_create || !api_.mpv_initialize || !api_.mpv_terminate_destroy ||
        !api_.mpv_destroy || !api_.mpv_set_option_string || !api_.mpv_command_string ||
        !api_.mpv_stream_cb_add_ro || !api_.mpv_render_context_create ||
        !api_.mpv_render_context_free || !api_.mpv_render_context_set_update_callback ||
        !api_.mpv_render_context_update || !api_.mpv_render_context_render ||
        !api_.mpv_render_context_report_swap) {
        CPS_LOG_ERROR("MpvVideoPlayer", "libmpv symbol resolution failed");
        ::FreeLibrary(dll);
        return false;
    }

    api_.loaded = true;
    CPS_LOG_INFO("MpvVideoPlayer", "libmpv loaded successfully");
    return true;
}

void MpvVideoPlayer::UnloadMpvLibrary() {
    if (api_.loaded) {
        // Note: we do not FreeLibrary here because the mpv instance may still
        // reference the DLL. The DLL is process-lifetime; freeing it is optional.
        api_.loaded = false;
    }
}

Common::Status MpvVideoPlayer::Init(const Common::VideoStreamSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder already initialized");
    }

    if (!LoadMpvLibrary()) {
        return Common::MakeError(Common::StatusCode::MediaPipelineError, "Failed to load libmpv");
    }

    spec_ = spec;
    ring_buffer_.assign(kRingBufferSize, 0);
    ring_head_ = 0;
    ring_size_ = 0;
    stream_eof_ = false;
    stream_error_ = false;
    shutdown_requested_ = false;

    // Create the mpv instance.
    mpv_ = api_.mpv_create();
    if (!mpv_) {
        return Common::MakeError(Common::StatusCode::MediaPipelineError, "mpv_create failed");
    }

    // Configure mpv for low-latency software video decode. Every name/value
    // below was verified against the linked libmpv build (v0.41.0-1092); the
    // previous list silently lost four options (`fps`, `demuxer-cache-secs`,
    // `no-audio`, `msg-level=demuxer=warn:...`), which left mpv running with
    // stock demuxer read-ahead and display pacing.
    //  - vo=libmpv: render API requires vo=libmpv (vo=none disables video pipeline entirely!)
    //  - hwdec=no: software decode (mpv's FFmpeg H.264 decoder is fast and reliable)
    //  - demuxer-lavf-format: raw H.264 Annex-B (or HEVC) format
    //  - untimed=yes: the stream carries no usable container timestamps, so play
    //    every frame as soon as it is decoded instead of pacing on guessed PTS
    //  - video-timing-offset=0 + video-latency-hacks=yes: no display delay
    //  - cache=no + tiny demuxer read-ahead: never hold frames in the demuxer
    const char* demuxFmt = (spec.codec == Common::VideoCodec::HEVC) ? "hevc" : "h264";
    const char* opts[] = {
        "vo", "libmpv",
        "hwdec", "no",
        "demuxer-lavf-format", demuxFmt,
        "untimed", "yes",
        "cache", "no",
        "demuxer-max-bytes", "128KiB",
        "demuxer-max-back-bytes", "0",
        "video-timing-offset", "0",
        "video-latency-hacks", "yes",
        "audio", "no",
        "sub", "no",
        "deband", "no",
        "keep-open", "no",
        "terminal", "no",
        "load-stats-overlay", "no",
        "msg-level", "all=no",
    };
    // mpv_set_option_string takes (name, value) pairs.
    for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); i += 2) {
        int r = api_.mpv_set_option_string(mpv_, opts[i], opts[i + 1]);
        if (r < 0) {
            CPS_LOG_WARN("MpvVideoPlayer", "mpv option '", opts[i], "' failed (",
                         std::to_string(r), "); continuing");
        }
    }

    int r = api_.mpv_initialize(mpv_);
    if (r < 0) {
        CPS_LOG_ERROR("MpvVideoPlayer", "mpv_initialize failed (", std::to_string(r), ")");
        api_.mpv_destroy(mpv_);
        mpv_ = nullptr;
        return Common::MakeError(Common::StatusCode::MediaPipelineError, "mpv_initialize failed");
    }

    // Register the custom read-only stream protocol.
    r = api_.mpv_stream_cb_add_ro(mpv_, kStreamProtocol, this,
                                  &MpvVideoPlayer::StreamOpenCallback);
    if (r < 0) {
        CPS_LOG_ERROR("MpvVideoPlayer", "mpv_stream_cb_add_ro failed (", std::to_string(r), ")");
        api_.mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return Common::MakeError(Common::StatusCode::MediaPipelineError, "mpv_stream_cb_add_ro failed");
    }

    // Configure the D3D11 shared texture.
    auto status = ConfigureD3D11SharedSurfaces();
    if (status.IsError()) {
        api_.mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return status;
    }

    // Allocate the SW render buffer (64-byte aligned).
    sw_stride_ = AlignUp(static_cast<size_t>(spec_.width) * 4, kSwAlign);
    sw_buffer_.resize(sw_stride_ * spec_.height);

    // Start the render thread (creates the render context and loads the stream).
    render_thread_running_ = true;
    render_thread_ = std::thread(&MpvVideoPlayer::RenderThreadMain, this);

    initialized_ = true;
    hardware_accelerated_ = false;
    CPS_LOG_INFO("MpvVideoPlayer", "Initialized libmpv decoder for ",
                 Common::VideoCodecToString(spec.codec), " ", spec.width, "x", spec.height,
                 " @", spec.fps, "fps (software decode, SW render)");
    return Common::Status::Ok();
}

Common::Status MpvVideoPlayer::ConfigureD3D11SharedSurfaces() {
    // Use the renderer's injected D3D11 device when available so the shared
    // texture lives on the SAME device the renderer samples from. Cross-device
    // shared textures in the same process are not coherent, so the reader would
    // never see the writer's UpdateSubresource updates (frozen first frame).
    if (injected_device_) {
        d3d_device_ = injected_device_;
        owns_d3d_device_ = false; // do not release the injected device
        // The renderer's immediate context is owned by the UI thread (Render/
        // Present). Sharing it with the writer thread deadlocks the driver, so
        // create a dedicated deferred context on the same device for the writer.
        HRESULT dhr = d3d_device_->CreateDeferredContext(0, &d3d_deferred_context_);
        if (SUCCEEDED(dhr) && d3d_deferred_context_) {
            d3d_context_ = d3d_deferred_context_;
            CPS_LOG_INFO("MpvVideoPlayer", "Using injected D3D11 device with deferred writer context");
        } else {
            // Fall back to the injected immediate context (legacy behavior).
            d3d_context_ = injected_context_;
            CPS_LOG_WARN("MpvVideoPlayer", "Deferred context creation failed (0x",
                         std::to_string(static_cast<unsigned long>(dhr)),
                         "); using injected immediate context");
        }
    } else {
        // No injected device: create our own (legacy path).
        owns_d3d_device_ = true;
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &d3d_device_, &featureLevel, &d3d_context_);
        if (FAILED(hr)) {
            return Common::MakeError(Common::StatusCode::MediaPipelineError,
                                     "D3D11 device creation failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        }
    }

    HRESULT hr = d3d_device_->QueryInterface(IID_PPV_ARGS(&dxgi_device_));
    if (FAILED(hr)) {
        return Common::MakeError(Common::StatusCode::MediaPipelineError,
                                 "DXGI device query failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
    }

    // Create a shared D3D11 texture (B8G8R8A8) for decoded frame presentation.
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = spec_.width;
    texDesc.Height = spec_.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    hr = d3d_device_->CreateTexture2D(&texDesc, nullptr, &shared_texture_);
    if (FAILED(hr)) {
        CPS_LOG_WARN("MpvVideoPlayer", "Keyed-mutex shared texture failed (0x",
                     std::to_string(static_cast<unsigned long>(hr)),
                     "); falling back to plain shared texture");
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        hr = d3d_device_->CreateTexture2D(&texDesc, nullptr, &shared_texture_);
        if (FAILED(hr)) {
            return Common::MakeError(Common::StatusCode::MediaPipelineError,
                                     "Shared texture creation failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        }
    }

    hr = shared_texture_->QueryInterface(IID_PPV_ARGS(&shared_resource_));
    if (FAILED(hr)) {
        return Common::MakeError(Common::StatusCode::MediaPipelineError,
                                 "IDXGIResource query failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
    }
    HANDLE handle = nullptr;
    hr = shared_resource_->GetSharedHandle(&handle);
    if (SUCCEEDED(hr)) {
        shared_handle_ = reinterpret_cast<uintptr_t>(handle);
    }

    (void)shared_texture_->QueryInterface(IID_PPV_ARGS(&shared_keyed_mutex_));

    CPS_LOG_INFO("MpvVideoPlayer", "D3D11 BGRA shared surface configured, handle=0x",
                 std::to_string(shared_handle_));
    return Common::Status::Ok();
}

void MpvVideoPlayer::ReleaseD3D11SharedSurfaces() {
    if (shared_keyed_mutex_) { shared_keyed_mutex_->Release(); shared_keyed_mutex_ = nullptr; }
    if (shared_resource_) { shared_resource_->Release(); shared_resource_ = nullptr; }
    if (shared_texture_)  { shared_texture_->Release();  shared_texture_ = nullptr; }
    if (dxgi_device_)     { dxgi_device_->Release();     dxgi_device_ = nullptr; }
    // Release the deferred writer context we created on the injected device.
    if (d3d_deferred_context_) { d3d_deferred_context_->Release(); d3d_deferred_context_ = nullptr; }
    // Only release the device/context we own. When the renderer's device was
    // injected via SetSharedDevice, the UI layer owns it and must keep it.
    if (owns_d3d_device_) {
        if (d3d_context_) { d3d_context_->Release(); d3d_context_ = nullptr; }
        if (d3d_device_)  { d3d_device_->Release();  d3d_device_ = nullptr; }
    } else {
        d3d_context_ = nullptr;
        d3d_device_ = nullptr;
    }
    injected_device_ = nullptr;
    injected_context_ = nullptr;
    shared_handle_ = 0;
}

// ---------------------------------------------------------------------------
// Stream callbacks (called on mpv's demuxer thread).
// ---------------------------------------------------------------------------

int MpvVideoPlayer::StreamOpenCallback(void* user_data, char* uri, mpv_stream_cb_info* info) {
    (void)uri;
    auto* self = static_cast<MpvVideoPlayer*>(user_data);
    info->cookie = self;
    info->read_fn = &MpvVideoPlayer::StreamReadCallback;
    info->close_fn = &MpvVideoPlayer::StreamCloseCallback;
    // seek_fn / size_fn / cancel_fn left NULL (unsupported for a live stream).
    info->seek_fn = nullptr;
    info->size_fn = nullptr;
    info->cancel_fn = nullptr;
    return 0; // MPV_ERROR_SUCCESS
}

int64_t MpvVideoPlayer::StreamReadCallback(void* cookie, char* buf, uint64_t nbytes) {
    auto* self = static_cast<MpvVideoPlayer*>(cookie);
    if (!self || nbytes == 0) {
        return 0;
    }

    // Block until data is available, EOF, or shutdown. Must NOT call any libmpv
    // API from here (deadlock).
    while (true) {
        std::unique_lock<std::mutex> lock(self->stream_mutex_);
        self->stream_cv_.wait_for(lock, std::chrono::milliseconds(50), [&]() {
            return self->shutdown_requested_.load() || self->stream_eof_.load() ||
                   self->stream_error_.load() || self->ring_size_ > 0;
        });

        if (self->shutdown_requested_.load() || self->stream_eof_.load() || self->stream_error_.load()) {
            return 0; // EOF
        }

        if (self->ring_size_ == 0) {
            continue; // No data yet; loop and wait again.
        }

        size_t to_read = std::min(static_cast<size_t>(nbytes), self->ring_size_);
        size_t first = std::min(to_read, kRingBufferSize - self->ring_head_);
        std::memcpy(buf, self->ring_buffer_.data() + self->ring_head_, first);
        if (first < to_read) {
            std::memcpy(buf + first, self->ring_buffer_.data(), to_read - first);
        }
        self->ring_head_ = (self->ring_head_ + to_read) % kRingBufferSize;
        self->ring_size_ -= to_read;
        return static_cast<int64_t>(to_read);
    }
}

void MpvVideoPlayer::StreamCloseCallback(void* cookie) {
    (void)cookie;
    // Nothing to release; the ring buffer is owned by the MpvVideoPlayer.
}

// ---------------------------------------------------------------------------
// Render thread.
// ---------------------------------------------------------------------------

void MpvVideoPlayer::RenderThreadMain() {
    if (!mpv_) {
        render_thread_running_ = false;
        return;
    }

    // Create the SW render context (must be done on this thread).
    const char* apiType = "sw";
    mpv_render_param params[2];
    params[0].type = MPV_RENDER_PARAM_API_TYPE;
    params[0].data = const_cast<char*>(apiType);
    params[1].type = static_cast<mpv_render_param_type>(0); // terminator

    int r = api_.mpv_render_context_create(&render_ctx_, mpv_, params);
    if (r < 0) {
        CPS_LOG_ERROR("MpvVideoPlayer", "mpv_render_context_create failed (", std::to_string(r), ")");
        render_thread_running_ = false;
        return;
    }
    CPS_LOG_INFO("MpvVideoPlayer", "SW render context created");

    // Set the update callback (notifies us when a new frame is ready).
    api_.mpv_render_context_set_update_callback(render_ctx_, [](void* ctx) {
        auto* self = static_cast<MpvVideoPlayer*>(ctx);
        self->new_frame_available_.store(true);
        std::lock_guard<std::mutex> lock(self->render_mutex_);
        self->render_cv_.notify_all();
    }, this);

    // Load the custom stream (starts the demuxer + decoder).
    std::string loadCmd = std::string("loadfile ") + kStreamUri;
    r = api_.mpv_command_string(mpv_, loadCmd.c_str());
    if (r < 0) {
        CPS_LOG_ERROR("MpvVideoPlayer", "loadfile failed (", std::to_string(r), ")");
    } else {
        CPS_LOG_INFO("MpvVideoPlayer", "Stream loaded, waiting for frames");
    }

    // Diagnostic: periodically dump mpv's internal state to diagnose why no
    // frames are produced (demuxer/decoder state, cache, etc.).
    auto lastDiag = std::chrono::steady_clock::now();
    auto dumpProp = [&](const char* name) -> std::string {
        char* v = api_.mpv_get_property_string(mpv_, name);
        std::string s = v ? v : "(null)";
        if (v) api_.mpv_free(v);
        return s;
    };

    // Render loop.
    while (render_thread_running_.load() && !shutdown_requested_.load()) {
        bool frameReady = false;
        {
            std::unique_lock<std::mutex> lock(render_mutex_);
            // Wait for a new frame, shutdown, or a 33ms timeout (frame pacing).
            render_cv_.wait_for(lock, std::chrono::milliseconds(33), [&]() {
                return new_frame_available_.load() || shutdown_requested_.load() ||
                       !render_thread_running_.load();
            });
            frameReady = new_frame_available_.load();
            if (frameReady) {
                new_frame_available_ = false;
            }
        }

        if (shutdown_requested_.load() || !render_thread_running_.load()) {
            break;
        }

        // Periodic diagnostic dump (every 2s) to see mpv's demuxer/decoder state.
        auto now = std::chrono::steady_clock::now();
        if (now - lastDiag >= std::chrono::seconds(2)) {
            lastDiag = now;
            size_t curAvail = 0;
            {
                std::lock_guard<std::mutex> lk(stream_mutex_);
                curAvail = ring_size_;
            }
            CPS_LOG_INFO("MpvVideoPlayer", "DIAG: demuxer=", dumpProp("demuxer"),
                         " decoder=", dumpProp("decoder"),
                         " video-codec=", dumpProp("video-codec"),
                         " cache=", dumpProp("demuxer-cache-duration"),
                         " ringAvail=", std::to_string(curAvail),
                         " decoded=", std::to_string(decoded_frames_.load()));
        }

        // Check if a new frame needs rendering.
        uint64_t flags = api_.mpv_render_context_update(render_ctx_);
        if (!(flags & MPV_RENDER_UPDATE_FRAME)) {
            continue;
        }

        // Render the current frame into the SW buffer.
        int swSize[2] = {static_cast<int>(spec_.width), static_cast<int>(spec_.height)};
        const char* swFormat = "bgr0";
        size_t swStride = sw_stride_;
        void* swPointer = sw_buffer_.data();

        mpv_render_param renderParams[5];
        renderParams[0].type = MPV_RENDER_PARAM_SW_SIZE;
        renderParams[0].data = swSize;
        renderParams[1].type = MPV_RENDER_PARAM_SW_FORMAT;
        renderParams[1].data = const_cast<char*>(swFormat);
        renderParams[2].type = MPV_RENDER_PARAM_SW_STRIDE;
        renderParams[2].data = &swStride;
        renderParams[3].type = MPV_RENDER_PARAM_SW_POINTER;
        renderParams[3].data = swPointer;
        renderParams[4].type = static_cast<mpv_render_param_type>(0); // terminator

        r = api_.mpv_render_context_render(render_ctx_, renderParams);
        if (r < 0) {
            // Transient render errors (e.g. no frame yet) are not fatal.
            continue;
        }

        // Upload the rendered frame to the shared D3D11 texture. Only needed by
        // the legacy shared-texture callback; when a CPU frame callback is
        // registered the renderer never looks at that texture, and the keyed
        // mutex + deferred-context command list costs several ms per frame.
        bool haveCpuCallback = false;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            haveCpuCallback = static_cast<bool>(cpu_frame_callback_);
        }
        if (!haveCpuCallback) {
            UploadFrameToSharedTexture(sw_buffer_.data(), spec_.width, spec_.height);
        }

        // Write the decoded frame into the CPU shared frame buffer (the
        // primary presentation path). The renderer copies the latest frame out
        // and uploads it to a local texture on its own immediate context.
        {
            std::lock_guard<std::mutex> bufLock(cpu_frame_buffer_.mutex);
            const size_t rowBytes = static_cast<size_t>(spec_.width) * 4;
            if (cpu_frame_buffer_.data.size() != rowBytes * spec_.height) {
                cpu_frame_buffer_.data.resize(rowBytes * spec_.height);
            }
            // sw_buffer_ rows are sw_stride_ bytes (64-byte aligned); the CPU
            // frame buffer is tightly packed (width*4). Copy row by row.
            uint8_t* dst = cpu_frame_buffer_.data.data();
            const uint8_t* src = sw_buffer_.data();
            for (uint32_t y = 0; y < spec_.height; ++y) {
                std::memcpy(dst + y * rowBytes, src + y * sw_stride_, rowBytes);
            }
            cpu_frame_buffer_.width = spec_.width;
            cpu_frame_buffer_.height = spec_.height;
            ++cpu_frame_buffer_.frame_counter;
        }

        // Report the swap (helps mpv with timing).
        api_.mpv_render_context_report_swap(render_ctx_);

        // Fire the CPU frame callback (presentation handoff).
        CpuFrameCallback cpuCallback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            cpuCallback = cpu_frame_callback_;
        }
        if (cpuCallback) {
            uint64_t total = decoded_frames_.fetch_add(1) + 1;
            if (total == 1) {
                CPS_LOG_INFO("MpvVideoPlayer", "First frame rendered and written to CPU frame buffer (",
                             spec_.width, "x", spec_.height, ")");
            }
            cpuCallback(&cpu_frame_buffer_);
        }
    }

    // Destroy the render context (on this thread).
    if (render_ctx_) {
        api_.mpv_render_context_free(render_ctx_);
        render_ctx_ = nullptr;
    }
    render_thread_running_ = false;
}

void MpvVideoPlayer::UploadFrameToSharedTexture(const uint8_t* bgraData, uint32_t width, uint32_t height) {
    if (!d3d_context_ || !shared_texture_) {
        return;
    }
    // Acquire the keyed mutex so the presentation device (which holds the
    // texture open via OpenSharedResource) is not reading while we write.
    if (shared_keyed_mutex_) {
        shared_keyed_mutex_->AcquireSync(0, 100);
    }
    D3D11_BOX box{};
    box.right = width;
    box.bottom = height;
    box.back = 1;
    d3d_context_->UpdateSubresource(shared_texture_, 0, &box, bgraData,
                                    static_cast<UINT>(sw_stride_), 0);
    // A deferred context does not execute commands on Flush(); it only
    // enqueues them. FinishCommandList() executes the queued commands so the
    // shared texture is actually updated and visible to the renderer's
    // immediate context. (For an immediate context this is a no-op.)
    if (d3d_deferred_context_) {
        d3d_context_->FinishCommandList(false, nullptr);
    } else {
        d3d_context_->Flush();
    }
    if (shared_keyed_mutex_) {
        shared_keyed_mutex_->ReleaseSync(0);
    }
}

// ---------------------------------------------------------------------------
// IVideoDecoder interface.
// ---------------------------------------------------------------------------

Common::Status MpvVideoPlayer::Reconfigure(const Common::VideoStreamSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
    }
    // For now, Reconfigure is a no-op (the stream spec is fixed during a
    // session). A full reconfigure would require restarting mpv.
    spec_ = spec;
    CPS_LOG_INFO("MpvVideoPlayer", "Reconfigure to ", Common::VideoCodecToString(spec.codec), " ",
                 spec.width, "x", spec.height, " @", spec.fps, "fps (no-op)");
    return Common::Status::Ok();
}

Common::Status MpvVideoPlayer::DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) {
    (void)ptsUs;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
    }

    if (nalData.empty()) {
        return Common::Status::Ok();
    }

    {
        std::lock_guard<std::mutex> streamLock(stream_mutex_);
        if (ring_buffer_.size() != kRingBufferSize) {
            ring_buffer_.assign(kRingBufferSize, 0);
        }

        const size_t toWrite = nalData.size();
        if (toWrite > kRingBufferSize) {
            // Buffer cannot hold an oversized NALU; drop
            dropped_frames_.fetch_add(1);
            return Common::Status::Ok();
        }

        // Drop oldest data if ring buffer would overflow
        if (ring_size_ + toWrite > kRingBufferSize) {
            size_t overflow = (ring_size_ + toWrite) - kRingBufferSize;
            ring_head_ = (ring_head_ + overflow) % kRingBufferSize;
            ring_size_ -= overflow;
            dropped_frames_.fetch_add(1);
        }

        size_t writePos = (ring_head_ + ring_size_) % kRingBufferSize;
        size_t first = std::min(toWrite, kRingBufferSize - writePos);
        std::memcpy(ring_buffer_.data() + writePos, nalData.data(), first);
        if (first < toWrite) {
            std::memcpy(ring_buffer_.data(), nalData.data() + first, toWrite - first);
        }
        ring_size_ += toWrite;
        stream_cv_.notify_one();
    }

    return Common::Status::Ok();
}

void MpvVideoPlayer::SetFrameCallback(VideoFrameCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_callback_ = std::move(callback);
}

void MpvVideoPlayer::SetCpuFrameCallback(CpuFrameCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    cpu_frame_callback_ = std::move(callback);
}

void MpvVideoPlayer::SetSharedDevice(void* device, void* context) {
#ifdef _WIN32
    // Inject the renderer's D3D11 device so the shared texture is created on
    // the SAME device the renderer samples from (cross-device shared textures
    // in the same process are not coherent). Must be called before Init().
    injected_device_ = static_cast<ID3D11Device*>(device);
    injected_context_ = static_cast<ID3D11DeviceContext*>(context);
    if (injected_device_) {
        CPS_LOG_INFO("MpvVideoPlayer", "Shared D3D11 device injected from renderer");
    }
#else
    (void)device; (void)context;
#endif
}

VideoCapabilities MpvVideoPlayer::QueryCapabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    VideoCapabilities caps;
    caps.supported_codecs = {Common::VideoCodec::H264, Common::VideoCodec::HEVC};
    caps.max_width = 3840;
    caps.max_height = 2160;
    caps.max_fps = 60;
    caps.hardware_accelerated = hardware_accelerated_;
    caps.supports_low_latency = true;
    return caps;
}

uint64_t MpvVideoPlayer::GetDecodedFrameCount() const noexcept {
    return decoded_frames_.load();
}

void MpvVideoPlayer::Flush() {
    std::lock_guard<std::mutex> streamLock(stream_mutex_);
    ring_head_ = 0;
    ring_size_ = 0;
}

void MpvVideoPlayer::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return;
    }

    // Signal shutdown to the render thread and the stream read callback.
    shutdown_requested_ = true;
    stream_eof_ = true;
    {
        std::lock_guard<std::mutex> slock(stream_mutex_);
        stream_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> rlock(render_mutex_);
        render_cv_.notify_all();
    }

    // Join the render thread (destroys the render context).
    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    // Terminate mpv (stops the demuxer/decoder threads and joins them).
    if (mpv_) {
        api_.mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }

    ReleaseD3D11SharedSurfaces();
    initialized_ = false;
    hardware_accelerated_ = false;
    CPS_LOG_INFO("MpvVideoPlayer", "Shutdown complete");
}

} // namespace CarPlayStation::Core::Media

#else // !_MSC_VER

// Non-MSVC fallback: libmpv is only available on Windows in this project.
namespace CarPlayStation::Core::Media {

MpvVideoPlayer::MpvVideoPlayer() = default;
MpvVideoPlayer::~MpvVideoPlayer() { Shutdown(); }

Common::Status MpvVideoPlayer::Init(const Common::VideoStreamSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder already initialized");
    }
    spec_ = spec;
    return Common::MakeError(Common::StatusCode::NotSupported,
                             "libmpv video player is only available on Windows");
}

Common::Status MpvVideoPlayer::Reconfigure(const Common::VideoStreamSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
    }
    spec_ = spec;
    return Common::Status::Ok();
}

Common::Status MpvVideoPlayer::DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) {
    (void)nalData; (void)ptsUs;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
    }
    return Common::Status::Ok();
}

void MpvVideoPlayer::SetFrameCallback(VideoFrameCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_callback_ = std::move(callback);
}

void MpvVideoPlayer::SetCpuFrameCallback(CpuFrameCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    cpu_frame_callback_ = std::move(callback);
}

VideoCapabilities MpvVideoPlayer::QueryCapabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    VideoCapabilities caps;
    caps.hardware_accelerated = false;
    return caps;
}

uint64_t MpvVideoPlayer::GetDecodedFrameCount() const noexcept {
    return decoded_frames_.load();
}

void MpvVideoPlayer::Flush() {}

void MpvVideoPlayer::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
}

} // namespace CarPlayStation::Core::Media

#endif // _MSC_VER
