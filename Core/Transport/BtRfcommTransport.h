#pragma once

#include "Core/Transport/ITransportChannel.h"
#include "Core/Transport/BtSdpHelper.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace CarPlayStation::Core::Transport {

/**
 * @brief Windows WinSock AF_BTH implementation of Bluetooth Classic RFCOMM
 * transport with SDP service publishing for Wireless CarPlay.
 */
class BtRfcommTransport : public ITransportChannel {
public:
    explicit BtRfcommTransport(uint8_t defaultChannel = 1);
    ~BtRfcommTransport() override;

    // --- ITransportChannel Implementation ---
    [[nodiscard]] Common::Status Send(Common::ByteSpan data) override;
    [[nodiscard]] Common::Status AsyncSend(Common::ByteBuffer data) override;
    void SetDataCallback(DataReceivedCallback callback) override;
    void SetStateCallback(StateChangedCallback callback) override;
    void Disconnect() override;
    [[nodiscard]] TransportState GetState() const noexcept override;
    [[nodiscard]] TransportType GetType() const noexcept override;
    [[nodiscard]] size_t GetMtu() const noexcept override;
    [[nodiscard]] std::string GetRemoteEndpoint() const override;
    [[nodiscard]] TransportStats GetStats() const override;

    // --- Bluetooth Server Lifecycle ---
    [[nodiscard]] Common::Status StartServer(uint8_t channel = 1);
    void StopServer();
    [[nodiscard]] bool IsListening() const noexcept;
    [[nodiscard]] uint8_t GetChannel() const noexcept;

    // --- Simulation & Test Mode (No Hardware Required) ---
    void EnableSimulatedMode(bool enable);
    [[nodiscard]] bool IsSimulatedMode() const noexcept;
    void SimulatePeerConnect(std::string_view peerAddress = "DC:A9:04:12:34:56");
    void SimulatePeerData(Common::ByteSpan data);
    void SimulatePeerDisconnect();

private:
    void ServerWorkerLoop();
    void HandleClientIo(SOCKET clientSock, const std::string& remoteAddr);

    mutable std::mutex m_mutex;
    std::atomic<TransportState> m_state{TransportState::Disconnected};
    std::atomic<bool> m_isListening{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_simulatedMode{false};

    uint8_t m_channel{1};
    SOCKET m_serverSocket{INVALID_SOCKET};
    SOCKET m_clientSocket{INVALID_SOCKET};
    HANDLE m_sdpHandle{nullptr};

    std::string m_remoteEndpoint{};
    TransportStats m_stats{};

    DataReceivedCallback m_dataCallback;
    StateChangedCallback m_stateCallback;

    std::thread m_workerThread;
};

} // namespace CarPlayStation::Core::Transport
