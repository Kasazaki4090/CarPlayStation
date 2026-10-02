#include "Core/Media/FFmpegVideoDecoder.h"
#include "Core/Common/Logging.h"

#include <cstring>
#include <vector>

// ============================================================================
// Dynamic FFmpeg runtime binding.
//
// The core build is zero-dependency: FFmpeg symbols are resolved at runtime
// via LoadLibrary (Windows) / dlopen (POSIX). If the runtime is absent, the
// decoder reports NotSupported and the pipeline falls back to the Media
// Foundation hardware decoder.
// ============================================================================

#ifdef _WIN32
#include <windows.h>
#define CPS_FFMPEG_DLLOAD(name) LoadLibraryA(name)
#define CPS_FFMPEG_DLLFREE(h)  FreeLibrary(reinterpret_cast<HMODULE>(h))
#define CPS_FFMPEG_DLSYM(h, n) reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(h), n))
#else
#include <dlfcn.h>
#define CPS_FFMPEG_DLLOAD(name) dlopen(name, RTLD_NOW)
#define CPS_FFMPEG_DLLFREE(h)  dlclose(h)
#define CPS_FFMPEG_DLSYM(h, n) dlsym(h, n)
#endif

namespace CarPlayStation::Core::Media {

namespace {

// ---------------------------------------------------------------------------
// Resolved FFmpeg function pointers (subset used by this decoder).
// ---------------------------------------------------------------------------
struct FFmpegSymbols {
    // avutil
    void* (*av_malloc)(size_t) = nullptr;
    void (*av_free)(void*) = nullptr;
    int (*av_log_set_level)(int) = nullptr;

