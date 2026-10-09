#pragma once

#include "Core/Common/Result.h"
#include "AppSettings.h"
#include "Core/Session/ICarPlaySession.h"
#include "Core/Media/IVideoDecoder.h"
#include "Core/Input/IInputEventSink.h"
#include <functional>
#include <memory>
#include <string>

namespace CarPlayStation::Application {

struct NowPlayingUi {
    std::string title;
    std::string artist;
    std::string album;
    bool is_playing{false};
    uint32_t position_seconds{0};
};

struct CallStateUi {
    std::string remote_id;     // Phone number
    std::string display_name;  // Contact name or number
    std::string call_uuid;
    uint8_t status{0};         // 0=Disconnected, 1=Dialing, 2=Ringing, 3=Active, etc.
    uint8_t direction{0};      // 1=Incoming, 2=Outgoing
    bool has_call{false};      // Call session in progress (ringing or active)
    bool is_incoming{false};   // Specifically incoming ring
};

struct AppUiState {
    Core::Session::SessionState session_state{Core::Session::SessionState::Idle};
    std::string connected_device_name;
    bool is_streaming{false};
    uint32_t current_video_width{0};   // Runtime fact: spec actually in effect
    uint32_t current_video_height{0};  // Runtime fact: spec actually in effect
    uint32_t current_video_fps{0};     // Runtime fact: frame rate advertised to the phone
    std::string current_video_codec;   // Runtime fact: codec on the wire ("H264" / "HEVC" / ...)
    bool is_hardware_decode{false};    // Runtime fact: whether decoder is running hardware acceleration
    std::string status_message;
    // Recorded phone addresses (runtime facts) so the UI can persist them.
    std::string connected_device_bt_mac;   // iPhone Bluetooth MAC
    std::string connected_device_wifi_mac; // iPhone Wi-Fi MAC (SoftAP station)
    AppSettings active_settings;          // Snapshot of last-applied user settings

    // Now Playing & Telephony states
    NowPlayingUi now_playing;
    CallStateUi call_state;
};

using UiStateChangeCallback = std::function<void(const AppUiState& state)>;

class IApplicationApi {
public:
    virtual ~IApplicationApi() = default;

    // Start CarPlay engine with target user preferences (resolved to Core parameters internally)
    [[nodiscard]] virtual Core::Common::Status StartCarPlay(const AppSettings& settings) = 0;

    // Dynamically adjust user preferences during runtime (e.g. 720p/1080p/4k, Fullscreen/Windowed, volumes, endpoints)
    [[nodiscard]] virtual Core::Common::Status ApplySettings(const AppSettings& newSettings) = 0;

    // Stop CarPlay session
    virtual void StopCarPlay() = 0;

    // Subscribe to UI-level state updates
    virtual void RegisterUiStateCallback(UiStateChangeCallback callback) = 0;

    // Register Direct3D 11 video surface frame consumer
    virtual void RegisterFrameConsumer(Core::Media::VideoFrameCallback callback) = 0;

    // Register a CPU frame-buffer consumer (used by CPU-based decoders such as
    // MpvVideoPlayer, which hand decoded BGRA pixels to the renderer instead of
    // a D3D11 shared texture).
    virtual void RegisterCpuFrameConsumer(Core::Media::CpuFrameCallback callback) = 0;

    // Inject the UI's D3D11 device/context into the video decoder so the decoder's
    // shared texture is created on the SAME device the renderer samples from.
    // Cross-device shared textures in the same process are not coherent, so the
    // reader would otherwise never see the writer's updates (frozen first frame).
    // Must be called before StartCarPlay().
    virtual void SetVideoSharedDevice(void* device, void* context) = 0;

    // Forward UI input to input subsystem
    [[nodiscard]] virtual Core::Common::Status ForwardTouchEvent(Core::Input::TouchAction action, const Core::Input::TouchPoint& point) = 0;
    [[nodiscard]] virtual Core::Common::Status ForwardKnobEvent(Core::Input::KnobAction action, uint32_t stepCount = 1) = 0;
    [[nodiscard]] virtual Core::Common::Status ForwardButtonEvent(Core::Input::ButtonType button, bool isPressed) = 0;

    // Telephony & Voice Assistant controls
    [[nodiscard]] virtual Core::Common::Status AcceptCall(std::string_view callUuid = {}) = 0;
    [[nodiscard]] virtual Core::Common::Status EndCall(std::string_view callUuid = {}) = 0;
    [[nodiscard]] virtual Core::Common::Status TriggerSiri(bool isPressed) = 0;

    // Tell the phone which appearance the host window is showing (CarPlay night mode).
    // night_mode = true means dark. The value is remembered by Core and re-sent to the
    // phone whenever an event channel comes up, so calling it with no session running is
    // fine. Returns Unavailable when no phone currently has an event channel.
    [[nodiscard]] virtual Core::Common::Status SetHostNightMode(bool night_mode) = 0;

    // Query current application state snapshot
    [[nodiscard]] virtual AppUiState GetCurrentState() const = 0;
};

} // namespace CarPlayStation::Application
