#include "Core/Media/WasapiMicrophoneUplink.h"
#include "Core/Media/WasapiAudioPipeline.h"
#include "Core/Common/Logging.h"

#include <chrono>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

namespace CarPlayStation::Core::Media {

namespace {
// The uplink is 16kHz mono; 30ms is one AAC-ELD frame (480 samples) and is
// also the packet size the phone's SDP advertises for the reverse stream.
constexpr size_t kUplinkPacketSamples = 480;
} // namespace

// ---------------------------------------------------------------------------
// IMicrophoneUplink
// ---------------------------------------------------------------------------

Common::Status WasapiMicrophoneUplink::Start(const Common::AudioOutputFormat& format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (capturing_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Microphone already capturing");
    }
    if (format.sample_rate == 0) {
        return Common::MakeError(Common::StatusCode::InvalidArgument, "Invalid microphone format");
    }
    format_ = format;
    capturing_ = true;
    captureStop_ = false;
    statsClockMs_ = 0;
    lastCapturedSamples_ = 0;

    // Bring up the WASAPI capture backend when a microphone is present;
    // otherwise fall back to the null capture source (driven by injected
    // frames in tests / headless CI).
    wasapiActive_ = false;
#ifdef _WIN32
    wasapiActive_ = StartCaptureBackend();
#endif
    CPS_LOG_INFO("WasapiMicrophoneUplink", "started at ", std::to_string(format.sample_rate), "Hz ",
                 std::to_string(static_cast<int>(format.channels)), "ch (wasapi=",
                 wasapiActive_.load() ? "yes" : "null", ")");
    return Common::Status::Ok();
}

void WasapiMicrophoneUplink::SetUplinkCallback(UplinkAudioCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    uplinkCallback_ = std::move(callback);
}

Common::Status WasapiMicrophoneUplink::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capturing_ = false;
    }
    // Joined without holding mutex_: the capture thread takes that lock while
    // dispatching a packet, so joining under it would deadlock.
    StopCaptureBackend();
    CPS_LOG_INFO("WasapiMicrophoneUplink", "stopped (captured=",
                 std::to_string(capturedSampleCount_.load()), " samples, packets=",
                 std::to_string(packetDispatchCount_.load()), ", drains=",
                 std::to_string(captureDrainCount_.load()), ")");
    return Common::Status::Ok();
}

bool WasapiMicrophoneUplink::IsCapturing() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return capturing_;
}

MicrophoneCapabilities WasapiMicrophoneUplink::QueryCapabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capabilities_;
}

void WasapiMicrophoneUplink::SetInputGain(float gain) {
    inputGain_ = gain;
}

uint64_t WasapiMicrophoneUplink::GetCapturedSampleCount() const noexcept {
    return capturedSampleCount_.load();
}

void WasapiMicrophoneUplink::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capturing_ = false;
    }
    StopCaptureBackend();
    std::lock_guard<std::mutex> lock(mutex_);
    wasapiActive_ = false;
}

// ---------------------------------------------------------------------------
// Resampling / packetization
// ---------------------------------------------------------------------------

void WasapiMicrophoneUplink::ProcessCapturedSamples(const float* src, size_t srcCount,
                                                    uint32_t srcRate, uint32_t dstRate,
                                                    size_t packetSamples, uint64_t timestampUs) {
    // The callback is copied under the lock and invoked outside it, so the
    // capture thread never holds mutex_ while the RTP sender encodes a frame.
    UplinkAudioCallback cb;
    bool capturing = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capturing = capturing_;
        cb = uplinkCallback_;
    }
    if (!capturing || srcCount == 0 || packetSamples == 0) {
        return;
    }

    // Resample to the target mono rate (16kHz / 24kHz). The output sample
    // count scales with the rate ratio: srcCount * dstRate / srcRate.
    const size_t dstCount =
        (srcRate > 0) ? (srcCount * dstRate) / srcRate : srcCount;
    const std::vector<float> resampled =
        WasapiAudioPipeline::ResampleLinear(src, srcCount, srcRate, dstRate, dstCount);

    const float gain = inputGain_.load();
    const size_t total = resampled.size();
    size_t offset = 0;
    while (offset < total) {
        const size_t n = (packetSamples < (total - offset)) ? packetSamples : (total - offset);
        std::vector<uint8_t> packet(n * sizeof(float));
        float* dst = reinterpret_cast<float*>(packet.data());
        for (size_t i = 0; i < n; ++i) {
            dst[i] = resampled[offset + i] * gain;
        }

        capturedSampleCount_ += n;
        packetDispatchCount_++;
        lastPacketSamples_ = static_cast<uint32_t>(n);

        UplinkAudioCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = uplinkCallback_;
        }
        if (cb) {
            cb(Common::ByteSpan{packet.data(), packet.size()}, timestampUs);
            callbackDispatchCount_++;
        }
        offset += n;
    }
}

