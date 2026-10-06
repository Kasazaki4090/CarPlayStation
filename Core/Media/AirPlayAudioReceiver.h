#ifndef CARPLAY_CORE_MEDIA_AIRPLAYAUDIORECEIVER_H
#define CARPLAY_CORE_MEDIA_AIRPLAYAUDIORECEIVER_H

#include "Core/Common/Result.h"
#include "Core/Media/AacCodecAdapter.h"
#include "Core/Media/AirPlayAudioDemuxer.h"
#include "Core/Media/IAudioPipeline.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace CarPlayStation::Core::Media {

/**
 * @brief Audio downlink receiver for CarPlay / AirPlay 2.
 *
 * Listens on the UDP data port advertised during RTSP SETUP phase 2, decrypts
 * each RTP packet (AirPlayAudioDemuxer), decodes the AAC AudioUnit
 * (AacAudioDecoder), then converts the result to the PCM container the audio
 * pipeline expects and injects it into one AudioLane.
 *
 * One receiver instance is created per negotiated audio lane (main media,
 * alternate navigation, Siri/telephony), because each lane has its own port,
 * its own stream keys and its own codec parameters.
 */
class AirPlayAudioReceiver {
public:
    struct Config {
        AudioLane lane{AudioLane::MainMedia};
        uint16_t udp_port{7000};
        // Negotiated stream parameters. When the codec is Unknown the receiver
        // still opens its socket but only counts packets.
        AudioStreamFormat stream{};
    };

    // PCM container the pipeline expects on this lane. Defaults to the service
    // default (48 kHz stereo S16).
    struct OutputFormat {
        uint32_t sample_rate{48000};
        uint8_t channels{2};
    };

    AirPlayAudioReceiver(std::shared_ptr<IAudioPipeline> pipeline, Config cfg);
    ~AirPlayAudioReceiver();

    [[nodiscard]] Common::Status Start();
    void Stop();
    [[nodiscard]] bool IsRunning() const noexcept { return m_running.load(); }

    [[nodiscard]] uint16_t GetPort() const noexcept { return m_cfg.udp_port; }
    [[nodiscard]] AudioLane GetLane() const noexcept { return m_cfg.lane; }
    [[nodiscard]] uint64_t GetReceivedPackets() const noexcept {
        return m_demux.GetStats().packetsEmitted;
    }

    // The pairing-derived *output* key for this stream's streamConnectionID.
    void SetDecryptionKey(const std::array<uint8_t, 32>& key);

    // Re-negotiate codec parameters at runtime (opens the decoder).
    [[nodiscard]] Common::Status SetStreamFormat(const AudioStreamFormat& format);

    // PCM container to produce. Must match the format the pipeline was started
    // with; call before Start().
    void SetOutputFormat(const OutputFormat& format);

private:
    void UdpListenLoop();
    void HandlePacket(const AirPlayAudioDemuxer::AudioPacket& pkt);
    void PushPcm(const int16_t* pcm, size_t frames, uint8_t srcChannels, uint32_t srcRate);
    // Periodic dataplane stats (UDP thread only) so a silent lane is visible in the log.
    void LogStats();

    std::shared_ptr<IAudioPipeline> m_pipeline;
    Config m_cfg;
    OutputFormat m_out{};

    AirPlayAudioDemuxer m_demux;
    std::unique_ptr<AacAudioDecoder> m_decoder;
    std::mutex m_codecMutex;

    uintptr_t m_udpSock{~0ULL};
    std::atomic<bool> m_running{false};
    std::thread m_udpThread;

    // Scratch buffers reused across packets (guarded by the UDP thread only).
    std::vector<float> m_resampleIn;
    std::vector<int16_t> m_convertBuf;
    // One-shot diagnostic: dumps the first access unit so the wire framing can be checked
    // against the decoder's expectations (UDP thread only).
    bool m_au_dumped{false};
    // Dataplane counters for the periodic stats line (UDP thread only).
    uint64_t m_decoded_ok{0};
    uint64_t m_decode_failed{0};
    uint64_t m_pcm_frames{0};
    uint64_t m_last_stats_packets{0};
    uint64_t m_stats_clock_ms{0};
};

}  // namespace CarPlayStation::Core::Media

#endif  // CARPLAY_CORE_MEDIA_AIRPLAYAUDIORECEIVER_H
