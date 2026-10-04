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
#include <vector>

namespace CarPlayStation::Core::Transport {
    class IWifiSoftApManager;
    class IMdnsAdvertiser;
}
namespace CarPlayStation::Core::Session {
    class AirPlayRtspServer;
    class IPairingStore;
    struct NegotiatedMediaParams;
}
namespace CarPlayStation::Core::Media {
    class AirPlayVideoReceiver;
    class AirPlayAudioReceiver;
    class MicrophoneRtpSender;
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
    // Bring up the Wi-Fi SoftAP + mDNS advertiser so the device is always
    // discoverable (continuous broadcasting), independent of an active session.
    void StartPersistentBroadcasting();
    // (Re)build the per-lane audio downlink receivers from an RTSP SETUP
    // phase-2 negotiation and hand each lane its stream decryption key.
    void ConfigureAudioReceivers(const Session::NegotiatedMediaParams& params);
    void ConfigureMicrophoneUplink(const Session::NegotiatedMediaParams& params);

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
    // RTP packetizer that carries captured microphone audio back to the iPhone
    // (AAC-ELD sealed with ChaCha20-Poly1305, or legacy PCM). Armed from the
    // RTSP SETUP phase-2 negotiation when the iPhone requests microphone input.
    std::shared_ptr<Media::MicrophoneRtpSender> microphone_sender_;
    // Format the audio pipeline was actually started with. Audio receivers
    // resample into this so their output is injectable without further work.
    Common::AudioOutputFormat audio_output_{};
    std::shared_ptr<Input::IInputEventSink> input_sink_;

    std::shared_ptr<Transport::IWifiSoftApManager> wifi_manager_;
    std::shared_ptr<Transport::IMdnsAdvertiser> mdns_advertiser_;
    std::shared_ptr<Session::AirPlayRtspServer> rtsp_server_;
    std::shared_ptr<Media::AirPlayVideoReceiver> video_receiver_;
    // One audio downlink receiver per negotiated lane (ports 7000/7001/7002).
    // Built lazily from the RTSP SETUP phase-2 stream negotiation.
    std::vector<std::shared_ptr<Media::AirPlayAudioReceiver>> audio_receivers_;
    // Shared pairing store (also owned by the session) so the service can
    // look up known devices by MAC and proactively dial a re-joining iPhone's
    // _carplay-ctrl port (fast reconnect without re-doing the BT/iAP2 flow).
    std::shared_ptr<Session::IPairingStore> pairing_store_;

    Common::CoreEnvironmentConfig env_config_;
    // Real device identity (Bluetooth MAC string) used for the mDNS
    // AirPlay-Receiver-Device-ID so the iPhone accepts the connection.
    std::string device_id_;
    // Last observed iPhone WiFi (station) MAC, captured when the station
    // joins the SoftAP. Used to publish the device-connected event once the
    // real Bluetooth MAC becomes known (the BT connection establishes after
    // the WiFi join, so the event must be emitted from the BT path).
    std::string last_wifi_mac_;
    bool initialized_{false};
    bool started_{false};
    mutable std::mutex lifecycle_mutex_;
};

} // namespace CarPlayStation::Core::Service