// ---------------------------------------------------------------------------
// WASAPI capture backend
// ---------------------------------------------------------------------------

#ifdef _WIN32
bool WasapiMicrophoneUplink::StartCaptureBackend() {
    std::lock_guard<std::mutex> backendLock(backendMutex_);
    if (captureThread_.joinable()) {
        return false; // already running
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        CPS_LOG_WARN("WasapiMicrophoneUplink", "no audio endpoint enumerator (hr=",
                     std::to_string(static_cast<long>(hr)), ")");
        return false;
    }
    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    if (FAILED(hr) || !device) {
        enumerator->Release();
        CPS_LOG_WARN("WasapiMicrophoneUplink", "no default capture endpoint (hr=",
                     std::to_string(static_cast<long>(hr)), ")");
        return false;
    }
    IAudioClient* client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(hr) || !client) {
        enumerator->Release();
        CPS_LOG_WARN("WasapiMicrophoneUplink", "capture device activation failed (hr=",
                     std::to_string(static_cast<long>(hr)), ")");
        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = client->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        client->Release();
        enumerator->Release();
        CPS_LOG_WARN("WasapiMicrophoneUplink", "GetMixFormat failed (hr=",
                     std::to_string(static_cast<long>(hr)), ")");
        return false;
    }
    captureRate_ = mixFormat->nSamplesPerSec;
    captureChannels_ = mixFormat->nChannels;
    captureBits_ = mixFormat->wBitsPerSample;
    captureIsFloat_ = false;
    if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        captureIsFloat_ = true;
    } else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            captureIsFloat_ = true;
        }
    }

    // Capture wants a shallow buffer: the audio engine delivers a packet per
    // device period and GetBuffer hands back everything that arrived. Asking
    // for a deep buffer here only adds latency to the voice assistant.
    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    if (defaultPeriod <= 0) {
        defaultPeriod = 100000; // 10 ms fallback
    }
    REFERENCE_TIME bufferDuration = defaultPeriod * 10; // 100 ms of slack

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            bufferDuration, 0, mixFormat, nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(mixFormat);
        client->Release();
        enumerator->Release();
        CPS_LOG_WARN("WasapiMicrophoneUplink", "capture Initialize failed (hr=",
                     std::to_string(static_cast<long>(hr)), ")");
        return false;
    }
    CoTaskMemFree(mixFormat);

    IAudioCaptureClient* captureClient = nullptr;
    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient));
    if (FAILED(hr) || !captureClient) {
        client->Release();
        enumerator->Release();
        CPS_LOG_WARN("WasapiMicrophoneUplink", "IAudioCaptureClient unavailable (hr=",
                     std::to_string(static_cast<long>(hr)), ")");
        return false;
    }

    HANDLE captureEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!captureEvent) {
        captureClient->Release();
        client->Release();
        enumerator->Release();
        CPS_LOG_WARN("WasapiMicrophoneUplink", "CreateEventExW failed");
        return false;
    }
    hr = client->SetEventHandle(captureEvent);
    if (FAILED(hr)) {
        CloseHandle(captureEvent);
        captureClient->Release();
        client->Release();
        enumerator->Release();
        CPS_LOG_WARN("WasapiMicrophoneUplink", "SetEventHandle failed (hr=",
                     std::to_string(static_cast<long>(hr)), ")");
        return false;
    }

    // Dispatch whole encoder frames: 30 ms of the uplink rate. The phone's
    // AAC-ELD encoder consumes exactly this many samples per AU, so keeping
    // the packets aligned stops the sender's accumulator from drifting.
    const uint32_t dstRate = format_.sample_rate;
    capturePacketSamples_ = kUplinkPacketSamples;
    if (dstRate == 0 || captureRate_ == 0) {
        capturePacketSamples_ = 0;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        CloseHandle(captureEvent);
        captureClient->Release();
        client->Release();
        enumerator->Release();
        CPS_LOG_WARN("WasapiMicrophoneUplink", "capture Start failed (hr=",
                     std::to_string(static_cast<long>(hr)), ")");
        return false;
    }

    wasapiCapture_ = client;
    wasapiCaptureClient_ = captureClient;
    wasapiEvent_ = captureEvent;
    captureStop_ = false;
    captureThread_ = std::thread([this] { CaptureLoop(); });
    enumerator->Release();

    CPS_LOG_INFO("WasapiMicrophoneUplink", "WASAPI capture active (",
                 std::to_string(captureRate_), "Hz, ", std::to_string(static_cast<int>(captureChannels_)),
                 "ch, ", captureIsFloat_ ? "FLOAT " : "PCM ", std::to_string(static_cast<int>(captureBits_)),
                 "bit, period=", std::to_string(static_cast<double>(defaultPeriod) / 10000.0), "ms)");
    return true;
}

