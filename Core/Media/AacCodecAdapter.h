#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace CarPlayStation::Core::Media {

// The audio codec selected during AirPlay/CarPlay RTSP negotiation.
enum class AudioCodecKind : uint8_t {
    Unknown = 0,
    PcmS16Be = 1, // Uncompressed 16-bit big-endian PCM (wired CarPlay)
    AacLc = 2,    // AAC-LC, raw (no ADTS) access units
    AacEld = 3    // AAC-ELD, raw access units (CarPlay voice / Siri)
};

[[nodiscard]] constexpr const char* AudioCodecKindName(AudioCodecKind kind) noexcept {
    switch (kind) {
        case AudioCodecKind::PcmS16Be: return "PcmS16Be";
        case AudioCodecKind::AacLc:    return "AacLc";
        case AudioCodecKind::AacEld:   return "AacEld";
        default:                       return "Unknown";
    }
}

// A negotiated audio stream description.
struct AudioStreamFormat {
    AudioCodecKind codec{AudioCodecKind::Unknown};
    uint32_t sample_rate{48000};
    uint8_t channels{2};
    uint32_t bitrate_bps{0}; // Encoder only; 0 = pick a sensible default for the format
};

// Decoded PCM: interleaved signed 16-bit samples at the stream's own rate/channel count.
// The audio pipeline resamples to the render device format.
struct DecodedAudioFrame {
    uint32_t sample_rate{0};
    uint8_t channels{0};
    std::vector<int16_t> pcm;
};

// AAC decoder hidden behind this interface (fdk-aac lives in Lib/, never in Core's headers).
class AacAudioDecoder {
public:
    AacAudioDecoder();
    ~AacAudioDecoder();

    AacAudioDecoder(const AacAudioDecoder&) = delete;
    AacAudioDecoder& operator=(const AacAudioDecoder&) = delete;

    // Configure the decoder for a negotiated stream. `asc` is the optional AudioSpecificConfig
    // (empty = let the decoder lock onto the first access unit).
    bool Open(const AudioStreamFormat& format, const std::vector<uint8_t>& asc = {});
    void Close();

    // Decode one raw access unit. Returns false when the AU produced no output yet (priming)
    // or on an unrecoverable error; `concealed` is set when error concealment filled the frame.
    bool Decode(const uint8_t* au, size_t au_len, DecodedAudioFrame& out, bool& concealed);

    // Ask the decoder to emit one concealed frame (used after a lost packet).
    bool Conceal(DecodedAudioFrame& out);

    [[nodiscard]] uint32_t sample_rate() const noexcept { return m_rate; }
    [[nodiscard]] uint8_t channels() const noexcept { return m_channels; }
    [[nodiscard]] uint32_t frame_size() const noexcept { return m_frame_size; }
    [[nodiscard]] bool is_open() const noexcept { return m_handle != nullptr; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    uint32_t m_rate{0};
    uint8_t m_channels{0};
    uint32_t m_frame_size{0};
    std::vector<int16_t> m_scratch;
    void* m_handle{nullptr};
};

// AAC-ELD encoder for the microphone uplink.
class AacAudioEncoder {
public:
    AacAudioEncoder();
    ~AacAudioEncoder();

    AacAudioEncoder(const AacAudioEncoder&) = delete;
    AacAudioEncoder& operator=(const AacAudioEncoder&) = delete;

    bool Open(const AudioStreamFormat& format);
    void Close();

    // Encode one frame of interleaved S16 PCM (frame_size() samples per channel).
    // Returns the raw access unit bytes; empty while the encoder is priming or on error.
    std::vector<uint8_t> Encode(const int16_t* pcm, size_t samples_per_channel);

    [[nodiscard]] uint32_t frame_size() const noexcept { return m_frame_size; }
    [[nodiscard]] uint8_t channels() const noexcept { return m_channels; }
    [[nodiscard]] const std::vector<uint8_t>& audio_specific_config() const noexcept { return m_asc; }
    [[nodiscard]] bool is_open() const noexcept { return m_handle != nullptr; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    uint32_t m_frame_size{0};
    uint8_t m_channels{0};
    std::vector<uint8_t> m_asc;
    void* m_handle{nullptr};
};

// Parse the sample rate + channel count out of an AudioSpecificConfig (ISO/IEC 14496-3).
// Handles the escaped audioObjectType (AOT >= 31, e.g. AAC-ELD) and the escaped 24-bit
// sampling frequency.  SBR signalling and PCE-defined channel configs are not used by CarPlay.
bool ParseAudioSpecificConfig(const uint8_t* asc, size_t asc_len,
                             uint32_t& sample_rate, uint8_t& channels) noexcept;

} // namespace CarPlayStation::Core::Media
