#include "Core/Media/AacCodecAdapter.h"

#include "Core/Common/Logging.h"
#include "Core/Common/Types.h"

#include <aacenc_lib.h>
#include <aacdecoder_lib.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace CarPlayStation::Core::Media {
namespace {

constexpr const char* kLogTag = "AacCodec";

[[nodiscard]] std::string HexErr(uint32_t v) {
    char buf[11]{};
    std::snprintf(buf, sizeof(buf), "0x%04X", v & 0xFFFFu);
    return buf;
}

// fdk-aac's built-in samplingFrequency table (ISO/IEC 14496-3 Table 1.18).
constexpr uint32_t kFreqTable[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000,  7350,  0,     0,     0};

// A bit reader over the AudioSpecificConfig.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t len) noexcept : m_p(data), m_bits(len * 8) {}

    uint32_t Read(size_t n) noexcept {
        uint32_t v = 0;
        for (size_t i = 0; i < n; ++i) {
            if (m_pos >= m_bits) return 0xFFFFFFFFu;
            v = (v << 1) | ((m_p[m_pos >> 3] >> (7 - (m_pos & 7))) & 1u);
            ++m_pos;
        }
        return v;
    }

    [[nodiscard]] bool ok() const noexcept { return m_pos <= m_bits; }

private:
    const uint8_t* m_p{nullptr};
    size_t m_bits{0};
    size_t m_pos{0};
};

} // namespace

bool ParseAudioSpecificConfig(const uint8_t* asc, size_t asc_len,
                             uint32_t& sample_rate, uint8_t& channels) noexcept {
    if (!asc || asc_len < 2) return false;
    BitReader br(asc, asc_len);

    // Both fields use the ISO/IEC 14496-3 "escaped value" coding: an all-ones prefix means the
    // real value is the prefix value plus the following extension field.
    uint32_t aot = br.Read(5);
    if (aot == 0xFFFFFFFFu) return false;
    if (aot == 31) {
        const uint32_t ext = br.Read(6);
        if (ext == 0xFFFFFFFFu) return false;
        aot += ext;
    }

    uint32_t freq_index = br.Read(4);
    if (freq_index == 0xFFFFFFFFu) return false;
    bool escaped_freq = false;
    uint32_t freq_ext = 0;
    if (freq_index == 15) {
        escaped_freq = true;
        freq_ext = br.Read(24); // explicit 24-bit sampling frequency
        if (freq_ext == 0xFFFFFFFFu) return false;
    }

    uint32_t rate = 0;
    if (escaped_freq) {
        rate = freq_ext; // the escape carries an absolute rate in Hz
    } else if (freq_index < 13) {
        rate = kFreqTable[freq_index];
    } else {
        return false; // 13/14 are reserved
    }
    if (rate == 0 || rate > 192000) return false;

    const uint32_t chan = br.Read(4);
    if (chan > 8) return false; // 0 = PCE-defined, which CarPlay never negotiates

    if (!br.ok()) return false;
    sample_rate = rate;
    channels = static_cast<uint8_t>(chan);
    return true;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

struct AacAudioDecoder::Impl {};

AacAudioDecoder::AacAudioDecoder() = default;
AacAudioDecoder::~AacAudioDecoder() { Close(); }

bool AacAudioDecoder::Open(const AudioStreamFormat& format, const std::vector<uint8_t>& asc) {
    Close();
    if (format.codec != AudioCodecKind::AacLc && format.codec != AudioCodecKind::AacEld) {
        CPS_LOG_ERROR(kLogTag, "unsupported codec: ", AudioCodecKindName(format.codec));
        return false;
    }

    HANDLE_AACDECODER h = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!h) {
        CPS_LOG_ERROR(kLogTag, "aacDecoder_Open failed");
        return false;
    }

    // Prefer the explicit AudioSpecificConfig when we have one (the iPhone's own config for
    // the voice stream); otherwise let the decoder lock onto the first access unit.
    if (!asc.empty()) {
        std::vector<UCHAR> conf(asc.begin(), asc.end());
        UINT conf_len = static_cast<UINT>(conf.size());
        UCHAR* conf_ptr = conf.data();
        const AAC_DECODER_ERROR cfg_err = aacDecoder_ConfigRaw(h, &conf_ptr, &conf_len);
        if (cfg_err != AAC_DEC_OK) {
            // Not fatal: the decoder can still sync on the bitstream itself.
            CPS_LOG_WARN(kLogTag, "aacDecoder_ConfigRaw rejected the AudioSpecificConfig: ",
                         HexErr(static_cast<uint32_t>(cfg_err)));
        }
    }

    // Keep the decoder's native channel order and 16-bit interleaved output (fdk-aac 2.x always
    // emits interleaved INT_PCM, there is no output-layout parameter).
    const INT chans = static_cast<INT>(format.channels ? format.channels : 2);
    aacDecoder_SetParam(h, AAC_PCM_MIN_OUTPUT_CHANNELS, chans);
    aacDecoder_SetParam(h, AAC_PCM_MAX_OUTPUT_CHANNELS, chans);
    aacDecoder_SetParam(h, AAC_CONCEAL_METHOD, 1); // noise substitution

    m_handle = h;
    m_rate = format.sample_rate;
    m_channels = format.channels;
    m_frame_size = format.codec == AudioCodecKind::AacEld ? 480 : 1024;
    m_scratch.resize(static_cast<size_t>(m_frame_size) * static_cast<size_t>(chans));

    CPS_LOG_INFO(kLogTag, "decoder ready: ", AudioCodecKindName(format.codec), " ", m_rate, " Hz x",
                 static_cast<int>(m_channels), " frame=", m_frame_size,
                 asc.empty() ? " (sync on bitstream)" : " (explicit AudioSpecificConfig)");
    return true;
}

