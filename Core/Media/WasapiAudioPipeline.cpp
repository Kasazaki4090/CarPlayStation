#include "Core/Media/WasapiAudioPipeline.h"
#include "Core/Common/Logging.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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
    isDucked_ = false;
    voiceDuckingActive_ = false;
    rtspDuckingActive_ = false;
    for (auto& peak : voiceLanePeakRms_) {
        peak = 0.0f;
    }
    for (auto& buf : laneBuffers_) {
        buf.clear();
    }
    running_ = true;

    // Attempt to bring up the WASAPI shared-mode backend. If no device is
    // available (headless / CI), fall back to the null renderer so the mixer
    // remains fully functional and testable.
    wasapiActive_ = false;
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr) && enumerator) {
        IMMDevice* device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr) && device) {
            IAudioClient* client = nullptr;
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
            if (SUCCEEDED(hr) && client) {
                WAVEFORMATEX* mixFormat = nullptr;
                hr = client->GetMixFormat(&mixFormat);
                if (SUCCEEDED(hr) && mixFormat) {
                    mixChannels_ = mixFormat->nChannels;
                    mixBitsPerSample_ = mixFormat->wBitsPerSample;
                    mixIsFloat_ = false;
                    if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                        mixIsFloat_ = true;
                    } else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
                        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                            mixIsFloat_ = true;
                        }
                    }

                    // Event-driven rendering (AUDCLNT_STREAMFLAGS_EVENTCALLBACK):
                    // the endpoint signals a handle once per device period, which
                    // is what drives the render pump below.
                    // Shared mode does honour the requested buffer duration
                    // (measured against the Windows audio engine: asking for
                    // 250 ms yields a 12000 frame buffer at 48 kHz). That buffer
                    // is the jitter cushion: the phone delivers audio at exactly
                    // real time but in bursts, and the audio engine consumes it
                    // evenly, so keeping the endpoint deep is what turns a bursty
                    // feed into gapless playback. 100 ms is deep enough to ride
                    // out a scheduler hiccup and still inside the ~1s audio
                    // latency budget CarPlay advertises for this session.
                    REFERENCE_TIME defaultPeriod = 0;
                    REFERENCE_TIME minimumPeriod = 0;
                    client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
                    if (defaultPeriod <= 0) {
                        defaultPeriod = 100000; // 10 ms fallback
                    }
                    REFERENCE_TIME bufferDuration = 1000000;  // 100 ms
                    // Never less than eight device periods: several missed pump
                    // passes must not be able to drain the endpoint.
                    if (bufferDuration < defaultPeriod * 8) {
                        bufferDuration = defaultPeriod * 8;
                    }
                    mixRate_ = mixFormat->nSamplesPerSec;
                    // Accumulate this much audio before the first write, so the
                    // endpoint starts off full instead of trailing the feed.
                    prebufferFrames_ = mixRate_ / 10u;  // 100 ms
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                           bufferDuration, 0, mixFormat, nullptr);
                    if (SUCCEEDED(hr)) {
                        IAudioRenderClient* renderClient = nullptr;
                        hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient));
                        if (SUCCEEDED(hr) && renderClient) {
                            HANDLE pumpEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
                            if (pumpEvent) {
                                hr = client->SetEventHandle(pumpEvent);
                                if (FAILED(hr)) {
                                    CloseHandle(pumpEvent);
                                    pumpEvent = nullptr;
                                }
                            }
                            if (pumpEvent) {
                                client->Start();
                                wasapiClient_ = client;
                                wasapiRender_ = renderClient;
                                wasapiEvent_ = pumpEvent;
                                wasapiActive_ = true;
                                CPS_LOG_INFO("WasapiAudioPipeline", "WASAPI shared-mode endpoint active (",
                                             std::to_string(mixFormat->nSamplesPerSec), "Hz, ",
                                             std::to_string(mixFormat->nChannels), "ch, ",
                                             mixIsFloat_ ? "FLOAT " : "PCM ",
                                             std::to_string(mixBitsPerSample_), "bit, period=",
                                             std::to_string(static_cast<double>(defaultPeriod) / 10000.0),
                                             "ms, buffer=",
                                             std::to_string(static_cast<double>(bufferDuration) / 10000.0), "ms)");
                            } else {
                                renderClient->Release();
                                client->Release();
                            }
                        } else {
                            client->Release();
                        }
                    } else {
                        client->Release();
                    }
                    CoTaskMemFree(mixFormat);
                } else {
                    client->Release();
                }
            }
            device->Release();
        }
        enumerator->Release();
    }