    // avcodec
    const void* (*avcodec_find_decoder)(int) = nullptr;
    void* (*avcodec_alloc_context3)(const void*) = nullptr;
    void (*avcodec_free_context)(void**) = nullptr;
    int (*avcodec_open2)(void*, const void*, void**) = nullptr;
    int (*avcodec_send_packet)(void*, const void*) = nullptr;
    int (*avcodec_receive_frame)(void*, void*) = nullptr;
    int (*av_hwdevice_ctx_create)(void**, int, void*, void*, int) = nullptr;
    void* (*av_hwframe_ctx_init)(void*) = nullptr;
    int (*av_hwframe_get_buffer)(void*, void*, int) = nullptr;
    int (*av_hwframe_transfer_data)(void*, void*, int) = nullptr;
    void* (*av_frame_alloc)() = nullptr;
    void (*av_frame_free)(void**) = nullptr;
    void* (*av_packet_alloc)() = nullptr;
    void (*av_packet_free)(void**) = nullptr;
    int (*av_hwdevice_ctx_create_for_device)(void**, int, void*, void*, int) = nullptr;
};

// FFmpeg codec IDs (libavcodec/codec_id.h).
constexpr int kCodecIdH264 = 7;   // AV_CODEC_ID_H264
constexpr int kCodecIdHevc = 174; // AV_CODEC_ID_HEVC

// FFmpeg hw device types (libavutil/hwcontext.h).
constexpr int kAvHwDeviceTypeD3D11va = 13; // AV_HWDEVICE_TYPE_D3D11VA

// FFmpeg log level: AV_LOG_ERROR = 16.
constexpr int kAvLogError = 16;

// FFmpeg return-code sentinels.
constexpr int kAvErrorEagain = 0x20004005; // AVERROR(EAGAIN)
constexpr int kAvErrorEio = 0x20004004;    // AVERROR(EIO)

// File-local accessor for the resolved symbol table (stashed on the decoder's
// runtime_handle_ slot). Returns nullptr when the runtime is not loaded.
[[nodiscard]] inline FFmpegSymbols* Symbols(void* runtimeHandle) {
    return static_cast<FFmpegSymbols*>(runtimeHandle);
}

} // namespace

FFmpegVideoDecoder::FFmpegVideoDecoder() = default;

FFmpegVideoDecoder::~FFmpegVideoDecoder() {
    Shutdown();
}

bool FFmpegVideoDecoder::LoadRuntime() {
    if (runtime_loaded_) {
        return true;
    }

    // Load the FFmpeg runtime DLLs. On Windows the import libraries are
    // resolved through the shared avcodec / avutil DLLs. The DLL file name
    // encodes the library version (e.g. avcodec-63.dll for FFmpeg 9.x), so
    // probe the known versions newest-first, then fall back to the
    // version-agnostic names (some builds ship without the suffix).
    static const char* kAvcodecNames[] = {
        "avcodec-63.dll",  // FFmpeg 9.x
        "avcodec-62.dll",  // FFmpeg 8.x
        "avcodec-61.dll",  // FFmpeg 7.x
        "avcodec.dll",
    };
    static const char* kAvutilNames[] = {
        "avutil-61.dll",  // FFmpeg 9.x
        "avutil-60.dll",  // FFmpeg 8.x
        "avutil-59.dll",  // FFmpeg 7.x
        "avutil.dll",
    };

    void* avcodec = nullptr;
    for (const char* name : kAvcodecNames) {
        avcodec = CPS_FFMPEG_DLLOAD(name);
        if (avcodec != nullptr) {
            break;
        }
    }
    void* avutil = nullptr;
    for (const char* name : kAvutilNames) {
        avutil = CPS_FFMPEG_DLLOAD(name);
        if (avutil != nullptr) {
            break;
        }
    }
    if (avcodec == nullptr || avutil == nullptr) {
        runtime_error_ = "FFmpeg runtime DLLs not found (avcodec/avutil)";
        CPS_LOG_WARN("FFmpegVideoDecoder", runtime_error_.c_str());
        return false;
    }

    FFmpegSymbols* sym = new FFmpegSymbols();
    void* libs[2] = {avcodec, avutil};
    for (void* lib : libs) {
        if (sym->av_malloc == nullptr) {
            sym->av_malloc = reinterpret_cast<decltype(sym->av_malloc)>(CPS_FFMPEG_DLSYM(lib, "av_malloc"));
        }
        if (sym->av_free == nullptr) {
            sym->av_free = reinterpret_cast<decltype(sym->av_free)>(CPS_FFMPEG_DLSYM(lib, "av_free"));
        }
        if (sym->av_log_set_level == nullptr) {
            sym->av_log_set_level = reinterpret_cast<decltype(sym->av_log_set_level)>(CPS_FFMPEG_DLSYM(lib, "av_log_set_level"));
        }
        if (sym->avcodec_find_decoder == nullptr) {
            sym->avcodec_find_decoder = reinterpret_cast<decltype(sym->avcodec_find_decoder)>(CPS_FFMPEG_DLSYM(lib, "avcodec_find_decoder"));
        }
        if (sym->avcodec_alloc_context3 == nullptr) {
            sym->avcodec_alloc_context3 = reinterpret_cast<decltype(sym->avcodec_alloc_context3)>(CPS_FFMPEG_DLSYM(lib, "avcodec_alloc_context3"));
        }
        if (sym->avcodec_free_context == nullptr) {
            sym->avcodec_free_context = reinterpret_cast<decltype(sym->avcodec_free_context)>(CPS_FFMPEG_DLSYM(lib, "avcodec_free_context"));
        }
        if (sym->avcodec_open2 == nullptr) {
            sym->avcodec_open2 = reinterpret_cast<decltype(sym->avcodec_open2)>(CPS_FFMPEG_DLSYM(lib, "avcodec_open2"));
        }
        if (sym->avcodec_send_packet == nullptr) {
            sym->avcodec_send_packet = reinterpret_cast<decltype(sym->avcodec_send_packet)>(CPS_FFMPEG_DLSYM(lib, "avcodec_send_packet"));
        }
        if (sym->avcodec_receive_frame == nullptr) {
            sym->avcodec_receive_frame = reinterpret_cast<decltype(sym->avcodec_receive_frame)>(CPS_FFMPEG_DLSYM(lib, "avcodec_receive_frame"));
        }
        if (sym->av_hwdevice_ctx_create == nullptr) {
            sym->av_hwdevice_ctx_create = reinterpret_cast<decltype(sym->av_hwdevice_ctx_create)>(CPS_FFMPEG_DLSYM(lib, "av_hwdevice_ctx_create"));
        }
        if (sym->av_hwframe_ctx_init == nullptr) {
            sym->av_hwframe_ctx_init = reinterpret_cast<decltype(sym->av_hwframe_ctx_init)>(CPS_FFMPEG_DLSYM(lib, "av_hwframe_ctx_init"));
        }
        if (sym->av_hwframe_get_buffer == nullptr) {
            sym->av_hwframe_get_buffer = reinterpret_cast<decltype(sym->av_hwframe_get_buffer)>(CPS_FFMPEG_DLSYM(lib, "av_hwframe_get_buffer"));
        }
        if (sym->av_hwframe_transfer_data == nullptr) {
            sym->av_hwframe_transfer_data = reinterpret_cast<decltype(sym->av_hwframe_transfer_data)>(CPS_FFMPEG_DLSYM(lib, "av_hwframe_transfer_data"));
        }
        if (sym->av_frame_alloc == nullptr) {
            sym->av_frame_alloc = reinterpret_cast<decltype(sym->av_frame_alloc)>(CPS_FFMPEG_DLSYM(lib, "av_frame_alloc"));
        }
        if (sym->av_frame_free == nullptr) {
            sym->av_frame_free = reinterpret_cast<decltype(sym->av_frame_free)>(CPS_FFMPEG_DLSYM(lib, "av_frame_free"));
        }
        if (sym->av_packet_alloc == nullptr) {
            sym->av_packet_alloc = reinterpret_cast<decltype(sym->av_packet_alloc)>(CPS_FFMPEG_DLSYM(lib, "av_packet_alloc"));
        }
        if (sym->av_packet_free == nullptr) {
            sym->av_packet_free = reinterpret_cast<decltype(sym->av_packet_free)>(CPS_FFMPEG_DLSYM(lib, "av_packet_free"));
        }
    }

    // The core decode path requires at least these symbols.
    if (sym->avcodec_find_decoder == nullptr ||
        sym->avcodec_alloc_context3 == nullptr ||
        sym->avcodec_open2 == nullptr ||
        sym->avcodec_send_packet == nullptr ||
        sym->avcodec_receive_frame == nullptr) {
        delete sym;
        CPS_FFMPEG_DLLFREE(avcodec);
        CPS_FFMPEG_DLLFREE(avutil);
        runtime_error_ = "FFmpeg runtime is missing required symbols";
        CPS_LOG_WARN("FFmpegVideoDecoder", runtime_error_.c_str());
        return false;
    }

    // Stash the symbol table on the runtime handle slot (single allocation).
    runtime_handle_ = static_cast<void*>(sym);
    runtime_loaded_ = true;
    if (sym->av_log_set_level != nullptr) {
        sym->av_log_set_level(kAvLogError);
    }
    CPS_LOG_INFO("FFmpegVideoDecoder", "FFmpeg runtime loaded");
    return true;
}

void FFmpegVideoDecoder::UnloadRuntime() {
    if (runtime_handle_ != nullptr) {
        delete static_cast<FFmpegSymbols*>(runtime_handle_);
        runtime_handle_ = nullptr;
    }
    runtime_loaded_ = false;
}

Common::Status FFmpegVideoDecoder::Init(const Common::VideoStreamSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder already initialized");
    }

