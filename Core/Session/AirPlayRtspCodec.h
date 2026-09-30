#pragma once

#include "Core/Session/AirPlayRtspTypes.h"
#include "Core/Common/Result.h"
#include "Core/Common/Types.h"
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace CarPlayStation::Core::Session {

/**
 * @brief Zero-copy RTSP request and response parser/serializer using C++20 std::string_view and std::span.
 */
class AirPlayRtspCodec {
public:
    AirPlayRtspCodec() = default;

    /**
     * @brief Parse a single complete standalone request buffer.
     */
    [[nodiscard]] static Common::Result<RtspRequest> ParseSingle(Common::ByteSpan data);

    /**
     * @brief Feed streaming data into the codec buffer.
     */
    void FeedData(Common::ByteSpan chunk);

    /**
     * @brief Pop next completely framed RTSP request from buffer if ready.
     */
    [[nodiscard]] std::optional<RtspRequest> PopRequest();

    /**
     * @brief Parse a raw incoming stream buffer.
     */
    [[nodiscard]] static Common::Result<std::optional<RtspRequest>> ParseRequest(
        std::span<const uint8_t> data,
        size_t& bytes_consumed);

    /**
     * @brief Serialize an RtspResponse struct into wire-format RFC 2326 bytes.
     */
    [[nodiscard]] static std::vector<uint8_t> SerializeResponse(const RtspResponse& response);

    /**
     * @brief Build standard OPTIONS response bytes.
     */
    [[nodiscard]] static std::vector<uint8_t> BuildOptionsResponse(const RtspRequest& req);

    /**
     * @brief Build standard SETUP response bytes with negotiated ports and session ID.
     */
    [[nodiscard]] static std::vector<uint8_t> BuildSetupResponse(
        const RtspRequest& req,
        uint16_t server_port,
        uint16_t control_port,
        std::string_view session_id);

    /**
     * @brief Build error response bytes.
     */
    [[nodiscard]] static std::vector<uint8_t> BuildErrorResponse(
        const RtspRequest& req,
        uint16_t status_code,
        std::string_view reason);

    /**
     * @brief Helper to generate a standard 200 OK response struct echoing CSeq.
     */
    [[nodiscard]] static RtspResponse MakeOkResponse(
        uint32_t cseq,
        std::string_view session_id = {});

    /**
     * @brief Helper to generate error response struct.
     */
    [[nodiscard]] static RtspResponse MakeErrorResponse(
        RtspStatusCode code,
        uint32_t cseq = 0);

    /**
     * @brief Helper to extract Content-Length from header map if present.
     */
    [[nodiscard]] static size_t ExtractContentLength(const RtspHeaders& headers) noexcept;

private:
    std::vector<uint8_t> buffer_;
};

} // namespace CarPlayStation::Core::Session