void AacAudioDecoder::Close() {
    if (m_handle) {
        aacDecoder_Close(static_cast<HANDLE_AACDECODER>(m_handle));
        m_handle = nullptr;
    }
    m_rate = 0;
    m_channels = 0;
    m_frame_size = 0;
    m_scratch.clear();
}

bool AacAudioDecoder::Decode(const uint8_t* au, size_t au_len, DecodedAudioFrame& out, bool& concealed) {
    out.pcm.clear();
    out.sample_rate = m_rate;
    out.channels = m_channels;
    concealed = false;
    if (!m_handle || !au || au_len == 0) return false;

    HANDLE_AACDECODER h = static_cast<HANDLE_AACDECODER>(m_handle);

    std::vector<UCHAR> buf(au, au + au_len);
    UINT valid = static_cast<UINT>(buf.size());
    UINT capacity = valid;
    UCHAR* ptr = buf.data();
    const AAC_DECODER_ERROR fill_err = aacDecoder_Fill(h, &ptr, &capacity, &valid);
    if (fill_err != AAC_DEC_OK) {
        CPS_LOG_WARN(kLogTag, "aacDecoder_Fill failed: ", HexErr(static_cast<uint32_t>(fill_err)));
        return false;
    }

    const AAC_DECODER_ERROR err =
        aacDecoder_DecodeFrame(h, m_scratch.data(), static_cast<INT>(m_scratch.size()), 0);
    if (!IS_OUTPUT_VALID(err)) {
        if (err != AAC_DEC_NOT_ENOUGH_BITS) {
            CPS_LOG_WARN(kLogTag, "aacDecoder_DecodeFrame failed: ", HexErr(static_cast<uint32_t>(err)));
        }
        return false;
    }

    // Trust the decoder's view of the stream: it knows the real rate/channel count once synced.
    if (CStreamInfo* si = aacDecoder_GetStreamInfo(h)) {
        if (si->sampleRate > 0) m_rate = static_cast<uint32_t>(si->sampleRate);
        if (si->numChannels > 0) m_channels = static_cast<uint8_t>(si->numChannels);
        if (si->frameSize > 0) m_frame_size = static_cast<uint32_t>(si->frameSize);
    }

    const size_t frames = static_cast<size_t>(m_frame_size);
    const size_t chans = m_channels ? m_channels : 1;
    out.sample_rate = m_rate;
    out.channels = m_channels;
    out.pcm.assign(m_scratch.begin(), m_scratch.begin() + frames * chans);
    concealed = (err != AAC_DEC_OK);
    return true;
}

