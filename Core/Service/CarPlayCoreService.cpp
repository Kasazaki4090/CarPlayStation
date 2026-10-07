#include "Core/Service/CarPlayCoreService.h"
#include "Core/Hardware/FileMfiSigner.h"
#include "Core/Transport/BtRfcommTransport.h"
#include "Core/Transport/BtSdpHelper.h"
#include "Core/Transport/WifiSoftApManager.h"
#include "Core/Transport/MdnsAdvertiser.h"
#include "Core/Hardware/RadioAdapterHelper.h"
#include "Core/Hardware/TouchscreenCapabilities.h"
#include "Core/Protocol/Iap2LinkEngine.h"
#include "Core/Protocol/Iap2MessageCatalog.h"
#include "Core/Protocol/Iap2ControlPlane.h"
#include "Core/Session/CarPlaySessionStateMachine.h"
#include "Core/Session/PairingStore.h"
#include "Core/Session/AccessoryIdentityStore.h"
#include "Core/Session/AirPlayRtspServer.h"
#include "Core/Session/Pairing/PairSetup.h"
#include "Core/Input/IInputEventSink.h"
#include "Core/Media/MpvVideoPlayer.h"
#include "Core/Media/WasapiAudioPipeline.h"
#include "Core/Media/WasapiMicrophoneUplink.h"
#include "Core/Media/AirPlayAudioReceiver.h"
#include "Core/Media/AirPlayVideoReceiver.h"
#include "Core/Media/MicrophoneRtpSender.h"
#include "Core/Common/Logging.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace CarPlayStation::Core::Service {

// ============================================================================
// AirPlay input sink — sends HID reports over the encrypted event channel.
// ============================================================================
namespace {

// A knob action translated into a drag on the touchscreen: the endpoints of the drag as
// fractions of the display.
struct SyntheticGesture {
    float x0 = 0.5f;
    float y0 = 0.5f;
    float x1 = 0.5f;
    float y1 = 0.5f;
};

// This receiver advertises no knob HID device — adding a third hidDevices entry to /info
// stopped the CarPlay session from coming up at all (see the same experiment documented in
// Reference/carplayd/.../receiver/src/info.rs). Keyboard arrows and the mouse wheel are
// therefore synthesized as touch drags on the touchscreen device that is already
// negotiated. The drag direction matches what the user expects from the key: pressing
// "down" scrolls the content up, which is a finger drag upwards.
[[nodiscard]] bool SynthesizeKnobGesture(
    Input::KnobAction action,
    uint32_t stepCount,
    SyntheticGesture& out) {
    struct Direction { float dx; float dy; };
    Direction dir{ 0.0f, 0.0f };
    switch (action) {
        case Input::KnobAction::PressUp:    dir = { 0.0f,  1.0f}; break;
        case Input::KnobAction::PressDown:  dir = { 0.0f, -1.0f}; break;
        case Input::KnobAction::PressLeft:  dir = { 1.0f,  0.0f}; break;
        case Input::KnobAction::PressRight: dir = {-1.0f,  0.0f}; break;
        case Input::KnobAction::RotateClockwise:       dir = { 0.0f, -1.0f}; break;
        case Input::KnobAction::RotateCounterClockwise:dir = { 0.0f,  1.0f}; break;
        case Input::KnobAction::PushDown:   dir = { 0.0f,  0.0f}; break;  // tap
        case Input::KnobAction::PushUp:     return false;                  // release: no-op
    }

    if (dir.dx == 0.0f && dir.dy == 0.0f) {
        out.x0 = out.x1 = 0.5f;
        out.y0 = out.y1 = 0.5f;
        return true;
    }
    // One knob step drags 45% of the display; more steps travel further but stay on screen.
    const float steps = static_cast<float>(std::max<uint32_t>(stepCount, 1u));
    const float half = std::min(0.225f * steps, 0.45f);
    const float hx = dir.dx * half;
    const float hy = dir.dy * half;
    out.x0 = std::clamp(0.5f - hx, 0.0f, 1.0f);
    out.y0 = std::clamp(0.5f - hy, 0.0f, 1.0f);
    out.x1 = std::clamp(0.5f + hx, 0.0f, 1.0f);
    out.y1 = std::clamp(0.5f + hy, 0.0f, 1.0f);
    return true;
}

class AirPlayInputSink final : public Input::IInputEventSink {
public:
    // Must match kMaxTouchContacts in the RTSP server's two-contact descriptor.
    static constexpr size_t kMaxContacts = 2;
    static constexpr size_t kBytesPerContact = 6;
    static constexpr size_t kReportBytes = kMaxContacts * kBytesPerContact;

    explicit AirPlayInputSink(std::weak_ptr<Session::AirPlayRtspServer> rtsp_server)
        : rtsp_server_(std::move(rtsp_server)) {}

    ~AirPlayInputSink() override {
        std::unique_ptr<std::thread> worker;
        {
            std::lock_guard<std::mutex> lock(gesture_mutex_);
            gesture_stop_ = true;
            gesture_queue_.clear();
            gesture_cv_.notify_all();
            worker.swap(gesture_worker_);
        }
        if (worker && worker->joinable()) worker->join();
    }

    [[nodiscard]] Common::Status SendTouchEvent(
        Input::TouchAction action,
        const Input::TouchPoint& point) override {
        auto server = rtsp_server_.lock();
        if (!server) {
            return Common::MakeError(Common::StatusCode::Unavailable,
                "AirPlay RTSP server is unavailable");
        }
        const Screen screen = GetScreen(*server);
        Common::Status status;
        if (server->hid_touch_contacts() >= 2) {
            status = SendMultiContact(*server, screen, action, point);
        } else {
            const bool contact = action == Input::TouchAction::Down ||
                                 action == Input::TouchAction::Move;
            status = SendTouchReport(*server, screen, contact,
                point.x_normalized, point.y_normalized);
        }
        if (status.IsOk() && action != Input::TouchAction::Move) {
            CPS_LOG_INFO("InputSink", "Sent HID touch ",
                action == Input::TouchAction::Down ? "down" :
                action == Input::TouchAction::Up ? "up" : "cancel",
                " at ", static_cast<uint32_t>(std::clamp(point.x_normalized, 0.0f, 1.0f) * screen.width),
                ",", static_cast<uint32_t>(std::clamp(point.y_normalized, 0.0f, 1.0f) * screen.height));
        }
        return status;
    }

    // Queued and played back by the gesture worker: iOS only recognises a drag when the
    // intermediate positions arrive over time, so the reports cannot be burst out.
    [[nodiscard]] Common::Status SendKnobEvent(Input::KnobAction action, uint32_t stepCount) override {
        SyntheticGesture gesture{};
        if (!SynthesizeKnobGesture(action, stepCount, gesture)) {
            return Common::MakeError(Common::StatusCode::NotSupported,
                "This receiver does not advertise a CarPlay knob HID device");
        }
        if (!rtsp_server_.lock()) {
            return Common::MakeError(Common::StatusCode::Unavailable,
                "AirPlay RTSP server is unavailable");
        }
        {
            std::lock_guard<std::mutex> lock(gesture_mutex_);
            if (gesture_stop_) {
                return Common::MakeError(Common::StatusCode::Unavailable,
                    "Input gesture worker is stopped");
            }
            if (gesture_queue_.size() >= kMaxPendingGestures) {
                gesture_queue_.pop_front();
                ++gesture_dropped_;
            }
            gesture_queue_.push_back(gesture);
            if (!gesture_worker_) {
                gesture_worker_ = std::make_unique<std::thread>([this]() { GestureWorker(); });
            }
        }
        gesture_cv_.notify_one();
        return Common::Status::Ok();
    }

    [[nodiscard]] Common::Status SendButtonEvent(
        Input::ButtonType button,
        bool isPressed) override {
        uint8_t usage = 0;
        if (isPressed) {
            switch (button) {
                case Input::ButtonType::PlayPause: usage = 3; break;
                case Input::ButtonType::NextTrack: usage = 4; break;
                case Input::ButtonType::PrevTrack: usage = 5; break;
                default:
                    return Common::MakeError(Common::StatusCode::NotSupported,
                        "This receiver does not advertise the requested HID button");
            }
        }

        auto server = rtsp_server_.lock();
        if (!server) {
            return Common::MakeError(Common::StatusCode::Unavailable,
                "AirPlay RTSP server is unavailable");
        }
        return server->SendHidReport(2, Common::ByteSpan(&usage, 1));
    }

private:
    struct Screen {
        uint16_t width = 1280;
        uint16_t height = 720;
    };

    static Screen GetScreen(const Session::AirPlayRtspServer& server) {
        const auto params = server.GetNegotiatedParams();
        Screen screen;
        screen.width = static_cast<uint16_t>(
            std::clamp(params.video_width, 1u, static_cast<uint32_t>(UINT16_MAX)));
        screen.height = static_cast<uint16_t>(
            std::clamp(params.video_height, 1u, static_cast<uint32_t>(UINT16_MAX)));
        return screen;
    }

