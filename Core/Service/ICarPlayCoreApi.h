#pragma once

#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include "Core/Session/ICarPlaySession.h"
#include "Core/Media/IVideoDecoder.h"
#include "Core/Media/IAudioPipeline.h"
#include "Core/Input/IInputEventSink.h"
#include "Core/Hardware/IMfiSigner.h"
#include "Core/Service/CoreEventBus.h"
#include <memory>

namespace CarPlayStation::Core::Service {

class ICarPlayCoreApi {
public:
    virtual ~ICarPlayCoreApi() = default;

    // Initialize all core subsystems (device identity + network endpoints only).
    // Media parameters are negotiated per session and flow through the session, not here.
    [[nodiscard]] virtual Common::Status Initialize(const Common::CoreEnvironmentConfig& env) = 0;

    // Hot-update runtime environment configuration (e.g. RTSP bind port, SoftAP credentials)
    [[nodiscard]] virtual Common::Status UpdateEnvironment(const Common::CoreEnvironmentConfig& env) = 0;

    // Start CarPlay advertising and discovery
    [[nodiscard]] virtual Common::Status Start() = 0;

    // Stop all active sessions and transports
    virtual void Stop() = 0;

    // Accessors for subsystem interfaces
    [[nodiscard]] virtual std::shared_ptr<Session::ICarPlaySession> GetSession() = 0;
    [[nodiscard]] virtual std::shared_ptr<Media::IVideoDecoder> GetVideoDecoder() = 0;
    [[nodiscard]] virtual std::shared_ptr<Media::IAudioPipeline> GetAudioPipeline() = 0;
    [[nodiscard]] virtual std::shared_ptr<Input::IInputEventSink> GetInputSink() = 0;

    // Subscribe to Core events (connection state, NowPlaying, artwork, night mode, audio route).
    // Returns a token for later unsubscription.
    [[nodiscard]] virtual uint64_t SubscribeToEvents(CoreEventCallback callback) = 0;

    // Unsubscribe a previously registered event listener.
    virtual void UnsubscribeFromEvents(uint64_t token) = 0;

    // Swap the MFi signer at runtime (e.g. after the user picks a backend in
    // the settings drawer). Safe to call before or after StartMFiAuth.
    [[nodiscard]] virtual Common::Status SetMfiSigner(std::shared_ptr<Hardware::IMfiSigner> signer) = 0;
};

} // namespace CarPlayStation::Core::Service
