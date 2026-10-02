#pragma once

#include "Core/Service/ICarPlayCoreApi.h"
#include "Core/Service/CoreEventBus.h"
#include "Core/Hardware/IMfiSigner.h"
#include "Core/Transport/ITransportChannel.h"
#include "Core/Protocol/IIap2Link.h"
#include "Core/Session/ICarPlaySession.h"
#include "Core/Media/IVideoDecoder.h"
#include "Core/Media/IAudioPipeline.h"
#include "Core/Media/IMicrophoneUplink.h"
#include "Core/Input/IInputEventSink.h"
#include <memory>
#include <mutex>

namespace CarPlayStation::Core::Transport {
    class IWifiSoftApManager;
}
namespace CarPlayStation::Core::Session {
    class AirPlayRtspServer;
}
namespace CarPlayStation::Core::Media {
    class AirPlayVideoReceiver;
}

namespace CarPlayStation::Core::Service {

// ============================================================================
// Dependency injection struct for CarPlayCoreService.
//
// All pointers are optional. When a pointer is null, the service will
// construct a default (real) implementation. When a pointer is provided,
// the service takes ownership via shared_ptr (for mock-based testing).
// ============================================================================
struct CarPlayCoreDependencies {
    std::shared_ptr<Hardware::IMfiSigner> mfi_signer;
    std::shared_ptr<Transport::ITransportChannel> transport;
    std::shared_ptr<Protocol::IIap2Link> iap2_link;
    std::shared_ptr<Session::ICarPlaySession> session;
    std::shared_ptr<Media::IVideoDecoder> video_decoder;
    std::shared_ptr<Media::IAudioPipeline> audio_pipeline;
    std::shared_ptr<Media::IMicrophoneUplink> microphone_uplink;
    std::shared_ptr<Input::IInputEventSink> input_sink;
};

/**
 * @brief CarPlay Core Service Facade.
 *
 * Central orchestrator managing the lifecycles of Layers 1-6 (Hardware,
 * Transport, Protocol, Session, Media, Input). Exposes a clean,
 * high-cohesion, framework-independent C++ public API to the Application
 * layer (DEC-002).
 *
 * Thread-safety:
 *   - Initialize / Stop are guarded by a mutex (single-threaded lifecycle).
 *   - Subsystem accessors return shared_ptr (thread-safe reference counting).
 *   - Event bus is internally thread-safe.
 */
class CarPlayCoreService final : public ICarPlayCoreApi {
public:
    explicit CarPlayCoreService(CarPlayCoreDependencies deps = {});
    ~CarPlayCoreService() override;

    // --- ICarPlayCoreApi Implementation ---
    [[nodiscard]] Common::Status Initialize(const Common::CoreEnvironmentConfig& env) override;
    [[nodiscard]] Common::Status UpdateEnvironment(const Common::CoreEnvironmentConfig& env) override;
    [[nodiscard]] Common::Status Start() override;
    void Stop() override;

    [[nodiscard]] std::shared_ptr<Session::ICarPlaySession> GetSession() override;
    [[nodiscard]] std::shared_ptr<Media::IVideoDecoder> GetVideoDecoder() override;
    [[nodiscard]] std::shared_ptr<Media::IAudioPipeline> GetAudioPipeline() override;
    [[nodiscard]] std::shared_ptr<Input::IInputEventSink> GetInputSink() override;

    [[nodiscard]] uint64_t SubscribeToEvents(CoreEventCallback callback) override;
    void UnsubscribeFromEvents(uint64_t token) override;

    // Swap the MFi signer at runtime (e.g. after the user picks a backend in
    // the settings drawer). Updates both the stored signer and the live
    // iAP2 packet handler so the next MFi exchange uses the new backend.
    [[nodiscard]] Common::Status SetMfiSigner(std::shared_ptr<Hardware::IMfiSigner> signer) override;

    // --- Additional accessors (not part of ICarPlayCoreApi) ---
    [[nodiscard]] std::shared_ptr<Hardware::IMfiSigner> GetMfiSigner() const;
    [[nodiscard]] std::shared_ptr<Transport::ITransportChannel> GetTransport() const;
    [[nodiscard]] std::shared_ptr<Protocol::IIap2Link> GetIap2Link() const;
    [[nodiscard]] std::shared_ptr<Media::IMicrophoneUplink> GetMicrophoneUplink() const;
    [[nodiscard]] CoreEventBus& GetEventBus() noexcept { return event_bus_; }

    // Check if the service has been initialized.
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

private:
    void WireSessionStateObserver();
    void TearDownSubsystems();

    CarPlayCoreDependencies deps_;
    CoreEventBus event_bus_;

    // Owned subsystem instances (constructed in Initialize when deps are null).
    std::shared_ptr<Hardware::IMfiSigner> mfi_signer_;
    std::shared_ptr<Transport::ITransportChannel> transport_;
    std::shared_ptr<Protocol::IIap2Link> iap2_link_;
    std::shared_ptr<Protocol::IIap2PacketHandler> iap2_handler_;
    std::shared_ptr<Session::ICarPlaySession> session_;
    std::shared_ptr<Media::IVideoDecoder> video_decoder_;
    std::shared_ptr<Media::IAudioPipeline> audio_pipeline_;
    std::shared_ptr<Media::IMicrophoneUplink> microphone_uplink_;
    std::shared_ptr<Input::IInputEventSink> input_sink_;

    std::shared_ptr<Transport::IWifiSoftApManager> wifi_manager_;
    std::shared_ptr<Session::AirPlayRtspServer> rtsp_server_;
    std::shared_ptr<Media::AirPlayVideoReceiver> video_receiver_;

    Common::CoreEnvironmentConfig env_config_;
    bool initialized_{false};
    bool started_{false};
    mutable std::mutex lifecycle_mutex_;
};

} // namespace CarPlayStation::Core::Service