    // Touchscreen HID (uuid 1) report: [contact:u8][x:u16 LE][y:u16 LE] in display pixels.
    [[nodiscard]] Common::Status SendTouchReport(
        Session::AirPlayRtspServer& server,
        const Screen& screen,
        bool contact,
        float x_normalized,
        float y_normalized) {
        const uint16_t x = static_cast<uint16_t>(std::clamp(x_normalized, 0.0f, 1.0f) * screen.width);
        const uint16_t y = static_cast<uint16_t>(std::clamp(y_normalized, 0.0f, 1.0f) * screen.height);
        const std::array<uint8_t, 5> report{
            static_cast<uint8_t>(contact ? 1 : 0),
            static_cast<uint8_t>(x & 0xFF),
            static_cast<uint8_t>((x >> 8) & 0xFF),
            static_cast<uint8_t>(y & 0xFF),
            static_cast<uint8_t>((y >> 8) & 0xFF)
        };
        return server.SendHidReport(1, report);
    }

    // One contact of the two-contact report: [slot:u8][tipSwitch:0/1][x:u16 LE][y:u16 LE].
    static void WriteContactSlot(std::array<uint8_t, kReportBytes>& report, size_t slot,
        bool down, float x_normalized, float y_normalized, const Screen& screen) {
        const uint16_t x = static_cast<uint16_t>(std::clamp(x_normalized, 0.0f, 1.0f) * screen.width);
        const uint16_t y = static_cast<uint16_t>(std::clamp(y_normalized, 0.0f, 1.0f) * screen.height);
        const size_t off = slot * kBytesPerContact;
        report[off] = static_cast<uint8_t>(slot);
        report[off + 1] = static_cast<uint8_t>(down ? 1 : 0);
        report[off + 2] = static_cast<uint8_t>(x & 0xFF);
        report[off + 3] = static_cast<uint8_t>((x >> 8) & 0xFF);
        report[off + 4] = static_cast<uint8_t>(y & 0xFF);
        report[off + 5] = static_cast<uint8_t>((y >> 8) & 0xFF);
    }

    // Multi-contact touchscreen HID (uuid 1) report. The descriptor declares two Finger
    // collections, so every report carries both slots; an idle slot is all zero except its
    // own index byte. That is why the whole frame is re-sent on every change rather than
    // just the pointer that moved.
    [[nodiscard]] Common::Status SendMultiContact(
        Session::AirPlayRtspServer& server,
        const Screen& screen,
        Input::TouchAction action,
        const Input::TouchPoint& point) {
        size_t slot = point.id;
        if (slot >= kMaxContacts) {
            // More fingers than the descriptor declares: fold the extra ones onto the
            // last slot so the report stays well formed.
            slot = kMaxContacts - 1;
        }
        switch (action) {
            case Input::TouchAction::Down:
            case Input::TouchAction::Move:
                slots_[slot] = Slot{ true, point.x_normalized, point.y_normalized };
                break;
            case Input::TouchAction::Up:
                slots_[slot] = Slot{};
                break;
            case Input::TouchAction::Cancel:
                slots_.fill(Slot{});
                break;
        }

        std::array<uint8_t, kReportBytes> report{};
        for (size_t i = 0; i < kMaxContacts; ++i) {
            WriteContactSlot(report, i, slots_[i].down, slots_[i].x, slots_[i].y, screen);
        }
        return server.SendHidReport(1, report);
    }

    // A single contact on whichever touchscreen shape is advertised. Used by the synthetic
    // gestures, which never need more than one finger.
    [[nodiscard]] Common::Status SendSingleContact(
        Session::AirPlayRtspServer& server,
        const Screen& screen,
        bool down,
        float x_normalized,
        float y_normalized) {
        if (server.hid_touch_contacts() < 2) {
            return SendTouchReport(server, screen, down, x_normalized, y_normalized);
        }
        // A local frame: the synthetic gestures run on the worker thread and must not
        // disturb the live contact state the UI thread is maintaining.
        std::array<uint8_t, kReportBytes> report{};
        WriteContactSlot(report, 0, down, x_normalized, y_normalized, screen);
        WriteContactSlot(report, 1, false, 0.0f, 0.0f, screen);
        return server.SendHidReport(1, report);
    }

    void GestureWorker() {
        std::deque<SyntheticGesture> queue;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(gesture_mutex_);
                gesture_cv_.wait(lock, [this]() {
                    return gesture_stop_ || !gesture_queue_.empty();
                });
                if (gesture_stop_ && gesture_queue_.empty()) break;
                queue.swap(gesture_queue_);
            }
            for (const auto& gesture : queue) {
                PlayGesture(gesture);
            }
            queue.clear();
        }
        if (gesture_dropped_ != 0) {
            CPS_LOG_INFO("InputSink", "Gesture worker dropped ",
                std::to_string(gesture_dropped_), " queued gestures");
        }
    }

    void PlayGesture(const SyntheticGesture& gesture) {
        auto server = rtsp_server_.lock();
        if (!server) return;
        const Screen screen = GetScreen(*server);

        // A tap: press and release in place.
        if (gesture.x0 == gesture.x1 && gesture.y0 == gesture.y1) {
            if (!SendSingleContact(*server, screen, true, gesture.x0, gesture.y0).IsOk()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(kGestureHoldMs));
            (void)SendSingleContact(*server, screen, false, gesture.x1, gesture.y1);
            CPS_LOG_INFO("InputSink", "Synthesized HID tap at centre for knob action");
            return;
        }

        if (!SendSingleContact(*server, screen, true, gesture.x0, gesture.y0).IsOk()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(kGestureHoldMs));
        for (int step = 1; step <= kGestureSteps; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(kGestureSteps);
            const float x = gesture.x0 + (gesture.x1 - gesture.x0) * t;
            const float y = gesture.y0 + (gesture.y1 - gesture.y0) * t;
            if (!SendSingleContact(*server, screen, true, x, y).IsOk()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(kGestureStepMs));
        }
        (void)SendSingleContact(*server, screen, false, gesture.x1, gesture.y1);
        CPS_LOG_INFO("InputSink", "Synthesized HID drag from ",
            static_cast<uint32_t>(gesture.x0 * screen.width), ",",
            static_cast<uint32_t>(gesture.y0 * screen.height), " to ",
            static_cast<uint32_t>(gesture.x1 * screen.width), ",",
            static_cast<uint32_t>(gesture.y1 * screen.height));
    }

    // A drag is played as: down, hold, kGestureSteps intermediate moves, up. ~120 ms total,
    // which is long enough for iOS to treat it as a scroll rather than a fling or a tap.
    static constexpr int kGestureHoldMs = 30;
    static constexpr int kGestureSteps = 5;
    static constexpr int kGestureStepMs = 18;
    static constexpr size_t kMaxPendingGestures = 16;

    struct Slot {
        bool down = false;
        float x = 0.0f;
        float y = 0.0f;
    };

    // Live contacts for the multi-contact report. Only written from the UI thread through
    // SendTouchEvent.
    std::array<Slot, kMaxContacts> slots_{};

    std::weak_ptr<Session::AirPlayRtspServer> rtsp_server_;
    std::mutex gesture_mutex_;
    std::condition_variable gesture_cv_;
    std::deque<SyntheticGesture> gesture_queue_;
    std::unique_ptr<std::thread> gesture_worker_;
    bool gesture_stop_ = false;
    uint64_t gesture_dropped_ = 0;
};

// MFi authentication message IDs (iAP2 control session, session 1).
namespace {
    constexpr uint16_t MfiMsgReqAuthCert = 0xAA00;  // device -> accessory: request certificate
    constexpr uint16_t MfiMsgAuthCert    = 0xAA01;  // accessory -> device: certificate
    constexpr uint16_t MfiMsgChallenge   = 0xAA02;  // device -> accessory: challenge
    constexpr uint16_t MfiMsgSignature   = 0xAA03;  // accessory -> device: signature
    constexpr uint16_t MfiMsgAuthFailed  = 0xAA04;  // device -> accessory: failure
    constexpr uint16_t MfiMsgAuthSuccess = 0xAA05;  // device -> accessory: success

