#include "Core/Session/CarPlaySessionStateMachine.h"
#include "Core/Common/Logging.h"
#include <iostream>

namespace CarPlayStation::Core::Session {

CarPlaySessionStateMachine::CarPlaySessionStateMachine(SessionConfig config, SessionDependencies deps)
    : config_(std::move(config)), deps_(std::move(deps)) {
    running_ = true;
    if (deps_.rtsp_server) {
        deps_.rtsp_server->SetStreamingStartedCallback([this](const NegotiatedMediaParams& params) {
            (void)OnAirPlayStreamingStarted(params);
        });
    }
    if (config_.enable_background_watchdog) {
        watchdog_thread_ = std::thread(&CarPlaySessionStateMachine::WatchdogLoop, this);
    }
}

CarPlaySessionStateMachine::~CarPlaySessionStateMachine() {
    running_ = false;
    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }
    Stop();
}

Common::Status CarPlaySessionStateMachine::Start() {
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SessionState::Idle && state_ != SessionState::Error) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Session already active");
        }

        old_state = state_;
        state_ = SessionState::Idle;
        active_session_ = ActiveSessionInfo{};
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    if (deps_.rtsp_server && !deps_.rtsp_server->IsRunning()) {
        auto rtsp_st = deps_.rtsp_server->Start();
        if (rtsp_st.IsError()) {
            return rtsp_st;
        }
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::Idle, Common::Status::Ok());
    return Common::Status::Ok();
}

void CarPlaySessionStateMachine::Stop() {
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == SessionState::Idle) {
            return;
        }
        old_state = state_;
        state_ = SessionState::Teardown;
        observers_to_notify = observers_;
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::Teardown, Common::Status::Ok());

    if (deps_.rtsp_server && deps_.rtsp_server->IsRunning()) {
        deps_.rtsp_server->Stop();
    }
    if (deps_.mdns_advertiser && deps_.mdns_advertiser->IsRunning()) {
        deps_.mdns_advertiser->Stop();
    }
    if (deps_.wifi_manager && deps_.wifi_manager->IsRunning()) {
        deps_.wifi_manager->StopAccessPoint();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        old_state = state_;
        state_ = SessionState::Idle;
        active_session_ = ActiveSessionInfo{};
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::Idle, Common::Status::Ok());
}

SessionState CarPlaySessionStateMachine::GetState() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void CarPlaySessionStateMachine::RegisterStateObserver(SessionStateObserver observer) {
    if (!observer) return;
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.push_back(std::move(observer));
}

ActiveSessionInfo CarPlaySessionStateMachine::GetActiveSessionInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_session_;
}

Common::Status CarPlaySessionStateMachine::OnBluetoothDeviceDiscovered(
    std::string_view device_id,
    const Common::MacAddress& mac,
    std::string_view device_name) {
    
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SessionState::Idle && state_ != SessionState::Reconnecting) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Cannot discover BT device from current state");
        }

        old_state = state_;
        active_session_.device_id = std::string(device_id);
        active_session_.bluetooth_mac = mac;
        active_session_.device_name = std::string(device_name);
        active_session_.is_fast_reconnect = false;

        if (deps_.pairing_store) {
            auto paired_res = deps_.pairing_store->GetDevice(active_session_.device_id);
            if (paired_res.IsOk()) {
                active_session_.is_fast_reconnect = true;
                if (active_session_.device_name.empty()) {
                    active_session_.device_name = paired_res.Value().device_name;
                }
                active_session_.wifi_mac = paired_res.Value().wifi_mac;
            } else {
                auto mac_res = deps_.pairing_store->FindByMac(mac);
                if (mac_res.IsOk()) {
                    active_session_.is_fast_reconnect = true;
                    if (active_session_.device_name.empty()) {
                        active_session_.device_name = mac_res.Value().device_name;
                    }
                    active_session_.wifi_mac = mac_res.Value().wifi_mac;
                }
            }
        }

        state_ = SessionState::BtDiscovered;
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::BtDiscovered, Common::Status::Ok());
    return Common::Status::Ok();
}

