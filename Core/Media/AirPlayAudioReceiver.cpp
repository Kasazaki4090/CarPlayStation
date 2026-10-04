#include "Core/Media/AirPlayAudioReceiver.h"

#include "Core/Common/Logging.h"
#include "Core/Common/Types.h"
#include "Core/Media/WasapiAudioPipeline.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace CarPlayStation::Core::Media {

namespace {

constexpr int kUdpReceiveBufferBytes = 1024 * 1024;
constexpr size_t kMaxDatagramBytes = 2048;

// Big-endian S16 (the container CarPlay uses when a stream is negotiated
// uncompressed) to the host-order S16 the pipeline expects.
void SwapBytesToHostS16(uint8_t* p, size_t len) {
    for (size_t i = 0; i + 1 < len; i += 2) {
        std::swap(p[i], p[i + 1]);
    }
}

}  // namespace

AirPlayAudioReceiver::AirPlayAudioReceiver(std::shared_ptr<IAudioPipeline> pipeline, Config cfg)
    : m_pipeline(std::move(pipeline)), m_cfg(std::move(cfg)) {
    m_demux.SetPacketCallback([this](const AirPlayAudioDemuxer::AudioPacket& pkt) {
        HandlePacket(pkt);
    });
}

AirPlayAudioReceiver::~AirPlayAudioReceiver() {
    Stop();
}

void AirPlayAudioReceiver::SetDecryptionKey(const std::array<uint8_t, 32>& key) {
    m_demux.SetDecryptionKey(key);
}

void AirPlayAudioReceiver::SetOutputFormat(const OutputFormat& format) {
    m_out = format;
}

Common::Status AirPlayAudioReceiver::SetStreamFormat(const AudioStreamFormat& format) {
    std::lock_guard<std::mutex> lock(m_codecMutex);
    m_cfg.stream = format;
    m_decoder.reset();
    if (format.codec == AudioCodecKind::AacLc || format.codec == AudioCodecKind::AacEld) {
        auto decoder = std::make_unique<AacAudioDecoder>();
        if (!decoder->Open(format)) {
            CPS_LOG_ERROR("AudioReceiver", "unable to open ", AudioCodecKindName(format.codec),
                          " decoder for lane ", AudioLaneToString(m_cfg.lane));
            return Common::MakeError(Common::StatusCode::Unavailable, "AAC decoder open failed");
        }
        CPS_LOG_INFO("AudioReceiver", "lane ", AudioLaneToString(m_cfg.lane), " decoding ",
                     AudioCodecKindName(format.codec), " at ", format.sample_rate, "Hz ",
                     static_cast<int>(format.channels), "ch");
        m_decoder = std::move(decoder);
    } else if (format.codec == AudioCodecKind::PcmS16Be) {
        CPS_LOG_INFO("AudioReceiver", "lane ", AudioLaneToString(m_cfg.lane),
                     " receiving uncompressed PCM at ", format.sample_rate, "Hz ",
                     static_cast<int>(format.channels), "ch");
    }
    return Common::Status::Ok();
}

Common::Status AirPlayAudioReceiver::Start() {
    if (m_running.load()) {
        return Common::Status::Ok();
    }
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return Common::MakeError(Common::StatusCode::Unavailable, "WSAStartup failed");
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return Common::MakeError(Common::StatusCode::IoError, "Failed to create audio UDP socket");
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    int buf = kUdpReceiveBufferBytes;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_cfg.udp_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return Common::MakeError(
            Common::StatusCode::IoError,
            std::string("Failed to bind audio UDP port ") + std::to_string(m_cfg.udp_port));
    }

    // A decoder is only useful once the codec parameters are known; opening it
    // here as well lets a caller pre-configure the stream before Start().
    if (m_cfg.stream.codec != AudioCodecKind::Unknown && !m_decoder) {
        (void)SetStreamFormat(m_cfg.stream);
    }

    m_udpSock = static_cast<uintptr_t>(sock);
    m_running.store(true);
    m_udpThread = std::thread([this]() { UdpListenLoop(); });

    CPS_LOG_INFO("AudioReceiver", "audio receiver for lane ", AudioLaneToString(m_cfg.lane),
                 " listening on UDP ", m_cfg.udp_port);
    return Common::Status::Ok();
#else
    return Common::MakeError(Common::StatusCode::NotImplemented, "Only supported on Windows");
#endif
}

void AirPlayAudioReceiver::Stop() {
    if (!m_running.load()) {
        return;
    }
    m_running.store(false);
#ifdef _WIN32
    if (m_udpSock != ~0ULL) {
        // Unblock the pending recvfrom() so the thread joins immediately.
        closesocket(static_cast<SOCKET>(m_udpSock));
        m_udpSock = ~0ULL;
    }
#endif
    if (m_udpThread.joinable()) {
        m_udpThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_codecMutex);
        m_decoder.reset();
    }
    m_demux.Reset();
}

void AirPlayAudioReceiver::UdpListenLoop() {
#ifdef _WIN32
    SOCKET sock = static_cast<SOCKET>(m_udpSock);
    std::vector<uint8_t> buf(kMaxDatagramBytes);
    while (m_running.load()) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(sock, reinterpret_cast<char*>(buf.data()),
                               static_cast<int>(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) {
            if (!m_running.load()) {
                break;
            }
            continue;
        }
        m_demux.Demux(buf.data(), static_cast<size_t>(n));
    }
#endif
}

