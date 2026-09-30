#include "Core/Media/MfVideoDecoder.h"
#include "Core/Common/Logging.h"

#ifdef _MSC_VER
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace CarPlayStation::Core::Media {

namespace {

// MFT CLSIDs for hardware decoders (H.264 / HEVC).
const GUID kClsidH264DecoderMFT =
    {0x626544c9, 0x9855, 0x4373, {0x88, 0x6c, 0x6c, 0x95, 0x28, 0x9b, 0x83, 0x07}};
const GUID kClsidHevcDecoderMFT =
    {0x56184b4d, 0x9b58, 0x4f47, {0x94, 0x6a, 0x2b, 0x9b, 0x2b, 0x4b, 0x4d, 0x00}};

// CODECAPI_AVLowLatencyMode attribute (low-latency hardware decode).
const GUID kCodecapAvLowLatencyMode =
    {0x666b3836, 0x57b1, 0x467c, {0x86, 0x2b, 0x44, 0x8b, 0x13, 0x1f, 0x4e, 0x8f}};

[[nodiscard]] Common::Status HResultToCommonStatus(HRESULT hr) {
    if (SUCCEEDED(hr)) {
        return Common::Status::Ok();
    }
    return Common::MakeError(Common::StatusCode::MediaPipelineError,
                             "Media Foundation error: 0x" + std::to_string(static_cast<unsigned long>(hr)));
}

} // namespace

MfVideoDecoder::MfVideoDecoder() = default;

MfVideoDecoder::~MfVideoDecoder() {
    Shutdown();
}

Common::Status MfVideoDecoder::Init(const Common::VideoStreamSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder already initialized");
    }

    // Initialize Media Foundation (once per process; reference-counted).
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        return Common::MakeError(Common::StatusCode::MediaPipelineError,
                                 "MFStartup failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
    }

    current_spec_ = spec;
    auto status = CreateMft(spec);
    if (status.IsError()) {
        MFShutdown();
        return status;
    }

    status = ConfigureD3D11SharedSurfaces();
    if (status.IsError()) {
        ReleaseComPointers();
        MFShutdown();
        return status;
    }

    initialized_ = true;
    CPS_LOG_INFO("MfVideoDecoder", "Initialized HW decoder for ",
                 Common::VideoCodecToString(spec.codec), " ", spec.width, "x", spec.height,
                 " @", spec.fps, "fps");
    return Common::Status::Ok();
}

Common::Status MfVideoDecoder::CreateMft(const Common::VideoStreamSpec& spec) {
    const GUID* clsid = (spec.codec == Common::VideoCodec::HEVC) ? &kClsidHevcDecoderMFT : &kClsidH264DecoderMFT;

    ComPtr<IMFTransform> mft;
    HRESULT hr = CoCreateInstance(*clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft));
    if (FAILED(hr)) {
        // Hardware MFT not available (no GPU / no HEVC extension). Fall back
        // to software decode path (FFmpeg) is handled by the caller; here we
        // report the failure so the pipeline can fall back.
        return Common::MakeError(Common::StatusCode::NotSupported,
                                 "Hardware MFT not available: 0x" + std::to_string(static_cast<unsigned long>(hr)));
    }

    // Enable low-latency mode (CODECAPI_AVLowLatencyMode = TRUE).
    ComPtr<IMFAttributes> attrs;
    hr = mft->GetAttributes(&attrs);
    if (SUCCEEDED(hr)) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    // Configure input media type (H.264 / HEVC).
    ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE,
                       (spec.codec == Common::VideoCodec::HEVC) ? MFVideoFormat_H265 : MFVideoFormat_H264);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, spec.width, spec.height);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, spec.fps, 1);

    hr = mft->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }

    // Query the output type (NV12 / D3D11 texture).
    ComPtr<IMFMediaType> outputType;
    hr = mft->GetOutputAvailableType(0, 0, &outputType);
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }
    hr = mft->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }

    mft_ = mft.Detach();
    hardware_accelerated_ = true;
    return Common::Status::Ok();
}

