#pragma once

#include "Core/Transport/ITransportChannel.h"
#include <mutex>
#include <vector>
#include <memory>
#include <optional>
#include <string>

namespace CarPlayStation::Core::Transport {

/**
 * @brief In-memory mock transport channel supporting loopback pairing,
 * packet inspection, deterministic timing, and fault injection.
 */
class MockTransportChannel : public ITransportChannel, public std::enable_shared_from_this<MockTransportChannel> {
public:
    explicit MockTransportChannel(TransportType type = TransportType::MockLoopback,
                                 TransportState initialState = TransportState::Connected);
    ~MockTransportChannel() override;

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

    // --- Pairing & Loopback Helpers ---
    /**
     * @brief Connects two mock channels into a bidirectional loopback link.
     */
    static void Pair(const std::shared_ptr<MockTransportChannel>& a,
                     const std::shared_ptr<MockTransportChannel>& b);

    void SetPeer(const std::shared_ptr<MockTransportChannel>& peer);
    void ClearPeer();

    // --- Simulation & Testing Controls ---
    void SimulateInboundData(Common::ByteSpan data);
    void SimulateDisconnect(Common::Status reason = Common::Status::Ok());
    void SimulateError(Common::Status error);
    void SimulateConnect(std::string remoteEndpoint = "mock://127.0.0.1:paired");

    void SetFailSends(bool fail, Common::Status err = Common::Status::Error(Common::StatusCode::IoError, "Injected send failure"));
    void SetDropSends(bool drop);
    void SetMtu(size_t mtu) noexcept;
    void SetRemoteEndpoint(std::string endpoint);

    // --- Inspection ---
    [[nodiscard]] std::vector<Common::ByteBuffer> GetSentPackets() const;
    [[nodiscard]] std::optional<Common::ByteBuffer> GetLastSentPacket() const;
    [[nodiscard]] size_t GetSentPacketCount() const;
    void ClearSentPackets();
    void ResetStats();

private:
    void ReceiveFromPeer(Common::ByteSpan data);

    mutable std::mutex m_mutex;
    TransportType m_type{TransportType::MockLoopback};
    TransportState m_state{TransportState::Connected};
    size_t m_mtu{0};
    std::string m_remoteEndpoint{"mock://127.0.0.1"};
    TransportStats m_stats{};

    DataReceivedCallback m_dataCallback;
    StateChangedCallback m_stateCallback;

    std::vector<Common::ByteBuffer> m_sentPackets;
    std::weak_ptr<MockTransportChannel> m_peer;

    bool m_failSends{false};
    Common::Status m_injectedError{Common::Status::Ok()};
    bool m_dropSends{false};
};

} // namespace CarPlayStation::Core::Transport
