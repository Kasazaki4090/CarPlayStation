#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include "Core/Media/AirPlayVideoDemuxer.h"
#include "Core/Media/IVideoDecoder.h"
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <array>
#include <mutex>

namespace CarPlayStation::Core::Media {

/**
 * @brief High-performance network video receiver for AirPlay & Wireless CarPlay.
 *
 * Listens on TCP and UDP video data ports (default 7100), receives inbound
 * H.264/HEVC AVCC video frames from the iPhone, extracts NAL units through
 * AirPlayVideoDemuxer, and delivers Annex-B frames to IVideoDecoder for D3D11 rendering.
 */
class AirPlayVideoReceiver {
public:
    explicit AirPlayVideoReceiver(std::shared_ptr<IVideoDecoder> decoder, uint16_t port = 7100);
    ~AirPlayVideoReceiver();

    [[nodiscard]] Common::Status Start();
    void Stop();
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] uint64_t GetReceivedFrames() const noexcept;
    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }

    void SetDecoder(std::shared_ptr<IVideoDecoder> decoder) {
        decoder_ = std::move(decoder);
    }

    void SetDecryptionKey(const std::array<uint8_t, 32>& key) {
        std::lock_guard<std::mutex> lock(client_mutex_);
        decrypt_key_ = key;
        decrypt_key_set_ = true;
        frame_counter_ = 0;
    }

private:
    void TcpListenLoop();
    void TcpClientWorker(uintptr_t clientSock);
    void UdpListenLoop();

    std::shared_ptr<IVideoDecoder> decoder_;
    AirPlayVideoDemuxer demuxer_;
    uint16_t port_{7100};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> received_frames_{0};
    std::atomic<bool> first_frame_logged_{false};
    std::atomic<bool> decoder_initialized_{false};

    std::array<uint8_t, 32> decrypt_key_{0};
    std::atomic<bool> decrypt_key_set_{false};
    std::atomic<uint64_t> frame_counter_{0};

    uintptr_t tcp_listen_sock_{~0ULL};
    uintptr_t udp_sock_{~0ULL};

    std::thread tcp_thread_;
    std::thread udp_thread_;
    std::vector<std::thread> client_threads_;
    std::mutex client_mutex_;
};

} // namespace CarPlayStation::Core::Media