    // Build a single-param iAP2 group body: [BE16 group_len][BE16 param_id][value].
    [[nodiscard]] Common::ByteBuffer BuildMfiGroup(uint16_t paramId, Common::ByteSpan value) {
        const uint16_t groupLen = static_cast<uint16_t>(4 + value.size());
        Common::ByteBuffer out;
        out.reserve(4 + value.size());
        out.push_back(static_cast<uint8_t>((groupLen >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(groupLen & 0xFF));
        out.push_back(static_cast<uint8_t>((paramId >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(paramId & 0xFF));
        for (uint8_t b : value) out.push_back(b);
        return out;
    }
}

class CoreIap2PacketHandler final : public Protocol::IIap2PacketHandler {
public:
    using SendControlMessage = std::function<Common::Status(uint16_t messageId, Common::ByteSpan body)>;

    CoreIap2PacketHandler(std::shared_ptr<Session::ICarPlaySession> session,
                          std::shared_ptr<Hardware::IMfiSigner> signer,
                          std::shared_ptr<Transport::IWifiSoftApManager> wifi_mgr,
                          SendControlMessage send_fn,
                          std::string device_name = "CarPlayStation",
                          Common::MacAddress bt_mac = {},
                          std::string fallback_ssid = "CarPlayStation",
                          std::string fallback_passphrase = "CarPlayStationSecret123")
        : session_(std::move(session)),
          mfi_signer_(std::move(signer)),
          wifi_mgr_(std::move(wifi_mgr)),
          send_fn_(std::move(send_fn)),
          device_name_(std::move(device_name)),
          bt_mac_(bt_mac),
          fallback_ssid_(std::move(fallback_ssid)),
          fallback_passphrase_(std::move(fallback_passphrase)) {}

    // Swap the signer at runtime (e.g. after the user picks a backend).
    void SetSigner(std::shared_ptr<Hardware::IMfiSigner> signer) {
        mfi_signer_ = std::move(signer);
    }

    void SetWifiManager(std::shared_ptr<Transport::IWifiSoftApManager> wifi_mgr) {
        wifi_mgr_ = std::move(wifi_mgr);
    }

    void SetDeviceIdentity(std::string device_name, Common::MacAddress bt_mac) {
        device_name_ = std::move(device_name);
        bt_mac_ = bt_mac;
    }

    void OnIap2Message(uint8_t sessionId, Common::ByteSpan payload) override {
        // Only the control session carries MFi authentication & identification.
        if (sessionId != static_cast<uint8_t>(Protocol::Iap2SessionId::Control)) {
            return;
        }
        // Control-session payload: [40 40][total BE16][msg_id BE16][groups].
        if (payload.size() < 6 || payload[0] != 0x40 || payload[1] != 0x40) {
            return;
        }
        const uint16_t msgId = static_cast<uint16_t>((payload[4] << 8) | payload[5]);
        const Common::ByteSpan groups(payload.data() + 6, payload.size() - 6);

        char hexBuf[16];
        std::snprintf(hexBuf, sizeof(hexBuf), "0x%04X", msgId);
        CPS_LOG_INFO("CoreService", "iAP2 control message received: ", hexBuf);

        try {
            switch (msgId) {
                case MfiMsgReqAuthCert:
                    HandleRequestCertificate();
                    break;
                case MfiMsgChallenge:
                    HandleChallenge(groups);
                    break;
                case MfiMsgAuthSuccess:
                    CompleteAuthentication(true);
                    break;
                case MfiMsgAuthFailed:
                    CompleteAuthentication(false);
                    break;
                case Protocol::Iap2MsgId::StartIdentification: // 0x1D00
                    CPS_LOG_INFO("CoreService", "iAP2 StartIdentification (0x1D00) received from iPhone — sending IdentifyInformation");
                    SendIdentifyInformation();
                    break;
                case Protocol::Iap2MsgId::IdentifyAccepted: // 0x1D02
                    CPS_LOG_INFO("CoreService", "iAP2 IdentifyAccepted (0x1D02) received! Wireless CarPlay confirmed by iPhone");
                    HandleIdentifyAccepted();
                    break;
                case Protocol::Iap2MsgId::IdentifyRejected: { // 0x1D03
                    auto rejected = Protocol::Iap2MessageCatalog::ParseRejectedParamIds(groups);
                    std::string rejStr;
                    for (uint16_t pid : rejected) {
                        if (!rejStr.empty()) rejStr += ", ";
                        rejStr += std::to_string(pid);
                    }
                    CPS_LOG_WARN("CoreService", "iAP2 IdentifyRejected (0x1D03) received from iPhone; rejected params: [", rejStr, "]");
                    break;
                }
                default: {
                    // Everything else on the control session belongs to the
                    // wireless control plane: 0x5700/0x5702 (Wi-Fi credentials),
                    // 0x4E0A/0x4E0B/0x4E0D/0x4E0E (device info + transport
                    // notifications) and 0x4300 (CarPlay availability).
                    auto replies = control_.OnControlMessage(msgId, groups);
                    if (!replies.empty()) {
                        SendAll(replies);
                    }
                    break;
                }
            }
        } catch (const std::exception& ex) {
            CPS_LOG_ERROR("CoreService", "Exception processing iAP2 message ", hexBuf, ": ", ex.what());
        } catch (...) {
            CPS_LOG_ERROR("CoreService", "Unknown exception processing iAP2 message ", hexBuf);
        }
    }

    void OnLinkEvent(std::string_view /*eventDescription*/) override {
        // Link-level events (SYN-ACK, etc.) do not complete MFi auth on their own.
    }

private:
    void HandleRequestCertificate() {
        if (!mfi_signer_) {
            CPS_LOG_ERROR("CoreService", "MFi ReqAuthCert received but no signer is configured — cannot authenticate");
            return;
        }
        CPS_LOG_INFO("CoreService", "MFi ReqAuthCert (0xAA00) received; fetching certificate...");
        auto cert_res = mfi_signer_->FetchCertificate();
        if (cert_res.IsError()) {
            CPS_LOG_WARN("CoreService", "MFi certificate fetch failed: ", cert_res.Error().Message());
            return;
        }
        auto group = BuildMfiGroup(0x0000, Common::ByteSpan(cert_res.Value().data(), cert_res.Value().size()));
        if (send_fn_) {
            auto send_st = send_fn_(MfiMsgAuthCert, Common::ByteSpan(group.data(), group.size()));
            if (send_st.IsError()) {
                CPS_LOG_ERROR("CoreService", "Failed to send MFi AuthCertificate packet: ", send_st.Message());
                return;
            }
        }
        CPS_LOG_INFO("CoreService", "MFi certificate sent (", std::to_string(cert_res.Value().size()), " bytes)");
    }

    void HandleChallenge(Common::ByteSpan groups) {
        if (!mfi_signer_) {
            CPS_LOG_ERROR("CoreService", "MFi Challenge received but no signer is configured");
            return;
        }
        if (groups.size() < 4) {
            CPS_LOG_WARN("CoreService", "MFi Challenge received with truncated group header (< 4 bytes)");
            return;
        }
        const uint16_t groupLen = static_cast<uint16_t>((groups[0] << 8) | groups[1]);
        if (groupLen < 4 || groupLen > groups.size()) {
            CPS_LOG_WARN("CoreService", "MFi Challenge groupLen invalid: ", std::to_string(groupLen), " / ", std::to_string(groups.size()));
            return;
        }
        const Common::ByteSpan challenge(groups.data() + 4, groupLen - 4);
        CPS_LOG_INFO("CoreService", "MFi Challenge (0xAA02) received; nonce size = ",
                     std::to_string(challenge.size()), " bytes. Signing challenge...");

        auto sig_res = mfi_signer_->SignChallenge(challenge);
        if (sig_res.IsError()) {
            CPS_LOG_WARN("CoreService", "MFi challenge signing failed: ", sig_res.Error().Message());
            return;
        }
        auto group = BuildMfiGroup(0x0000, Common::ByteSpan(sig_res.Value().data(), sig_res.Value().size()));
        if (send_fn_) {
            auto send_st = send_fn_(MfiMsgSignature, Common::ByteSpan(group.data(), group.size()));
            if (send_st.IsError()) {
                CPS_LOG_ERROR("CoreService", "Failed to send MFi AuthResponse signature packet: ", send_st.Message());
                return;
            }
        }
        CPS_LOG_INFO("CoreService", "MFi challenge signature sent (", std::to_string(sig_res.Value().size()), " bytes)");
    }

    void CompleteAuthentication(bool success) {
        if (auto session_sm = std::dynamic_pointer_cast<Session::CarPlaySessionStateMachine>(session_)) {
            if (session_sm->GetState() == Session::SessionState::BtDiscovered) {
                CPS_LOG_INFO("CoreService", success ? "MFi authentication succeeded; transitioning to authenticated"
                                                    : "MFi authentication failed");
                (void)session_sm->OnIap2Authenticated(success, "iPhone");
            }
        }
        if (success) {
            CPS_LOG_INFO("CoreService", "MFi authentication succeeded — waiting for iPhone StartIdentification (0x1D00)...");
        }
    }

    void SendIdentifyInformation() {
        if (!send_fn_) return;
        Common::ByteBuffer identBody = Protocol::Iap2MessageCatalog::BuildIdentifyInfo(
            device_name_, bt_mac_);
        auto status = send_fn_(Protocol::Iap2MsgId::IdentifyInformation,
                               Common::ByteSpan(identBody.data(), identBody.size()));
        if (status.IsOk()) {
            CPS_LOG_INFO("CoreService", "iAP2 IdentifyInformation (0x1D01) sent to iPhone (",
                         std::to_string(identBody.size()), " bytes)");
        } else {
            CPS_LOG_WARN("CoreService", "Failed to send iAP2 IdentifyInformation: ", status.Message());
        }
    }

public:
    // Refreshes the hotspot + AirPlay receiver parameters the control plane
    // needs for 0x5703 and 0x4301. Safe to call repeatedly; the WinRT hotspot
    // only reports its SSID/passphrase once it is actually up.
    void RefreshAccessPointConfig() {
        Protocol::Iap2WirelessControlPlane::WiFiConfig cfg;
        cfg.ssid = fallback_ssid_;
        cfg.passphrase = fallback_passphrase_;
        // Default to 0 (unknown channel) so the iPhone scans for the SSID on its
        // real channel. Only advertise a concrete channel if we actually know one.
        cfg.channel = 0;

        if (wifi_mgr_) {
            auto creds = wifi_mgr_->GetCredentials();
            if (!creds.ssid.empty()) {
                cfg.ssid = creds.ssid;
            }
            if (!creds.passphrase.empty()) {
                cfg.passphrase = creds.passphrase;
            }
            if (creds.channel != 0) {
                cfg.channel = static_cast<uint8_t>(creds.channel);
            }
            if (auto bssid = ParseMac(creds.bssid); !bssid.IsZero()) {
                cfg.bssid = bssid.bytes;
                cfg.hasBssid = true;
            }
            // gatewayIp is our own address on the hotspot subnet.
            if (!creds.gatewayIp.empty()) {
                session_params_.ipAddress = creds.gatewayIp;
            }
        }
        control_.SetWiFiConfig(std::move(cfg));
        control_.SetSessionParams(session_params_);
    }

    // The AirPlay receiver identity carried by 0x4301. The publicKey is the
    // accessory's persisted Ed25519 LTPK (lowercase hex).
    void SetAirPlayReceiver(uint32_t port, std::string deviceIdentifier, std::string publicKey) {
        session_params_.airPlayPort = port;
        session_params_.deviceIdentifier = std::move(deviceIdentifier);
        session_params_.publicKey = std::move(publicKey);
        control_.SetSessionParams(session_params_);
    }

private:
    static Common::MacAddress ParseMac(std::string_view str) {
        Common::MacAddress mac{};
        unsigned int b[6] = {0};
        if (sscanf_s(str.data(), "%02x:%02x:%02x:%02x:%02x:%02x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            for (int i = 0; i < 6; ++i) {
                mac.bytes[i] = static_cast<uint8_t>(b[i]);
            }
        }
        return mac;
    }

    static bool IsZeroMac(const Common::MacAddress& mac) {
        for (uint8_t b : mac.bytes) {
            if (b != 0) return false;
        }
        return true;
    }

    void SendAll(const std::vector<Protocol::Iap2WirelessControlPlane::OutgoingMessage>& messages) {
        if (!send_fn_) return;
        for (const auto& msg : messages) {
            auto status = send_fn_(msg.msgId, Common::ByteSpan(msg.body.data(), msg.body.size()));
            if (status.IsError()) {
                CPS_LOG_WARN("CoreService", "Failed to send iAP2 control message: ", status.Message());
            }
        }
    }

    // After IdentifyAccepted the iPhone expects the five update subscriptions and
    // then our hotspot credentials; without that sequence it never reaches the
    // CarPlay session (it just keeps re-sending 0x5702).
    void HandleIdentifyAccepted() {
        RefreshAccessPointConfig();
        auto subscriptions = control_.OnIdentifyAccepted();
        SendAll(subscriptions);

        // The first 0x5703 is sent unsolicited: the iPhone only asks for it
        // (0x5702) once it has already been told the hotspot exists.
        auto first = control_.OnControlMessage(Protocol::Iap2MsgId::RequestWiFiInfo, {});
        SendAll(first);
    }

    std::shared_ptr<Session::ICarPlaySession> session_;
    std::shared_ptr<Hardware::IMfiSigner> mfi_signer_;
    std::shared_ptr<Transport::IWifiSoftApManager> wifi_mgr_;
    SendControlMessage send_fn_;
    std::string device_name_{"CarPlayStation"};
    Common::MacAddress bt_mac_{0x00, 0x1A, 0x7D, 0xDA, 0x71, 0x13};
    std::string fallback_ssid_{"CarPlayStation"};
    std::string fallback_passphrase_{"CarPlayStationSecret123"};
    Protocol::Iap2WirelessControlPlane control_;
    Protocol::Iap2WirelessControlPlane::SessionParams session_params_;
};

} // anonymous namespace

// ============================================================================
// CarPlayCoreService
// ============================================================================

CarPlayCoreService::CarPlayCoreService(CarPlayCoreDependencies deps)
    : deps_(std::move(deps)) {
}

CarPlayCoreService::~CarPlayCoreService() {
    Stop();
}

Common::Status CarPlayCoreService::Initialize(const Common::CoreEnvironmentConfig& env) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    if (initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Already initialized");
    }

    env_config_ = env;

    // Construct subsystems: use provided dependencies or create defaults.
    // The facade takes ownership via shared_ptr.

    // Layer 1: Hardware (MFi Signer)
    if (deps_.mfi_signer) {
        mfi_signer_ = deps_.mfi_signer;
    } else {
        // Default: FileMfiSigner (offline software signer, DEC-006)
        mfi_signer_ = std::make_shared<Hardware::FileMfiSigner>();
    }

    // Layer 2: Transport
    if (deps_.transport) {
        transport_ = deps_.transport;
    } else {
        transport_ = std::make_shared<Transport::BtRfcommTransport>();
    }

    // Layer 3: Protocol (iAP2 Link)
    if (deps_.iap2_link) {
        iap2_link_ = deps_.iap2_link;
    } else {
        iap2_link_ = std::make_shared<Protocol::Iap2LinkEngine>();
    }

    // Query physical Bluetooth adapter MAC to establish matching CarKit identity for iAP2 and mDNS
    Hardware::RadioAdapterHelper radio_helper;
    auto bt_caps = radio_helper.DetectBluetoothCapabilities();
    Common::MacAddress local_bt_mac{};
    std::string local_bt_mac_str = bt_caps.address_string;
    if (bt_caps.has_bt_adapter && !local_bt_mac_str.empty()) {
        uint64_t bth_addr = Transport::BtSdpHelper::ParseBluetoothAddress(local_bt_mac_str);
        for (size_t i = 0; i < 6; ++i) {
            local_bt_mac.bytes[5 - i] = static_cast<uint8_t>((bth_addr >> (8 * i)) & 0xFF);
        }
        Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
            "Local Bluetooth Radio: MAC=", local_bt_mac_str, ", Name='", bt_caps.adapter_name, "'");
    } else {
        local_bt_mac = Common::MacAddress{0x00, 0x1A, 0x7D, 0xDA, 0x71, 0x05};
        local_bt_mac_str = "00:1A:7D:DA:71:05";
        Common::Logger::Instance().Log(Common::LogLevel::Warn, "CoreService",
            "Using fallback Bluetooth MAC: ", local_bt_mac_str);
    }

    // Remember the real device identity so the persistent mDNS advertiser can
    // advertise the correct AirPlay-Receiver-Device-ID (not the default).
    device_id_ = local_bt_mac_str;

    // Layer 4: Session
    if (deps_.session) {
        session_ = deps_.session;
    } else {
        Session::SessionConfig session_config{};
        session_config.device_id = local_bt_mac_str;
        session_config.ap_ssid = env.network.wifi_softap_ssid.empty() ? (env.identity.device_name.empty() ? "CarPlayStation" : env.identity.device_name) : env.network.wifi_softap_ssid;
        session_config.ap_passphrase = env.network.wifi_softap_password.empty() ? "CarPlayStationSecret123" : env.network.wifi_softap_password;
        session_config.rtsp_port = env.network.rtsp_port != 0 ? env.network.rtsp_port : 5000;

        wifi_manager_ = std::make_shared<Transport::WifiSoftApManager>();

        Session::AirPlayRtspServer::Options rtsp_opts{};
        rtsp_opts.port = session_config.rtsp_port;
        // The display geometry advertised in /info comes from the host's configured
        // resolution, so the phone lays out and encodes at the size we asked for.
        rtsp_opts.default_video_width = video_spec_.width;
        rtsp_opts.default_video_height = video_spec_.height;
        rtsp_opts.default_fps = video_spec_.fps;
        // HEVC is only ever sent when we ask for it: the iPhone proposes the codec
        // but the receiver has to accept it in /info and in SETUP phase 1.
        rtsp_opts.advertise_hevc = video_spec_.codec == Common::VideoCodec::HEVC;
        // How many contacts the touchscreen HID report may carry is a property of
        // this machine's hardware, not a setting: only hosts with a display
        // digitizer can produce a second contact, and the descriptor advertised
        // in /info has to match what will actually be sent.
        {
            const auto touch_caps = Hardware::DetectTouchscreenCapabilities();
            rtsp_opts.hid_touch_contacts = touch_caps.max_contacts >= 2 ? 2 : 1;
            if (rtsp_opts.hid_touch_contacts >= 2) {
                Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
                    "HID: multi-touch enabled - touch screen '" +
                    (touch_caps.device_description.empty() ? std::string("unnamed")
                                                          : touch_caps.device_description) +
                    "' reports up to " + std::to_string(touch_caps.max_contacts) +
                    " contacts; advertising a 2-contact touchscreen");
            } else if (touch_caps.has_touch_screen) {
                Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
                    "HID: single-touch mode - touch screen supports one contact");
            } else {
                Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
                    "HID: single-touch mode - no touch screen detected");
            }
        }
        rtsp_server_ = std::make_shared<Session::AirPlayRtspServer>(rtsp_opts);

        // Persistent mDNS advertiser: owned by the service so it can keep
        // broadcasting for the whole service lifetime (continuous discovery),
        // not just during an active session.
        mdns_advertiser_ = std::make_shared<Transport::MdnsAdvertiser>();

        Session::SessionDependencies session_deps{};
        pairing_store_ = std::make_shared<Session::PairingStore>("carplay_pairings.json");
        session_deps.pairing_store = pairing_store_;

        // Enable the RTSP pair-setup handshake (POST /pair-setup). The accessory's
        // Ed25519 long-term keypair is loaded from (or created in) a small file so
        // the iPhone recognises us as the same accessory next time; each newly
        // paired controller is recorded in the pairing store.
        {
            accessory_identity_ = std::make_shared<Session::AccessoryIdentityStore>();
            Session::Pairing::AccessoryIdentity identity = accessory_identity_->Identity().ToAccessoryIdentity();
            identity.id = std::vector<uint8_t>(local_bt_mac_str.begin(), local_bt_mac_str.end());
            rtsp_server_->SetPairSetupIdentity(identity,
                [store = pairing_store_](const uint8_t* id, size_t idLen, const uint8_t* ltpk) {
                    Session::PairedDevice dev;
                    dev.device_id = std::string(reinterpret_cast<const char*>(id), idLen);
                    dev.public_key.assign(ltpk, ltpk + 32);
                    dev.last_connected_timestamp = static_cast<uint64_t>(::time(nullptr));
                    (void)store->SaveDevice(dev);
                    CPS_LOG_INFO("CoreService", "Saved paired device to store: ", dev.device_id);
                });
            rtsp_server_->SetPairVerify(std::move(identity),
                [store = pairing_store_](const uint8_t* id, size_t idLen) -> std::optional<std::array<uint8_t, 32>> {
                    std::string dev_id(reinterpret_cast<const char*>(id), idLen);
                    auto dev = store->GetDevice(dev_id);
                    if (dev.IsError() || dev.Value().public_key.size() != 32) {
                        CPS_LOG_WARN("CoreService", "Device not found in pairing store: ", dev_id);
                        return std::nullopt;
                    }
                    std::array<uint8_t, 32> ltpk;
                    memcpy(ltpk.data(), dev.Value().public_key.data(), 32);
                    CPS_LOG_INFO("CoreService", "Found paired device in store: ", dev_id);
                    return ltpk;
                });
            rtsp_server_->SetMfiSigner(mfi_signer_);
        }
        session_deps.rtsp_server = rtsp_server_;
        session_deps.wifi_manager = wifi_manager_;
        session_deps.mdns_advertiser = mdns_advertiser_;

        session_ = std::make_shared<Session::CarPlaySessionStateMachine>(session_config, session_deps);

        // Forward streaming started callback from RTSP server to configure video decryption
        rtsp_server_->SetStreamingStartedCallback([this](const Session::NegotiatedMediaParams& params) {
            CPS_LOG_INFO("CoreService", "AirPlay streaming started, configuring video decoder & receiver");
            if (video_decoder_) {
                Common::VideoStreamSpec spec = video_spec_;
                auto st = video_decoder_->Init(spec);
                CPS_LOG_INFO("CoreService", "Video decoder initialized: ", st.IsOk() ? "OK" : st.Message());
            }
            if (video_receiver_ && params.video_encrypted) {
                video_receiver_->SetDecryptionKey(params.video_decrypt_key);
            }
            ConfigureAudioReceivers(params);
            ConfigureMicrophoneUplink(params);
            if (auto session_sm = std::dynamic_pointer_cast<Session::CarPlaySessionStateMachine>(session_)) {
                (void)session_sm->OnAirPlayStreamingStarted(params);
            }
        });

        rtsp_server_->SetDuckAudioCallback([this](bool ducked) {
            CPS_LOG_INFO("CoreService", "Audio ducking command: ducked=", ducked ? "yes" : "no");
            if (audio_pipeline_) {
                audio_pipeline_->SetDuckingActive(ducked);
            }
        });

        rtsp_server_->SetStreamTeardownCallback([this](uint32_t stream_type) {
            CPS_LOG_INFO("CoreService", "Audio stream teardown: type=", stream_type);
            if (stream_type == 101) {
                auto it = std::find_if(audio_receivers_.begin(), audio_receivers_.end(),
                    [](const std::shared_ptr<Media::AirPlayAudioReceiver>& r) {
                        return r && r->GetLane() == Media::AudioLane::AltNavigation;
                    });
                if (it != audio_receivers_.end()) {
                    (*it)->Stop();
                    audio_receivers_.erase(it);
                }
                if (audio_pipeline_) {
                    audio_pipeline_->StopLane(Media::AudioLane::AltNavigation);
                    audio_pipeline_->SetDuckingActive(false);
                }
            } else if (stream_type == 100) {
                auto it = std::find_if(audio_receivers_.begin(), audio_receivers_.end(),
                    [](const std::shared_ptr<Media::AirPlayAudioReceiver>& r) {
                        return r && r->GetLane() == Media::AudioLane::SiriTelephony;
                    });
                if (it != audio_receivers_.end()) {
                    (*it)->Stop();
                    audio_receivers_.erase(it);
                }
                if (microphone_sender_) {
                    microphone_sender_->Stop();
                    microphone_sender_.reset();
                }
                if (audio_pipeline_) {
                    audio_pipeline_->StopLane(Media::AudioLane::SiriTelephony);
                }
            } else if (stream_type == 102) {
                auto it = std::find_if(audio_receivers_.begin(), audio_receivers_.end(),
                    [](const std::shared_ptr<Media::AirPlayAudioReceiver>& r) {
                        return r && r->GetLane() == Media::AudioLane::MainMedia;
                    });
                if (it != audio_receivers_.end()) {
                    (*it)->Stop();
                    audio_receivers_.erase(it);
                }
                if (audio_pipeline_) {
                    audio_pipeline_->StopLane(Media::AudioLane::MainMedia);
                }
            }
        });

        if (wifi_manager_) {
            wifi_manager_->SetStationConnectedCallback([this](const Transport::StationInfo& station) {
                CPS_LOG_INFO("CoreService", "WiFi Station joined: MAC=", station.macAddress, " IP=", station.ipAddress);
                if (auto session_sm = std::dynamic_pointer_cast<Session::CarPlaySessionStateMachine>(session_)) {
                    uint64_t bth_addr = Transport::BtSdpHelper::ParseBluetoothAddress(station.macAddress);
                    Common::MacAddress mac{};
                    for (size_t i = 0; i < 6; ++i) {
                        mac.bytes[5 - i] = static_cast<uint8_t>((bth_addr >> (8 * i)) & 0xFF);
                    }
                    session_sm->OnWifiConnected(mac, station.ipAddress);

                    // Remember the station (WiFi) MAC. The real Bluetooth MAC
                    // is only known once the BT transport connects (which
                    // happens AFTER the WiFi join), so the device-connected
                    // event is published from the BT-connection path below.
                    last_wifi_mac_ = station.macAddress;

                    // If the BT MAC is already known (reconnect where BT
                    // connected first), publish now; otherwise wait for the
                    // transport Connected callback.
                    auto info = session_sm->GetActiveSessionInfo();
                    if (!info.bluetooth_mac.IsZero()) {
                        event_bus_.PublishDeviceConnected(
                            info.bluetooth_mac.ToString(),
                            station.macAddress,
                            info.device_name);
                    }

                    // --- Known-device fast reconnect ---
                    // On a second connection the iPhone often re-joins the
                    // SoftAP on its own WITHOUT re-establishing the Bluetooth
                    // link (no BtRfcomm accept, no iAP2 flow). In that case the
                    // session never reaches WifiHandoff, OnWifiConnected is
                    // rejected, and no targeted _carplay-ctrl mDNS query is
                    // ever sent — so the /ctrl-int/1/connect dial (HTTP 200)
                    // never happens and the session stalls.
                    //
                    // Fix: if this station's MAC matches a known/paired device
                    // (the persisted address), proactively send a targeted
                    // _carplay-ctrl mDNS query to its IP. The advertiser's
                    // worker loop picks up the response and dials the
                    // (ephemeral) control port, restoring the HTTP 200
                    // handshake and enabling automatic reconnection.
                    if (!station.ipAddress.empty() && mdns_advertiser_) {
                        bool is_known = false;
                        if (pairing_store_ && !station.macAddress.empty()) {
                            uint64_t wifi_bth = Transport::BtSdpHelper::ParseBluetoothAddress(station.macAddress);
                            Common::MacAddress wifi_mac{};
                            for (size_t i = 0; i < 6; ++i) {
                                wifi_mac.bytes[5 - i] = static_cast<uint8_t>((wifi_bth >> (8 * i)) & 0xFF);
                            }
                            auto known = pairing_store_->FindByMac(wifi_mac);
                            if (known.IsOk() && !known.Value().public_key.empty()) {
                                is_known = true;
                                CPS_LOG_INFO("CoreService",
                                    "Known paired device re-joined WiFi (", station.macAddress,
                                    "); proactively announcing and querying _carplay-ctrl at ", station.ipAddress);
                            }
                        }
                        if (!is_known) {
                            CPS_LOG_INFO("CoreService", "Station joined WiFi (", station.macAddress,
                                         "); announcing AirPlay service to ", station.ipAddress);
                        }

                        if (!mdns_advertiser_->IsRunning()) {
                            Transport::MdnsServiceConfig mdns_cfg{};
                            mdns_cfg.instanceName = env_config_.identity.device_name.empty()
                                ? "CarPlayStation" : env_config_.identity.device_name;
                            mdns_cfg.port = env_config_.network.rtsp_port != 0
                                ? env_config_.network.rtsp_port : 5000;
                            mdns_cfg.ipv4Address = "192.168.137.1";
                            if (wifi_manager_) {
                                auto creds = wifi_manager_->GetCredentials();
                                if (!creds.gatewayIp.empty()) {
                                    mdns_cfg.ipv4Address = creds.gatewayIp;
                                }
                            }
                            if (!device_id_.empty()) {
                                mdns_cfg.deviceId = device_id_;
                            }
                            ApplyAirPlayPublicKey(mdns_cfg);
                            (void)mdns_advertiser_->Start(mdns_cfg);
                        }
                        (void)mdns_advertiser_->AnnounceTo(station.ipAddress);
                    }
                }
            });
        }
    }

    // Layer 5: Media
    if (deps_.video_decoder) {
        video_decoder_ = deps_.video_decoder;
    } else {
        video_decoder_ = std::make_shared<Media::MpvVideoPlayer>();
    }
    video_receiver_ = std::make_shared<Media::AirPlayVideoReceiver>(video_decoder_, static_cast<uint16_t>(7100));
    video_receiver_->SetDefaultStreamSpec(video_spec_);

    if (deps_.audio_pipeline) {
        audio_pipeline_ = deps_.audio_pipeline;
    } else {
        audio_pipeline_ = std::make_shared<Media::WasapiAudioPipeline>();
    }

    if (deps_.microphone_uplink) {
        microphone_uplink_ = deps_.microphone_uplink;
    } else {
        microphone_uplink_ = std::make_shared<Media::WasapiMicrophoneUplink>();
    }

    // Layer 6: Input
    if (deps_.input_sink) {
        input_sink_ = deps_.input_sink;
    } else {
        input_sink_ = std::make_shared<AirPlayInputSink>(rtsp_server_);
    }

    // Wire Transport <-> iAP2 link
    if (transport_ && iap2_link_) {
        transport_->SetDataCallback([this](Common::ByteSpan data) {
            if (iap2_link_) {
                iap2_link_->FeedData(data);
            }
        });

        if (auto iap2_engine = std::dynamic_pointer_cast<Protocol::Iap2LinkEngine>(iap2_link_)) {
            iap2_engine->SetSendFunction([this](Common::ByteSpan d) {
                if (transport_) {
                    return transport_->Send(d);
                }
                return Common::MakeError(Common::StatusCode::Unavailable, "Transport unavailable");
            });
        }
    }

    // Wire iAP2 handler to Session. The handler needs the MFi signer (to answer the
    // certificate/challenge exchange) and a way to emit control-session messages.
    auto send_control_message = [this](uint16_t messageId, Common::ByteSpan body) -> Common::Status {
        if (auto engine = std::dynamic_pointer_cast<Protocol::Iap2LinkEngine>(iap2_link_)) {
            auto pkt = engine->BuildMessage(static_cast<uint8_t>(Protocol::Iap2SessionId::Control),
                                            messageId, body);
            if (transport_) {
                return transport_->Send(Common::ByteSpan(pkt.data(), pkt.size()));
            }
        }
        return Common::MakeError(Common::StatusCode::Unavailable, "iAP2 link unavailable");
    };
    auto handler = std::make_shared<CoreIap2PacketHandler>(
        session_, mfi_signer_, wifi_manager_, send_control_message,
        env_config_.identity.device_name,
        local_bt_mac,
        env_config_.network.wifi_softap_ssid,
        env_config_.network.wifi_softap_password);
    // The 0x4301 wirelessCarPlayStartSession record has to name the same AirPlay
    // receiver (port, device id, Ed25519 publicKey) that mDNS advertises and that
    // RTSP pair-setup authenticates with, otherwise the iPhone drops the session.
    if (accessory_identity_) {
        const auto& ltpk = accessory_identity_->Identity().public_key;
        handler->SetAirPlayReceiver(env_config_.network.rtsp_port != 0 ? env_config_.network.rtsp_port : 5000,
                                    device_id_, Common::ToHexCompact(ltpk));
    }
    iap2_handler_ = handler;
    if (iap2_link_) {
        iap2_link_->SetPacketHandler(iap2_handler_.get());
    }

    // Wire Transport connection state to Session
    if (transport_) {
        transport_->SetStateCallback([this](Transport::TransportState state, Common::Status /*status*/) {
            auto session_sm = std::dynamic_pointer_cast<Session::CarPlaySessionStateMachine>(session_);
            if (!session_sm) return;

            if (state == Transport::TransportState::Connected) {
                std::string endpoint = transport_->GetRemoteEndpoint();
                std::string mac_str = endpoint.size() >= 17 ? endpoint.substr(0, 17) : endpoint;
                uint64_t bth_addr = Transport::BtSdpHelper::ParseBluetoothAddress(mac_str);
                Common::MacAddress mac{};
                for (size_t i = 0; i < 6; ++i) {
                    mac.bytes[5 - i] = static_cast<uint8_t>((bth_addr >> (8 * i)) & 0xFF);
                }
                Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
                    "Bluetooth device connected: ", endpoint);
                session_sm->OnBluetoothDeviceDiscovered("iPhone", mac, env_config_.identity.device_name);

                // The real Bluetooth MAC is now known. Publish the
                // device-connected event so the Application layer can persist
                // the correct BT + WiFi MACs (the WiFi join happens before the
                // BT connection, so this is the only point where both are
                // available).
                auto info = session_sm->GetActiveSessionInfo();
                event_bus_.PublishDeviceConnected(
                    mac.ToString(),
                    last_wifi_mac_,
                    info.device_name);

                // A new session is starting: the iPhone's _carplay-ctrl port is
                // ephemeral, so clear the mDNS dial cache to allow re-dialing
                // /ctrl-int/1/connect (restores the HTTP 200 handshake and
                // enables automatic reconnection).
                if (mdns_advertiser_) {
                    mdns_advertiser_->ClearDialedTargets();
                }

                // Initiate the iAP2 link: send the DETECT prelude + SYN so the iPhone
                // can complete the link handshake and begin the MFi auth exchange.
                if (auto engine = std::dynamic_pointer_cast<Protocol::Iap2LinkEngine>(iap2_link_)) {
                    engine->Reset();
                    engine->InitiateLink();
                }
            } else if (state == Transport::TransportState::Disconnected) {
                Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
                    "Bluetooth device disconnected");
                if (iap2_link_) {
                    iap2_link_->Reset();
                }
                session_sm->OnDisconnect("Bluetooth transport disconnected");
            }
        });
    }

    // Wire the session state observer to the event bus.
    WireSessionStateObserver();

    initialized_ = true;
    Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
        "Initialized with env: ", env.identity.device_name);
    return Common::Status::Ok();
}

