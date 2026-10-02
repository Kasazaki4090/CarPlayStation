#include "Core/Media/MicrophoneRtpSender.h"
#include "Core/Common/Logging.h"
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
constexpr int SOCK_ERR = SOCKET_ERROR;
#define CLOSE_SOCK(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;
constexpr int SOCK_ERR = -1;
#define CLOSE_SOCK(s) close(s)
#endif

namespace CarPlayStation::Core::Media {

MicrophoneRtpSender::MicrophoneRtpSender() = default;

MicrophoneRtpSender::~MicrophoneRtpSender() {
    Stop();
}

Common::Status MicrophoneRtpSender::Start(const MicrophoneRtpConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return Common::Status::Ok();
    }

    config_ = config;
    if (config_.packet_samples == 0) {
        config_.packet_samples = 320; // Default ~20ms @ 16kHz
    }

    sample_accumulator_.clear();
    sample_accumulator_.reserve(config_.packet_samples * 2);
    seq_ = 0;
    ts_ = 0;
    packets_sent_ = 0;

#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCK) {
        return Common::MakeError(Common::StatusCode::IoError, "Failed to create UDP socket for Microphone RTP");
    }

    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(config_.local_bind_port);

    if (bind(sock, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) == SOCK_ERR) {
        CLOSE_SOCK(sock);
        return Common::MakeError(Common::StatusCode::IoError, "Failed to bind UDP socket for Microphone RTP");
    }

    // Retrieve actual assigned port if bound dynamically (port 0)
    sockaddr_in bound_addr{};
    int addr_len = sizeof(bound_addr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&bound_addr), &addr_len) == 0) {
        bound_port_ = ntohs(bound_addr.sin_port);
    } else {
        bound_port_ = config_.local_bind_port;
    }

    socket_ = static_cast<uintptr_t>(sock);
    running_ = true;

    CPS_LOG_INFO("MicrophoneRtpSender started on local UDP port %u -> %s:%u",
                 bound_port_, config_.destination_ip.c_str(), config_.destination_port);

    return Common::Status::Ok();
}

void MicrophoneRtpSender::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }

    running_ = false;
    if (socket_ != ~static_cast<uintptr_t>(0)) {
        socket_t s = static_cast<socket_t>(socket_);
        CLOSE_SOCK(s);
        socket_ = ~static_cast<uintptr_t>(0);
    }

    bound_port_ = 0;
    sample_accumulator_.clear();
}

bool MicrophoneRtpSender::IsRunning() const noexcept {
    return running_.load();
}

void MicrophoneRtpSender::PushPcmData(Common::ByteSpan pcmBytes, uint64_t timestampUs) {
    if (pcmBytes.empty()) return;

    if (config_.input_sample_type == Common::AudioSampleType::PcmFloat32) {
        const size_t sampleCount = pcmBytes.size() / sizeof(float);
        const float* floatSamples = reinterpret_cast<const float*>(pcmBytes.data());
        PushFloatSamples(floatSamples, sampleCount, timestampUs);
    } else {
        const size_t sampleCount = pcmBytes.size() / sizeof(int16_t);
        const int16_t* pcmSamples = reinterpret_cast<const int16_t*>(pcmBytes.data());
        PushPcmSamples(pcmSamples, sampleCount, timestampUs);
    }
}

void MicrophoneRtpSender::PushFloatSamples(const float* samples, size_t count, uint64_t timestampUs) {
    if (!samples || count == 0) return;
    std::vector<int16_t> pcm(count);
    for (size_t i = 0; i < count; ++i) {
        float f = samples[i];
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        pcm[i] = static_cast<int16_t>(f * 32767.0f);
    }
    PushPcmSamples(pcm.data(), count, timestampUs);
}

