#include "Core/Media/AirPlayAudioDemuxer.h"

#include "Core/Common/Logging.h"
#include "Core/Common/Types.h"
#include "Core/Session/Pairing/Crypto.h"

#include <cstring>

namespace CarPlayStation::Core::Media {

namespace {

constexpr size_t kRtpHeaderSize = 12;
constexpr size_t kAeadTagSize = 16;
constexpr size_t kNonceSuffixSize = 8;
constexpr size_t kAadSize = 8;  // RTP timestamp + SSRC fields

}  // namespace

void AirPlayAudioDemuxer::SetDecryptionKey(const std::array<uint8_t, 32>& key) {
    std::memcpy(m_key.data(), key.data(), m_key.size());
    m_encrypted = true;
    CPS_LOG_INFO("AudioDemux", "audio decryption key set (encrypted stream)");
}

void AirPlayAudioDemuxer::ClearDecryptionKey() {
    m_key.fill(0);
    m_encrypted = false;
}

bool AirPlayAudioDemuxer::Demux(const uint8_t* data, size_t len) {
    m_stats.packetsReceived++;

    if (!data) {
        m_stats.malformedPackets++;
        return false;
    }

    // RTP header sanity check. CarPlay audio uses version 0/2 with a fixed
    // 12-byte header and no CSRCP or extensions.
    const uint8_t version = (data[0] >> 6) & 0x3F;
    const size_t headerSize = kRtpHeaderSize + static_cast<size_t>(data[0] & 0x0F) * 4;
    // 加密流必须容得下 AEAD tag + 尾部 nonce；未加密的 PCM 流没有这两段。
    const size_t overhead = m_encrypted ? kAeadTagSize + kNonceSuffixSize : 0;
    if (version != 2 || len < headerSize + overhead + 1) {
        m_stats.malformedPackets++;
        return false;
    }

    const uint16_t seq = Common::Endian::ReadBe16(data + 2);
    const uint32_t timestamp = Common::Endian::ReadBe32(data + 4);
    const uint8_t payloadType = data[1] & 0x7F;

    if (m_hasLastSeq && seq != static_cast<uint16_t>(m_lastSeq + 1)) {
        m_stats.seqDiscontinuities++;
        CPS_LOG_DEBUG("AudioDemux", "seq discontinuity: ", m_lastSeq, " -> ", seq,
                      " (pt ", static_cast<int>(payloadType), ")");
    }
    m_hasLastSeq = true;
    m_lastSeq = seq;

    const uint8_t* auStart = data + headerSize;
    const size_t auLen = len - headerSize - overhead;

    AudioPacket pkt;
    pkt.payload_type = payloadType;
    pkt.seq = seq;
    pkt.timestamp = timestamp;

    if (m_encrypted) {
        // carplayd stream.rs::nonce12()：nonce12 = 4 字节 0 || 报文尾 8 字节。
        uint8_t nonce12[12] = {};
        std::memcpy(nonce12 + 4, data + len - kNonceSuffixSize, kNonceSuffixSize);

        std::vector<uint8_t> plaintext;
        if (!pairing::ChaCha20Poly1305OpenWithAad(
                m_key.data(), nonce12, data + 4, kAadSize, auStart, auLen + kAeadTagSize,
                plaintext)) {
            m_stats.decryptErrors++;
            if (m_stats.decryptErrors <= 5 || (m_stats.decryptErrors % 100) == 0) {
                CPS_LOG_WARN("AudioDemux", "audio decrypt failed (seq ", seq, ", pt ",
                             static_cast<int>(payloadType), ", len ", len, ")");
            }
            return false;
        }
        pkt.au = std::move(plaintext);
    } else {
        pkt.au.assign(auStart, auStart + auLen);
    }

    if (pkt.au.empty()) {
        m_stats.malformedPackets++;
        return false;
    }

    m_stats.packetsEmitted++;
    if (m_onPacket) {
        m_onPacket(pkt);
    }
    return true;
}

void AirPlayAudioDemuxer::Reset() {
    m_stats = {};
    m_hasLastSeq = false;
    m_lastSeq = 0;
}

}  // namespace CarPlayStation::Core::Media