void CarPlayCoreService::SetVideoStreamSpec(const Common::VideoStreamSpec& spec) {
    if (spec.width == 0 || spec.height == 0) return;
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    video_spec_ = spec;
    // The RTSP advert is what tells the iPhone how big to lay out and encode the
    // screen; the video receiver seeds the decoder with the same geometry.
    if (rtsp_server_) rtsp_server_->SetDisplayGeometry(spec.width, spec.height, spec.fps);
    if (rtsp_server_) rtsp_server_->SetVideoCodec(spec.codec);
    if (video_receiver_) video_receiver_->SetDefaultStreamSpec(spec);
    CPS_LOG_INFO("CoreService", "Video stream spec set to ",
                 std::to_string(spec.width), "x", std::to_string(spec.height), "@",
                 std::to_string(spec.fps), " ", std::string(Common::VideoCodecToString(spec.codec)));
}

Common::VideoStreamSpec CarPlayCoreService::GetActiveVideoStreamSpec() const {
    std::shared_ptr<Media::AirPlayVideoReceiver> receiver;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        receiver = video_receiver_;
    }
    // The receiver owns the truth: its spec is seeded from the settings and its
    // codec is corrected from the sender's first VideoConfig.
    if (receiver) return receiver->GetActiveStreamSpec();
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return video_spec_;
}

