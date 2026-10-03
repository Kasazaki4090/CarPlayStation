#include "Core/Session/AirPlayRtspCodec.h"
#include <sstream>
#include <charconv>
#include <cstring>

namespace CarPlayStation::Core::Session {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

size_t AirPlayRtspCodec::ExtractContentLength(const RtspHeaders& headers) noexcept {
    auto it = headers.find("Content-Length");
    if (it == headers.end()) {
        return 0;
    }
    size_t length = 0;
    auto sv = Trim(it->second);
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), length);
    if (ec == std::errc()) {
        return length;
    }
    return 0;
}

Common::Result<RtspRequest> AirPlayRtspCodec::ParseSingle(Common::ByteSpan data) {
    size_t consumed = 0;
    auto res = ParseRequest(data, consumed);
    if (res.IsError()) {
        return res.Error();
    }
    if (!res.Value().has_value()) {
        return Common::MakeError(Common::StatusCode::InvalidArgument, "Incomplete RTSP frame");
    }
    return std::move(*res.Value());
}

void AirPlayRtspCodec::FeedData(Common::ByteSpan chunk) {
    if (!chunk.empty()) {
        buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    }
}

std::optional<RtspRequest> AirPlayRtspCodec::PopRequest() {
    if (buffer_.empty()) {
        return std::nullopt;
    }

    size_t consumed = 0;
    auto res = ParseRequest(buffer_, consumed);
    if (res.IsError() || !res.Value().has_value() || consumed == 0) {
        return std::nullopt;
    }

    RtspRequest req = std::move(*res.Value());
    buffer_.erase(buffer_.begin(), buffer_.begin() + consumed);
    return req;
}

Common::Result<std::optional<RtspRequest>> AirPlayRtspCodec::ParseRequest(
    std::span<const uint8_t> data,
    size_t& bytes_consumed) {

    bytes_consumed = 0;
    if (data.empty()) {
        return std::optional<RtspRequest>(std::nullopt);
    }

    std::string_view stream_sv(reinterpret_cast<const char*>(data.data()), data.size());

    // Locate double CRLF separating header section from body
    size_t header_end = stream_sv.find("\r\n\r\n");
    size_t delimiter_len = 4;
    if (header_end == std::string_view::npos) {
        header_end = stream_sv.find("\n\n");
        if (header_end == std::string_view::npos) {
            return std::optional<RtspRequest>(std::nullopt);
        }
        delimiter_len = 2;
    }

    std::string_view header_section = stream_sv.substr(0, header_end);
    size_t body_start_idx = header_end + delimiter_len;

    // Parse Request-Line (first line)
    size_t first_line_end = header_section.find('\n');
    if (first_line_end == std::string_view::npos) {
        return Common::MakeError(Common::StatusCode::InvalidArgument, "Malformed RTSP request: missing newline in request line");
    }

    std::string_view request_line = Trim(header_section.substr(0, first_line_end));
    
    // Split request line by space: METHOD URI VERSION
    size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos) {
        return Common::MakeError(Common::StatusCode::InvalidArgument, "Malformed RTSP request: missing URI in request line");
    }
    std::string_view method_sv = request_line.substr(0, first_space);
    
    size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return Common::MakeError(Common::StatusCode::InvalidArgument, "Malformed RTSP request: missing RTSP version in request line");
    }
    std::string_view uri_sv = request_line.substr(first_space + 1, second_space - (first_space + 1));
    std::string_view version_sv = request_line.substr(second_space + 1);

    RtspRequest req;
    req.method = StringToRtspMethod(method_sv);
    req.uri = std::string(uri_sv);
    req.protocol = std::string(version_sv);

    // Parse Headers
    size_t line_start = first_line_end + 1;
    while (line_start < header_section.size()) {
        size_t line_end = header_section.find('\n', line_start);
        std::string_view raw_line;
        if (line_end == std::string_view::npos) {
            raw_line = header_section.substr(line_start);
            line_start = header_section.size();
        } else {
            raw_line = header_section.substr(line_start, line_end - line_start);
            line_start = line_end + 1;
        }

        std::string_view line = Trim(raw_line);
        if (line.empty()) {
            continue;
        }

        size_t colon_pos = line.find(':');
        if (colon_pos == std::string_view::npos) {
            continue;
        }

        std::string header_name(Trim(line.substr(0, colon_pos)));
        std::string header_val(Trim(line.substr(colon_pos + 1)));

        req.headers[std::move(header_name)] = std::move(header_val);
    }

    // Check Body size via Content-Length
    size_t content_length = ExtractContentLength(req.headers);
    size_t available_body_bytes = data.size() - body_start_idx;

    if (available_body_bytes < content_length) {
        return std::optional<RtspRequest>(std::nullopt);
    }

    // Slice Body
    if (content_length > 0) {
        req.body.assign(data.data() + body_start_idx, data.data() + body_start_idx + content_length);
    }

    bytes_consumed = body_start_idx + content_length;
    return std::optional<RtspRequest>(std::move(req));
}

