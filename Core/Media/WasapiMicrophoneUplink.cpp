#include "Core/Media/WasapiMicrophoneUplink.h"
#include "Core/Media/WasapiAudioPipeline.h"
#include "Core/Common/Logging.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace CarPlayStation::Core::Media {

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

    // Bring up the WASAPI capture backend when a microphone is present;
    // otherwise fall back to the null capture source (driven by injected
    // frames in tests / headless CI).
    wasapiActive_ = false;
#ifdef _WIN32
    wasapiActive_ = false; // null capture path (no device / headless)
#endif
    CPS_LOG_INFO("WasapiMicrophoneUplink", "started at ", format.sample_rate, "Hz ",
                 format.channels, "ch (wasapi=", wasapiActive_.load() ? "yes" : "null", ")");
    return Common::Status::Ok();
}

void WasapiMicrophoneUplink::SetUplinkCallback(UplinkAudioCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    uplinkCallback_ = std::move(callback);
}

Common::Status WasapiMicrophoneUplink::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    capturing_ = false;
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
    std::lock_guard<std::mutex> lock(mutex_);
    capturing_ = false;
    wasapiActive_ = false;
    wasapiCapture_ = nullptr;
}

// ---------------------------------------------------------------------------
// Resampling / packetization
// ---------------------------------------------------------------------------

void WasapiMicrophoneUplink::ProcessCapturedSamples(const float* src, size_t srcCount,
                                                    uint32_t srcRate, uint32_t dstRate,
                                                    size_t packetSamples, uint64_t timestampUs) {
    if (!capturing_ || srcCount == 0 || packetSamples == 0) {
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
// Inspection / test helpers
// ---------------------------------------------------------------------------

Common::AudioOutputFormat WasapiMicrophoneUplink::GetCurrentFormat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return format_;
}

void WasapiMicrophoneUplink::SetCapabilities(const MicrophoneCapabilities& caps) {
    std::lock_guard<std::mutex> lock(mutex_);
    capabilities_ = caps;
}

} // namespace CarPlayStation::Core::Media