Common::Status CarPlaySessionStateMachine::OnIap2Authenticated(
    bool success,
    std::string_view device_name) {

    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;

    if (!success) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != SessionState::BtDiscovered) {
                return Common::MakeError(Common::StatusCode::InvalidState, "Unexpected iAP2 auth failure in current state");
            }
            old_state = state_;
            state_ = SessionState::Error;
            active_session_.state_entered_time = std::chrono::steady_clock::now();
            observers_to_notify = observers_;
        }
        Common::Status err = Common::MakeError(Common::StatusCode::PermissionDenied, "iAP2 Authentication Failed");
        DispatchObservers(observers_to_notify, old_state, SessionState::Error, err);
        return err;
    }

    // 1. Enter iAP2Authenticated
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SessionState::BtDiscovered) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Unexpected iAP2 auth success in current state");
        }
        old_state = state_;
        if (!device_name.empty()) {
            active_session_.device_name = std::string(device_name);
        }

        // Persist to PairingStore
        if (deps_.pairing_store && !active_session_.device_id.empty()) {
            PairedDevice dev{};
            auto dev_res = deps_.pairing_store->GetDevice(active_session_.device_id);
            if (dev_res.IsOk()) {
                dev = dev_res.Value();
            } else {
                dev.device_id = active_session_.device_id;
            }
            if (!active_session_.device_name.empty()) dev.device_name = active_session_.device_name;
            dev.bluetooth_mac = active_session_.bluetooth_mac;
            dev.wifi_mac = active_session_.wifi_mac;
            dev.last_connected_timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            deps_.pairing_store->SaveDevice(dev);
        }

        state_ = SessionState::iAP2Authenticated;
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::iAP2Authenticated, Common::Status::Ok());

    // 2. Ensure Wi-Fi SoftAP and mDNS are up (idempotent — the service may have
    //    already started them for continuous broadcasting). Only start if not
    //    already running, and use channel=0 so the iPhone scans for the real
    //    channel instead of being told a possibly-wrong one.
    if (deps_.wifi_manager && !deps_.wifi_manager->IsRunning()) {
        Transport::WifiApCredentials creds{};
        creds.ssid = config_.ap_ssid;
        creds.passphrase = config_.ap_passphrase;
        creds.channel = config_.ap_channel;
        creds.band = Transport::WifiBand::Band5GHz;
        deps_.wifi_manager->StartAccessPoint(creds);
    }

    if (deps_.mdns_advertiser && !deps_.mdns_advertiser->IsRunning()) {
        Transport::MdnsServiceConfig mdns_cfg{};
        if (deps_.wifi_manager) {
            auto creds = deps_.wifi_manager->GetCredentials();
            if (!creds.gatewayIp.empty()) {
                mdns_cfg.ipv4Address = creds.gatewayIp;
            }
        }
        mdns_cfg.instanceName = config_.ap_ssid.empty() ? (active_session_.device_name.empty() ? "CarPlayStation" : active_session_.device_name) : config_.ap_ssid;
        mdns_cfg.port = config_.rtsp_port;
        if (!config_.device_id.empty()) {
            mdns_cfg.deviceId = config_.device_id;
        }
        deps_.mdns_advertiser->Start(mdns_cfg);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        old_state = state_;
        state_ = SessionState::WifiHandoff;
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::WifiHandoff, Common::Status::Ok());
    return Common::Status::Ok();
}