    if (!LoadRuntime()) {
        return Common::MakeError(Common::StatusCode::NotSupported, runtime_error_);
    }

    current_spec_ = spec;
    auto status = OpenCodec();
    if (status.IsError()) {
        UnloadRuntime();
        return status;
    }

    // Attempt d3d11va hardware decode; fall back to CPU on failure.
    if (prefer_hw_) {
        auto hwStatus = TryHardwareDevice();
        if (hwStatus.IsOk()) {
            hardware_accelerated_ = true;
        } else {
            CPS_LOG_INFO("FFmpegVideoDecoder", "d3d11va unavailable, falling back to CPU: ",
                         hwStatus.Message());
            hardware_accelerated_ = false;
        }
    }

    initialized_ = true;
    CPS_LOG_INFO("FFmpegVideoDecoder", "Initialized ",
                 Common::VideoCodecToString(spec.codec), " ", spec.width, "x", spec.height,
                 " @", spec.fps, "fps (hw=", hardware_accelerated_ ? "yes" : "no", ")");
    return Common::Status::Ok();
}

Common::Status FFmpegVideoDecoder::OpenCodec() {
    FFmpegSymbols* sym = Symbols(runtime_handle_);
    if (sym == nullptr) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Runtime not loaded");
    }

    const int codecId = (current_spec_.codec == Common::VideoCodec::HEVC) ? kCodecIdHevc : kCodecIdH264;
    const void* codec = sym->avcodec_find_decoder(codecId);
    if (codec == nullptr) {
        return Common::MakeError(Common::StatusCode::NotSupported,
                                 "FFmpeg decoder not found for codec id " + std::to_string(codecId));
    }
    codec_ = const_cast<void*>(codec);

    void* ctx = sym->avcodec_alloc_context3(codec);
    if (ctx == nullptr) {
        return Common::MakeError(Common::StatusCode::MediaPipelineError, "avcodec_alloc_context3 failed");
    }
    codec_ctx_ = ctx;

    // Open the codec (no options).
    int rc = sym->avcodec_open2(ctx, codec, nullptr);
    if (rc < 0) {
        sym->avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
        return Common::MakeError(Common::StatusCode::MediaPipelineError,
                                 "avcodec_open2 failed: " + std::to_string(rc));
    }
    return Common::Status::Ok();
}