void WasapiMicrophoneUplink::StopCaptureBackend() {
    std::lock_guard<std::mutex> backendLock(backendMutex_);
    captureStop_ = true;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    if (wasapiCapture_) {
        auto* client = static_cast<IAudioClient*>(wasapiCapture_);
        client->Stop();
    }
    if (wasapiCaptureClient_) {
        static_cast<IAudioCaptureClient*>(wasapiCaptureClient_)->Release();
        wasapiCaptureClient_ = nullptr;
    }
    if (wasapiCapture_) {
        static_cast<IAudioClient*>(wasapiCapture_)->Release();
        wasapiCapture_ = nullptr;
    }
    if (wasapiEvent_) {
        CloseHandle(static_cast<HANDLE>(wasapiEvent_));
        wasapiEvent_ = nullptr;
    }
    wasapiActive_ = false;
    captureRate_ = 0;
    captureChannels_ = 0;
}

// Drains the capture endpoint on the device clock. The endpoint signals once
// per device period; everything that arrived since the last GetBuffer is
// downmixed to mono, resampled to the negotiated uplink rate, and handed to
// ProcessCapturedSamples in whole 30 ms packets.
void WasapiMicrophoneUplink::CaptureLoop() {
    auto* client = static_cast<IAudioClient*>(wasapiCapture_);
    auto* captureClient = static_cast<IAudioCaptureClient*>(wasapiCaptureClient_);
    HANDLE event = static_cast<HANDLE>(wasapiEvent_);
    if (!client || !captureClient || !event) {
        return;
    }

    const uint32_t dstRate = format_.sample_rate;
    const uint32_t srcRate = captureRate_;
    const size_t packetSamples = capturePacketSamples_;
    if (dstRate == 0 || srcRate == 0 || packetSamples == 0) {
        return;
    }
    // Source samples that produce exactly one whole uplink packet.
    const size_t packetSourceSamples = (packetSamples * srcRate + dstRate - 1) / dstRate;

    // Hold whole packets of device-format samples so every dispatch is exactly
    // packetSamples long at the uplink rate.
    std::vector<float> pending;
    pending.reserve(packetSourceSamples * 2);
    const auto startClock = std::chrono::steady_clock::now();

    while (!captureStop_.load()) {
        // Wake up with the device. A timeout keeps a dead endpoint from
        // wedging Stop().
        WaitForSingleObject(event, 50);
        if (captureStop_.load()) {
            break;
        }

        UINT32 packets = 0;
        if (FAILED(captureClient->GetNextPacketSize(&packets))) {
            break;
        }
        while (packets > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 position = 0;
            UINT64 qpc = 0;
            if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, &position, &qpc))) {
                break;
            }
            if (frames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                // Downmix to mono into the pending buffer.
                const size_t base = pending.size();
                pending.resize(base + frames, 0.0f);
                if (captureIsFloat_ && captureBits_ == 32) {
                    const auto* src = reinterpret_cast<const float*>(data);
                    for (UINT32 f = 0; f < frames; ++f) {
                        float acc = 0.0f;
                        for (uint16_t c = 0; c < captureChannels_; ++c) {
                            acc += src[f * captureChannels_ + c];
                        }
                        pending[base + f] = acc / static_cast<float>(captureChannels_);
                    }
                } else if (captureBits_ == 16) {
                    const auto* src = reinterpret_cast<const int16_t*>(data);
                    for (UINT32 f = 0; f < frames; ++f) {
                        float acc = 0.0f;
                        for (uint16_t c = 0; c < captureChannels_; ++c) {
                            acc += static_cast<float>(src[f * captureChannels_ + c]) / 32768.0f;
                        }
                        pending[base + f] = acc / static_cast<float>(captureChannels_);
                    }
                } else if (captureBits_ == 32) {
                    const auto* src = reinterpret_cast<const int32_t*>(data);
                    for (UINT32 f = 0; f < frames; ++f) {
                        float acc = 0.0f;
                        for (uint16_t c = 0; c < captureChannels_; ++c) {
                            acc += static_cast<float>(src[f * captureChannels_ + c]) / 2147483648.0f;
                        }
                        pending[base + f] = acc / static_cast<float>(captureChannels_);
                    }
                } else if (captureBits_ == 24) {
                    for (UINT32 f = 0; f < frames; ++f) {
                        float acc = 0.0f;
                        for (uint16_t c = 0; c < captureChannels_; ++c) {
                            const uint8_t* p = data + (f * captureChannels_ + c) * 3;
                            int32_t v = static_cast<int32_t>(p[0]) |
                                        (static_cast<int32_t>(p[1]) << 8) |
                                        (static_cast<int32_t>(p[2]) << 16);
                            if (v & 0x00800000) {
                                v |= 0xFF000000;
                            }
                            acc += static_cast<float>(v) / 8388608.0f;
                        }
                        pending[base + f] = acc / static_cast<float>(captureChannels_);
                    }
                }
            }
            captureClient->ReleaseBuffer(frames);
            captureDrainCount_++;
            if (FAILED(captureClient->GetNextPacketSize(&packets))) {
                break;
            }
        }

        // Drop any silence the endpoint inserted so it cannot stretch a packet.
        if (pending.size() >= packetSourceSamples) {
            const uint64_t timestampUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - startClock).count());
            while (pending.size() >= packetSourceSamples) {
                ProcessCapturedSamples(pending.data(), packetSourceSamples, srcRate, dstRate,
                                       packetSamples, timestampUs);
                pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(packetSourceSamples));
            }
            LogCaptureStats();
        }
    }
}

