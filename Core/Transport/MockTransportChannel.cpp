#include "Core/Transport/MockTransportChannel.h"

namespace CarPlayStation::Core::Transport {

MockTransportChannel::MockTransportChannel(TransportType type, TransportState initialState)
    : m_type(type), m_state(initialState) {}

MockTransportChannel::~MockTransportChannel() {
    Disconnect();
}

Common::Status MockTransportChannel::Send(Common::ByteSpan data) {
    std::shared_ptr<MockTransportChannel> peer;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_failSends) {
            m_stats.errors_count++;
            return m_injectedError.IsError()
                ? m_injectedError
                : Common::Status::Error(Common::StatusCode::IoError, "Injected mock send failure");
        }

        if (m_state != TransportState::Connected) {
            m_stats.errors_count++;
            return Common::Status::Error(Common::StatusCode::InvalidState, "Transport is not connected");
        }

        if (m_mtu > 0 && data.size() > m_mtu) {
            m_stats.errors_count++;
            return Common::Status::Error(Common::StatusCode::InvalidArgument, "Packet exceeds transport MTU");
        }

        m_sentPackets.emplace_back(data.begin(), data.end());
        m_stats.bytes_sent += data.size();
        m_stats.packets_sent++;

        if (m_dropSends) {
            return Common::Status::Ok();
        }

        peer = m_peer.lock();
    }

    if (peer) {
        peer->ReceiveFromPeer(data);
    }

    return Common::Status::Ok();
}

Common::Status MockTransportChannel::AsyncSend(Common::ByteBuffer data) {
    return Send(Common::ByteSpan(data.data(), data.size()));
}

void MockTransportChannel::SetDataCallback(DataReceivedCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dataCallback = std::move(callback);
}

void MockTransportChannel::SetStateCallback(StateChangedCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stateCallback = std::move(callback);
}

void MockTransportChannel::Disconnect() {
    StateChangedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state == TransportState::Disconnected) {
            return;
        }
        m_state = TransportState::Disconnected;
        cb = m_stateCallback;
    }

    if (cb) {
        cb(TransportState::Disconnected, Common::Status::Ok());
    }
}

TransportState MockTransportChannel::GetState() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

TransportType MockTransportChannel::GetType() const noexcept {
    return m_type;
}

size_t MockTransportChannel::GetMtu() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mtu;
}

std::string MockTransportChannel::GetRemoteEndpoint() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_remoteEndpoint;
}

TransportStats MockTransportChannel::GetStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void MockTransportChannel::Pair(const std::shared_ptr<MockTransportChannel>& a,
                                const std::shared_ptr<MockTransportChannel>& b) {
    if (!a || !b) return;
    a->SetPeer(b);
    b->SetPeer(a);
}

void MockTransportChannel::SetPeer(const std::shared_ptr<MockTransportChannel>& peer) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_peer = peer;
}

void MockTransportChannel::ClearPeer() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_peer.reset();
}

void MockTransportChannel::SimulateInboundData(Common::ByteSpan data) {
    DataReceivedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.bytes_received += data.size();
        m_stats.packets_received++;
        cb = m_dataCallback;
    }

    if (cb) {
        cb(data);
    }
}

void MockTransportChannel::ReceiveFromPeer(Common::ByteSpan data) {
    DataReceivedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != TransportState::Connected) {
            return;
        }
        m_stats.bytes_received += data.size();
        m_stats.packets_received++;
        cb = m_dataCallback;
    }

    if (cb) {
        cb(data);
    }
}

void MockTransportChannel::SimulateDisconnect(Common::Status reason) {
    StateChangedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = TransportState::Disconnected;
        cb = m_stateCallback;
    }

    if (cb) {
        cb(TransportState::Disconnected, reason);
    }
}

void MockTransportChannel::SimulateError(Common::Status error) {
    StateChangedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = TransportState::Error;
        m_stats.errors_count++;
        cb = m_stateCallback;
    }

    if (cb) {
        cb(TransportState::Error, error);
    }
}

void MockTransportChannel::SimulateConnect(std::string remoteEndpoint) {
    StateChangedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = TransportState::Connected;
        if (!remoteEndpoint.empty()) {
            m_remoteEndpoint = std::move(remoteEndpoint);
        }
        cb = m_stateCallback;
    }

    if (cb) {
        cb(TransportState::Connected, Common::Status::Ok());
    }
}

void MockTransportChannel::SetFailSends(bool fail, Common::Status err) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failSends = fail;
    m_injectedError = err;
}

void MockTransportChannel::SetDropSends(bool drop) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dropSends = drop;
}

void MockTransportChannel::SetMtu(size_t mtu) noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mtu = mtu;
}

void MockTransportChannel::SetRemoteEndpoint(std::string endpoint) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_remoteEndpoint = std::move(endpoint);
}

std::vector<Common::ByteBuffer> MockTransportChannel::GetSentPackets() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sentPackets;
}

std::optional<Common::ByteBuffer> MockTransportChannel::GetLastSentPacket() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sentPackets.empty()) {
        return std::nullopt;
    }
    return m_sentPackets.back();
}

size_t MockTransportChannel::GetSentPacketCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sentPackets.size();
}

void MockTransportChannel::ClearSentPackets() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sentPackets.clear();
}

void MockTransportChannel::ResetStats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats = {};
}

} // namespace CarPlayStation::Core::Transport
