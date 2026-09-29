#pragma once

#include "Core/Transport/IMdnsAdvertiser.h"
#include "Core/Transport/MdnsPacketBuilder.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <optional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace CarPlayStation::Core::Transport {

/**
 * @brief Self-contained native mDNS (Multicast DNS) service advertiser for
 * publishing _airplay._tcp and Wireless CarPlay discovery on the local subnet.
 */
class MdnsAdvertiser : public IMdnsAdvertiser {
public:
    MdnsAdvertiser();
    ~MdnsAdvertiser() override;

    // --- IMdnsAdvertiser Implementation ---
    [[nodiscard]] Common::Status Start(const MdnsServiceConfig& config) override;
    void Stop() override;
    [[nodiscard]] bool IsRunning() const noexcept override;
    [[nodiscard]] MdnsServiceConfig GetConfig() const override;
    [[nodiscard]] Common::Status BroadcastAnnouncement() override;

    // --- Test & Simulation Controls ---
    void EnableSimulatedMode(bool enable);
    [[nodiscard]] bool IsSimulatedMode() const noexcept;
    [[nodiscard]] size_t GetBroadcastCount() const;
    [[nodiscard]] std::optional<Common::ByteBuffer> GetLastBroadcastPacket() const;

private:
    void BroadcastWorkerLoop();
    Common::Status SendRawMulticast(Common::ByteSpan packet);

    mutable std::mutex m_mutex;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_simulatedMode{false};

    MdnsServiceConfig m_config{};
    size_t m_broadcastCount{0};
    Common::ByteBuffer m_lastPacket{};

#ifdef _WIN32
    SOCKET m_socket{INVALID_SOCKET};
#endif

    std::thread m_workerThread;
};

} // namespace CarPlayStation::Core::Transport