void MicrophoneRtpSender::PushPcmSamples(const int16_t* samples, size_t count, uint64_t /*timestampUs*/) {
    if (!samples || count == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;

    sample_accumulator_.insert(sample_accumulator_.end(), samples, samples + count);

    const size_t frameSize = config_.packet_samples;
    while (sample_accumulator_.size() >= frameSize) {
        std::vector<int16_t> chunk(sample_accumulator_.begin(), sample_accumulator_.begin() + frameSize);
        sample_accumulator_.erase(sample_accumulator_.begin(), sample_accumulator_.begin() + frameSize);
        FlushPacketLocked(chunk);
    }
}

void MicrophoneRtpSender::FlushPacketLocked(const std::vector<int16_t>& chunk) {
    if (chunk.empty()) return;

    // Standard 12-byte RTP header + payload
    // RFC 3550:
    // 0                   1                   2                   3
    // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |V=2|P|X|  CC   |M|     PT      |       sequence number         |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                           timestamp                           |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |           synchronization source (SSRC) identifier            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    std::vector<uint8_t> rtpPacket;
    rtpPacket.resize(12 + chunk.size() * 2);

    rtpPacket[0] = 0x80; // V=2, P=0, X=0, CC=0
    rtpPacket[1] = config_.payload_type & 0x7F; // M=0, PT=payload_type (100)

    // Sequence number (Big-Endian)
    rtpPacket[2] = static_cast<uint8_t>((seq_ >> 8) & 0xFF);
    rtpPacket[3] = static_cast<uint8_t>(seq_ & 0xFF);

    // Timestamp (Big-Endian)
    rtpPacket[4] = static_cast<uint8_t>((ts_ >> 24) & 0xFF);
    rtpPacket[5] = static_cast<uint8_t>((ts_ >> 16) & 0xFF);
    rtpPacket[6] = static_cast<uint8_t>((ts_ >> 8) & 0xFF);
    rtpPacket[7] = static_cast<uint8_t>(ts_ & 0xFF);

    // SSRC (Big-Endian)
    rtpPacket[8]  = static_cast<uint8_t>((config_.ssrc >> 24) & 0xFF);
    rtpPacket[9]  = static_cast<uint8_t>((config_.ssrc >> 16) & 0xFF);
    rtpPacket[10] = static_cast<uint8_t>((config_.ssrc >> 8) & 0xFF);
    rtpPacket[11] = static_cast<uint8_t>(config_.ssrc & 0xFF);

    // Apple expects Big-Endian 16-bit PCM for the uplink stream payload
    uint8_t* payloadPtr = rtpPacket.data() + 12;
    for (size_t i = 0; i < chunk.size(); ++i) {
        int16_t sample = chunk[i];
        payloadPtr[i * 2]     = static_cast<uint8_t>((sample >> 8) & 0xFF);
        payloadPtr[i * 2 + 1] = static_cast<uint8_t>(sample & 0xFF);
    }

    // Deliver to interceptor if set
    if (interceptor_) {
        interceptor_(Common::ByteSpan(rtpPacket.data(), rtpPacket.size()));
    }

    // Send UDP packet over network socket
    if (socket_ != ~static_cast<uintptr_t>(0)) {
        sockaddr_in dst_addr{};
        dst_addr.sin_family = AF_INET;
        dst_addr.sin_port = htons(config_.destination_port);
        inet_pton(AF_INET, config_.destination_ip.c_str(), &dst_addr.sin_addr);

        socket_t s = static_cast<socket_t>(socket_);
        sendto(s, reinterpret_cast<const char*>(rtpPacket.data()),
               static_cast<int>(rtpPacket.size()), 0,
               reinterpret_cast<const sockaddr*>(&dst_addr), sizeof(dst_addr));
    }

    seq_ = static_cast<uint16_t>((seq_ + 1) & 0xFFFF);
    ts_ += static_cast<uint32_t>(chunk.size());
    packets_sent_++;
}

void MicrophoneRtpSender::AttachToUplink(const std::shared_ptr<IMicrophoneUplink>& uplink) {
    if (!uplink) return;
    uplink->SetUplinkCallback([this](Common::ByteSpan pcmData, uint64_t timestampUs) {
        PushPcmData(pcmData, timestampUs);
    });
}

uint16_t MicrophoneRtpSender::GetBoundPort() const noexcept {
    return bound_port_;
}

uint64_t MicrophoneRtpSender::GetPacketsSent() const noexcept {
    return packets_sent_.load();
}

uint16_t MicrophoneRtpSender::GetLastSequenceNumber() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return seq_;
}

uint32_t MicrophoneRtpSender::GetLastTimestamp() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return ts_;
}

void MicrophoneRtpSender::SetPacketInterceptor(PacketInterceptor interceptor) {
    std::lock_guard<std::mutex> lock(mutex_);
    interceptor_ = std::move(interceptor);
}

} // namespace CarPlayStation::Core::Media