Common::Status CarPlaySessionStateMachine::OnWifiConnected(const Common::MacAddress& client_mac, std::string_view client_ip) {
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state = SessionState::Idle;
    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SessionState::WifiHandoff) {
            if (state_ == SessionState::Idle || state_ == SessionState::BtDiscovered) {
                CPS_LOG_INFO("SessionSM", "OnWifiConnected: device reconnected on Wi-Fi from state ",
                             static_cast<int>(state_), ", transitioning to WifiHandoff");
                old_state = state_;
                state_ = SessionState::WifiHandoff;
                active_session_.state_entered_time = std::chrono::steady_clock::now();
                observers_to_notify = observers_;
                should_notify = true;
            } else {
                CPS_LOG_WARN("SessionSM", "OnWifiConnected rejected: state is not WifiHandoff (state=",
                             static_cast<int>(state_), ") — station re-joined outside the iAP2 handoff path");
                return Common::MakeError(Common::StatusCode::InvalidState, "Station connected outside WifiHandoff state");
            }
        }

        active_session_.wifi_mac = client_mac;
        if (deps_.pairing_store && !active_session_.device_id.empty()) {
            auto dev_res = deps_.pairing_store->GetDevice(active_session_.device_id);
            if (dev_res.IsOk()) {
                auto dev = dev_res.Value();
                dev.wifi_mac = client_mac;
                dev.last_connected_timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                deps_.pairing_store->SaveDevice(dev);
            }
        }
    }

    if (should_notify) {
        DispatchObservers(observers_to_notify, old_state, SessionState::WifiHandoff, Common::Status::Ok());
    }

    // Station joined! Trigger immediate mDNS announcement burst and target unicast!
    if (deps_.mdns_advertiser) {
        if (!client_ip.empty()) {
            deps_.mdns_advertiser->AnnounceTo(std::string(client_ip));
        } else {
            deps_.mdns_advertiser->BroadcastAnnouncement();
        }
    }
    return Common::Status::Ok();
}

Common::Status CarPlaySessionStateMachine::OnAirPlayStreamingStarted(const NegotiatedMediaParams& params) {
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SessionState::WifiHandoff && state_ != SessionState::Paused &&
            state_ != SessionState::Reconnecting && state_ != SessionState::AirPlayStreaming &&
            state_ != SessionState::Idle && state_ != SessionState::BtDiscovered &&
            state_ != SessionState::iAP2Authenticated) {
            CPS_LOG_WARN("SessionSM", "OnAirPlayStreamingStarted rejected: current state is not valid for streaming (state=",
                         static_cast<int>(state_), ")");
            return Common::MakeError(Common::StatusCode::InvalidState, "Cannot start streaming from current state");
        }

        old_state = state_;
        state_ = SessionState::AirPlayStreaming;
        active_session_.media_params = params;
        active_session_.reconnect_attempts = 0;
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    CPS_LOG_INFO("SessionSM", "AirPlay streaming started (video data port ", std::to_string(params.video_data_port),
                 ", control port ", std::to_string(params.video_control_port), ")");
    DispatchObservers(observers_to_notify, old_state, SessionState::AirPlayStreaming, Common::Status::Ok());
    return Common::Status::Ok();
}

Common::Status CarPlaySessionStateMachine::OnAirPlayStreamingPaused() {
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SessionState::AirPlayStreaming) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Cannot pause streaming when not streaming");
        }

        old_state = state_;
        state_ = SessionState::Paused;
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::Paused, Common::Status::Ok());
    return Common::Status::Ok();
}

Common::Status CarPlaySessionStateMachine::OnAirPlayStreamingResumed() {
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SessionState::Paused) {
            return Common::MakeError(Common::StatusCode::InvalidState, "Cannot resume streaming when not paused");
        }

        old_state = state_;
        state_ = SessionState::AirPlayStreaming;
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::AirPlayStreaming, Common::Status::Ok());
    return Common::Status::Ok();
}

Common::Status CarPlaySessionStateMachine::OnAirPlayTeardown() {
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == SessionState::Idle || state_ == SessionState::Teardown) {
            return Common::Status::Ok();
        }

        old_state = state_;
        state_ = SessionState::Teardown;
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::Teardown, Common::Status::Ok());

    // Check if auto reconnect should trigger
    bool should_reconnect = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_.auto_reconnect && active_session_.reconnect_attempts < config_.max_reconnect_attempts) {
            active_session_.reconnect_attempts++;
            should_reconnect = true;
            old_state = state_;
            state_ = SessionState::Reconnecting;
            active_session_.state_entered_time = std::chrono::steady_clock::now();
            observers_to_notify = observers_;
        } else {
            old_state = state_;
            state_ = SessionState::Idle;
            active_session_ = ActiveSessionInfo{};
            active_session_.state_entered_time = std::chrono::steady_clock::now();
            observers_to_notify = observers_;
        }
    }

    if (should_reconnect) {
        DispatchObservers(observers_to_notify, old_state, SessionState::Reconnecting, Common::Status::Ok());
    } else {
        DispatchObservers(observers_to_notify, old_state, SessionState::Idle, Common::Status::Ok());
    }

    return Common::Status::Ok();
}