Common::Status CarPlayCoreService::UpdateEnvironment(const Common::CoreEnvironmentConfig& env) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Not initialized");
    }

    env_config_ = env;

    // The Application layer constructs the correct signer type; the facade only
    // stores the environment config, so no signer endpoint is updated here.

    Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
        "Environment updated: ", env.identity.device_name);
    return Common::Status::Ok();
}

Common::Status CarPlayCoreService::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Not initialized");
    }
    if (started_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Already started");
    }

    // Start Bluetooth RFCOMM server listener and register Wireless iAP2 SDP service
    if (auto rfcomm = std::dynamic_pointer_cast<Transport::BtRfcommTransport>(transport_)) {
        auto tr_st = rfcomm->StartServer(1);
        if (tr_st.IsError()) {
            Common::Logger::Instance().Log(Common::LogLevel::Warn, "CoreService",
                "Bluetooth RFCOMM listener could not be started: ", tr_st.Message());
        } else {
            Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
                "Bluetooth RFCOMM server started on channel 1 with Wireless iAP2 SDP record registered");
        }
    }

    // Start the session (which triggers discovery / advertising).
    auto status = session_->Start();
    if (status.IsError()) {
        Common::Logger::Instance().Log(Common::LogLevel::Error, "CoreService",
            "Session start failed: ", status.Message());
        return status;
    }

    // Bring up the Wi-Fi SoftAP + mDNS advertiser so the device is always
    // discoverable (continuous broadcasting), independent of an active session.
    StartPersistentBroadcasting();

    // Start AirPlay Video Receiver on TCP/UDP 7100
    if (video_receiver_) {
        auto vr_st = video_receiver_->Start();
        if (vr_st.IsOk()) {
            Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
                "AirPlay Video Receiver listening on port 7100");
        } else {
            Common::Logger::Instance().Log(Common::LogLevel::Warn, "CoreService",
                "Failed to start AirPlay Video Receiver: ", vr_st.Message());
        }
    }

    // Start Audio Pipeline
    if (audio_pipeline_) {
        audio_output_ = Common::AudioOutputFormat{};
        audio_output_.sample_rate = 48000;
        audio_output_.channels = 2;
        audio_output_.bit_depth = 16;
        audio_output_.sample_type = Common::AudioSampleType::PcmInt16;
        auto ap_st = audio_pipeline_->Start(audio_output_);
        if (ap_st.IsOk()) {
            Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
                "Audio Pipeline started at 48000Hz stereo");
        } else {
            Common::Logger::Instance().Log(Common::LogLevel::Warn, "CoreService",
                "Failed to start Audio Pipeline: ", ap_st.Message());
        }
    }

    started_ = true;
    Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService", "Started");
    return Common::Status::Ok();
}

