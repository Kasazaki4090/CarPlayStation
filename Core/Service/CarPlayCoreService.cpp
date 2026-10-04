#include "Core/Service/CarPlayCoreService.h"
#include "Core/Hardware/FileMfiSigner.h"
#include "Core/Transport/BtRfcommTransport.h"
#include "Core/Transport/BtSdpHelper.h"
#include "Core/Transport/WifiSoftApManager.h"
#include "Core/Transport/MdnsAdvertiser.h"
#include "Core/Hardware/RadioAdapterHelper.h"
#include "Core/Protocol/Iap2LinkEngine.h"
#include "Core/Protocol/Iap2MessageCatalog.h"
#include "Core/Session/CarPlaySessionStateMachine.h"
#include "Core/Session/PairingStore.h"
#include "Core/Session/AirPlayRtspServer.h"
#include "Core/Session/Pairing/Ed25519.h"
#include "Core/Input/IInputEventSink.h"
#include "Core/Media/MpvVideoPlayer.h"
#include "Core/Media/WasapiAudioPipeline.h"
#include "Core/Media/WasapiMicrophoneUplink.h"
#include "Core/Media/AirPlayVideoReceiver.h"
#include "Core/Common/Logging.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <cstdio>

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

class AirPlayInputSink final : public Input::IInputEventSink {
public:
    explicit AirPlayInputSink(std::weak_ptr<Session::AirPlayRtspServer> rtsp_server)
        : rtsp_server_(std::move(rtsp_server)) {}

    [[nodiscard]] Common::Status SendTouchEvent(
        Input::TouchAction action,
        const Input::TouchPoint& point) override {
        auto server = rtsp_server_.lock();
        if (!server) {
            return Common::MakeError(Common::StatusCode::Unavailable,
                "AirPlay RTSP server is unavailable");
        }

        const auto params = server->GetNegotiatedParams();
        const uint16_t width = static_cast<uint16_t>(
            std::clamp(params.video_width, 1u, static_cast<uint32_t>(UINT16_MAX)));
        const uint16_t height = static_cast<uint16_t>(
            std::clamp(params.video_height, 1u, static_cast<uint32_t>(UINT16_MAX)));
        const bool contact = action == Input::TouchAction::Down ||
                             action == Input::TouchAction::Move;
        const uint16_t x = static_cast<uint16_t>(
            std::clamp(point.x_normalized, 0.0f, 1.0f) * width);
        const uint16_t y = static_cast<uint16_t>(
            std::clamp(point.y_normalized, 0.0f, 1.0f) * height);
        const std::array<uint8_t, 5> report{
            static_cast<uint8_t>(contact ? 1 : 0),
            static_cast<uint8_t>(x & 0xFF),
            static_cast<uint8_t>((x >> 8) & 0xFF),
            static_cast<uint8_t>(y & 0xFF),
            static_cast<uint8_t>((y >> 8) & 0xFF)
        };
        auto status = server->SendHidReport(1, report);
        if (status.IsOk() && action != Input::TouchAction::Move) {
            CPS_LOG_INFO("InputSink", "Sent HID touch ",
                action == Input::TouchAction::Down ? "down" :
                action == Input::TouchAction::Up ? "up" : "cancel",
                " at ", x, ",", y);
        }
        return status;
    }