Common::Status MfVideoDecoder::ConfigureD3D11SharedSurfaces() {
    // Create a D3D11 device with DXGI shared-surface support.
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

    // Get the DXGI device for shared handle management.
    hr = d3d_device_->QueryInterface(IID_PPV_ARGS(&dxgi_device_));
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }

    // Create a shared D3D11 texture (NV12) for decoded frame presentation.
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = current_spec_.width;
    texDesc.Height = current_spec_.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_NV12;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    hr = d3d_device_->CreateTexture2D(&texDesc, nullptr, &shared_texture_);
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }

    // Obtain the shared surface handle for cross-process / cross-device sharing.
    hr = shared_texture_->QueryInterface(IID_PPV_ARGS(&shared_resource_));
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }
    HANDLE handle = nullptr;
    hr = shared_resource_->GetSharedHandle(&handle);
    if (SUCCEEDED(hr)) {
        shared_handle_ = reinterpret_cast<uintptr_t>(handle);
    }

    CPS_LOG_INFO("MfVideoDecoder", "D3D11 shared surface configured, handle=0x",
                 std::to_string(shared_handle_));
    return Common::Status::Ok();
}

Common::Status MfVideoDecoder::Reconfigure(const Common::VideoStreamSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
    }

    // Debounce: coalesce rapid Reconfigure calls (e.g. during window drag).
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs - last_reconfigure_tick_ms_ < reconfigure_debounce_ms_) {
        debounced_reconfigure_count_++;
        CPS_LOG_DEBUG("MfVideoDecoder", "Reconfigure debounced (", nowMs - last_reconfigure_tick_ms_, "ms < ",
                      reconfigure_debounce_ms_, "ms)");
        return Common::Status::Ok();
    }
    last_reconfigure_tick_ms_ = nowMs;

    // Only renegotiate if the spec actually changed (resolution / codec / fps).
    if (spec.width == current_spec_.width &&
        spec.height == current_spec_.height &&
        spec.codec == current_spec_.codec &&
        spec.fps == current_spec_.fps) {
        return Common::Status::Ok();
    }

    // Release and recreate the MFT with the new spec.
    ReleaseComPointers();
    current_spec_ = spec;
    auto status = CreateMft(spec);
    if (status.IsError()) {
        return status;
    }
    status = ConfigureD3D11SharedSurfaces();
    if (status.IsError()) {
        ReleaseComPointers();
        return status;
    }
    reconfigure_count_++;
    CPS_LOG_INFO("MfVideoDecoder", "Reconfigured to ", Common::VideoCodecToString(spec.codec), " ",
                 spec.width, "x", spec.height, " @", spec.fps, "fps");
    return Common::Status::Ok();
}

Common::Status MfVideoDecoder::DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
    }
    auto status = ProcessInput(nalData, ptsUs);
    if (status.IsError()) {
        return status;
    }
    return DrainOutput();
}

Common::Status MfVideoDecoder::ProcessInput(Common::ByteSpan nalData, uint64_t ptsUs) {
    if (mft_ == nullptr) {
        return Common::MakeError(Common::StatusCode::InvalidState, "MFT not created");
    }

    // Wrap the Annex-B NALU data in an IMFMediaBuffer.
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(nalData.size()), &buffer);
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }
    BYTE* bufferData = nullptr;
    DWORD bufferLen = 0;
    hr = buffer->Lock(&bufferData, nullptr, &bufferLen);
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }
    std::memcpy(bufferData, nalData.data(), nalData.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(nalData.size()));

    // Stamp the PTS (100ns units) on the buffer.
    const LONGLONG pts100ns = static_cast<LONGLONG>(ptsUs) * 10;

    // Wrap the buffer in a sample and push it into the MFT.
    ComPtr<IMFSample> sample;
    hr = MFCreateSample(sample.GetAddressOf());
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }
    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }
    sample->SetSampleTime(pts100ns);
    sample->SetSampleDuration(0);

    // MFSampleInput_Regular == 0.
    const DWORD inputFlags = 0;
    hr = mft_->ProcessInput(0, sample.Get(), inputFlags);
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }
    return Common::Status::Ok();
}

Common::Status MfVideoDecoder::DrainOutput() {
    if (mft_ == nullptr) {
        return Common::Status::Ok();
    }

    // Build an output data buffer (sample + status) for the MFT to fill.
    ComPtr<IMFSample> outputSample;
    HRESULT hr = MFCreateSample(outputSample.GetAddressOf());
    if (FAILED(hr)) {
        return HResultToCommonStatus(hr);
    }

    MFT_OUTPUT_DATA_BUFFER outputBuffer{};
    outputBuffer.dwStreamID = 0;
    outputBuffer.pSample = outputSample.Get();
    outputBuffer.dwStatus = 0;
    outputBuffer.pEvents = nullptr;

    while (true) {
        hr = mft_->ProcessOutput(0, 1, &outputBuffer, nullptr);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (FAILED(hr)) {
            break;
        }
        if (outputBuffer.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) {
            break;
        }
        decoded_frame_count_++;
        if (frame_callback_) {
            frame_callback_(shared_handle_, current_spec_.width, current_spec_.height, 0);
        }
    }
    return Common::Status::Ok();
}

