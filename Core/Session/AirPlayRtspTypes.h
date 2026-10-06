#pragma once

#include "Core/Common/Types.h"
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <cstdint>
#include <charconv>
#include <algorithm>
#include <array>

namespace CarPlayStation::Core::Session {

enum class RtspMethod {
    Options,
    Announce,
    Setup,
    Record,
    SetParameter,
    Teardown,
    GetParameter,
    Post,
    Get,
    Unknown
};

[[nodiscard]] constexpr std::string_view RtspMethodToString(RtspMethod method) noexcept {
    switch (method) {
        case RtspMethod::Options:       return "OPTIONS";
        case RtspMethod::Announce:      return "ANNOUNCE";
        case RtspMethod::Setup:         return "SETUP";
        case RtspMethod::Record:        return "RECORD";
        case RtspMethod::SetParameter:  return "SET_PARAMETER";
        case RtspMethod::Teardown:      return "TEARDOWN";
        case RtspMethod::GetParameter:  return "GET_PARAMETER";
        case RtspMethod::Post:          return "POST";
        case RtspMethod::Get:           return "GET";
        case RtspMethod::Unknown:       return "UNKNOWN";
    }
    return "UNKNOWN";
}

[[nodiscard]] inline RtspMethod StringToRtspMethod(std::string_view sv) noexcept {
    if (sv == "OPTIONS")       return RtspMethod::Options;
    if (sv == "ANNOUNCE")      return RtspMethod::Announce;
    if (sv == "SETUP")         return RtspMethod::Setup;
    if (sv == "RECORD")        return RtspMethod::Record;
    if (sv == "SET_PARAMETER") return RtspMethod::SetParameter;
    if (sv == "TEARDOWN")      return RtspMethod::Teardown;
    if (sv == "GET_PARAMETER") return RtspMethod::GetParameter;
    if (sv == "POST")          return RtspMethod::Post;
    if (sv == "GET")           return RtspMethod::Get;
    return RtspMethod::Unknown;
}

using RtspStatusCode = uint16_t;
namespace RtspStatus {
    constexpr RtspStatusCode Ok = 200;
    constexpr RtspStatusCode BadRequest = 400;
    constexpr RtspStatusCode Unauthorized = 401;
    constexpr RtspStatusCode NotFound = 404;
    constexpr RtspStatusCode MethodNotAllowed = 405;
    constexpr RtspStatusCode SessionNotFound = 454;
    constexpr RtspStatusCode InternalServerError = 500;
}

[[nodiscard]] constexpr std::string_view RtspStatusCodeToString(RtspStatusCode code) noexcept {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 453: return "Not Enough Bandwidth";
        case 454: return "Session Not Found";
        case 500: return "Internal Server Error";
    }
    return "Unknown";
}

struct CaseInsensitiveCompare {
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](char c1, char c2) {
                return static_cast<unsigned char>(std::tolower(c1)) <
                       static_cast<unsigned char>(std::tolower(c2));
            });
    }
};

using RtspHeaders = std::map<std::string, std::string, CaseInsensitiveCompare>;

struct RtspRequest {
    RtspMethod method{RtspMethod::Unknown};
    std::string uri;
    std::string protocol{"RTSP/1.0"};
    RtspHeaders headers;
    std::vector<uint8_t> body;

    [[nodiscard]] std::string_view GetHeader(std::string_view name) const noexcept {
        auto it = headers.find(std::string(name));
        if (it != headers.end()) {
            return it->second;
        }
        return {};
    }

    [[nodiscard]] uint32_t GetCSeq() const noexcept {
        auto it = headers.find("CSeq");
        if (it != headers.end()) {
            uint32_t c = 0;
            auto sv = it->second;
            auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), c);
            if (ec == std::errc()) return c;
        }
        return 0;
    }

    [[nodiscard]] std::string_view Path() const noexcept {
        size_t p = uri.find("://");
        if (p != std::string::npos) {
            size_t slash = uri.find('/', p + 3);
            if (slash != std::string::npos) {
                return std::string_view(uri).substr(slash);
            }
        }
        return uri;
    }
};