std::vector<uint8_t> AirPlayRtspCodec::SerializeResponse(const RtspResponse& response) {
    std::string out;
    out.reserve(256 + response.body.size());

    // Status line: RTSP/1.0 200 OK\r\n
    out += response.protocol.empty() ? "RTSP/1.0" : response.protocol;
    out += ' ';
    out += std::to_string(response.status_code);
    out += ' ';
    out += response.status_phrase.empty() ? RtspStatusCodeToString(response.status_code) : response.status_phrase;
    out += "\r\n";

    // CSeq
    if (response.cseq > 0 || response.headers.find("CSeq") == response.headers.end()) {
        out += "CSeq: ";
        out += std::to_string(response.cseq);
        out += "\r\n";
    }

    // Headers
    for (const auto& [name, val] : response.headers) {
        if (name == "CSeq" || name == "cseq") {
            continue;
        }
        out += name;
        out += ": ";
        out += val;
        out += "\r\n";
    }

    // Content-Length
    if (!response.body.empty() && response.headers.find("Content-Length") == response.headers.end()) {
        out += "Content-Length: ";
        out += std::to_string(response.body.size());
        out += "\r\n";
    }

    out += "\r\n";

    std::vector<uint8_t> result(out.begin(), out.end());
    if (!response.body.empty()) {
        result.insert(result.end(), response.body.begin(), response.body.end());
    }

    return result;
}

std::vector<uint8_t> AirPlayRtspCodec::BuildOptionsResponse(const RtspRequest& req) {
    RtspResponse resp;
    resp.protocol = req.protocol.empty() ? "RTSP/1.0" : req.protocol;
    resp.status_code = 200;
    resp.status_phrase = "OK";
    resp.cseq = req.GetCSeq();
    resp.SetHeader("Public", "OPTIONS, ANNOUNCE, SETUP, RECORD");
    resp.SetHeader("Server", "AirTunes/220.68");
    return SerializeResponse(resp);
}

std::vector<uint8_t> AirPlayRtspCodec::BuildSetupResponse(
    const RtspRequest& req,
    uint16_t server_port,
    uint16_t control_port,
    std::string_view session_id) {

    RtspResponse resp;
    resp.protocol = req.protocol.empty() ? "RTSP/1.0" : req.protocol;
    resp.status_code = 200;
    resp.status_phrase = "OK";
    resp.cseq = req.GetCSeq();
    resp.SetHeader("Server", "AirTunes/220.68");
    resp.SetHeader("Transport", "RTP/AVP/UDP;unicast;server_port=" + std::to_string(server_port) +
                                ";control_port=" + std::to_string(control_port));
    if (!session_id.empty()) {
        resp.SetHeader("Session", std::string(session_id));
    }
    return SerializeResponse(resp);
}

std::vector<uint8_t> AirPlayRtspCodec::BuildErrorResponse(
    const RtspRequest& req,
    uint16_t status_code,
    std::string_view reason) {

    RtspResponse resp;
    resp.protocol = req.protocol.empty() ? "RTSP/1.0" : req.protocol;
    resp.status_code = status_code;
    resp.status_phrase = std::string(reason);
    resp.cseq = req.GetCSeq();
    resp.SetHeader("Server", "AirTunes/220.68");
    return SerializeResponse(resp);
}

RtspResponse AirPlayRtspCodec::MakeOkResponse(uint32_t cseq, std::string_view session_id) {
    RtspResponse resp;
    resp.protocol = "RTSP/1.0";
    resp.status_code = 200;
    resp.status_phrase = "OK";
    resp.cseq = cseq;
    resp.SetHeader("Server", "AirTunes/320.17");
    if (!session_id.empty()) {
        resp.SetHeader("Session", std::string(session_id));
    }
    return resp;
}

RtspResponse AirPlayRtspCodec::MakeErrorResponse(RtspStatusCode code, uint32_t cseq) {
    RtspResponse resp;
    resp.protocol = "RTSP/1.0";
    resp.status_code = code;
    resp.status_phrase = std::string(RtspStatusCodeToString(code));
    resp.cseq = cseq;
    resp.SetHeader("Server", "AirPlay/320.17");
    return resp;
}

} // namespace CarPlayStation::Core::Session
