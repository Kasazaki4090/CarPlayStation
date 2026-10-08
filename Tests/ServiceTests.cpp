#include "Core/Service/CoreEventBus.h"
#include "Core/Service/CarPlayCoreService.h"
#include "Core/Service/ICarPlayCoreApi.h"
#include "Application/ApplicationService.h"
#include "Application/AppSettings.h"
#include "Application/IApplicationApi.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include "Core/Session/ICarPlaySession.h"
#include "Core/Media/IVideoDecoder.h"
#include "Core/Media/IAudioPipeline.h"
#include "Core/Media/IMicrophoneUplink.h"
#include "Core/Input/IInputEventSink.h"
#include "Core/Hardware/IMfiSigner.h"
#include "Core/Transport/ITransportChannel.h"
#include "Core/Protocol/IIap2Link.h"
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

using namespace CarPlayStation;

// ============================================================================
// Mock implementations for testing
// ============================================================================

namespace {

// Minimal mock session for testing
class MockSession : public Core::Session::ICarPlaySession {
public:
    Core::Common::Status Start() override {
        state_ = Core::Session::SessionState::BtDiscovered;
        NotifyStateChange(Core::Session::SessionState::Idle, state_);
        return Core::Common::Status::Ok();
    }
    void Stop() override {
        state_ = Core::Session::SessionState::Idle;
        NotifyStateChange(Core::Session::SessionState::BtDiscovered, state_);
    }
    [[nodiscard]] Core::Session::SessionState GetState() const noexcept { return state_; }
    void RegisterStateObserver(Core::Session::SessionStateObserver observer) override {
        observers_.push_back(std::move(observer));
    }

private:
    void NotifyStateChange(Core::Session::SessionState old_s, Core::Session::SessionState new_s) {
        for (auto& obs : observers_) {
            obs(old_s, new_s, Core::Common::Status::Ok());
        }
    }
    Core::Session::SessionState state_{Core::Session::SessionState::Idle};
    std::vector<Core::Session::SessionStateObserver> observers_;
};

// Minimal mock video decoder
class MockVideoDecoder : public Core::Media::IVideoDecoder {
public:
    Core::Common::Status Init(const Core::Common::VideoStreamSpec& spec) override {
        spec_ = spec;
        return Core::Common::Status::Ok();
    }
    Core::Common::Status Reconfigure(const Core::Common::VideoStreamSpec& spec) override {
        spec_ = spec;
        reconfigure_count_++;
        return Core::Common::Status::Ok();
    }
    Core::Common::Status DecodeAccessUnit(Core::Common::ByteSpan, uint64_t) override {
        return Core::Common::Status::Ok();
    }
    void SetFrameCallback(Core::Media::VideoFrameCallback callback) override {
        frame_callback_ = std::move(callback);
    }
    [[nodiscard]] Core::Media::VideoCapabilities QueryCapabilities() const override {
        return {};
    }
    [[nodiscard]] uint64_t GetDecodedFrameCount() const noexcept { return 0; }
    void Flush() override {}
    void Shutdown() override {}

    [[nodiscard]] Core::Common::VideoStreamSpec GetSpec() const { return spec_; }
    [[nodiscard]] uint32_t GetReconfigureCount() const { return reconfigure_count_; }
    [[nodiscard]] bool HasFrameCallback() const { return static_cast<bool>(frame_callback_); }

private:
    Core::Common::VideoStreamSpec spec_{};
    uint32_t reconfigure_count_{0};
    Core::Media::VideoFrameCallback frame_callback_;
};

// Minimal mock audio pipeline
class MockAudioPipeline : public Core::Media::IAudioPipeline {
public:
    Core::Common::Status Start(const Core::Common::AudioOutputFormat& fmt) override {
        format_ = fmt;
        return Core::Common::Status::Ok();
    }
    Core::Common::Status Reconfigure(const Core::Common::AudioOutputFormat& fmt) override {
        format_ = fmt;
        reconfigure_count_++;
        return Core::Common::Status::Ok();
    }
    Core::Common::Status WriteAudio(Core::Media::AudioLane, Core::Common::ByteSpan) override {
        return Core::Common::Status::Ok();
    }
    void SetLaneVolume(Core::Media::AudioLane lane, float volume) override {
        lane_volumes_[lane] = volume;
    }
    void SetDuckingRatio(float ratio) override { ducking_ratio_ = ratio; }
    void SetTargetDuckingRatio(float ratio) override { ducking_ratio_ = ratio; }
    void SetDuckingActive(bool active) override { ducking_active_ = active; }
    [[nodiscard]] Core::Media::AudioCapabilities QueryCapabilities() const override { return {}; }
    [[nodiscard]] uint64_t GetRenderedSampleCount() const noexcept { return 0; }
    void StopLane(Core::Media::AudioLane) override {}
    void Shutdown() override {}

