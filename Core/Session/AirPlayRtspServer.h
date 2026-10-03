#pragma once

#include "Core/Session/AirPlayRtspTypes.h"
#include "Core/Session/AirPlayRtspCodec.h"
#include "Core/Session/Pairing/PairSetup.h"
#include "Core/Session/Pairing/PairVerify.h"
#include "Core/Common/Result.h"
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <map>

namespace CarPlayStation::Core::Hardware {
class IMfiSigner;
}

namespace CarPlayStation::Core::Session {

/**
 * @brief Callback signature for handling inbound RTSP requests and generating responses.
 */
using RtspRequestHandler = std::function<RtspResponse(const RtspRequest& req)>;
using StreamingStartedCallback = std::function<void(const NegotiatedMediaParams&)>;

/**
 * @brief High-performance asynchronous RTSP control channel server for AirPlay & CarPlay.
 *
 * Listens on a TCP port (default 5000 or dynamic 0 for testing), parses incoming RTSP requests,
 * negotiates streaming parameters, and delivers responses to the connecting iPhone.
 */
class AirPlayRtspServer {
public:
    struct Options {
        uint16_t port{5000};                           // 0 = ephemeral dynamic port
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
     * @brief Register a callback invoked when RTSP streaming starts (RECORD request).
     */
    void SetStreamingStartedCallback(StreamingStartedCallback callback);

    /**
     * @brief Enable the RTSP pair-setup handshake (POST /pair-setup) with the given
     *        accessory identity and a callback that records each newly-paired controller.
     */
    void SetPairSetupIdentity(Pairing::AccessoryIdentity identity, Pairing::PeerSaver saver);

    /**
     * @brief Enable the RTSP pair-verify handshake (POST /pair-verify) with the given
     *        accessory identity and a callback that looks up paired controllers.
     */
    void SetPairVerify(Pairing::AccessoryIdentity identity, Pairing::PeerFinder finder);

    /**
     * @brief Configure the MFi authentication signer for /auth-setup (MFi-SAP handshake).
     */
    void SetMfiSigner(std::shared_ptr<Hardware::IMfiSigner> signer);

    /**
     * @brief Query currently negotiated streaming parameters.
     */
    [[nodiscard]] NegotiatedMediaParams GetNegotiatedParams() const noexcept;

    /**
     * @brief Manually update or preset negotiated parameters.
     */
    void SetNegotiatedParams(NegotiatedMediaParams params);

    /**
     * @brief Send a HID input report to the active AirPlay event channel.
     */
    [[nodiscard]] Common::Status SendHidReport(uint32_t deviceUid, Common::ByteSpan report);

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