struct RtspResponse {
    std::string protocol{"RTSP/1.0"};
    RtspStatusCode status_code{200};
    std::string status_phrase{"OK"};
    uint32_t cseq{0};
    RtspHeaders headers;
    std::vector<uint8_t> body;

    void SetHeader(std::string name, std::string value) {
        headers[std::move(name)] = std::move(value);
    }

    [[nodiscard]] std::string_view GetHeader(std::string_view name) const noexcept {
        auto it = headers.find(std::string(name));
        if (it != headers.end()) {
            return it->second;
        }
        return {};
    }
};

// Codec the iPhone selected for one audio stream, decoded from the `audioFormat` bitmask.
// The numeric values deliberately mirror Core/Media/AacCodecAdapter.h :: AudioCodecKind so the
// Service can static_cast them; Session must not include Media (Media already depends on Session).
enum class RtspAudioCodec : uint8_t {
    Unknown = 0,
    PcmS16Be = 1,
    AacLc = 2,
    AacEld = 3
};

// One entry of the SETUP phase-2 `streams` array with type 100/101/102.
struct AudioStreamNegotiation {
    int64_t type{0};                  // 100 = MainAudio, 101 = AltAudio, 102 = MainHighAudio
    std::string audio_type;           // "media" | "speechRecognition" | "telephony" | "alert" | ...
    RtspAudioCodec codec{RtspAudioCodec::Unknown};
    uint32_t sample_rate{0};
    uint8_t channels{0};
    uint64_t format_mask{0};          // raw audioFormat bitmask as offered by the iPhone
    uint64_t stream_connection_id{0};
    std::array<uint8_t, 32> output_key{}; // decrypt the iPhone -> accessory downlink
    std::array<uint8_t, 32> input_key{};  // encrypt the accessory -> iPhone mic uplink
    bool input_requested{false};      // type 100 with input=true: the iPhone wants our mic
    uint16_t iphone_uplink_port{0};   // the iPhone's dataPort for the mic uplink
    uint16_t data_port{0};            // the port we answered with (downlink)
    uint16_t control_port{0};         // answered for type 102 only
};