void MfVideoDecoder::SetFrameCallback(VideoFrameCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_callback_ = std::move(callback);
}

VideoCapabilities MfVideoDecoder::QueryCapabilities() const {
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

uint64_t MfVideoDecoder::GetDecodedFrameCount() const noexcept {
    return decoded_frame_count_.load();
}

void MfVideoDecoder::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mft_ != nullptr) {
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
}

void MfVideoDecoder::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return;
    }
    ReleaseComPointers();
    MFShutdown();
    initialized_ = false;
    hardware_accelerated_ = false;
}

void MfVideoDecoder::ReleaseComPointers() {
    if (shared_resource_) { shared_resource_->Release(); shared_resource_ = nullptr; }
    if (shared_texture_)  { shared_texture_->Release();  shared_texture_ = nullptr; }
    if (dxgi_device_)     { dxgi_device_->Release();     dxgi_device_ = nullptr; }
    if (d3d_context_)     { d3d_context_->Release();     d3d_context_ = nullptr; }
    if (d3d_device_)      { d3d_device_->Release();      d3d_device_ = nullptr; }
    if (mft_)             { mft_->Release();             mft_ = nullptr; }
    shared_handle_ = 0;
}

} // namespace CarPlayStation::Core::Media

#else // !_MSC_VER

// Non-MSVC fallback (e.g. MinGW): the Media Foundation hardware decoder is
// unavailable. The class still compiles and reports NotSupported so the
// pipeline can fall back to the FFmpeg software decoder.
// The class still compiles and reports NotSupported so the pipeline can fall
// back to the FFmpeg software decoder.
namespace CarPlayStation::Core::Media {

MfVideoDecoder::MfVideoDecoder() = default;

MfVideoDecoder::~MfVideoDecoder() {
    Shutdown();
}

Common::Status MfVideoDecoder::Init(const Common::VideoStreamSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder already initialized");
    }
    current_spec_ = spec;
    return Common::MakeError(Common::StatusCode::NotSupported,
                             "Media Foundation hardware decoder is only available on Windows");
}

Common::Status MfVideoDecoder::Reconfigure(const Common::VideoStreamSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
    }
    current_spec_ = spec;
    reconfigure_count_++;
    return Common::Status::Ok();
}

Common::Status MfVideoDecoder::DecodeAccessUnit(Common::ByteSpan nalData, uint64_t ptsUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Decoder not initialized");
    }
    (void)nalData;
    (void)ptsUs;
    return Common::MakeError(Common::StatusCode::NotSupported,
                             "Media Foundation hardware decoder is only available on Windows");
}

void MfVideoDecoder::SetFrameCallback(VideoFrameCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_callback_ = std::move(callback);
}

VideoCapabilities MfVideoDecoder::QueryCapabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    VideoCapabilities caps;
    caps.supported_codecs = {Common::VideoCodec::H264, Common::VideoCodec::HEVC};
    caps.max_width = 3840;
    caps.max_height = 2160;
    caps.max_fps = 60;
    caps.hardware_accelerated = false;
    caps.supports_low_latency = false;
    return caps;
}

uint64_t MfVideoDecoder::GetDecodedFrameCount() const noexcept {
    return decoded_frame_count_.load();
}

void MfVideoDecoder::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
}

void MfVideoDecoder::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    hardware_accelerated_ = false;
}

Common::Status MfVideoDecoder::CreateMft(const Common::VideoStreamSpec& spec) {
    (void)spec;
    return Common::MakeError(Common::StatusCode::NotSupported, "Not available on this platform");
}

Common::Status MfVideoDecoder::ConfigureD3D11SharedSurfaces() {
    return Common::MakeError(Common::StatusCode::NotSupported, "Not available on this platform");
}

Common::Status MfVideoDecoder::ProcessInput(Common::ByteSpan nalData, uint64_t ptsUs) {
    (void)nalData;
    (void)ptsUs;
    return Common::MakeError(Common::StatusCode::NotSupported, "Not available on this platform");
}

Common::Status MfVideoDecoder::DrainOutput() {
    return Common::Status::Ok();
}

void MfVideoDecoder::ReleaseComPointers() {
    // No COM pointers on non-Windows.
}

} // namespace CarPlayStation::Core::Media

#endif // _MSC_VER