bool AacAudioDecoder::Conceal(DecodedAudioFrame& out) {
    out.pcm.clear();
    out.sample_rate = m_rate;
    out.channels = m_channels;
    if (!m_handle) return false;

    HANDLE_AACDECODER h = static_cast<HANDLE_AACDECODER>(m_handle);
    const AAC_DECODER_ERROR err =
        aacDecoder_DecodeFrame(h, m_scratch.data(), static_cast<INT>(m_scratch.size()), AACDEC_CONCEAL);
    if (!IS_OUTPUT_VALID(err)) return false;

    const size_t frames = static_cast<size_t>(m_frame_size);
    const size_t chans = m_channels ? m_channels : 1;
    out.pcm.assign(m_scratch.begin(), m_scratch.begin() + frames * chans);
    return true;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

struct AacAudioEncoder::Impl {};

AacAudioEncoder::AacAudioEncoder() = default;
AacAudioEncoder::~AacAudioEncoder() { Close(); }

bool AacAudioEncoder::Open(const AudioStreamFormat& format) {
    Close();
    if (format.channels != 1 && format.channels != 2) {
        CPS_LOG_ERROR(kLogTag, "encoder needs 1 or 2 channels");
        return false;
    }

    HANDLE_AACENCODER h = nullptr;
    if (aacEncOpen(&h, 0, static_cast<UINT>(format.channels)) != AACENC_OK) {
        CPS_LOG_ERROR(kLogTag, "aacEncOpen failed");
        return false;
    }

    // The CarPlay microphone uplink is AAC-ELD: the iPhone only accepts ELD for the voice
    // stream, and the 480-sample frame is what keeps Siri/Call latency usable.
    const bool eld = format.codec != AudioCodecKind::AacLc;
    auto set_param = [&](AACENC_PARAM p, UINT v) {
        if (const AACENC_ERROR err = aacEncoder_SetParam(h, p, v); err != AACENC_OK) {
            CPS_LOG_WARN(kLogTag, "aacEncoder_SetParam(", static_cast<int>(p), ") failed: ",
                         static_cast<int>(err));
        }
    };

    set_param(AACENC_AOT, eld ? static_cast<UINT>(AOT_ER_AAC_ELD) : static_cast<UINT>(AOT_AAC_LC));
    set_param(AACENC_SAMPLERATE, static_cast<UINT>(format.sample_rate));
    set_param(AACENC_CHANNELMODE, format.channels == 1 ? static_cast<UINT>(MODE_1)
                                                      : static_cast<UINT>(MODE_2));
    set_param(AACENC_BITRATE,
              static_cast<UINT>(format.bitrate_bps ? format.bitrate_bps
                                                   : (format.channels == 1 ? 24000 : 96000)));
    set_param(AACENC_TRANSMUX, static_cast<UINT>(TT_MP4_RAW)); // raw access units, no ADTS framing
    set_param(AACENC_AFTERBURNER, 0);
    if (eld) {
        set_param(AACENC_GRANULE_LENGTH, 480);
    }

    // The NULL-argument call performs the actual configuration.
    if (const AACENC_ERROR err = aacEncEncode(h, nullptr, nullptr, nullptr, nullptr); err != AACENC_OK) {
        CPS_LOG_ERROR(kLogTag, "aacEncEncode configure failed: ", static_cast<int>(err));
        aacEncClose(&h);
        return false;
    }

    AACENC_InfoStruct info{};
    if (aacEncInfo(h, &info) != AACENC_OK) {
        CPS_LOG_ERROR(kLogTag, "aacEncInfo failed");
        aacEncClose(&h);
        return false;
    }

    m_frame_size = static_cast<uint32_t>(info.frameLength);
    m_channels = info.inputChannels ? static_cast<uint8_t>(info.inputChannels) : format.channels;
    m_asc.assign(info.confBuf, info.confBuf + info.confSize);
    m_handle = h;

    CPS_LOG_INFO(kLogTag, "encoder ready: ", eld ? "AAC-ELD" : "AAC-LC", " ", format.sample_rate, " Hz x",
                 static_cast<int>(format.channels), " frame=", m_frame_size, " asc=",
                 Common::ToHexString(Common::ByteSpan(m_asc.data(), m_asc.size())));
    return true;
}

void AacAudioEncoder::Close() {
    if (m_handle) {
        HANDLE_AACENCODER h = static_cast<HANDLE_AACENCODER>(m_handle);
        aacEncClose(&h);
        m_handle = nullptr;
    }
    m_frame_size = 0;
    m_channels = 0;
    m_asc.clear();
}

std::vector<uint8_t> AacAudioEncoder::Encode(const int16_t* pcm, size_t samples_per_channel) {
    if (!m_handle || !pcm || !m_frame_size) return {};
    HANDLE_AACENCODER h = static_cast<HANDLE_AACENCODER>(m_handle);

    // Samples are interleaved INT_PCM (16-bit), so the element size is 2 bytes.
    INT el_size = static_cast<INT>(sizeof(INT_PCM));
    INT in_size = static_cast<INT>(samples_per_channel) * m_channels * el_size;
    INT in_ident = IN_AUDIO_DATA;
    void* in_buf = const_cast<int16_t*>(pcm);

    AACENC_BufDesc in_desc{};
    in_desc.numBufs = 1;
    in_desc.bufs = &in_buf;
    in_desc.bufferIdentifiers = &in_ident;
    in_desc.bufSizes = &in_size;
    in_desc.bufElSizes = &el_size;

    std::vector<uint8_t> out(4096);
    INT out_ident = OUT_BITSTREAM_DATA;
    INT out_size = static_cast<INT>(out.size());
    INT out_el = 1;
    void* out_buf = out.data();

    AACENC_BufDesc out_desc{};
    out_desc.numBufs = 1;
    out_desc.bufs = &out_buf;
    out_desc.bufferIdentifiers = &out_ident;
    out_desc.bufSizes = &out_size;
    out_desc.bufElSizes = &out_el;

    AACENC_InArgs in_args{};
    in_args.numInSamples = static_cast<INT>(samples_per_channel) * m_channels;
    AACENC_OutArgs out_args{};

    if (const AACENC_ERROR err = aacEncEncode(h, &in_desc, &out_desc, &in_args, &out_args); err != AACENC_OK) {
        CPS_LOG_WARN(kLogTag, "aacEncEncode failed: ", static_cast<int>(err));
        return {};
    }
    if (out_args.numOutBytes == 0) return {}; // priming
    out.resize(static_cast<size_t>(out_args.numOutBytes));
    return out;
}

} // namespace CarPlayStation::Core::Media