void WasapiMicrophoneUplink::LogCaptureStats() {
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (statsClockMs_ == 0) {
        statsClockMs_ = nowMs;
        lastCapturedSamples_ = capturedSampleCount_.load();
        return;
    }
    if (nowMs - statsClockMs_ < 1000) {
        return;
    }
    const uint64_t total = capturedSampleCount_.load();
    CPS_LOG_INFO("WasapiMicrophoneUplink", "capture: +", std::to_string(total - lastCapturedSamples_),
                 " samples/s (total=", std::to_string(total), "), packets=",
                 std::to_string(packetDispatchCount_.load()), ", drains=",
                 std::to_string(captureDrainCount_.load()), ", callbacks=",
                 std::to_string(callbackDispatchCount_.load()), ")");
    statsClockMs_ = nowMs;
    lastCapturedSamples_ = total;
}
#else
bool WasapiMicrophoneUplink::StartCaptureBackend() { return false; }
void WasapiMicrophoneUplink::StopCaptureBackend() {}
void WasapiMicrophoneUplink::CaptureLoop() {}
void WasapiMicrophoneUplink::LogCaptureStats() {}
#endif

Common::AudioOutputFormat WasapiMicrophoneUplink::GetCurrentFormat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return format_;
}

void WasapiMicrophoneUplink::SetCapabilities(const MicrophoneCapabilities& caps) {
    std::lock_guard<std::mutex> lock(mutex_);
    capabilities_ = caps;
}

} // namespace CarPlayStation::Core::Media