// Decode the CarPlay `audioFormat` bitmask into codec/sample rate/channel count.
// Faithful to carplayd's `decode_audio_format` (session.rs:818-834), with the PCM half of the
// table completed from xcertplay's AudioStream.PCM_FORMAT list: we now advertise the whole PCM
// range, so every bit in it has to decode rather than fall into the AAC-ELD fallback.
inline void DecodeAudioFormat(uint64_t mask, RtspAudioCodec& codec, uint32_t& sample_rate,
                              uint8_t& channels) {
    struct Row {
        uint64_t mask;
        RtspAudioCodec codec;
        uint32_t rate;
        uint8_t channels;
    };
    static constexpr Row kRows[] = {
        {0x0000000000000004ULL, RtspAudioCodec::PcmS16Be, 8000, 1},
        {0x0000000000000008ULL, RtspAudioCodec::PcmS16Be, 8000, 2},
        {0x0000000000000010ULL, RtspAudioCodec::PcmS16Be, 16000, 1},
        {0x0000000000000020ULL, RtspAudioCodec::PcmS16Be, 16000, 2},
        {0x0000000000000040ULL, RtspAudioCodec::PcmS16Be, 24000, 1},
        {0x0000000000000080ULL, RtspAudioCodec::PcmS16Be, 24000, 2},
        {0x0000000000000100ULL, RtspAudioCodec::PcmS16Be, 32000, 1},
        {0x0000000000000200ULL, RtspAudioCodec::PcmS16Be, 32000, 2},
        {0x0000000000000400ULL, RtspAudioCodec::PcmS16Be, 44100, 1},
        {0x0000000000000800ULL, RtspAudioCodec::PcmS16Be, 44100, 2},
        {0x0000000000004000ULL, RtspAudioCodec::PcmS16Be, 48000, 1},
        {0x0000000000008000ULL, RtspAudioCodec::PcmS16Be, 48000, 2},
        {0x0000000000400000ULL, RtspAudioCodec::AacLc, 44100, 2},
        {0x0000000000800000ULL, RtspAudioCodec::AacLc, 48000, 2},
        {0x0000000001000000ULL, RtspAudioCodec::AacEld, 44100, 2},
        {0x0000000002000000ULL, RtspAudioCodec::AacEld, 48000, 2},
        {0x0000000004000000ULL, RtspAudioCodec::AacEld, 16000, 1},
        {0x0000000008000000ULL, RtspAudioCodec::AacEld, 24000, 1},
        {0x0000000080000000ULL, RtspAudioCodec::AacEld, 44100, 1},
        {0x0000000100000000ULL, RtspAudioCodec::AacEld, 48000, 1},
    };
    for (const Row& row : kRows) {
        if (mask == row.mask) {
            codec = row.codec;
            sample_rate = row.rate;
            channels = row.channels;
            return;
        }
    }
    // The iPhone may offer several bits at once (e.g. 0x8010). Prefer the richest PCM entry,
    // matching carplayd's handling of the combined wired mask.
    static constexpr uint64_t kPcmPreference[] = {
        0x0000000000008000ULL, 0x0000000000004000ULL, 0x0000000000000800ULL,
        0x0000000000000400ULL, 0x0000000000000200ULL, 0x0000000000000100ULL,
        0x0000000000000080ULL, 0x0000000000000040ULL, 0x0000000000000020ULL,
        0x0000000000000010ULL, 0x0000000000000008ULL, 0x0000000000000004ULL,
    };
    for (uint64_t bit : kPcmPreference) {
        if (mask & bit) {
            for (const Row& row : kRows) {
                if (row.mask == bit) {
                    codec = row.codec;
                    sample_rate = row.rate;
                    channels = row.channels;
                    return;
                }
            }
        }
    }
    // Nothing matched: CarPlay's documented fallback is AAC-ELD 24 kHz mono.
    codec = RtspAudioCodec::AacEld;
    sample_rate = 24000;
    channels = 1;
}

struct NegotiatedMediaParams {
    uint32_t video_width{1280};
    uint32_t video_height{720};
    uint32_t fps{60};

    uint16_t video_data_port{0};
    uint16_t video_control_port{0};

    uint16_t screen_data_port{0};
    uint16_t screen_control_port{0};

    uint16_t main_audio_data_port{0};
    uint16_t main_audio_control_port{0};
    uint32_t audio_sample_rate{48000};
    uint8_t audio_channels{2};

    uint16_t alt_audio_data_port{0};
    uint16_t alt_audio_control_port{0};

    uint16_t mic_audio_data_port{0};
    uint16_t mic_audio_control_port{0};

    // Every audio stream the iPhone set up in SETUP phase 2, in request order. The
    // Service reads this to start one downlink receiver per lane and to arm the mic
    // uplink when the iPhone asked for input.
    std::vector<AudioStreamNegotiation> audio_streams;
    bool mic_input_requested{false};
    uint16_t mic_uplink_port{0};
    std::string mic_peer_address; // the RTSP control peer's address for the mic uplink
    RtspAudioCodec mic_codec{RtspAudioCodec::Unknown};
    uint32_t mic_sample_rate{0};
    uint8_t mic_channels{0};
    std::array<uint8_t, 32> mic_input_key{};

    std::string session_id;
    std::array<uint8_t, 32> video_decrypt_key{0};
    bool video_encrypted{false};
};

} // namespace CarPlayStation::Core::Session
