#pragma once

#include "Core/Session/AirPlayRtspTypes.h"
#include "Core/Session/AirPlayRtspCodec.h"
#include "Core/Session/Pairing/PairSetup.h"
#include "Core/Session/Pairing/PairVerify.h"
#include "Core/Common/Result.h"
#include "Core/Common/Config.h"
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <map>

namespace CarPlayStation::Core::Hardware {
class IMfiSigner;
}

namespace CarPlayStation::Core::Session {

/**
 * @brief Callback signature for handling inbound RTSP requests and generating responses.
 */
using RtspRequestHandler = std::function<RtspResponse(const RtspRequest& req)>;
using StreamingStartedCallback = std::function<void(const NegotiatedMediaParams&)>;

/**
 * @brief High-performance asynchronous RTSP control channel server for AirPlay & CarPlay.
 *
 * Listens on a TCP port (default 5000 or dynamic 0 for testing), parses incoming RTSP requests,
 * negotiates streaming parameters, and delivers responses to the connecting iPhone.
 */
class AirPlayRtspServer {
public:
    struct Options {
        uint16_t port{5000};                           // 0 = ephemeral dynamic port
        std::string bind_address{"0.0.0.0"};
        uint32_t default_video_width{1280};
        uint32_t default_video_height{720};
        uint32_t default_fps{60};
        uint32_t default_audio_sample_rate{48000}; // 44100 or 48000
        // The `features` bitmap advertised in /info. Defaults to the mask xcertplay
        // (Reference/diplay) advertises: the one verified on hardware to make an iPhone
        // hand audio over to a receiver.
        uint64_t info_features{0x615653AEE2ULL};
        // Reply to the phase-1 SETUP with `enabledFeatures`: the list of optional features
        // the accessory is switching on. Both carplayd and xcertplay send it; the iPhone
        // uses it to seed the negotiated feature set for the rest of the session.
        bool phase1_enabled_features{true};
        // Advertise `viewAreas`/`initialViewArea` on the display entry (and claim the matching
        // `viewAreas` feature in the phase-1 SETUP). The two are always coupled: xcertplay's
        // comment is "the session SETUP response enables viewAreas, so /info must always describe
        // one". xcertplay sends it unconditionally and is the receiver verified working on this
        // iPhone, so it is on by default; the C reference drops it on iOS 27, hence the switch.
        bool display_view_areas{true};
        // Advertise `bluetoothIDs`/`rightHandDrive`. xcertplay always sends both; iOS uses the
        // list to hand the car's Bluetooth radio over for calls, and the audio-routing decision
        // appears to depend on that being known.
        bool advertise_bluetooth{true};
        // Offer HEVC to the iPhone. Off by default: the phone encodes H.264 unless the
        // receiver claims HEVC, and the codec the phone then uses is what the video
        // receiver must decode.
        //
        // Both halves are required together (the two working references do exactly this):
        //   * `hevcInfo = {}` in /info — a presence-only empty dict.
        //   * `"hevc"` in the phase-1 SETUP response's `enabledFeatures`.
        // The Rust reference's notes (Reference/carplayd/docs/05_HEVC_AND_MODULAR.md) are
        // explicit that `hevcInfo` alone only works when iOS's negotiated feature list also
        // carries `hevc`, that `"hevc"` in `extendedFeatures` is a category error, that
        // `enabledFeatures` is not a valid /info key, and that the base `features` u64 has
        // no HEVC bit at all. Wireless is the proven transport, which is the one we use.
        bool advertise_hevc{false};
        // Which `audioFormats`/`audioLatencies` shape the advert carries.
        //
        // true (default) = the C reference's set: eight entries, all AAC, no
        // PCM, where every entry either has no `audioType` key at all or names
        // one of compatibility/alert/default/telephony/speechRecognition/media —
        // plus a single catch-all `audioLatencies` entry with no
        // `type`/`audioType` keys. Proven on hardware over the same transport we
        // use: every session in that reference's trace gets a
        // streamType=100/101/102 SETUP.
        // false = xcertplay's nine PCM-first entries plus nine typed latency
        // entries.
        //
        // Why this is the switch to reach for when iOS borrows MainAudio but
        // never SETs UP the audio stream: the Rust reference records exactly that
        // failure and pins it on an `audioFormats` advert iOS cannot map the
        // stream it wants onto ("iOS saw the advert, never borrowed MainAudio ->
        // no audio"). Neither working reference advertises PCM for the media
        // lane, and neither splits the MainAudio lane across per-`audioType`
        // entries the way the xcertplay set does.
        bool audio_advert_c_reference{true};
        // How many contacts the uuid 1 touchscreen HID descriptor declares
        // (default 1, may be set to 2).
        //
        // Set by CarPlayCoreService::Initialize() from
        // Hardware::DetectTouchscreenCapabilities(): only a machine with a
        // display digitizer can produce a second contact, so this is read from
        // the hardware rather than offered as a user setting.
        //
        // Why this defaults to 1: the `hidDevices` list is the most fragile part
        // of the /info advert. The Rust reference records an incident in
        // info.rs (2026-07-06) where adding a third HID device — a knob —
        // stopped both the CarPlay session and the separate iAP2 session from
        // ever reconnecting, unrecoverable even by unplugging USB, and the only
        // change left standing was that extra entry. So multi-touch here never
        // adds a device; it rewrites the descriptor of the uuid 1 device that
        // is already advertised, and the default keeps today's byte-for-byte
        // single-contact advert untouched.
        //
        // 2 swaps in the two-Finger-collection descriptor (109 bytes) and makes
        // every hidSendReport 12 bytes instead of 5 (see AirPlayInputSink in
        // CarPlayCoreService.cpp). Both reference implementations ship the
        // two-contact shape, which is why iOS is expected to accept it.
        int hid_touch_contacts{1};
    };

