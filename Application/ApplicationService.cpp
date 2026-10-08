#include "Application/ApplicationService.h"
#include "Application/MfiSignerFactory.h"
#include "Core/Service/CoreEventBus.h"
#include "Core/Common/Logging.h"

#include <algorithm>
#include <cstdio>
#include <thread>

namespace CarPlayStation::Application {

ApplicationService::ApplicationService(std::shared_ptr<Core::Service::ICarPlayCoreApi> core_api)
    : core_api_(std::move(core_api)) {
}

ApplicationService::~ApplicationService() {
    StopCarPlay();
}

// ============================================================================
// IApplicationApi — Command Relay & Dispatch
// ============================================================================

Core::Common::Status ApplicationService::StartCarPlay(const AppSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_running_) {
        return Core::Common::MakeError(Core::Common::StatusCode::InvalidState,
            "CarPlay is already running");
    }

    current_settings_ = settings;

    // Resolve AppSettings → CoreEnvironmentConfig.
    // DevicePreferences → DeviceIdentity; network endpoints use defaults.
    Core::Common::CoreEnvironmentConfig env;
    env.identity.device_name = settings.device.device_name;
    env.identity.manufacturer = settings.device.manufacturer;
    env.identity.model = settings.device.model;
    // Network endpoints use their defaults (0.0.0.0:5000, etc.) —
    // they are environment facts, not user preferences (DEC-004).

    // Hand Core the display geometry before Initialize(): the RTSP /info advert is
    // built during Initialize, and it is what tells the iPhone what size to lay
    // out and encode.
    core_api_->SetVideoStreamSpec(ResolveVideoSpec(settings));
    // Seed the UI's video readout with the spec just handed down; the codec in
    // it is corrected later from the sender's first VideoConfig.
    SnapshotVideoFacts(ui_state_);

    auto status = core_api_->Initialize(env);
    if (status.IsError()) {
        Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Error, "AppService",
            "Core Initialize failed: ", status.Message());
        return status;
    }

    // Inject the UI's D3D11 device into the video decoder so its shared texture
    // is created on the SAME device the renderer samples from. Cross-device
    // shared textures in the same process are not coherent, so without this the
    // reader would never see the writer's updates (frozen first frame).
    if (video_shared_device_) {
        if (auto decoder = core_api_->GetVideoDecoder()) {
            decoder->SetSharedDevice(video_shared_device_, video_shared_context_);
        }
    }

    // Build + inject the MFi signer for the selected backend (LocalFile /
    // Network / HardwareI2C) so MFi auth uses the user's configuration.
    ApplyMfiSigner(settings);

    status = core_api_->Start();
    if (status.IsError()) {
        Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Error, "AppService",
            "Core Start failed: ", status.Message());
        core_api_->Stop();
        return status;
    }

    // Wire the frame consumer to the video decoder if already registered.
    if (frame_consumer_) {
        if (auto decoder = core_api_->GetVideoDecoder()) {
            decoder->SetFrameCallback(frame_consumer_);
            Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Info, "AppService",
                "Wired video frame consumer to video decoder");
        }
    }

    // Wire the CPU frame consumer (MpvVideoPlayer presentation path).
    if (cpu_frame_consumer_) {
        if (auto decoder = core_api_->GetVideoDecoder()) {
            decoder->SetCpuFrameCallback(cpu_frame_consumer_);
            Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Info, "AppService",
                "Wired CPU frame consumer to video decoder");
        }
    }

    // Subscribe to Core events.
    core_event_token_ = core_api_->SubscribeToEvents(
        [this](const Core::Service::CoreEvent& event) {
            OnCoreEvent(event);
        });

    // Apply initial audio volumes and ducking ratio to audio pipeline.
    if (auto audio = core_api_->GetAudioPipeline()) {
        audio->SetLaneVolume(Core::Media::AudioLane::MainMedia, settings.audio.media_volume);
        audio->SetLaneVolume(Core::Media::AudioLane::AltNavigation, settings.audio.navigation_volume);
        audio->SetLaneVolume(Core::Media::AudioLane::SiriTelephony, settings.audio.telephony_volume);
        audio->SetTargetDuckingRatio(settings.audio.ducking_ratio);
    }

    is_running_ = true;
    ui_state_.session_state = Core::Session::SessionState::Idle;
    ui_state_.is_streaming = false;
    ui_state_.status_message = "CarPlay started";
    ui_state_.active_settings = settings;

    // Notify UI (outside lock — copy callbacks first).
    std::vector<UiStateChangeCallback> cbs;
    {
        cbs = ui_state_callbacks_;
    }
    for (auto& cb : cbs) {
        cb(ui_state_);
    }

    Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Info, "AppService",
        "CarPlay started with device: ", settings.device.device_name);
    return Core::Common::Status::Ok();
}

