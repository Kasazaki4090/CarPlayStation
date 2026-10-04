#ifndef CARPLAY_CORE_MEDIA_AIRPLAYAUDIODEMUXER_H
#define CARPLAY_CORE_MEDIA_AIRPLAYAUDIODEMUXER_H

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace CarPlayStation::Core::Media {

// Demuxes and decrypts CarPlay / AirPlay 2 *audio* RTP packets.
//
// Wire format (carplayd receiver_core/crates/receiver/src/stream.rs):
//
//   [RTP header 12][ciphertext][Poly1305 tag 16][nonce suffix 8]
//
// The 12-byte nonce is the eight-byte nonce suffix prefixed with four zero
// bytes. The AEAD additional authenticated data is the timestamp + SSRC field
// pair (header bytes 4..12) for every audio stream on modern iOS. Note that
// this differs from the video stream, which authenticates the whole header.
//
// The demuxer hands the caller the decrypted (plaintext) AudioUnit. Decoding it
// is the caller's business, which keeps this class testable without a codec.
class AirPlayAudioDemuxer {
public:
    struct AudioPacket {
        uint8_t payload_type{0};
        uint16_t seq{0};
        uint32_t timestamp{0};
        // Plaintext AudioUnit: one raw AAC access unit, or big-endian signed
        // 16-bit PCM when the stream was negotiated uncompressed.
        std::vector<uint8_t> au;
    };

    using PacketCallback = std::function<void(const AudioPacket&)>;

    struct Stats {
        uint64_t packetsReceived{0};
        uint64_t packetsEmitted{0};
        uint64_t decryptErrors{0};
        uint64_t malformedPackets{0};
        uint64_t seqDiscontinuities{0};
    };

    AirPlayAudioDemuxer() = default;

    void SetPacketCallback(PacketCallback cb) { m_onPacket = std::move(cb); }

    // 32-byte ChaCha20-Poly1305 key for this stream (the pairing-derived
    // *output* key). Without a key the payload is treated as plaintext.
    void SetDecryptionKey(const std::array<uint8_t, 32>& key);
    void ClearDecryptionKey();
    bool IsEncrypted() const { return m_encrypted; }

    // Feed one datagram.
    bool Demux(const uint8_t* data, size_t len);

    const Stats& GetStats() const { return m_stats; }
    void Reset();

    // Smallest datagram that can carry a valid encrypted audio packet.
    static constexpr size_t kMinEncryptedPacket = 12 + 16 + 8;

private:
    PacketCallback m_onPacket;
    std::array<uint8_t, 32> m_key{};
    bool m_encrypted{false};
    bool m_hasLastSeq{false};
    uint16_t m_lastSeq{0};
    Stats m_stats{};
};

}  // namespace CarPlayStation::Core::Media

#endif  // CARPLAY_CORE_MEDIA_AIRPLAYAUDIODEMUXER_H