void CarPlayCoreService::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    if (!started_ && !initialized_) {
        return;
    }

    TearDownSubsystems();
    started_ = false;
    Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService", "Stopped");
}

std::shared_ptr<Session::ICarPlaySession> CarPlayCoreService::GetSession() {
    return session_;
}

std::shared_ptr<Media::IVideoDecoder> CarPlayCoreService::GetVideoDecoder() {
    return video_decoder_;
}

std::shared_ptr<Media::IAudioPipeline> CarPlayCoreService::GetAudioPipeline() {
    return audio_pipeline_;
}

std::shared_ptr<Input::IInputEventSink> CarPlayCoreService::GetInputSink() {
    return input_sink_;
}

uint64_t CarPlayCoreService::SubscribeToEvents(CoreEventCallback callback) {
    return event_bus_.Subscribe(std::move(callback));
}

void CarPlayCoreService::UnsubscribeFromEvents(uint64_t token) {
    event_bus_.Unsubscribe(token);
}

std::shared_ptr<Hardware::IMfiSigner> CarPlayCoreService::GetMfiSigner() const {
    return mfi_signer_;
}

Common::Status CarPlayCoreService::SetMfiSigner(std::shared_ptr<Hardware::IMfiSigner> signer) {
    if (!signer) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "MFi signer is null");
    }
    mfi_signer_ = std::move(signer);

    if (auto handler = std::dynamic_pointer_cast<CoreIap2PacketHandler>(iap2_handler_)) {
        handler->SetSigner(mfi_signer_);
    }
    if (rtsp_server_) {
        rtsp_server_->SetMfiSigner(mfi_signer_);
    }
    return Common::Status::Ok();
}

