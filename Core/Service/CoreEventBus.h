#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Session/ICarPlaySession.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace CarPlayStation::Core::Service {

// ============================================================================
// Event DTOs emitted by the Core Event Bus to the Application layer.
// All events are value types (POD / std::string / std::vector) — no platform
// or third-party header leaks (DEC-002).
// ============================================================================

struct ConnectionStateChangedEvent {
    Session::SessionState old_state{Session::SessionState::Idle};
    Session::SessionState new_state{Session::SessionState::Idle};
    Common::Status status{Common::Status::Ok()};
};

struct NowPlayingEvent {
    std::string track_title;
    std::string artist;
    std::string album;
    bool is_playing{false};
    uint32_t position_seconds{0};
};

struct AlbumArtworkEvent {
    Common::ByteBuffer data;  // JPEG / PNG bytes
    std::string mime_type{"image/jpeg"};
};

struct AudioRouteChangedEvent {
    std::string route_description;  // e.g. "WASAPI Shared Mode / 48kHz Stereo"
};

// Emitted when a phone (iPhone) joins the Wi-Fi SoftAP, carrying the recorded
// device addresses so the Application layer can persist them (e.g. to the INI).
struct DeviceConnectedEvent {
    std::string bluetooth_mac;  // iPhone Bluetooth MAC (from the pairing record)
    std::string wifi_mac;       // iPhone Wi-Fi MAC (station that joined the SoftAP)
    std::string device_name;    // Human-readable device name, if known
};

// Emitted when call state changes (0x4155 CallStateUpdate).
struct CallStateEvent {
    std::string remote_id;       // phone number
    std::string display_name;    // contact name
    uint8_t status{0};           // raw iAP2 status
    uint8_t direction{0};        // 1=Incoming, 2=Outgoing
    std::string call_uuid;
    bool is_active{false};
    bool is_incoming{false};
};

// Variant type for all Core events.
using CoreEvent = std::variant<
    ConnectionStateChangedEvent,
    NowPlayingEvent,
    AlbumArtworkEvent,
    AudioRouteChangedEvent,
    DeviceConnectedEvent,
    CallStateEvent
>;

using CoreEventCallback = std::function<void(const CoreEvent&)>;

// ============================================================================
// CoreEventBus — thread-safe publish-subscribe event mechanism.
//
// Emits events to the Application layer: connection state changes,
// NowPlaying metadata, album artwork, audio route changes.
//
// Thread-safety:
//   - Subscribe / Unsubscribe are thread-safe (mutex-guarded).
//   - Publish copies the listener list under the lock, then invokes callbacks
//     outside the lock to avoid deadlock if a callback re-enters the bus.
// ============================================================================
class CoreEventBus {
public:
    CoreEventBus() = default;
    ~CoreEventBus() = default;

    CoreEventBus(const CoreEventBus&) = delete;
    CoreEventBus& operator=(const CoreEventBus&) = delete;

    // Subscribe to all events. Returns a token for later unsubscription.
    [[nodiscard]] uint64_t Subscribe(CoreEventCallback callback);

    // Unsubscribe a previously registered listener.
    void Unsubscribe(uint64_t token);

    // Publish an event to all registered listeners.
    // Callbacks are invoked outside the internal lock (no deadlock).
    void Publish(const CoreEvent& event);

    // Convenience: publish a connection state change.
    void PublishConnectionStateChanged(
        Session::SessionState old_state,
        Session::SessionState new_state,
        Common::Status status = Common::Status::Ok());

    // Convenience: publish a NowPlaying update.
    void PublishNowPlaying(
        std::string track_title,
        std::string artist,
        std::string album,
        bool is_playing,
        uint32_t position_seconds = 0);

    // Convenience: publish album artwork.
    void PublishAlbumArtwork(Common::ByteBuffer data, std::string mime_type = "image/jpeg");

    // Convenience: publish audio route change.
    void PublishAudioRouteChanged(std::string route_description);

    // Convenience: publish a device-connected event (recorded phone addresses).
    void PublishDeviceConnected(std::string bluetooth_mac, std::string wifi_mac,
                                std::string device_name = {});

    // Convenience: publish a call state event.
    void PublishCallState(std::string remote_id, std::string display_name,
                         uint8_t status, uint8_t direction, std::string call_uuid,
                         bool is_active, bool is_incoming);

    // Number of currently registered listeners.
    [[nodiscard]] size_t ListenerCount() const noexcept;

private:
    struct Listener {
        uint64_t token;
        CoreEventCallback callback;
    };

    mutable std::mutex mutex_;
    std::vector<Listener> listeners_;
    uint64_t next_token_{1};
};

} // namespace CarPlayStation::Core::Service
