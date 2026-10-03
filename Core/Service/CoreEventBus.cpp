#include "Core/Service/CoreEventBus.h"

namespace CarPlayStation::Core::Service {

uint64_t CoreEventBus::Subscribe(CoreEventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t token = next_token_++;
    listeners_.push_back(Listener{token, std::move(callback)});
    return token;
}

void CoreEventBus::Unsubscribe(uint64_t token) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
                       [token](const Listener& l) { return l.token == token; }),
        listeners_.end());
}

void CoreEventBus::Publish(const CoreEvent& event) {
    // Copy the listener list under the lock, then invoke callbacks outside
    // the lock so that a callback re-entering the bus (e.g. calling
    // Subscribe/Unsubscribe) does not deadlock.
    std::vector<Listener> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = listeners_;
    }
    for (const auto& listener : snapshot) {
        if (listener.callback) {
            listener.callback(event);
        }
    }
}

void CoreEventBus::PublishConnectionStateChanged(
    Session::SessionState old_state,
    Session::SessionState new_state,
    Common::Status status) {
    Publish(ConnectionStateChangedEvent{old_state, new_state, std::move(status)});
}

void CoreEventBus::PublishNowPlaying(
    std::string track_title,
    std::string artist,
    std::string album,
    bool is_playing,
    uint32_t position_seconds) {
    Publish(NowPlayingEvent{
        std::move(track_title),
        std::move(artist),
        std::move(album),
        is_playing,
        position_seconds
    });
}

void CoreEventBus::PublishAlbumArtwork(Common::ByteBuffer data, std::string mime_type) {
    Publish(AlbumArtworkEvent{std::move(data), std::move(mime_type)});
}

void CoreEventBus::PublishVehicleNightMode(bool is_night_mode) {
    Publish(VehicleNightModeEvent{is_night_mode});
}

void CoreEventBus::PublishAudioRouteChanged(std::string route_description) {
    Publish(AudioRouteChangedEvent{std::move(route_description)});
}

void CoreEventBus::PublishDeviceConnected(std::string bluetooth_mac, std::string wifi_mac,
                                          std::string device_name) {
    Publish(DeviceConnectedEvent{std::move(bluetooth_mac), std::move(wifi_mac), std::move(device_name)});
}

size_t CoreEventBus::ListenerCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return listeners_.size();
}

} // namespace CarPlayStation::Core::Service