Core::Common::Status ApplicationService::ApplySettings(const AppSettings& newSettings) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_running_) {
        return Core::Common::MakeError(Core::Common::StatusCode::InvalidState,
            "CarPlay is not running");
    }

    current_settings_ = newSettings;

    // Resolve and hot-update the core environment.
    Core::Common::CoreEnvironmentConfig env;
    env.identity.device_name = newSettings.device.device_name;
    env.identity.manufacturer = newSettings.device.manufacturer;
    env.identity.model = newSettings.device.model;

    auto status = core_api_->UpdateEnvironment(env);
    if (status.IsError()) {
        Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Warn, "AppService",
            "UpdateEnvironment failed: ", status.Message());
        return status;
    }

    // Rebuild + inject the MFi signer if the MFi backend changed.
    ApplyMfiSigner(newSettings);

    // Reconfigure video decoder if resolution/codec changed.
    {
        auto spec = ResolveVideoSpec(newSettings);
        // Keep the advertised geometry in sync for the next session; a phone that
        // already fetched /info keeps its current layout until then.
        core_api_->SetVideoStreamSpec(spec);
        if (auto decoder = core_api_->GetVideoDecoder()) {
            auto reconfig_status = decoder->Reconfigure(spec);
            if (reconfig_status.IsError()) {
                Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Warn, "AppService",
                    "Video decoder reconfigure failed: ", reconfig_status.Message());
            }
        }
        // The HUD follows the settings immediately; the codec is corrected later
        // from what the sender actually sends.
        SnapshotVideoFacts(ui_state_);
    }

    // Apply lane volumes and ducking attenuation ratio.
    if (auto audio = core_api_->GetAudioPipeline()) {
        audio->SetLaneVolume(Core::Media::AudioLane::MainMedia, newSettings.audio.media_volume);
        audio->SetLaneVolume(Core::Media::AudioLane::AltNavigation, newSettings.audio.navigation_volume);
        audio->SetLaneVolume(Core::Media::AudioLane::SiriTelephony, newSettings.audio.telephony_volume);
        audio->SetTargetDuckingRatio(newSettings.audio.ducking_ratio);
    }

    ui_state_.active_settings = newSettings;
    ui_state_.status_message = "Settings applied";

    // Notify UI.
    std::vector<UiStateChangeCallback> cbs;
    {
        cbs = ui_state_callbacks_;
    }
    for (auto& cb : cbs) {
        cb(ui_state_);
    }

    return Core::Common::Status::Ok();
}

void ApplicationService::StopCarPlay() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_running_) {
        return;
    }

    // Unsubscribe from core events.
    if (core_event_token_ != 0) {
        core_api_->UnsubscribeFromEvents(core_event_token_);
        core_event_token_ = 0;
    }

    core_api_->Stop();

    is_running_ = false;
    ui_state_.session_state = Core::Session::SessionState::Idle;
    ui_state_.is_streaming = false;
    ui_state_.status_message = "CarPlay stopped";

    // Notify UI.
    std::vector<UiStateChangeCallback> cbs;
    {
        cbs = ui_state_callbacks_;
    }
    for (auto& cb : cbs) {
        cb(ui_state_);
    }

    Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Info, "AppService", "CarPlay stopped");
}

// ============================================================================
// IApplicationApi — Event Observation
// ============================================================================

void ApplicationService::RegisterUiStateCallback(UiStateChangeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    ui_state_callbacks_.push_back(std::move(callback));
}

void ApplicationService::RegisterFrameConsumer(Core::Media::VideoFrameCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_consumer_ = std::move(callback);

    // Wire the frame consumer to the video decoder.
    if (auto decoder = core_api_->GetVideoDecoder()) {
        decoder->SetFrameCallback(frame_consumer_);
    }
}

void ApplicationService::RegisterCpuFrameConsumer(Core::Media::CpuFrameCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    cpu_frame_consumer_ = std::move(callback);

    // Wire the CPU frame consumer to the video decoder.
    if (auto decoder = core_api_->GetVideoDecoder()) {
        decoder->SetCpuFrameCallback(cpu_frame_consumer_);
    }
}