    [[nodiscard]] Common::Status SendKnobEvent(Input::KnobAction, uint32_t) override {
        return Common::MakeError(Common::StatusCode::NotSupported,
            "This receiver does not advertise a CarPlay knob HID device");
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
    std::weak_ptr<Session::AirPlayRtspServer> rtsp_server_;
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
                    HandleRequestAccessoryWiFi();
                    break;
                case Protocol::Iap2MsgId::RequestAccessoryWiFi: // 0x5702
                    CPS_LOG_INFO("CoreService", "iAP2 RequestAccessoryWiFi (0x5702) received from iPhone — sending WiFi configuration");
                    HandleRequestAccessoryWiFi();
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
                default:
                    break;
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
            device_name_, bt_mac_, /*declareWired=*/false);
        auto status = send_fn_(Protocol::Iap2MsgId::IdentifyInformation,
                               Common::ByteSpan(identBody.data(), identBody.size()));
        if (status.IsOk()) {
            CPS_LOG_INFO("CoreService", "iAP2 IdentifyInformation (0x1D01) sent to iPhone (",
                         std::to_string(identBody.size()), " bytes)");
        } else {
            CPS_LOG_WARN("CoreService", "Failed to send iAP2 IdentifyInformation: ", status.Message());
        }
    }

    void HandleRequestAccessoryWiFi() {
        std::string ssid = fallback_ssid_;
        std::string pass = fallback_passphrase_;
        // Default to 0 (omit channel) so the iPhone scans for the SSID on its
        // real channel. Only override with a concrete channel if we actually
        // know one (creds.channel != 0).
        uint8_t channel = 0;

        if (wifi_mgr_) {
            auto creds = wifi_mgr_->GetCredentials();
            if (!creds.ssid.empty()) {
                ssid = creds.ssid;
            }
            if (!creds.passphrase.empty()) {
                pass = creds.passphrase;
            }
            if (creds.channel != 0) {
                channel = static_cast<uint8_t>(creds.channel);
            }
        }

        CPS_LOG_INFO("CoreService", "Sending iAP2 AccessoryWiFiConfig (0x5703) to iPhone: SSID='", ssid, "', channel=", std::to_string(channel));
        auto body = Protocol::Iap2MessageCatalog::BuildAccessoryWiFiConfig(ssid, pass, 2, channel);
        if (send_fn_) {
            auto status = send_fn_(Protocol::Iap2MsgId::AccessoryWiFiConfig,
                                   Common::ByteSpan(body.data(), body.size()));
            if (status.IsOk()) {
                CPS_LOG_INFO("CoreService", "iAP2 AccessoryWiFiConfig (0x5703) sent successfully (",
                             std::to_string(body.size()), " bytes)");
            } else {
                CPS_LOG_WARN("CoreService", "Failed to send iAP2 AccessoryWiFiConfig: ", status.Message());
            }
        }
    }

    std::shared_ptr<Session::ICarPlaySession> session_;
    std::shared_ptr<Hardware::IMfiSigner> mfi_signer_;
    std::shared_ptr<Transport::IWifiSoftApManager> wifi_mgr_;
    SendControlMessage send_fn_;
    std::string device_name_{"CarPlayStation"};
    Common::MacAddress bt_mac_{0x00, 0x1A, 0x7D, 0xDA, 0x71, 0x13};
    std::string fallback_ssid_{"CarPlayStation"};
    std::string fallback_passphrase_{"CarPlayStationSecret123"};
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
        rtsp_server_ = std::make_shared<Session::AirPlayRtspServer>(rtsp_opts);

        // Persistent mDNS advertiser: owned by the service so it can keep
        // broadcasting for the whole service lifetime (continuous discovery),
        // not just during an active session.
        mdns_advertiser_ = std::make_shared<Transport::MdnsAdvertiser>();

        Session::SessionDependencies session_deps{};
        pairing_store_ = std::make_shared<Session::PairingStore>("carplay_pairings.json");
        session_deps.pairing_store = pairing_store_;

        // Enable the RTSP pair-setup handshake (POST /pair-setup). The accessory's
        // Ed25519 long-term keypair is generated on the fly; each newly-paired
        // controller is recorded in the pairing store.
        {
            auto kp = Session::Pairing::ed25519::GenerateKeyPair();
            Session::Pairing::AccessoryIdentity identity;
            identity.id = std::vector<uint8_t>(local_bt_mac_str.begin(), local_bt_mac_str.end());
            identity.ltpk = kp.pubKey;
            identity.ltpk_secret = kp.secret;
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
                Common::VideoStreamSpec spec;
                spec.width = 1280;
                spec.height = 720;
                spec.fps = 60;
                spec.codec = Common::VideoCodec::H264;
                auto st = video_decoder_->Init(spec);
                CPS_LOG_INFO("CoreService", "Video decoder initialized: ", st.IsOk() ? "OK" : st.Message());
            }
            if (video_receiver_ && params.video_encrypted) {
                video_receiver_->SetDecryptionKey(params.video_decrypt_key);
            }
            if (auto session_sm = std::dynamic_pointer_cast<Session::CarPlaySessionStateMachine>(session_)) {
                (void)session_sm->OnAirPlayStreamingStarted(params);
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
    iap2_handler_ = std::make_shared<CoreIap2PacketHandler>(
        session_, mfi_signer_, wifi_manager_, send_control_message,
        env_config_.identity.device_name,
        local_bt_mac,
        env_config_.network.wifi_softap_ssid,
        env_config_.network.wifi_softap_password);
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

Common::Status CarPlayCoreService::UpdateEnvironment(const Common::CoreEnvironmentConfig& env) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    if (!initialized_) {
        return Common::MakeError(Common::StatusCode::InvalidState, "Not initialized");
    }

    env_config_ = env;

    // Hot-update the MFi signer endpoint if it's a NetworkMfiClient.
    // (The Application layer is responsible for constructing the correct
    //  signer type; the facade just stores the config.)

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
        Common::AudioOutputFormat default_audio_format{};
        default_audio_format.sample_rate = 48000;
        default_audio_format.channels = 2;
        default_audio_format.bit_depth = 16;
        default_audio_format.sample_type = Common::AudioSampleType::PcmInt16;
        auto ap_st = audio_pipeline_->Start(default_audio_format);
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