std::shared_ptr<Transport::ITransportChannel> CarPlayCoreService::GetTransport() const {
    return transport_;
}

std::shared_ptr<Protocol::IIap2Link> CarPlayCoreService::GetIap2Link() const {
    return iap2_link_;
}

std::shared_ptr<Media::IMicrophoneUplink> CarPlayCoreService::GetMicrophoneUplink() const {
    return microphone_uplink_;
}

void CarPlayCoreService::WireSessionStateObserver() {
    if (!session_) {
        return;
    }

    // Bridge session state transitions to the Core Event Bus.
    session_->RegisterStateObserver(
        [this](Session::SessionState old_state,
               Session::SessionState new_state,
               Common::Status status) {
            event_bus_.PublishConnectionStateChanged(old_state, new_state, status);
        });
}

// Maps a negotiated audio stream onto the local playback lane that owns it.
// Mirrors the lane split documented in IAudioPipeline: stream 102 is the
// buffered media lane, 101 is the secondary/navigation lane, and every
// unbuffered voice stream (alert/default/telephony/speechRecognition) shares
// the Siri/telephony lane, which is also the only stream the iPhone expects
// a microphone back-channel for.
static Media::AudioLane SelectAudioLane(const Session::AudioStreamNegotiation& stream) {
    if (stream.type == 102 || stream.audio_type == "media") {
        return Media::AudioLane::MainMedia;
    }
    if (stream.type == 101) {
        return Media::AudioLane::AltNavigation;
    }
    return Media::AudioLane::SiriTelephony;
}

void CarPlayCoreService::ConfigureAudioReceivers(const Session::NegotiatedMediaParams& params) {
    std::unordered_set<Media::AudioLane> active_lanes;

    for (const auto& stream : params.audio_streams) {
        if (stream.data_port == 0) {
            CPS_LOG_WARN("CoreService", "Audio stream without a data port skipped: type=", stream.type,
                         " audioType=", stream.audio_type);
            continue;
        }

        Media::AudioLane lane = SelectAudioLane(stream);
        active_lanes.insert(lane);

        // Check if we already have an active receiver for this lane on the same port
        auto existing = std::find_if(audio_receivers_.begin(), audio_receivers_.end(),
            [lane](const std::shared_ptr<Media::AirPlayAudioReceiver>& r) {
                return r && r->GetLane() == lane;
            });

        if (existing != audio_receivers_.end() && (*existing)->IsRunning() && (*existing)->GetPort() == stream.data_port) {
            (*existing)->SetDecryptionKey(stream.output_key);
            CPS_LOG_INFO("CoreService", "Audio receiver for lane ", static_cast<int>(lane),
                         " already running on port ", stream.data_port, "; preserved with updated decryption key");
            continue;
        }

        if (existing != audio_receivers_.end()) {
            (*existing)->Stop();
            audio_receivers_.erase(existing);
        }

        Media::AirPlayAudioReceiver::Config config;
        config.lane = lane;
        config.udp_port = stream.data_port;
        // RtspAudioCodec and AudioCodecKind share the same numbering; Core/Session
        // cannot include Core/Media (Media already depends on Session/Pairing).
        config.stream.codec = static_cast<Media::AudioCodecKind>(stream.codec);
        config.stream.sample_rate = stream.sample_rate;
        config.stream.channels = stream.channels;

        auto receiver = std::make_shared<Media::AirPlayAudioReceiver>(audio_pipeline_, config);
        // The render format is fixed when the pipeline starts, so the receiver
        // has to resample into it.
        Media::AirPlayAudioReceiver::OutputFormat output{};
        output.sample_rate = audio_output_.sample_rate;
        output.channels = audio_output_.channels;
        receiver->SetOutputFormat(output);
        receiver->SetDecryptionKey(stream.output_key);

        auto st = receiver->Start();
        if (!st.IsOk()) {
            CPS_LOG_ERROR("CoreService", "Audio receiver start failed: lane=", static_cast<int>(config.lane),
                          " port=", config.udp_port, " codec=", Media::AudioCodecKindName(config.stream.codec),
                          " rate=", config.stream.sample_rate, " ch=", static_cast<int>(config.stream.channels),
                          " err=", st.Message());
            continue;
        }
        CPS_LOG_INFO("CoreService", "Audio receiver started: lane=", static_cast<int>(config.lane),
                     " port=", receiver->GetPort(), " codec=", Media::AudioCodecKindName(config.stream.codec),
                     " rate=", config.stream.sample_rate, " ch=", static_cast<int>(config.stream.channels),
                     " audioType=", stream.audio_type);
        audio_receivers_.push_back(receiver);
    }

    // Stop receivers for lanes that are no longer in params.audio_streams
    for (auto it = audio_receivers_.begin(); it != audio_receivers_.end(); ) {
        if (*it && active_lanes.find((*it)->GetLane()) == active_lanes.end()) {
            CPS_LOG_INFO("CoreService", "Stopping inactive audio receiver for lane ", static_cast<int>((*it)->GetLane()));
            (*it)->Stop();
            it = audio_receivers_.erase(it);
        } else {
            ++it;
        }
    }

    CPS_LOG_INFO("CoreService", "Audio receivers configured: count=", audio_receivers_.size());
}