#endif
    if (wasapiActive_.load() && pumpEnabled_) {
        pumpStop_ = false;
        pumpWrites_ = 0;
        pumpFramesWritten_ = 0;
        pumpUnderruns_ = 0;
        peakQueuedFrames_ = 0;
        maxPaddingFrames_ = 0;
        lastPumpFrames_ = 0;
        statsClockMs_ = 0;
        pumpThread_ = std::thread([this] { PumpLoop(); });
    }
    CPS_LOG_INFO("WasapiAudioPipeline", "started at ", format.sample_rate, "Hz ",
                 static_cast<int>(format.channels), "ch (wasapi=", wasapiActive_.load() ? "yes" : "null", ")");
    return Common::Status::Ok();
}

Common::Status WasapiAudioPipeline::Reconfigure(const Common::AudioOutputFormat& format) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!running_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Audio pipeline not running");
    }
    if (format_ == format) {
        return Common::Status::Ok();
    }
    format_ = format;
    for (auto& buf : laneBuffers_) {
        buf.clear();
    }
    reconfigureCount_++;
    return Common::Status::Ok();
}

Common::Status WasapiAudioPipeline::WriteAudio(AudioLane lane, Common::ByteSpan pcmData) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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

    // Accumulate into the lane's mix queue (applying volume + ducking).
    // We already hold mutex_ here, so use the unlocked variant. The render
    // pump thread is the only thing that drains these queues, so nothing is
    // thrown away here.
    PushLaneSamplesLocked(lane, samples.data(), samples.size());

    const size_t bytesPerFrame = (static_cast<size_t>(format_.bit_depth) / 8) * (format_.channels ? format_.channels : 1);
    if (bytesPerFrame > 0) {
        renderedSampleCount_ += pcmData.size() / bytesPerFrame;
    }
    return Common::Status::Ok();
}

void WasapiAudioPipeline::SetLaneVolume(AudioLane lane, float volume) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    laneVolumes_[lane] = volume;
}

// Two independent triggers can hold the media lane down, and either one alone
// is enough:
//   * voiceDuckingActive_ - we heard audible audio on the navigation lane.
//     Restored kDuckingHangoverMs after the last audible chunk.
//   * rtspDuckingActive_ - the phone sent duckAudio. Cleared by unduckAudio, or
//     by kRtspDuckingSafetyMs as a safety net.
// They are polled independently. The previous code only checked the RTSP
// timeout while the local detector happened to be idle, so one stuck trigger
// blocked the other's timeout forever - which is how a session ended up
// permanently ducked.
void WasapiAudioPipeline::UpdateDuckingStateLocked() {
    const auto now = std::chrono::steady_clock::now();
    if (voiceDuckingActive_) {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastVoiceActivityTime_).count();
        if (elapsedMs >= static_cast<decltype(elapsedMs)>(kDuckingHangoverMs)) {
            voiceDuckingActive_ = false;
        }
    }
    if (rtspDuckingActive_) {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - rtspDuckingStartTime_).count();
        if (elapsedMs >= static_cast<decltype(elapsedMs)>(kRtspDuckingSafetyMs)) {
            rtspDuckingActive_ = false;
            CPS_LOG_WARN("WasapiAudioPipeline",
                         "RTSP duckAudio had no matching unduckAudio within ",
                         std::to_string(kRtspDuckingSafetyMs),
                         "ms; restoring media volume");
        }
    }
    ApplyDuckingGainLocked();
}

