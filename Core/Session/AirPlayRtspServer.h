#pragma once

#include "Core/Session/AirPlayRtspTypes.h"
#include "Core/Session/AirPlayRtspCodec.h"
#include "Core/Common/Result.h"
#include <functional>
#include <memory>
#include <string>
#include <mutex>

namespace CarPlayStation::Core::Session {

/**
 * @brief Callback signature for handling inbound RTSP requests and generating responses.
 */
using RtspRequestHandler = std::function<RtspResponse(const RtspRequest& req)>;

/**
 * @brief High-performance asynchronous RTSP control channel server for AirPlay & CarPlay.
 *
 * Listens on a TCP port (default 7000 or dynamic 0 for testing), parses incoming RTSP requests,
 * negotiates streaming parameters, and delivers responses to the connecting iPhone.
 */
class AirPlayRtspServer {
public:
    struct Options {
        uint16_t port{7000};                           // 0 = ephemeral dynamic port
        std::string bind_address{"0.0.0.0"};
        uint32_t default_video_width{1280};
        uint32_t default_video_height{720};
        uint32_t default_fps{60};
    };

    explicit AirPlayRtspServer(Options options = {});
    ~AirPlayRtspServer();

    /**
     * @brief Start listening on the configured TCP port.
     */
    [[nodiscard]] Common::Status Start();

    /**
     * @brief Stop the server and terminate active client connections.
     */
    void Stop();

    /**
     * @brief Check whether the listener is active.
     */
    [[nodiscard]] bool IsRunning() const noexcept;

    /**
     * @brief Get the actual bound TCP port (useful when configured with port=0 for tests).
     */
    [[nodiscard]] uint16_t GetBoundPort() const noexcept;

    /**
     * @brief Count of currently connected iPhone clients.
     */
    [[nodiscard]] size_t GetActiveClientCount() const noexcept;

    /**
     * @brief Override or intercept standard request handling with a custom handler.
     */
    void SetRequestHandler(RtspRequestHandler handler);

    /**
     * @brief Query currently negotiated streaming parameters.
     */
    [[nodiscard]] NegotiatedMediaParams GetNegotiatedParams() const noexcept;

    /**
     * @brief Manually update or preset negotiated parameters.
     */
    void SetNegotiatedParams(NegotiatedMediaParams params);

    // --- Simulation & In-Memory Testing ---

    /**
     * @brief Process an RTSP request in-memory without network sockets.
     */
    [[nodiscard]] RtspResponse ProcessSimulatedRequest(const RtspRequest& req);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace CarPlayStation::Core::Session