    [[nodiscard]] Core::Common::AudioOutputFormat GetFormat() const { return format_; }
    [[nodiscard]] uint32_t GetReconfigureCount() const { return reconfigure_count_; }
    [[nodiscard]] float GetLaneVolume(Core::Media::AudioLane lane) const {
        auto it = lane_volumes_.find(lane);
        return it != lane_volumes_.end() ? it->second : 1.0f;
    }
    [[nodiscard]] float GetDuckingRatio() const { return ducking_ratio_; }

private:
    Core::Common::AudioOutputFormat format_{};
    uint32_t reconfigure_count_{0};
    std::map<Core::Media::AudioLane, float> lane_volumes_;
    float ducking_ratio_{0.25f};
    bool ducking_active_{false};
};

// Minimal mock input sink
class MockInputSink : public Core::Input::IInputEventSink {
public:
    Core::Common::Status SendTouchEvent(Core::Input::TouchAction, const Core::Input::TouchPoint&) override {
        touch_count_++;
        return Core::Common::Status::Ok();
    }
    Core::Common::Status SendKnobEvent(Core::Input::KnobAction, uint32_t) override {
        knob_count_++;
        return Core::Common::Status::Ok();
    }
    Core::Common::Status SendButtonEvent(Core::Input::ButtonType, bool) override {
        button_count_++;
        return Core::Common::Status::Ok();
    }