// Decide-and-apply, with mutex_ held. carplayd holds its ducking lock across
// the set_volume() side effect on purpose: if the decision and the application
// can interleave, the two triggers apply their gains in reverse order and the
// reported state stops matching what is audible.
//
// The media gain is a hard step between targetDuckingRatio_ and 1.0. The
// reference applies DUCK_GAIN/UNDUCK_GAIN with no fade at all, because the
// phone's own duck window is short (~1.5s); a fade on both edges spends most
// of that window in transition.
void WasapiAudioPipeline::ApplyDuckingGainLocked() {
    const bool ducked = voiceDuckingActive_ || rtspDuckingActive_;
    const float target = ducked ? targetDuckingRatio_.load() : 1.0f;
    isDucked_.store(ducked);
    if (duckingRatio_.load() == target) {
        return;
    }
    duckingRatio_.store(target);
    CPS_LOG_INFO("WasapiAudioPipeline",
                 "media gain ", ducked ? "ducked to " : "restored to ",
                 std::to_string(target),
                 " (voice=", voiceDuckingActive_ ? "1" : "0",
                 " rtsp=", rtspDuckingActive_ ? "1" : "0", ")");
}

void WasapiAudioPipeline::SetDuckingRatio(float ratio) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // A ratio of 1.0 means no ducking (restore to full volume).
    // Any ratio < 1.0 means ducking is active.
    const bool active = (ratio < 1.0f);
    const float clamped = (ratio < 0.0f) ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
    targetDuckingRatio_.store(active ? clamped : 1.0f);
    if (active) {
        rtspDuckingActive_ = true;
        rtspDuckingStartTime_ = std::chrono::steady_clock::now();
    } else {
        rtspDuckingActive_ = false;
    }
    ApplyDuckingGainLocked();
}

void WasapiAudioPipeline::SetTargetDuckingRatio(float ratio) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const float clamped = (ratio < 0.0f) ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
    targetDuckingRatio_.store(clamped);
    // Retune the level of a duck that is already in progress.
    ApplyDuckingGainLocked();
}

void WasapiAudioPipeline::SetDuckingActive(bool active) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    rtspDuckingActive_ = active;
    if (active) {
        rtspDuckingStartTime_ = std::chrono::steady_clock::now();
    }
    ApplyDuckingGainLocked();
}

AudioCapabilities WasapiAudioPipeline::QueryCapabilities() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return capabilities_;
}

uint64_t WasapiAudioPipeline::GetRenderedSampleCount() const noexcept {
    return renderedSampleCount_.load();
}

void WasapiAudioPipeline::StopLane(AudioLane lane) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    laneStopCounts_[lane]++;
    laneBuffers_[static_cast<size_t>(lane)].clear();
    if (lane == AudioLane::AltNavigation || lane == AudioLane::SiriTelephony) {
        // The prompt lane is gone; nothing can be speaking, so drop the local
        // detector. The RTSP trigger is left alone - the phone owns it.
        voiceDuckingActive_ = false;
        ApplyDuckingGainLocked();
    }
}

void WasapiAudioPipeline::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    running_ = false;
    wasapiActive_ = false;
    pumpStop_ = true;
    if (pumpThread_.joinable()) {
        pumpThread_.join();
    }
#ifdef _WIN32
    if (wasapiClient_) {
        auto* client = reinterpret_cast<IAudioClient*>(wasapiClient_);
        client->Stop();
        client->Release();
        wasapiClient_ = nullptr;
    }
    if (wasapiRender_) {
        auto* render = reinterpret_cast<IAudioRenderClient*>(wasapiRender_);
        render->Release();
        wasapiRender_ = nullptr;
    }
    if (wasapiEvent_) {
        CloseHandle(static_cast<HANDLE>(wasapiEvent_));
        wasapiEvent_ = nullptr;
    }
#endif
    for (auto& buf : laneBuffers_) {
        buf.clear();
    }
}

// ---------------------------------------------------------------------------
// Software mixer
// ---------------------------------------------------------------------------

void WasapiAudioPipeline::PushLaneSamples(AudioLane lane, const float* samples, size_t sampleCount) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    PushLaneSamplesLocked(lane, samples, sampleCount);
}

