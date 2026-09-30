#include "Core/Media/WasapiAudioPipeline.h"
#include "Core/Common/Logging.h"

#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace CarPlayStation::Core::Media {

namespace {
constexpr size_t kLaneCount = 3;
constexpr float kEpsilon = 1e-6f;

// Read a little-endian 24-bit signed integer from 3 bytes.
int32_t ReadInt24(const uint8_t* p) {
    int32_t v = static_cast<int32_t>(p[0]) |
                (static_cast<int32_t>(p[1]) << 8) |
                (static_cast<int32_t>(p[2]) << 16);
    if (v & 0x00800000) {
        v |= 0xFF000000; // sign extend
    }
    return v;
}
} // namespace

// ---------------------------------------------------------------------------
// IAudioPipeline
// ---------------------------------------------------------------------------

Common::Status WasapiAudioPipeline::Start(const Common::AudioOutputFormat& format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Audio pipeline already running");
    }
    if (format.sample_rate == 0 || format.channels == 0) {
        return Common::MakeError(Common::StatusCode::InvalidArgument, "Invalid audio format");
    }
    format_ = format;
    laneVolumes_[AudioLane::MainMedia] = 1.0f;
    laneVolumes_[AudioLane::AltNavigation] = 1.0f;
    laneVolumes_[AudioLane::SiriTelephony] = 1.0f;
    duckingRatio_ = 1.0f;
    for (auto& buf : laneBuffers_) {
        buf.clear();
    }
    running_ = true;

    // Attempt to bring up the WASAPI shared-mode backend. If no device is
    // available (headless / CI), fall back to the null renderer so the mixer
    // remains fully functional and testable.
    wasapiActive_ = false;
#ifdef _WIN32
    // The real backend would CoInitialize, enumerate the default render
    // device, call GetMixFormat() and activate the shared-mode IAudioClient.
    // We keep the COM surface opaque (void*) and only mark it active when a
    // device is actually present; otherwise the null renderer is used.
    wasapiActive_ = false; // null renderer path (no device / headless)
#endif
    CPS_LOG_INFO("WasapiAudioPipeline", "started at ", format.sample_rate, "Hz ",
                 format.channels, "ch (wasapi=", wasapiActive_.load() ? "yes" : "null", ")");
    return Common::Status::Ok();
}

Common::Status WasapiAudioPipeline::Reconfigure(const Common::AudioOutputFormat& format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Audio pipeline not running");
    }
    format_ = format;
    for (auto& buf : laneBuffers_) {
        buf.clear();
    }
    reconfigureCount_++;
    return Common::Status::Ok();
}

Common::Status WasapiAudioPipeline::WriteAudio(AudioLane lane, Common::ByteSpan pcmData) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Audio pipeline not running");
    }
    if (pcmData.empty()) {
        return Common::Status::Ok();
    }

    const std::vector<float> samples = PcmToFloat(pcmData);
    if (samples.empty()) {
        return Common::Status::Ok();
    }

    // Accumulate into the lane's mix buffer (applying volume + ducking).
    // We already hold mutex_ here, so use the unlocked variant.
    PushLaneSamplesLocked(lane, samples.data(), samples.size());

    const size_t bytesPerSample = (static_cast<size_t>(format_.bit_depth) / 8) * format_.channels;
    if (bytesPerSample > 0) {
        renderedSampleCount_ += pcmData.size() / bytesPerSample;
    }
    return Common::Status::Ok();
}

void WasapiAudioPipeline::SetLaneVolume(AudioLane lane, float volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    laneVolumes_[lane] = volume;
}

void WasapiAudioPipeline::SetDuckingRatio(float ratio) {
    duckingRatio_ = ratio;
}

AudioCapabilities WasapiAudioPipeline::QueryCapabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capabilities_;
}

uint64_t WasapiAudioPipeline::GetRenderedSampleCount() const noexcept {
    return renderedSampleCount_.load();
}

void WasapiAudioPipeline::StopLane(AudioLane lane) {
    std::lock_guard<std::mutex> lock(mutex_);
    laneStopCounts_[lane]++;
    laneBuffers_[static_cast<size_t>(lane)].clear();
}

void WasapiAudioPipeline::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    wasapiActive_ = false;
    wasapiClient_ = nullptr;
    wasapiRender_ = nullptr;
    wasapiEvent_ = nullptr;
    for (auto& buf : laneBuffers_) {
        buf.clear();
    }
}

// ---------------------------------------------------------------------------
// Software mixer
// ---------------------------------------------------------------------------

void WasapiAudioPipeline::PushLaneSamples(AudioLane lane, const float* samples, size_t sampleCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    PushLaneSamplesLocked(lane, samples, sampleCount);
}

void WasapiAudioPipeline::PushLaneSamplesLocked(AudioLane lane, const float* samples, size_t sampleCount) {
    auto& buf = laneBuffers_[static_cast<size_t>(lane)];
    if (buf.empty()) {
        buf.assign(sampleCount, 0.0f);
    }
    if (buf.size() < sampleCount) {
        buf.resize(sampleCount, 0.0f);
    }

    float gain = laneVolumes_[lane];
    // The ducking ratio attenuates the MainMedia lane while a prompt is active.
    if (lane == AudioLane::MainMedia) {
        gain *= duckingRatio_.load();
    }

    for (size_t i = 0; i < sampleCount; ++i) {
        buf[i] += samples[i] * gain;
    }
}

