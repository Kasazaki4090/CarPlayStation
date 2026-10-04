#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Media/AacCodecAdapter.h"
#include "Core/Media/IMicrophoneUplink.h"
#include <array>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>
#include <cstdint>

namespace CarPlayStation::Core::Media {

/**
 * @brief Configuration for AirPlay Reverse Audio (Microphone) RTP Sender.
 */
struct MicrophoneRtpConfig {
    std::string destination_ip{"127.0.0.1"};
    uint16_t destination_port{5002};
    uint16_t local_bind_port{0};            // 0 = ephemeral dynamic port
    uint8_t payload_type{100};               // Stream type 100 (MainAudio voice input)
    uint32_t ssrc{0};
    uint32_t sample_rate{16000};             // Typically 16kHz or 24kHz mono
    uint16_t channels{1};
    size_t packet_samples{320};              // ~20ms frames (320 samples @ 16kHz mono)
    Common::AudioSampleType input_sample_type{Common::AudioSampleType::PcmFloat32};

    // Wireless CarPlay encodes the uplink instead of sending linear PCM: the
    // iPhone selects AAC-ELD (16 kHz mono, 24 kbps, 480-sample frames) for the
    // voice streams. When enabled, packet_samples is replaced by the encoder's
    // frame length.
    bool aac_eld{false};
    uint32_t bitrate_bps{24000};
    // ChaCha20-Poly1305 key derived from the stream's streamConnectionID
    // (*input* direction). Ignored unless aac_eld is set.
    bool encrypted{false};
    std::array<uint8_t, 32> input_key{};
};

/**
 * @brief Real-time RTP packetizer and UDP transmitter for CarPlay Microphone Uplink.
 *
 * Takes captured/resampled PCM from WasapiMicrophoneUplink, converts samples
 * to network byte order (Big-Endian) required by Apple iOS, frames into standard 12-byte
 * RTP packets (Payload Type 100), and transmits over UDP to the iPhone.
 */
class MicrophoneRtpSender {
public:
    using PacketInterceptor = std::function<void(Common::ByteSpan rtpPacket)>;

    MicrophoneRtpSender();
    ~MicrophoneRtpSender();

    /**
     * @brief Start RTP sender with configuration and bind UDP socket.
     */
    [[nodiscard]] Common::Status Start(const MicrophoneRtpConfig& config);

    /**
     * @brief Stop RTP sender and close socket.
     */
    void Stop();

    /**
     * @brief Check whether the sender is running.
     */
    [[nodiscard]] bool IsRunning() const noexcept;

    /**
     * @brief Feed raw PCM bytes (from WASAPI or upstream) into RTP packetizer.
     */
    void PushPcmData(Common::ByteSpan pcmBytes, uint64_t timestampUs);

    /**
     * @brief Feed float32 samples into RTP packetizer (converted to S16).
     */
    void PushFloatSamples(const float* samples, size_t count, uint64_t timestampUs);

    /**
     * @brief Feed 16-bit PCM samples into RTP packetizer.
     */
    void PushPcmSamples(const int16_t* samples, size_t count, uint64_t timestampUs);

    /**
     * @brief Wire directly to an IMicrophoneUplink instance as its callback.
     */
    void AttachToUplink(const std::shared_ptr<IMicrophoneUplink>& uplink);

    /**
     * @brief Get bound local UDP port.
     */
    [[nodiscard]] uint16_t GetBoundPort() const noexcept;

    /**
     * @brief Total count of RTP packets dispatched.
     */
    [[nodiscard]] uint64_t GetPacketsSent() const noexcept;

    /**
     * @brief Latest RTP sequence number.
     */
    [[nodiscard]] uint16_t GetLastSequenceNumber() const noexcept;

    /**
     * @brief Latest RTP timestamp.
     */
    [[nodiscard]] uint32_t GetLastTimestamp() const noexcept;

    /**
     * @brief Intercept outgoing RTP packets in memory (for testing without live sockets).
     */
    void SetPacketInterceptor(PacketInterceptor interceptor);

private:
    void FlushPcmPacketLocked(const std::vector<int16_t>& chunk);
    void FlushAacFrameLocked(const int16_t* pcm);
    // Writes the 12-byte RTP header shared by both payload formats.
    void WriteHeaderLocked(uint8_t* hdr);
    void SendLocked(const uint8_t* data, size_t len);

    mutable std::mutex mutex_;
    MicrophoneRtpConfig config_{};
    std::atomic<bool> running_{false};
    uintptr_t socket_{~static_cast<uintptr_t>(0)}; // socket handle (SOCKET / int)
    uint16_t bound_port_{0};

    std::vector<int16_t> sample_accumulator_;
    uint16_t seq_{0};
    uint32_t ts_{0};
    std::atomic<uint64_t> packets_sent_{0};
    PacketInterceptor interceptor_;

    // AAC-ELD uplink state.
    std::unique_ptr<AacAudioEncoder> encoder_;
    uint64_t nonce_counter_{0};
    std::vector<uint8_t> send_buffer_;
};

} // namespace CarPlayStation::Core::Media