void WasapiAudioPipeline::PushLaneSamplesLocked(AudioLane lane, const float* samples, size_t sampleCount) {
    auto& buf = laneBuffers_[static_cast<size_t>(lane)];

    // The queues are a playback FIFO drained by the render pump. Cap the depth
    // so a stalled endpoint or a burst from the phone cannot grow them without
    // bound; anything past the cap is dropped and counted.
    const uint32_t inCh = format_.channels > 0 ? format_.channels : 2;
    const size_t maxSamples = static_cast<size_t>(maxQueuedFrames_) * inCh;
    if (buf.size() + sampleCount > maxSamples) {
        const size_t excess = buf.size() + sampleCount - maxSamples;
        for (size_t i = 0; i < excess; ++i) {
            buf.pop_front();
        }
        droppedFrames_ += excess / inCh;
    }

    // Voice-activity gate for the AltNavigation / SiriTelephony lanes.
    //
    // The navigation stream stays open for the whole session and keeps
    // delivering padding between prompts, so "the lane is carrying data" is not
    // the trigger; only an *audible* chunk is. The reference (carplayd
    // duck.rs) measures audibility as an RMS of 50 on the 16-bit full-scale
    // range: real speech measures in the thousands, silence and codec dither
    // far below 50. We mix in float32, so the same gate is 50/32767.
    //
    // There is deliberately no hysteresis and no lower "continuation"
    // threshold: with one, padding that once crossed the onset level kept
    // refreshing the activity clock and the media lane never came back
    // (the stuck-duck symptom).
    if (lane == AudioLane::AltNavigation || lane == AudioLane::SiriTelephony) {
        float sumSq = 0.0f;
        for (size_t i = 0; i < sampleCount; ++i) {
            sumSq += samples[i] * samples[i];
        }
        const float rms = (sampleCount > 0) ? std::sqrt(sumSq / static_cast<float>(sampleCount)) : 0.0f;
        if (rms > voiceLanePeakRms_[static_cast<size_t>(lane)]) {
            voiceLanePeakRms_[static_cast<size_t>(lane)] = rms;
        }

        if (rms > kVoiceAudibleRms) {
            lastVoiceActivityTime_ = std::chrono::steady_clock::now();
            voiceDuckingActive_ = true;
        }
        UpdateDuckingStateLocked();
    } else if (lane == AudioLane::MainMedia) {
        UpdateDuckingStateLocked();
    }

    const float gain = laneVolumes_[lane];
    if (lane == AudioLane::MainMedia) {
        // duckingRatio_ is already the decided gain (targetDuckingRatio_ while
        // ducked, 1.0 otherwise), applied as a hard step. Ramping it spends a
        // large part of the phone's short duck window in transition.
        const float mediaGain = gain * duckingRatio_.load();
        for (size_t i = 0; i < sampleCount; ++i) {
            buf.push_back(samples[i] * mediaGain);
        }
    } else {
        for (size_t i = 0; i < sampleCount; ++i) {
            buf.push_back(samples[i] * gain);
        }
    }
}

std::vector<float> WasapiAudioPipeline::RenderMixed(size_t sampleCount) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<float> out(sampleCount, 0.0f);
    for (size_t l = 0; l < kLaneCount; ++l) {
        const auto& buf = laneBuffers_[l];
        const size_t n = (buf.size() < sampleCount) ? buf.size() : sampleCount;
        size_t i = 0;
        for (auto it = buf.begin(); it != buf.end() && i < n; ++it, ++i) {
            out[i] += *it;
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& buf : laneBuffers_) {
        buf.clear();
    }
}

void WasapiAudioPipeline::ConsumeLaneSamplesLocked(AudioLane lane, size_t frames) {
    auto& buf = laneBuffers_[static_cast<size_t>(lane)];
    const uint32_t inCh = format_.channels > 0 ? format_.channels : 2;
    const size_t samples = frames * inCh;
    for (size_t i = 0; i < samples && !buf.empty(); ++i) {
        buf.pop_front();
    }
}

size_t WasapiAudioPipeline::GetQueuedFrameCount(AudioLane lane) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto& buf = laneBuffers_[static_cast<size_t>(lane)];
    const uint32_t inCh = format_.channels > 0 ? format_.channels : 2;
    return buf.size() / inCh;
}

// Frames the mixer can render in one pass: the deepest queue across the lanes.
// A lane with nothing queued simply contributes silence, so an idle lane must
// not hold the other lanes back (in a media-only session two of the three
// lanes are permanently empty).
size_t WasapiAudioPipeline::MixAvailableFramesLocked() const {
    const uint32_t inCh = format_.channels > 0 ? format_.channels : 2;
    size_t frames = 0;
    for (size_t l = 0; l < kLaneCount; ++l) {
        const size_t f = laneBuffers_[l].size() / inCh;
        if (f > frames) {
            frames = f;
        }
    }
    return frames;
}