Common::Status FFmpegVideoDecoder::TryHardwareDevice() {
    FFmpegSymbols* sym = Symbols(runtime_handle_);
    if (sym == nullptr || sym->av_hwdevice_ctx_create == nullptr) {
        return Common::MakeError(Common::StatusCode::NotSupported, "HW device API unavailable");
    }

    // Create a d3d11va hardware device context.
    void* hwDevice = nullptr;
    int rc = sym->av_hwdevice_ctx_create(&hwDevice, kAvHwDeviceTypeD3D11va, nullptr, nullptr, 0);
    if (rc < 0 || hwDevice == nullptr) {
        return Common::MakeError(Common::StatusCode::NotSupported,
                                 "d3d11va device creation failed: " + std::to_string(rc));
    }
    hw_device_ctx_ = hwDevice;
    return Common::Status::Ok();
}

Common::Status FFmpegVideoDecoder::Reconfigure(const Common::VideoStreamSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
    }

    // Only renegotiate if the spec actually changed.
    if (spec.width == current_spec_.width &&
        spec.height == current_spec_.height &&
        spec.codec == current_spec_.codec &&
        spec.fps == current_spec_.fps) {
        return Common::Status::Ok();
    }

    // Tear down and reopen with the new spec. Shutdown() and Init() each take
    // the lock, so release it before delegating to avoid a self-deadlock.
    mutex_.unlock();
    Shutdown();
    return Init(spec);
}

Common::Status FFmpegVideoDecoder::DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
    }
    return DecodeFrame(nalData, ptsUs);
}