    explicit AirPlayRtspServer(Options options = {});
    ~AirPlayRtspServer();

    /**
     * @brief Start listening on the configured TCP port.
     */
    [[nodiscard]] Common::Status Start();

    /**
     * @brief Stop the server and terminate active client connections.
     */
    void Stop();

    /**
     * @brief Check whether the listener is active.
     */
    [[nodiscard]] bool IsRunning() const noexcept;

    /**
     * @brief Get the actual bound TCP port (useful when configured with port=0 for tests).
     */
    [[nodiscard]] uint16_t GetBoundPort() const noexcept;

    /**
     * @brief Count of currently connected iPhone clients.
     */
    [[nodiscard]] size_t GetActiveClientCount() const noexcept;

    /**
     * @brief Override or intercept standard request handling with a custom handler.
     */
    void SetRequestHandler(RtspRequestHandler handler);

    /**
     * @brief Register a callback invoked when RTSP streaming starts (RECORD request).
     */
    void SetStreamingStartedCallback(StreamingStartedCallback callback);

    /**
     * @brief Register a callback invoked on duckAudio / unduckAudio commands.
     */
    using DuckAudioCallback = std::function<void(bool ducked)>;
    void SetDuckAudioCallback(DuckAudioCallback callback);

    /**
     * @brief Register a callback invoked when a specific stream is torn down.
     */
    using StreamTeardownCallback = std::function<void(uint32_t stream_type)>;
    void SetStreamTeardownCallback(StreamTeardownCallback callback);

    /**
     * @brief Enable the RTSP pair-setup handshake (POST /pair-setup) with the given
     *        accessory identity and a callback that records each newly-paired controller.
     */
    void SetPairSetupIdentity(Pairing::AccessoryIdentity identity, Pairing::PeerSaver saver);

    /**
     * @brief Enable the RTSP pair-verify handshake (POST /pair-verify) with the given
     *        accessory identity and a callback that looks up paired controllers.
     */
    void SetPairVerify(Pairing::AccessoryIdentity identity, Pairing::PeerFinder finder);

    /**
     * @brief Configure the MFi authentication signer for /auth-setup (MFi-SAP handshake).
     */
    void SetMfiSigner(std::shared_ptr<Hardware::IMfiSigner> signer);

    /**
     * @brief Query currently negotiated streaming parameters.
     */
    [[nodiscard]] NegotiatedMediaParams GetNegotiatedParams() const noexcept;

    /**
     * @brief Manually update or preset negotiated parameters.
     */
    void SetNegotiatedParams(NegotiatedMediaParams params);

    /**
     * @brief Set the display geometry advertised to the iPhone.
     *
     * The /info `displays` dictionary (widthPixels/heightPixels/widthPhysical/
     * heightPhysical/viewAreas) and the HID touchscreen scaling both come from
     * these values, so the phone lays out and encodes the screen at this size.
     * Call before the session starts; a phone that already fetched /info keeps
     * its current layout until the next session.
     */
    void SetDisplayGeometry(uint32_t width, uint32_t height, uint32_t fps);

    /**
     * @brief Turn the HEVC capability advertisement on or off.
     *
     * HEVC is not a bit in the base `features` mask: the receiver has to say so
     * twice, by putting an empty `hevcInfo` dictionary in the /info response and
     * by adding the literal string "hevc" to the `enabledFeatures` array of the
     * SETUP phase 1 response. The iPhone only encodes H.265 when it sees both.
     * Call before the session starts; a phone that already fetched /info keeps
     * H.264 until the next session.
     */
    void SetVideoCodec(Common::VideoCodec codec);

    /**
     * @brief Configure the advertised audio sample rate (44100 or 48000 Hz).
     *
     * Informs the iPhone of the desired sample rate for the media lane to minimize SRC.
     */
    void SetAudioSampleRate(uint32_t sample_rate);

    /**
     * @brief Send a HID input report to the active AirPlay event channel.
     */
    [[nodiscard]] Common::Status SendHidReport(uint32_t deviceUid, Common::ByteSpan report);

    /**
     * @brief Tell the phone which appearance the host is showing (setNightMode).
     *
     * `night_mode = true` means dark/night. The phone re-colors its CarPlay UI to
     * match, which is what lets a desktop host keep the phone and its own window in
     * the same theme. The value is also remembered and re-sent whenever a new event
     * channel comes up, so a session started after the theme change still gets it.
     */
    [[nodiscard]] Common::Status SetNightMode(bool night_mode);

    enum class SiriAction : uint8_t {
        Prewarm = 1,
        ButtonDown = 2,
        ButtonUp = 3,
    };

    /**
     * @brief Trigger or signal Siri button action over the encrypted event channel (requestSiri).
     */
    [[nodiscard]] Common::Status SendRequestSiri(SiriAction action);

    /**
     * @brief Contacts declared by the advertised uuid 1 touchscreen descriptor.
     *
     * The report length has to match the descriptor, so whoever builds a touch report
     * needs this. It is the advert value, not a negotiated one: iOS never gets to
     * change it.
     */
    int hid_touch_contacts() const noexcept;

    // --- Simulation & In-Memory Testing ---

    /**
     * @brief Process an RTSP request in-memory without network sockets.
     */
    [[nodiscard]] RtspResponse ProcessSimulatedRequest(const RtspRequest& req);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace CarPlayStation::Core::Session