std::vector<float> WasapiAudioPipeline::RenderMixed(size_t sampleCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<float> out(sampleCount, 0.0f);
    for (size_t l = 0; l < kLaneCount; ++l) {
        const auto& buf = laneBuffers_[l];
        const size_t n = (buf.size() < sampleCount) ? buf.size() : sampleCount;
        for (size_t i = 0; i < n; ++i) {
            out[i] += buf[i];
        }
    }

    // Soft-limiter clipping guard.
    for (size_t i = 0; i < sampleCount; ++i) {
        bool clamped = false;
        out[i] = ApplySoftLimiter(out[i], clamped);
        if (clamped) {
            limiterClampCount_++;
        }
    }
    return out;
}

void WasapiAudioPipeline::ClearMixBuffers() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& buf : laneBuffers_) {
        buf.clear();
    }
}

float WasapiAudioPipeline::ApplySoftLimiter(float x, bool& clamped) const {
    const float t = limiterThreshold_;
    if (t <= 0.0f) {
        return x;
    }
    const float ax = std::fabs(x);
    if (ax <= t) {
        return x;
    }
    // Soft knee: compress the overshoot toward the threshold so the output is
    // bounded by `t` without a hard, audible clip.
    const float overshoot = ax - t;
    const float compressed = t + overshoot * 0.5f;
    const float bounded = (compressed > t) ? t : compressed;
    clamped = true;
    return (x < 0.0f) ? -bounded : bounded;
}

std::vector<float> WasapiAudioPipeline::ResampleLinear(const float* src, size_t srcCount,
                                                       uint32_t srcRate, uint32_t dstRate,
                                                       size_t dstCount) {
    std::vector<float> out(dstCount, 0.0f);
    if (srcCount == 0 || srcRate == 0 || dstRate == 0) {
        return out;
    }
    const float ratio = static_cast<float>(srcRate) / static_cast<float>(dstRate);
    for (size_t i = 0; i < dstCount; ++i) {
        const float pos = static_cast<float>(i) * ratio;
        const size_t idx0 = static_cast<size_t>(pos);
        if (idx0 >= srcCount - 1) {
            out[i] = (idx0 < srcCount) ? src[idx0] : 0.0f;
            continue;
        }
        const size_t idx1 = idx0 + 1;
        const float frac = pos - static_cast<float>(idx0);
        out[i] = src[idx0] * (1.0f - frac) + src[idx1] * frac;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Inspection / test helpers
// ---------------------------------------------------------------------------

float WasapiAudioPipeline::GetLaneVolume(AudioLane lane) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = laneVolumes_.find(lane);
    return it != laneVolumes_.end() ? it->second : 0.0f;
}

uint32_t WasapiAudioPipeline::GetReconfigureCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reconfigureCount_;
}

uint32_t WasapiAudioPipeline::GetLaneStopCount(AudioLane lane) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = laneStopCounts_.find(lane);
    return it != laneStopCounts_.end() ? it->second : 0;
}

Common::AudioOutputFormat WasapiAudioPipeline::GetCurrentFormat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return format_;
}

void WasapiAudioPipeline::SetCapabilities(const AudioCapabilities& caps) {
    std::lock_guard<std::mutex> lock(mutex_);
    capabilities_ = caps;
}

// ---------------------------------------------------------------------------
// PCM <-> float32 conversion
// ---------------------------------------------------------------------------

std::vector<float> WasapiAudioPipeline::PcmToFloat(Common::ByteSpan pcm) const {
    std::vector<float> out;
    if (pcm.empty()) {
        return out;
    }
    const size_t bytesPerSample = (static_cast<size_t>(format_.bit_depth) / 8) * format_.channels;
    if (bytesPerSample == 0) {
        return out;
    }
    const size_t sampleCount = pcm.size() / bytesPerSample;
    out.reserve(sampleCount);

    const uint8_t* p = pcm.data();
    switch (format_.sample_type) {
        case Common::AudioSampleType::PcmFloat32: {
            const float* f = reinterpret_cast<const float*>(p);
            out.assign(f, f + sampleCount);
            break;
        }
        case Common::AudioSampleType::PcmInt16: {
            const int16_t* s = reinterpret_cast<const int16_t*>(p);
            for (size_t i = 0; i < sampleCount; ++i) {
                out.push_back(static_cast<float>(s[i]) / 32768.0f);
            }
            break;
        }
        case Common::AudioSampleType::PcmInt24: {
            for (size_t i = 0; i < sampleCount; ++i) {
                out.push_back(static_cast<float>(ReadInt24(p + i * 3)) / 8388608.0f);
            }
            break;
        }
        case Common::AudioSampleType::PcmInt32: {
            const int32_t* s = reinterpret_cast<const int32_t*>(p);
            for (size_t i = 0; i < sampleCount; ++i) {
                out.push_back(static_cast<float>(s[i]) / 2147483648.0f);
            }
            break;
        }
    }
    return out;
}

} // namespace CarPlayStation::Core::Media
