#pragma once

#include "Core/Common/Types.h"
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <cstdint>
#include <charconv>
#include <algorithm>

namespace CarPlayStation::Core::Session {

enum class RtspMethod {
    Options,
    Announce,
    Setup,
    Record,
    SetParameter,
    Teardown,
    GetParameter,
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

    std::string session_id;
};

} // namespace CarPlayStation::Core::Session