void AirPlayAudioReceiver::HandlePacket(const AirPlayAudioDemuxer::AudioPacket& pkt) {
    const AudioStreamFormat stream = m_cfg.stream;

    if (stream.codec == AudioCodecKind::Unknown) {
        // Codec not negotiated (or the receiver is only counting): drop payload.
        return;
    }

    if (stream.codec == AudioCodecKind::PcmS16Be) {
        m_convertBuf.assign(pkt.au.begin(), pkt.au.end());
        SwapBytesToHostS16(reinterpret_cast<uint8_t*>(m_convertBuf.data()), m_convertBuf.size());
        const size_t frames = m_convertBuf.size() / (stream.channels ? stream.channels : 1);
        PushPcm(m_convertBuf.data(), frames, stream.channels, stream.sample_rate);
        return;
    }

    std::lock_guard<std::mutex> lock(m_codecMutex);
    if (!m_decoder) {
        return;
    }
    DecodedAudioFrame frame{};
    bool concealed = false;
    if (!m_decoder->Decode(pkt.au.data(), pkt.au.size(), frame, concealed)) {
        if (m_demux.GetStats().packetsEmitted < 5) {
            CPS_LOG_WARN("AudioReceiver", "AAC decode failed on lane ",
                         AudioLaneToString(m_cfg.lane), " (seq ", pkt.seq, ")");
        }
        return;
    }
    if (frame.pcm.empty()) {
        return;
    }
    PushPcm(frame.pcm.data(), frame.pcm.size() / (frame.channels ? frame.channels : 1),
            frame.channels, frame.sample_rate);
}

// Adapt decoded PCM (its own rate/channel count) to the container the pipeline
// was started with, then inject it into this receiver's lane.
void AirPlayAudioReceiver::PushPcm(const int16_t* pcm, size_t frames, uint8_t srcChannels,
                                   uint32_t srcRate) {
    if (!m_pipeline || frames == 0) {
        return;
    }
    const uint8_t dstChannels = m_out.channels ? m_out.channels : 2;
    const uint32_t dstRate = m_out.sample_rate ? m_out.sample_rate : 48000;
    const uint8_t inCh = srcChannels ? srcChannels : 2;

    // Channel adaptation: mono -> stereo duplicates both channels; stereo ->
    // mono averages the pair. Anything else is passed through unchanged.
    std::vector<int16_t> converted;
    const int16_t* src = pcm;
    size_t srcFrames = frames;
    uint8_t srcCh = inCh;
    if (inCh != dstChannels) {
        if (inCh == 1 && dstChannels == 2) {
            converted.resize(frames * 2);
            for (size_t i = 0; i < frames; ++i) {
                converted[i * 2] = pcm[i];
                converted[i * 2 + 1] = pcm[i];
            }
        } else if (inCh == 2 && dstChannels == 1) {
            converted.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                converted[i] = static_cast<int16_t>(
                    (static_cast<int32_t>(pcm[i * 2]) + static_cast<int32_t>(pcm[i * 2 + 1])) / 2);
            }
        } else {
            // Unsupported pairing: pass the bytes through unmodified.
            srcCh = dstChannels;
        }
        if (!converted.empty()) {
            src = converted.data();
            srcCh = dstChannels;
            srcFrames = converted.size() / dstChannels;
        }
    }

    // Sample-rate adaptation. The pipeline interprets the bytes it is given at
    // its own container rate, so anything else has to be resampled here.
    if (srcRate != dstRate && srcRate > 0) {
        const size_t inSamples = srcFrames * srcCh;
        m_resampleIn.assign(inSamples, 0.0f);
        for (size_t i = 0; i < inSamples; ++i) {
            m_resampleIn[i] = static_cast<float>(src[i]) / 32768.0f;
        }
        const size_t outSamples = (inSamples * dstRate) / srcRate;
        // ResampleLinear is per-channel; interleave channel by channel.
        std::vector<float> interleaved(outSamples, 0.0f);
        const size_t inFrames = inSamples / srcCh;
        const size_t outFrames = outSamples / dstChannels;
        for (uint8_t c = 0; c < srcCh; ++c) {
            std::vector<float> chBuf(inFrames, 0.0f);
            for (size_t i = 0; i < inFrames; ++i) {
                chBuf[i] = m_resampleIn[i * srcCh + c];
            }
            const std::vector<float> resampled = WasapiAudioPipeline::ResampleLinear(
                chBuf.data(), inFrames, srcRate, dstRate, outFrames);
            for (size_t i = 0; i < resampled.size() && i < outFrames; ++i) {
                interleaved[i * dstChannels + c] = resampled[i];
            }
        }
        m_convertBuf.resize(interleaved.size());
        for (size_t i = 0; i < interleaved.size(); ++i) {
            const float clamped = std::clamp(interleaved[i], -1.0f, 1.0f);
            m_convertBuf[i] = static_cast<int16_t>(clamped * 32767.0f);
        }
        (void)m_pipeline->WriteAudio(
            m_cfg.lane,
            Common::ByteSpan(reinterpret_cast<const uint8_t*>(m_convertBuf.data()),
                            m_convertBuf.size() * sizeof(int16_t)));
        return;
    }

    (void)m_pipeline->WriteAudio(
        m_cfg.lane,
        Common::ByteSpan(reinterpret_cast<const uint8_t*>(src),
                        srcFrames * srcCh * sizeof(int16_t)));
}

}  // namespace CarPlayStation::Core::Media