Common::Status CarPlaySessionStateMachine::OnDisconnect(std::string_view reason) {
    (void)reason;
    return OnAirPlayTeardown();
}

Common::Status CarPlaySessionStateMachine::OnError(Common::Status error_status) {
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old_state = state_;
        state_ = SessionState::Error;
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::Error, error_status);
    return error_status;
}

void CarPlaySessionStateMachine::TickWatchdog(std::chrono::steady_clock::time_point now) {
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;
    SessionState next_state = SessionState::Idle;
    Common::Status transition_status = Common::Status::Ok();
    bool do_transition = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        old_state = state_;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - active_session_.state_entered_time);

        switch (state_) {
            case SessionState::BtDiscovered:
                if (elapsed >= config_.bt_auth_timeout) {
                    next_state = SessionState::Error;
                    transition_status = Common::MakeError(Common::StatusCode::TimedOut, "iAP2 Authentication timed out");
                    do_transition = true;
                }
                break;

            case SessionState::WifiHandoff:
                if (elapsed >= config_.wifi_handoff_timeout) {
                    if (config_.auto_reconnect && active_session_.reconnect_attempts < config_.max_reconnect_attempts) {
                        active_session_.reconnect_attempts++;
                        next_state = SessionState::Reconnecting;
                        transition_status = Common::MakeError(Common::StatusCode::TimedOut, "Wi-Fi handoff timed out, retrying");
                    } else {
                        next_state = SessionState::Error;
                        transition_status = Common::MakeError(Common::StatusCode::TimedOut, "Wi-Fi handoff timed out");
                    }
                    do_transition = true;
                }
                break;

            case SessionState::Reconnecting:
                if (elapsed >= config_.reconnect_interval) {
                    if (active_session_.reconnect_attempts < config_.max_reconnect_attempts) {
                        // Retry handoff
                        next_state = SessionState::WifiHandoff;
                        transition_status = Common::Status::Ok();
                    } else {
                        next_state = SessionState::Error;
                        transition_status = Common::MakeError(Common::StatusCode::TimedOut, "Max reconnect attempts exceeded");
                    }
                    do_transition = true;
                }
                break;

            default:
                break;
        }

        if (do_transition) {
            state_ = next_state;
            active_session_.state_entered_time = now;
            observers_to_notify = observers_;
        }
    }

    if (do_transition) {
        DispatchObservers(observers_to_notify, old_state, next_state, transition_status);
    }
}

void CarPlaySessionStateMachine::Reset() {
    std::vector<SessionStateObserver> observers_to_notify;
    SessionState old_state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old_state = state_;
        state_ = SessionState::Idle;
        active_session_ = ActiveSessionInfo{};
        active_session_.state_entered_time = std::chrono::steady_clock::now();
        observers_to_notify = observers_;
    }

    DispatchObservers(observers_to_notify, old_state, SessionState::Idle, Common::Status::Ok());
}

void CarPlaySessionStateMachine::SetDependencies(SessionDependencies deps) {
    std::lock_guard<std::mutex> lock(mutex_);
    deps_ = std::move(deps);
}

void CarPlaySessionStateMachine::DispatchObservers(
    const std::vector<SessionStateObserver>& observers,
    SessionState old_state,
    SessionState new_state,
    Common::Status status) {
    
    for (const auto& observer : observers) {
        if (observer) {
            observer(old_state, new_state, status);
        }
    }
}

void CarPlaySessionStateMachine::WatchdogLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!running_) break;
        TickWatchdog(std::chrono::steady_clock::now());
    }
}

} // namespace CarPlayStation::Core::Session