void ApplicationService::SetVideoSharedDevice(void* device, void* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    video_shared_device_ = device;
    video_shared_context_ = context;

    // Apply immediately if the decoder already exists (e.g. re-init path).
    if (auto decoder = core_api_->GetVideoDecoder()) {
        decoder->SetSharedDevice(device, context);
    }
}

// ============================================================================
// IApplicationApi — Input Forwarding
// ============================================================================

Core::Common::Status ApplicationService::ForwardTouchEvent(
    Core::Input::TouchAction action,
    const Core::Input::TouchPoint& point) {
    auto sink = core_api_->GetInputSink();
    if (!sink) {
        return Core::Common::MakeError(Core::Common::StatusCode::InvalidState,
            "Input sink not available");
    }
    return sink->SendTouchEvent(action, point);
}

Core::Common::Status ApplicationService::ForwardKnobEvent(
    Core::Input::KnobAction action,
    uint32_t stepCount) {
    auto sink = core_api_->GetInputSink();
    if (!sink) {
        return Core::Common::MakeError(Core::Common::StatusCode::InvalidState,
            "Input sink not available");
    }
    return sink->SendKnobEvent(action, stepCount);
}

Core::Common::Status ApplicationService::ForwardButtonEvent(
    Core::Input::ButtonType button,
    bool isPressed) {
    auto sink = core_api_->GetInputSink();
    if (!sink) {
        return Core::Common::MakeError(Core::Common::StatusCode::InvalidState,
            "Input sink not available");
    }
    return sink->SendButtonEvent(button, isPressed);
}

Core::Common::Status ApplicationService::SetHostNightMode(bool night_mode) {
    // Pure pass-through: Core owns the event channel and the remembered value.
    return core_api_->SetHostNightMode(night_mode);
}

// ============================================================================
// IApplicationApi — State Query
// ============================================================================

AppUiState ApplicationService::GetCurrentState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ui_state_;
}

// ============================================================================
// Private — Core Event Handling
// ============================================================================

void ApplicationService::OnCoreEvent(const Core::Service::CoreEvent& event) {
    // Copy the UI state under the lock, then notify outside the lock.
    AppUiState state_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (std::holds_alternative<Core::Service::ConnectionStateChangedEvent>(event)) {
            auto& conn = std::get<Core::Service::ConnectionStateChangedEvent>(event);
            ui_state_.session_state = conn.new_state;
            ui_state_.is_streaming = (conn.new_state == Core::Session::SessionState::AirPlayStreaming);
            ui_state_.status_message = std::string(Core::Session::SessionStateToString(conn.new_state));
        }
        else if (std::holds_alternative<Core::Service::NowPlayingEvent>(event)) {
            auto& np = std::get<Core::Service::NowPlayingEvent>(event);
            ui_state_.status_message = np.track_title + " — " + np.artist;
        }
        else if (std::holds_alternative<Core::Service::AudioRouteChangedEvent>(event)) {
            auto& route = std::get<Core::Service::AudioRouteChangedEvent>(event);
            ui_state_.status_message = "Audio: " + route.route_description;
        }
        else if (std::holds_alternative<Core::Service::DeviceConnectedEvent>(event)) {
            auto& dev = std::get<Core::Service::DeviceConnectedEvent>(event);
            ui_state_.connected_device_bt_mac = dev.bluetooth_mac;
            ui_state_.connected_device_wifi_mac = dev.wifi_mac;
            if (!dev.device_name.empty()) {
                ui_state_.connected_device_name = dev.device_name;
            }
            ui_state_.status_message = "Device connected";
        }

        state_snapshot = ui_state_;
    }

    // Refresh the video facts outside the lock (this calls into Core).
    SnapshotVideoFacts(state_snapshot);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ui_state_.current_video_width = state_snapshot.current_video_width;
        ui_state_.current_video_height = state_snapshot.current_video_height;
        ui_state_.current_video_fps = state_snapshot.current_video_fps;
        ui_state_.current_video_codec = state_snapshot.current_video_codec;
    }

    // Notify UI callbacks outside the lock.
    std::vector<UiStateChangeCallback> cbs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cbs = ui_state_callbacks_;
    }
    for (auto& cb : cbs) {
        cb(state_snapshot);
    }
}