// ---------------------------------------------------------------------------
// WASAPI render pump
//
// Drains the mix queues at the endpoint's clock. The endpoint signals
// wasapiEvent_ once per device period; each wake-up fills exactly the free
// space in the render buffer, so the stream stays gapless instead of being
// burst-driven by arriving RTP packets.
// ---------------------------------------------------------------------------

void WasapiAudioPipeline::PumpLoop() {
#ifdef _WIN32
    auto* client = reinterpret_cast<IAudioClient*>(wasapiClient_);
    auto* render = reinterpret_cast<IAudioRenderClient*>(wasapiRender_);
    HANDLE event = static_cast<HANDLE>(wasapiEvent_);
    const uint32_t ch = mixChannels_ > 0 ? mixChannels_ : 2;
    // The endpoint is filled with silence until enough audio has accumulated.
    // Starting to play the instant the first packet lands is what made the very
    // beginning of a stream sound ragged.
    bool primed = false;

    while (!pumpStop_.load()) {
        WaitForSingleObject(event, 100);
        if (pumpStop_.load() || !wasapiActive_.load()) {
            break;
        }

        UINT32 bufferFrameCount = 0;
        UINT32 paddingFrames = 0;
        if (FAILED(client->GetBufferSize(&bufferFrameCount)) ||
            FAILED(client->GetCurrentPadding(&paddingFrames))) {
            continue;
        }
        if (paddingFrames > maxPaddingFrames_) {
            maxPaddingFrames_ = paddingFrames;
        }
        const UINT32 availableFrames = (bufferFrameCount > paddingFrames)
                                           ? (bufferFrameCount - paddingFrames)
                                           : 0;
        if (availableFrames == 0) {
            continue;
        }

        std::unique_lock<std::recursive_mutex> lock(mutex_);
        UpdateDuckingStateLocked();
        const size_t queued = MixAvailableFramesLocked();
        if (queued > peakQueuedFrames_) {
            peakQueuedFrames_ = queued;
        }
        if (!primed) {
            if (queued < prebufferFrames_) {
                // Not enough accumulated yet. Leave the endpoint alone; it is
                // already playing whatever the audio engine had in it.
                continue;
            }
            // Dump the whole cushion into the endpoint in one go. From here on
            // the endpoint buffer itself carries the jitter, and we only ever
            // write what has actually arrived.
            primed = true;
        }
        if (queued == 0) {
            // The phone stopped delivering. Do not insert silence into the
            // stream: that would push a gap in front of the audio that follows.
            // Only treat it as a real stall once the endpoint has drained too,
            // and then rebuild a cushion before resuming.
            if (paddingFrames == 0) {
                pumpUnderruns_++;
                primed = false;
            }
            continue;
        }
        const size_t want = queued;
        UINT32 frames = static_cast<UINT32>((std::min)(want, static_cast<size_t>(availableFrames)));
        if (frames == 0) {
            continue;
        }

        BYTE* pData = nullptr;
        if (FAILED(render->GetBuffer(frames, &pData)) || !pData) {
            continue;
        }
        // The mix queues hold `inCh` interleaved channels; the endpoint wants
        // `ch` channels. Copy channel-for-channel and replicate the last input
        // channel when the endpoint has more (e.g. mono -> stereo).
        const uint32_t inCh = format_.channels > 0 ? format_.channels : 2;
        const size_t mixSamples = static_cast<size_t>(frames) * inCh;
        const size_t endpointSamples = static_cast<size_t>(frames) * ch;
        std::vector<float> mixed = RenderMixed(mixSamples);
        auto sampleAt = [&](size_t frame, size_t c) -> float {
            const size_t src = (c < inCh) ? c : (inCh - 1);
            const size_t idx = frame * inCh + src;
            return (idx < mixed.size()) ? mixed[idx] : 0.0f;
        };
        if (mixIsFloat_ && mixBitsPerSample_ == 32) {
            float* outFloat = reinterpret_cast<float*>(pData);
            for (size_t f = 0; f < frames; ++f) {
                for (size_t c = 0; c < ch; ++c) {
                    outFloat[f * ch + c] = sampleAt(f, c);
                }
            }
        } else if (mixBitsPerSample_ == 16) {
            int16_t* out16 = reinterpret_cast<int16_t*>(pData);
            for (size_t f = 0; f < frames; ++f) {
                for (size_t c = 0; c < ch; ++c) {
                    const float clamped = std::clamp(sampleAt(f, c), -1.0f, 1.0f);
                    out16[f * ch + c] = static_cast<int16_t>(clamped * 32767.0f);
                }
            }
        } else {
            memset(pData, 0, endpointSamples * (mixBitsPerSample_ / 8));
        }
        render->ReleaseBuffer(frames, 0);

        // Only lanes that actually hold data advance; an idle lane keeps its
        // (empty) queue empty instead of being padded.
        for (size_t l = 0; l < kLaneCount; ++l) {
            if (!laneBuffers_[l].empty()) {
                ConsumeLaneSamplesLocked(static_cast<AudioLane>(l), frames);
            }
        }
        lock.unlock();

        pumpWrites_++;
        pumpFramesWritten_ += frames;
        LogPumpStats();
    }
#endif
}