// Arm the microphone back-channel when the iPhone asked for input on the voice
// stream. The capture format follows the negotiation (AAC-ELD 16 kHz mono in
// practice), and the sender encodes + seals each frame before sending it to the
// iPhone's dataPort on the RTSP control connection's address.
void CarPlayCoreService::ConfigureMicrophoneUplink(const Session::NegotiatedMediaParams& params) {
    // A re-SETUP supersedes the previous mic state, so always drop the old
    // sender first (it owns the UDP socket and the uplink callback).
    if (microphone_sender_) {
        microphone_sender_->Stop();
        microphone_sender_.reset();
    }
    if (!params.mic_input_requested || params.mic_uplink_port == 0 || params.mic_peer_address.empty()) {
        if (microphone_uplink_ && microphone_uplink_->IsCapturing()) {
            (void)microphone_uplink_->Stop();
        }
        return;
    }
    if (!microphone_uplink_) {
        CPS_LOG_WARN("CoreService", "Microphone uplink requested but no capture device available");
        return;
    }

    const uint32_t rate = params.mic_sample_rate ? params.mic_sample_rate : 16000;
    const uint8_t channels = params.mic_channels ? params.mic_channels : 1;

    Media::MicrophoneRtpConfig cfg{};
    cfg.destination_ip = params.mic_peer_address;
    cfg.destination_port = params.mic_uplink_port;
    cfg.payload_type = 100;
    cfg.ssrc = 0;
    cfg.sample_rate = rate;
    cfg.channels = channels;
    cfg.input_sample_type = Common::AudioSampleType::PcmFloat32;
    cfg.aac_eld = params.mic_codec == Session::RtspAudioCodec::AacEld;
    cfg.bitrate_bps = 24000;
    cfg.encrypted = true;
    cfg.input_key = params.mic_input_key;

    microphone_sender_ = std::make_shared<Media::MicrophoneRtpSender>();
    microphone_sender_->AttachToUplink(microphone_uplink_);

    auto st = microphone_sender_->Start(cfg);
    if (!st.IsOk()) {
        CPS_LOG_ERROR("CoreService", "Microphone RTP sender start failed: ", st.Message());
        microphone_sender_.reset();
        return;
    }

    Common::AudioOutputFormat cap{};
    cap.sample_rate = rate;
    cap.channels = channels;
    cap.bit_depth = 16;
    cap.sample_type = Common::AudioSampleType::PcmFloat32;
    auto cap_st = microphone_uplink_->Start(cap);
    if (!cap_st.IsOk()) {
        CPS_LOG_WARN("CoreService", "Microphone capture start failed: ", cap_st.Message());
    }

    CPS_LOG_INFO("CoreService", "Microphone uplink started: dst=", cfg.destination_ip,
                 ":", std::to_string(cfg.destination_port), " local=", std::to_string(microphone_sender_->GetBoundPort()),
                 " codec=", cfg.aac_eld ? "aac-eld" : "pcm", " rate=", std::to_string(rate),
                 " ch=", std::to_string(static_cast<int>(channels)),
                 " encrypted=", cfg.encrypted ? 1 : 0);
}

void CarPlayCoreService::ApplyAirPlayPublicKey(Transport::MdnsServiceConfig& cfg) const
{
    // CarPlay requires the accessory Ed25519 public key in the _airplay._tcp TXT
    // record ("pk", hex). Without it the iPhone refuses to open the RTSP control
    // channel even though it resolves the _carplay-ctrl._tcp SRV record.
    if (!accessory_identity_)
        return;
    const auto& identity = accessory_identity_->Identity();
    if (identity.public_key.empty())
        return;
    cfg.customTxtProps["pk"] = Common::ToHexCompact(identity.public_key);
}

void CarPlayCoreService::StartPersistentBroadcasting() {
    // Bring up the Wi-Fi SoftAP (Mobile Hotspot) so the iPhone can always
    // find and join the network, and start the mDNS advertiser so the device
    // is continuously discoverable. This runs for the whole service lifetime,
    // so the user does not have to delete and re-add the device on every
    // reconnect.
    if (wifi_manager_ && !wifi_manager_->IsRunning()) {
        Transport::WifiApCredentials creds{};
        creds.ssid = env_config_.network.wifi_softap_ssid.empty()
            ? (env_config_.identity.device_name.empty() ? "CarPlayStation" : env_config_.identity.device_name)
            : env_config_.network.wifi_softap_ssid;
        creds.passphrase = env_config_.network.wifi_softap_password.empty()
            ? "CarPlayStationSecret123"
            : env_config_.network.wifi_softap_password;
        creds.channel = 0; // 0 = omit channel in iAP2 config; let the iPhone scan for the real channel
        creds.band = Transport::WifiBand::Band5GHz;
        auto ap_st = wifi_manager_->StartAccessPoint(creds);
        if (ap_st.IsError()) {
            Common::Logger::Instance().Log(Common::LogLevel::Warn, "CoreService",
                "Persistent SoftAP start failed: ", ap_st.Message());
        }
    }

    // Build the mDNS config with the real device identity so the iPhone
    // accepts the AirPlay-Receiver-Device-ID (the default AABBCCDDEE01 is
    // rejected, which is why the video stream never started).
    Transport::MdnsServiceConfig mdns_cfg{};
    mdns_cfg.instanceName = env_config_.identity.device_name.empty() ? "CarPlayStation" : env_config_.identity.device_name;
    mdns_cfg.port = env_config_.network.rtsp_port != 0 ? env_config_.network.rtsp_port : 5000;
    mdns_cfg.ipv4Address = "192.168.137.1";
    if (wifi_manager_) {
        auto creds = wifi_manager_->GetCredentials();
        if (!creds.gatewayIp.empty()) {
            mdns_cfg.ipv4Address = creds.gatewayIp;
        }
    }
    if (!device_id_.empty()) {
        mdns_cfg.deviceId = device_id_;
    }
    ApplyAirPlayPublicKey(mdns_cfg);

    if (mdns_advertiser_ && !mdns_advertiser_->IsRunning()) {
        auto mdns_st = mdns_advertiser_->Start(mdns_cfg);
        if (mdns_st.IsError()) {
            Common::Logger::Instance().Log(Common::LogLevel::Warn, "CoreService",
                "Persistent mDNS advertising start failed: ", mdns_st.Message());
        } else {
            Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
                "Persistent broadcasting active (SoftAP + mDNS), deviceId=", mdns_cfg.deviceId);
        }
    } else if (mdns_advertiser_ && mdns_advertiser_->IsRunning()) {
        // Already running (e.g. started by the session). Refresh the config so
        // the real deviceId is used, then re-announce so the iPhone re-discovers.
        mdns_advertiser_->Start(mdns_cfg);
        (void)mdns_advertiser_->BroadcastAnnouncement();
        Common::Logger::Instance().Log(Common::LogLevel::Info, "CoreService",
            "Persistent mDNS config refreshed, deviceId=", mdns_cfg.deviceId);
    }
}

void CarPlayCoreService::TearDownSubsystems() {
    // Stop in reverse dependency order: Input → Media → Session → Protocol → Transport → Hardware.

    // Layer 6: Input
    input_sink_.reset();

    // Layer 5: Media
    if (video_receiver_) {
        video_receiver_->Stop();
    }
    video_receiver_.reset();

    // Audio receivers feed the pipeline, so they stop before it does.
    for (auto& receiver : audio_receivers_) {
        if (receiver) {
            receiver->Stop();
        }
    }
    audio_receivers_.clear();

    // The RTP sender owns the uplink callback, so it stops before the capture.
    if (microphone_sender_) {
        microphone_sender_->Stop();
        microphone_sender_.reset();
    }
    if (microphone_uplink_) {
        microphone_uplink_->Shutdown();
    }
    microphone_uplink_.reset();

    if (audio_pipeline_) {
        audio_pipeline_->Shutdown();
    }
    audio_pipeline_.reset();

    if (video_decoder_) {
        video_decoder_->Shutdown();
    }
    video_decoder_.reset();

    // Layer 4: Session
    if (session_) {
        session_->Stop();
    }
    session_.reset();

    // Stop persistent broadcasting (SoftAP + mDNS advertiser) owned by the service.
    if (mdns_advertiser_ && mdns_advertiser_->IsRunning()) {
        mdns_advertiser_->Stop();
    }
    mdns_advertiser_.reset();
    if (wifi_manager_ && wifi_manager_->IsRunning()) {
        wifi_manager_->StopAccessPoint();
    }
    wifi_manager_.reset();
    rtsp_server_.reset();

    // Layer 3: Protocol
    if (iap2_link_) {
        iap2_link_->SetPacketHandler(nullptr);
        iap2_link_->Reset();
    }
    iap2_link_.reset();
    iap2_handler_.reset();

    // Layer 2: Transport
    if (transport_) {
        if (auto rfcomm = std::dynamic_pointer_cast<Transport::BtRfcommTransport>(transport_)) {
            rfcomm->StopServer();
        }
        transport_->Disconnect();
    }
    transport_.reset();

    // Layer 1: Hardware
    if (mfi_signer_) {
        mfi_signer_->Shutdown();
    }
    mfi_signer_.reset();

    initialized_ = false;
}

} // namespace CarPlayStation::Core::Service