void ApplicationService::NotifyUiState(const AppUiState& state) {
    std::vector<UiStateChangeCallback> cbs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cbs = ui_state_callbacks_;
    }
    for (auto& cb : cbs) {
        cb(state);
    }
}

void ApplicationService::ResolveSettingsToCore(const AppSettings& settings) {
    // This helper is reserved for future use when the Application layer
    // needs to resolve settings into Core parameters beyond what
    // StartCarPlay / ApplySettings already do.
    (void)settings;
}

void ApplicationService::SnapshotVideoFacts(AppUiState& state) const {
    if (!core_api_) return;
    // Core owns the truth: the geometry it advertises to the phone plus the codec
    // it actually sees on the wire. The UI must report that, not the settings.
    auto spec = core_api_->GetActiveVideoStreamSpec();
    state.current_video_width = spec.width;
    state.current_video_height = spec.height;
    state.current_video_fps = spec.fps;
    state.current_video_codec = std::string(Core::Common::VideoCodecToString(spec.codec));
    if (auto decoder = core_api_->GetVideoDecoder()) {
        state.is_hardware_decode = decoder->IsHardwareAccelerated();
    }
}

Core::Common::VideoStreamSpec ApplicationService::ResolveVideoSpec(const AppSettings& settings) {
    Core::Common::VideoStreamSpec spec;
    auto [width, height] = settings.video.GetEffectiveDimensions();
    spec.width = width;
    spec.height = height;
    spec.fps = settings.video.fps;
    spec.hardware_decode = settings.video.hardware_decode;
    // Codec preference is advisory; the session negotiates the final codec and the
    // decoder reconfigures itself from the VideoConfig packet (avcC/hvcC).
    switch (settings.video.preferred_codec) {
        case VideoCodecPreference::H264:  spec.codec = Core::Common::VideoCodec::H264; break;
        case VideoCodecPreference::HEVC:  spec.codec = Core::Common::VideoCodec::HEVC; break;
        case VideoCodecPreference::Auto:  spec.codec = Core::Common::VideoCodec::H264; break;
    }
    return spec;
}

void ApplicationService::ApplyMfiSigner(const AppSettings& settings) {
    auto signer = MfiSignerFactory::Build(settings.mfi);
    if (!signer) {
        Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Warn, "AppService",
            "MFi signer could not be built for backend: ",
            MfiSignerFactory::Describe(settings.mfi));
        return;
    }
    auto status = core_api_->SetMfiSigner(signer);
    if (status.IsError()) {
        Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Warn, "AppService",
            "SetMfiSigner failed: ", status.Message());
        return;
    }
    Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Info, "AppService",
        "MFi signer active: ", MfiSignerFactory::Describe(settings.mfi));

    // A hardware backend is only reachable through a phone's 0xAA00 request, so
    // a dead bus looks identical to an unused feature. Probe it once here.
    if (settings.mfi.backend_type == MFiBackendType::HardwareI2C) {
        ProbeMfiHardwareAsync(signer);
    }
}

void ApplicationService::ProbeMfiHardwareAsync(
        const std::shared_ptr<Core::Hardware::IMfiSigner>& signer) {
    // Detached on purpose: the warm-up and bus sweep can take a couple of
    // seconds and must never block the UI thread or session startup. The signer
    // is held by the Core service for the lifetime of the process, and the
    // logger is a singleton, so the capture stays valid.
    std::thread([signer]() {
        auto info = signer->GetChipInfo();
        if (!info.present) {
            Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Warn, "AppService",
                "MFi self-probe: no coprocessor answered. Check power, SDA/SCL, "
                "pull-ups and the slave address before blaming the protocol stack.");
            return;
        }
        char id[16];
        std::snprintf(id, sizeof(id), "0x%08X", info.chip_id);
        Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Info, "AppService",
            "MFi self-probe: coprocessor present, chip_id=", id,
            " firmware=", info.firmware_version);
        auto cert = signer->FetchCertificate();
        if (cert.IsError()) {
            Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Warn, "AppService",
                "MFi self-probe: chip answered but certificate read failed: ",
                cert.Error().Message());
            return;
        }
        Core::Common::Logger::Instance().Log(Core::Common::LogLevel::Info, "AppService",
            "MFi self-probe: certificate read, ", cert.Value().size(), " bytes.");
    }).detach();
}

} // namespace CarPlayStation::Application