void WasapiAudioPipeline::LogPumpStats() {
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (statsClockMs_ == 0) {
        statsClockMs_ = nowMs;
        return;
    }
    if (nowMs < statsClockMs_ + 1000) {
        return;
    }
    const uint64_t deltaFrames = pumpFramesWritten_ - lastPumpFrames_;
    lastPumpFrames_ = pumpFramesWritten_;
    statsClockMs_ = nowMs;
    CPS_LOG_INFO("WasapiAudioPipeline",
                 "render: writes=", std::to_string(pumpWrites_),
                 " frames=", std::to_string(pumpFramesWritten_),
                 " rate=", std::to_string(deltaFrames / 10.0 / 100.0), "fps",
                 " underruns=", std::to_string(pumpUnderruns_),
                 " maxQueued=", std::to_string(peakQueuedFrames_),
                 " maxPadding=", std::to_string(maxPaddingFrames_),
                 " dropped=", std::to_string(droppedFrames_.load()));
    // Report the loudest navigation/telephony chunk seen in the window. This is
    // the number the audibility gate is judged against: if it sits near the
    // gate the gate is wrong, if it is in the thousands the gate is fine and
    // any stuck duck must come from the RTSP trigger instead.
    const float navPeak = voiceLanePeakRms_[static_cast<size_t>(AudioLane::AltNavigation)];
    const float siriPeak = voiceLanePeakRms_[static_cast<size_t>(AudioLane::SiriTelephony)];
    if (navPeak > 0.0f || siriPeak > 0.0f) {
        CPS_LOG_INFO("WasapiAudioPipeline",
                     "voice peak rms: nav=", std::to_string(navPeak),
                     " siri=", std::to_string(siriPeak),
                     " gate=", std::to_string(kVoiceAudibleRms),
                     " ducked=", isDucked_.load() ? "1" : "0");
        voiceLanePeakRms_[static_cast<size_t>(AudioLane::AltNavigation)] = 0.0f;
        voiceLanePeakRms_[static_cast<size_t>(AudioLane::SiriTelephony)] = 0.0f;
    }
    maxPaddingFrames_ = 0;
    peakQueuedFrames_ = 0;
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = laneVolumes_.find(lane);
    return it != laneVolumes_.end() ? it->second : 0.0f;
}

uint32_t WasapiAudioPipeline::GetReconfigureCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return reconfigureCount_;
}

uint32_t WasapiAudioPipeline::GetLaneStopCount(AudioLane lane) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = laneStopCounts_.find(lane);
    return it != laneStopCounts_.end() ? it->second : 0;
}

Common::AudioOutputFormat WasapiAudioPipeline::GetCurrentFormat() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return format_;
}

void WasapiAudioPipeline::SetCapabilities(const AudioCapabilities& caps) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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
    const size_t bytesPerSample = static_cast<size_t>(format_.bit_depth) / 8;
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
