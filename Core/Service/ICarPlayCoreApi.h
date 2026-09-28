#pragma once

#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include "Core/Session/ICarPlaySession.h"
#include "Core/Media/IVideoDecoder.h"
#include "Core/Media/IAudioPipeline.h"
#include "Core/Input/IInputEventSink.h"
#include <memory>

namespace CarPlayStation::Core::Service {

class ICarPlayCoreApi {
public:
    virtual ~ICarPlayCoreApi() = default;

    // Initialize all core subsystems with full dynamic configuration
    [[nodiscard]] virtual Common::Status Initialize(const Common::CarPlayStationConfig& config) = 0;

    // Dynamically update runtime configuration
    [[nodiscard]] virtual Common::Status UpdateConfig(const Common::CarPlayStationConfig& config) = 0;

    // Start CarPlay advertising and discovery
    [[nodiscard]] virtual Common::Status Start() = 0;

    // Stop all active sessions and transports
    virtual void Stop() = 0;

    // Accessors for subsystem interfaces
    [[nodiscard]] virtual std::shared_ptr<Session::ICarPlaySession> GetSession() = 0;
    [[nodiscard]] virtual std::shared_ptr<Media::IVideoDecoder> GetVideoDecoder() = 0;
    [[nodiscard]] virtual std::shared_ptr<Media::IAudioPipeline> GetAudioPipeline() = 0;
    [[nodiscard]] virtual std::shared_ptr<Input::IInputEventSink> GetInputSink() = 0;
};

} // namespace CarPlayStation::Core::Service