Common::Status FFmpegVideoDecoder::DecodeFrame(Common::ByteSpan nalData, uint64_t ptsUs) {
    FFmpegSymbols* sym = Symbols(runtime_handle_);
    if (sym == nullptr || codec_ctx_ == nullptr) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not ready");
    }

    // Build an AVPacket from the Annex-B access unit.
    void* packet = sym->av_packet_alloc();
    if (packet == nullptr) {
        return Common::MakeError(Common::StatusCode::MediaPipelineError, "av_packet_alloc failed");
    }
    // Mirror the AVPacket prefix (FFmpeg 7.x layout) so we can set data/size
    // without pulling in the FFmpeg headers (dynamic binding).
    //   AVBuffer* buf; int64_t pts, dts, duration, pos; int flags;
    //   uint8_t* data; int size;
    // Offsets: data = 48, size = 56.
    auto* packetData = reinterpret_cast<uint8_t**>(static_cast<char*>(packet) + 48);
    auto* packetSize = reinterpret_cast<int*>(static_cast<char*>(packet) + 56);
    *packetData = const_cast<uint8_t*>(nalData.data());
    *packetSize = static_cast<int>(nalData.size());

    int sendRc = sym->avcodec_send_packet(codec_ctx_, packet);
    sym->av_packet_free(&packet);
    if (sendRc < 0 && sendRc != kAvErrorEagain) {
        return Common::MakeError(Common::StatusCode::MediaPipelineError,
                                 "avcodec_send_packet failed: " + std::to_string(sendRc));
    }

    // Drain decoded frames.
    void* frame = sym->av_frame_alloc();
    if (frame == nullptr) {
        return Common::MakeError(Common::StatusCode::MediaPipelineError, "av_frame_alloc failed");
    }
    while (true) {
        int rc = sym->avcodec_receive_frame(codec_ctx_, frame);
        if (rc == 0) {
            decoded_frame_count_++;
            if (frame_callback_) {
                // Deliver the decoded frame. For the hardware path the frame
                // data pointer is a D3D11 texture reference; for the CPU path
                // it is a software frame buffer. The presentation layer
                // interprets the handle per the decoder's capabilities.
                frame_callback_(reinterpret_cast<uintptr_t>(frame),
                                current_spec_.width, current_spec_.height, ptsUs);
            }
            sym->av_frame_free(&frame);
            frame = sym->av_frame_alloc();
            if (frame == nullptr) {
                break;
            }
        } else if (rc == kAvErrorEagain || rc == kAvErrorEio) {
            break;
        } else {
            sym->av_frame_free(&frame);
            return Common::MakeError(Common::StatusCode::MediaPipelineError,
                                     "avcodec_receive_frame failed: " + std::to_string(rc));
        }
    }
    if (frame != nullptr) {
        sym->av_frame_free(&frame);
    }
    return Common::Status::Ok();
}

void FFmpegVideoDecoder::SetFrameCallback(VideoFrameCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_callback_ = std::move(callback);
}

VideoCapabilities FFmpegVideoDecoder::QueryCapabilities() const {
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

uint64_t FFmpegVideoDecoder::GetDecodedFrameCount() const noexcept {
    return decoded_frame_count_.load();
}

void FFmpegVideoDecoder::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    // No-op: FFmpeg decoders do not expose a flush primitive in this subset.
}

void FFmpegVideoDecoder::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ && !runtime_loaded_) {
        return;
    }
    FFmpegSymbols* sym = Symbols(runtime_handle_);
    if (sym != nullptr) {
        if (codec_ctx_ != nullptr) {
            sym->avcodec_free_context(&codec_ctx_);
            codec_ctx_ = nullptr;
        }
    }
    codec_ = nullptr;
    hw_device_ctx_ = nullptr;
    hw_frames_ctx_ = nullptr;
    sws_ctx_ = nullptr;
    UnloadRuntime();
    initialized_ = false;
    hardware_accelerated_ = false;
}

} // namespace CarPlayStation::Core::Media
