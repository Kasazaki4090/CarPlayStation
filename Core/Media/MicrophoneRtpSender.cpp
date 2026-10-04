#include "Core/Media/MicrophoneRtpSender.h"
#include "Core/Common/Logging.h"
#include "Core/Session/Pairing/Crypto.h"
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

    seq_ = 0;
    ts_ = 0;
    packets_sent_ = 0;
    nonce_counter_ = 0;
    sample_accumulator_.clear();

    if (config_.aac_eld) {
        // The C reference receiver encodes the mic at AAC-ELD 16 kHz mono,
        // 24 kbps, 480-sample frames. Frame length comes from the encoder.
        AudioStreamFormat enc{};
        enc.codec = AudioCodecKind::AacEld;
        enc.sample_rate = config_.sample_rate;
        enc.channels = static_cast<uint8_t>(config_.channels);
        enc.bitrate_bps = config_.bitrate_bps ? config_.bitrate_bps : 24000;
        encoder_ = std::make_unique<AacAudioEncoder>();
        if (!encoder_->Open(enc)) {
            encoder_.reset();
            return Common::MakeError(Common::StatusCode::IoError, "Failed to open AAC-ELD encoder for microphone uplink");
        }
        config_.packet_samples = encoder_->frame_size();
        CPS_LOG_INFO("MicrophoneRtpSender", "AAC-ELD uplink encoder open: rate=", config_.sample_rate,
                     " ch=", static_cast<int>(config_.channels), " bitrate=", enc.bitrate_bps,
                     " frame=", encoder_->frame_size(), " asc=",
                     Common::ToHexString(Common::ByteSpan(encoder_->audio_specific_config().data(),
                                                         encoder_->audio_specific_config().size())),
                     " encrypted=", config_.encrypted ? 1 : 0);
    }

    if (config_.packet_samples == 0) {
        config_.packet_samples = 320; // Default ~20ms @ 16kHz
    }

    sample_accumulator_.reserve(config_.packet_samples * 2);
    send_buffer_.reserve(1536);

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

    CPS_LOG_INFO("MicrophoneRtpSender", "started on local UDP port ", bound_port_, " -> ",
                 config_.destination_ip, ":", std::to_string(config_.destination_port));

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
    encoder_.reset();
    nonce_counter_ = 0;
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
    if (encoder_) {
        while (sample_accumulator_.size() >= frameSize) {
            // The encoder consumes interleaved S16 in host order; the AU it
            // emits is what goes on the wire.
            FlushAacFrameLocked(sample_accumulator_.data());
            sample_accumulator_.erase(sample_accumulator_.begin(), sample_accumulator_.begin() + frameSize);
        }
        return;
    }

    while (sample_accumulator_.size() >= frameSize) {
        std::vector<int16_t> chunk(sample_accumulator_.begin(), sample_accumulator_.begin() + frameSize);
        sample_accumulator_.erase(sample_accumulator_.begin(), sample_accumulator_.begin() + frameSize);
        FlushPcmPacketLocked(chunk);
    }
}

void MicrophoneRtpSender::WriteHeaderLocked(uint8_t* hdr) {
    // Standard 12-byte RTP header
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
    hdr[0] = 0x80; // V=2, P=0, X=0, CC=0
    hdr[1] = config_.payload_type & 0x7F; // M=0, PT=payload_type (100)
    Common::Endian::WriteBe16(hdr + 2, seq_);
    Common::Endian::WriteBe32(hdr + 4, ts_);
    // SSRC stays 0 to match the reference C uplink.
    Common::Endian::WriteBe32(hdr + 8, config_.ssrc);
}

void MicrophoneRtpSender::FlushPcmPacketLocked(const std::vector<int16_t>& chunk) {
    if (chunk.empty()) return;

    std::vector<uint8_t> rtpPacket;
    rtpPacket.resize(12 + chunk.size() * 2);

    WriteHeaderLocked(rtpPacket.data());

    // Apple expects Big-Endian 16-bit PCM for the uplink stream payload
    uint8_t* payloadPtr = rtpPacket.data() + 12;
    for (size_t i = 0; i < chunk.size(); ++i) {
        Common::Endian::WriteBe16(payloadPtr + i * 2, static_cast<uint16_t>(chunk[i]));
    }

    if (interceptor_) {
        interceptor_(Common::ByteSpan(rtpPacket.data(), rtpPacket.size()));
    }

    SendLocked(rtpPacket.data(), rtpPacket.size());

    seq_ = static_cast<uint16_t>(seq_ + 1);
    ts_ += static_cast<uint32_t>(chunk.size());
    packets_sent_++;
}

// One AAC-ELD access unit per RTP packet: [header][ciphertext][tag:16][nonce:8].
// The AAD is the timestamp+SSRC fields of the header, matching the downlink.
void MicrophoneRtpSender::FlushAacFrameLocked(const int16_t* pcm) {
    if (!encoder_) return;

    const size_t frameSize = encoder_->frame_size();
    std::vector<uint8_t> au = encoder_->Encode(pcm, frameSize);

    // Advance the RTP clock per input frame even while the encoder is priming
    // (empty AU), so timestamps stay aligned to the audio sample position.
    const uint32_t advance = static_cast<uint32_t>(frameSize);
    if (au.empty()) {
        ts_ += advance;
        return;
    }

    uint8_t hdr[12];
    WriteHeaderLocked(hdr);

    send_buffer_.clear();
    if (config_.encrypted) {
        uint8_t nonce12[12] = {0};
        for (int i = 0; i < 8; ++i) {
            nonce12[4 + i] = static_cast<uint8_t>((nonce_counter_ >> (8 * i)) & 0xFF);
        }
        const uint8_t* nonce8 = nonce12 + 4;
        // AAD = hdr[4..12] (timestamp ‖ SSRC).
        std::vector<uint8_t> sealed = pairing::ChaCha20Poly1305SealWithAad(
            config_.input_key.data(), nonce12, hdr + 4, 8, au.data(), au.size());
        if (sealed.empty()) {
            CPS_LOG_ERROR("MicrophoneRtpSender", "AAC uplink AEAD seal failed; packet dropped");
            ts_ += advance;
            return;
        }
        send_buffer_.reserve(12 + sealed.size() + 8);
        send_buffer_.insert(send_buffer_.end(), hdr, hdr + 12);
        send_buffer_.insert(send_buffer_.end(), sealed.begin(), sealed.end());
        send_buffer_.insert(send_buffer_.end(), nonce8, nonce8 + 8);
        nonce_counter_++;
    } else {
        send_buffer_.reserve(12 + au.size());
        send_buffer_.insert(send_buffer_.end(), hdr, hdr + 12);
        send_buffer_.insert(send_buffer_.end(), au.begin(), au.end());
    }

    if (interceptor_) {
        interceptor_(Common::ByteSpan(send_buffer_.data(), send_buffer_.size()));
    }

    SendLocked(send_buffer_.data(), send_buffer_.size());

    seq_ = static_cast<uint16_t>(seq_ + 1);
    ts_ += advance;
    packets_sent_++;
}

void MicrophoneRtpSender::SendLocked(const uint8_t* data, size_t len) {
    if (socket_ == ~static_cast<uintptr_t>(0) || !data || len == 0) {
        return;
    }
    sockaddr_in dst_addr{};
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(config_.destination_port);
    inet_pton(AF_INET, config_.destination_ip.c_str(), &dst_addr.sin_addr);

    socket_t s = static_cast<socket_t>(socket_);
    sendto(s, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
           reinterpret_cast<const sockaddr*>(&dst_addr), sizeof(dst_addr));
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