    [[nodiscard]] uint32_t GetTouchCount() const { return touch_count_; }
    [[nodiscard]] uint32_t GetKnobCount() const { return knob_count_; }
    [[nodiscard]] uint32_t GetButtonCount() const { return button_count_; }

private:
    uint32_t touch_count_{0};
    uint32_t knob_count_{0};
    uint32_t button_count_{0};
};

// Minimal mock MFi signer
class MockMfiSigner : public Core::Hardware::IMfiSigner {
public:
    Core::Common::Result<Core::Common::ByteBuffer> FetchCertificate() override {
        return Core::Common::ByteBuffer{};
    }
    Core::Common::Result<Core::Common::ByteBuffer> SignChallenge(Core::Common::ByteSpan) override {
        return Core::Common::ByteBuffer{};
    }
    [[nodiscard]] Core::Hardware::MFiChipInfo GetChipInfo() const noexcept override {
        return {};
    }
    void Shutdown() override {}
};

// Minimal mock transport
class MockTransport : public Core::Transport::ITransportChannel {
public:
    Core::Common::Status Send(Core::Common::ByteSpan) override { return Core::Common::Status::Ok(); }
    Core::Common::Status AsyncSend(Core::Common::ByteBuffer) override { return Core::Common::Status::Ok(); }
    void SetDataCallback(Core::Transport::DataReceivedCallback) override {}
    void SetStateCallback(Core::Transport::StateChangedCallback) override {}
    void Disconnect() override {}
    [[nodiscard]] Core::Transport::TransportState GetState() const noexcept { return Core::Transport::TransportState::Disconnected; }
    [[nodiscard]] bool IsConnected() const noexcept { return false; }
    [[nodiscard]] Core::Transport::TransportType GetType() const noexcept { return Core::Transport::TransportType::MockLoopback; }
    [[nodiscard]] size_t GetMtu() const noexcept { return 1024; }
    [[nodiscard]] std::string GetRemoteEndpoint() const { return "mock"; }
    [[nodiscard]] Core::Transport::TransportStats GetStats() const { return {}; }
};

// Minimal mock iAP2 link
class MockIap2Link : public Core::Protocol::IIap2Link {
public:
    Core::Common::Status FeedData(Core::Common::ByteSpan) override { return Core::Common::Status::Ok(); }
    Core::Common::Status SendMessage(uint8_t, Core::Common::ByteSpan) override { return Core::Common::Status::Ok(); }
    void SetPacketHandler(Core::Protocol::IIap2PacketHandler*) override {}
    void Reset() override {}
};

// Minimal mock microphone uplink
class MockMicrophoneUplink : public Core::Media::IMicrophoneUplink {
public:
    Core::Common::Status Start(const Core::Common::AudioOutputFormat&) override { return Core::Common::Status::Ok(); }
    void SetUplinkCallback(Core::Media::UplinkAudioCallback) override {}
    Core::Common::Status Stop() override { return Core::Common::Status::Ok(); }
    [[nodiscard]] bool IsCapturing() const noexcept { return false; }
    [[nodiscard]] Core::Media::MicrophoneCapabilities QueryCapabilities() const override { return {}; }
    void SetInputGain(float) override {}
    [[nodiscard]] uint64_t GetCapturedSampleCount() const noexcept { return 0; }
    void Shutdown() override {}
};

// Helper to build a fully-mocked CarPlayCoreService
std::shared_ptr<Core::Service::CarPlayCoreService> MakeMockCoreService(
    std::shared_ptr<MockSession>& session,
    std::shared_ptr<MockVideoDecoder>& decoder,
    std::shared_ptr<MockAudioPipeline>& audio,
    std::shared_ptr<MockInputSink>& input) {
    auto mfi = std::make_shared<MockMfiSigner>();
    auto transport = std::make_shared<MockTransport>();
    auto iap2 = std::make_shared<MockIap2Link>();
    auto mic = std::make_shared<MockMicrophoneUplink>();

    Core::Service::CarPlayCoreDependencies deps;
    deps.mfi_signer = mfi;
    deps.transport = transport;
    deps.iap2_link = iap2;
    deps.session = session;
    deps.video_decoder = decoder;
    deps.audio_pipeline = audio;
    deps.microphone_uplink = mic;
    deps.input_sink = input;

    return std::make_shared<Core::Service::CarPlayCoreService>(std::move(deps));
}

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

void TestCoreEventBus() {
    Core::Service::CoreEventBus bus;

    int event_count = 0;
    Core::Service::ConnectionStateChangedEvent last_conn_event;
    bool received_conn_event = false;

    auto token = bus.Subscribe([&](const Core::Service::CoreEvent& event) {
        event_count++;
        if (auto* conn = std::get_if<Core::Service::ConnectionStateChangedEvent>(&event)) {
            last_conn_event = *conn;
            received_conn_event = true;
        }
    });

    assert(token != 0);
    assert(bus.ListenerCount() == 1);

    // Publish a connection state change
    bus.PublishConnectionStateChanged(
        Core::Session::SessionState::Idle,
        Core::Session::SessionState::BtDiscovered);

    assert(event_count == 1);
    assert(received_conn_event);
    assert(last_conn_event.old_state == Core::Session::SessionState::Idle);
    assert(last_conn_event.new_state == Core::Session::SessionState::BtDiscovered);

    // Unsubscribe
    bus.Unsubscribe(token);
    assert(bus.ListenerCount() == 0);

    // Publish after unsubscribe — no callback
    bus.PublishConnectionStateChanged(
        Core::Session::SessionState::BtDiscovered,
        Core::Session::SessionState::iAP2Authenticated);
    assert(event_count == 1); // unchanged

    std::cout << "[PASS] TestCoreEventBus\n";
}

void TestCoreEventBusMultipleListeners() {
    Core::Service::CoreEventBus bus;

    int count1 = 0, count2 = 0;
    auto t1 = bus.Subscribe([&count1](const Core::Service::CoreEvent&) { count1++; });
    auto t2 = bus.Subscribe([&count2](const Core::Service::CoreEvent&) { count2++; });

    assert(bus.ListenerCount() == 2);

    bus.PublishNowPlaying("Song", "Artist", "Album", true, 42);
    assert(count1 == 1);
    assert(count2 == 1);

    bus.Unsubscribe(t1);
    assert(bus.ListenerCount() == 1);

    bus.PublishNowPlaying("Song2", "Artist2", "Album2", false, 0);
    assert(count1 == 1);  // unsubscribed
    assert(count2 == 2);  // still subscribed

    bus.Unsubscribe(t2);
    assert(bus.ListenerCount() == 0);

    std::cout << "[PASS] TestCoreEventBusMultipleListeners\n";
}

void TestCarPlayCoreServiceWithMocks() {
    auto mock_session = std::make_shared<MockSession>();
    auto mock_decoder = std::make_shared<MockVideoDecoder>();
    auto mock_audio = std::make_shared<MockAudioPipeline>();
    auto mock_input = std::make_shared<MockInputSink>();

    auto service = MakeMockCoreService(mock_session, mock_decoder, mock_audio, mock_input);

    // Initialize
    Core::Common::CoreEnvironmentConfig env;
    env.identity.device_name = "TestDevice";
    auto status = service->Initialize(env);
    assert(status.IsOk());
    assert(service->IsInitialized());

    // Accessors return the injected mocks
    assert(service->GetSession() == mock_session);
    assert(service->GetVideoDecoder() == mock_decoder);
    assert(service->GetAudioPipeline() == mock_audio);
    assert(service->GetInputSink() == mock_input);

    // Start
    status = service->Start();
    assert(status.IsOk());

    // Stop
    service->Stop();

    std::cout << "[PASS] TestCarPlayCoreServiceWithMocks\n";
}

void TestApplicationServiceWithMocks() {
    auto mock_session = std::make_shared<MockSession>();
    auto mock_decoder = std::make_shared<MockVideoDecoder>();
    auto mock_audio = std::make_shared<MockAudioPipeline>();
    auto mock_input = std::make_shared<MockInputSink>();

    auto core_service = MakeMockCoreService(mock_session, mock_decoder, mock_audio, mock_input);

    // Build the application service
    Application::ApplicationService app_service(core_service);

    // Start CarPlay
    Application::AppSettings settings;
    settings.device.device_name = "TestCar";
    settings.video.resolution_preset = Application::VideoResolutionPreset::Res1080P;
    settings.audio.media_volume = 0.8f;

    auto status = app_service.StartCarPlay(settings);
    assert(status.IsOk());

    // Verify state
    auto state = app_service.GetCurrentState();
    assert(state.is_streaming == false); // just started, not streaming yet
    assert(state.status_message == "CarPlay started");

    // Apply settings (hot update)
    Application::AppSettings new_settings = settings;
    new_settings.audio.media_volume = 0.5f;
    new_settings.audio.navigation_volume = 0.4f;
    new_settings.audio.telephony_volume = 0.6f;
    new_settings.audio.ducking_ratio = 0.35f;
    new_settings.video.resolution_preset = Application::VideoResolutionPreset::Res720P;

    status = app_service.ApplySettings(new_settings);
    assert(status.IsOk());

    // Verify the mock decoder was reconfigured
    assert(mock_decoder->GetReconfigureCount() >= 1);
    assert(mock_decoder->GetSpec().width == 1280);
    assert(mock_decoder->GetSpec().height == 720);

    // Only lane volumes and ducking ratio may be touched. The output format is the Windows
    // mix format and is never reconfigured from the settings.
    assert(mock_audio->GetReconfigureCount() == 0);
    assert(mock_audio->GetLaneVolume(Core::Media::AudioLane::MainMedia) == 0.5f);
    assert(mock_audio->GetLaneVolume(Core::Media::AudioLane::AltNavigation) == 0.4f);
    assert(mock_audio->GetLaneVolume(Core::Media::AudioLane::SiriTelephony) == 0.6f);
    assert(mock_audio->GetDuckingRatio() == 0.35f);

    // Forward input events
    Core::Input::TouchPoint point{1, 0.5f, 0.5f};
    status = app_service.ForwardTouchEvent(Core::Input::TouchAction::Down, point);
    assert(status.IsOk());
    assert(mock_input->GetTouchCount() == 1);

    status = app_service.ForwardKnobEvent(Core::Input::KnobAction::RotateClockwise, 5);
    assert(status.IsOk());
    assert(mock_input->GetKnobCount() == 1);

    status = app_service.ForwardButtonEvent(Core::Input::ButtonType::Home, true);
    assert(status.IsOk());
    assert(mock_input->GetButtonCount() == 1);

    // Register frame consumer
    bool frame_received = false;
    app_service.RegisterFrameConsumer([&frame_received](uintptr_t, uint32_t, uint32_t, uint64_t) {
        frame_received = true;
    });
    assert(mock_decoder->HasFrameCallback());

    // Stop
    app_service.StopCarPlay();
    state = app_service.GetCurrentState();
    assert(state.status_message == "CarPlay stopped");

    std::cout << "[PASS] TestApplicationServiceWithMocks\n";
}

void TestApplicationServiceUiStateCallback() {
    auto mock_session = std::make_shared<MockSession>();
    auto mock_decoder = std::make_shared<MockVideoDecoder>();
    auto mock_audio = std::make_shared<MockAudioPipeline>();
    auto mock_input = std::make_shared<MockInputSink>();

    auto core_service = MakeMockCoreService(mock_session, mock_decoder, mock_audio, mock_input);
    Application::ApplicationService app_service(core_service);

    // Register a UI state callback
    Application::AppUiState last_state;
    int callback_count = 0;
    app_service.RegisterUiStateCallback([&last_state, &callback_count](const Application::AppUiState& s) {
        last_state = s;
        callback_count++;
    });

    Application::AppSettings settings;
    auto status = app_service.StartCarPlay(settings);
    assert(status.IsOk());
    assert(callback_count >= 1);
    assert(last_state.status_message == "CarPlay started");

    app_service.StopCarPlay();
    assert(callback_count >= 2);
    assert(last_state.status_message == "CarPlay stopped");

    std::cout << "[PASS] TestApplicationServiceUiStateCallback\n";
}

void TestApplicationServiceDoubleStart() {
    auto mock_session = std::make_shared<MockSession>();
    auto mock_decoder = std::make_shared<MockVideoDecoder>();
    auto mock_audio = std::make_shared<MockAudioPipeline>();
    auto mock_input = std::make_shared<MockInputSink>();

    auto core_service = MakeMockCoreService(mock_session, mock_decoder, mock_audio, mock_input);
    Application::ApplicationService app_service(core_service);

    Application::AppSettings settings;
    auto status = app_service.StartCarPlay(settings);
    assert(status.IsOk());

    // Double start should fail
    status = app_service.StartCarPlay(settings);
    assert(status.IsError());
    assert(status.Code() == Core::Common::StatusCode::InvalidState);

    app_service.StopCarPlay();

    std::cout << "[PASS] TestApplicationServiceDoubleStart\n";
}

void TestApplicationServiceApplySettingsNotRunning() {
    auto mock_session = std::make_shared<MockSession>();
    auto mock_decoder = std::make_shared<MockVideoDecoder>();
    auto mock_audio = std::make_shared<MockAudioPipeline>();
    auto mock_input = std::make_shared<MockInputSink>();

    auto core_service = MakeMockCoreService(mock_session, mock_decoder, mock_audio, mock_input);
    Application::ApplicationService app_service(core_service);

    // ApplySettings without StartCarPlay should fail
    Application::AppSettings settings;
    auto status = app_service.ApplySettings(settings);
    assert(status.IsError());
    assert(status.Code() == Core::Common::StatusCode::InvalidState);

    std::cout << "[PASS] TestApplicationServiceApplySettingsNotRunning\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== CarPlayStation Service Tests ===\n";

    TestCoreEventBus();
    TestCoreEventBusMultipleListeners();
    TestCarPlayCoreServiceWithMocks();
    TestApplicationServiceWithMocks();
    TestApplicationServiceUiStateCallback();
    TestApplicationServiceDoubleStart();
    TestApplicationServiceApplySettingsNotRunning();

    std::cout << "=== All Service Tests Passed ===\n";
    return 0;
}
