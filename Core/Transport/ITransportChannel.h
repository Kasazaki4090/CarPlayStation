#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <functional>
#include <string_view>
#include <string>

namespace CarPlayStation::Core::Transport {

enum class TransportType {
    BluetoothRfcomm,
    WifiTcp,
    UsbNcm,
    MockLoopback
};

[[nodiscard]] constexpr std::string_view TransportTypeToString(TransportType type) noexcept {
    switch (type) {
        case TransportType::BluetoothRfcomm: return "BluetoothRfcomm";
        case TransportType::WifiTcp: return "WifiTcp";
        case TransportType::UsbNcm: return "UsbNcm";
        case TransportType::MockLoopback: return "MockLoopback";
    }
    return "Unknown";
}

enum class TransportState {
    Disconnected,
    Connecting,
    Connected,
    Disconnecting,
    Error
};

[[nodiscard]] constexpr std::string_view TransportStateToString(TransportState state) noexcept {
    switch (state) {
        case TransportState::Disconnected: return "Disconnected";
        case TransportState::Connecting: return "Connecting";
        case TransportState::Connected: return "Connected";
        case TransportState::Disconnecting: return "Disconnecting";
        case TransportState::Error: return "Error";
    }
    return "Unknown";
}

struct TransportStats {
    uint64_t bytes_sent{0};
    uint64_t bytes_received{0};
    uint64_t packets_sent{0};
    uint64_t packets_received{0};
    uint64_t errors_count{0};
};

using DataReceivedCallback = std::function<void(Common::ByteSpan data)>;
using StateChangedCallback = std::function<void(TransportState newState, Common::Status status)>;

class ITransportChannel {
public:
    virtual ~ITransportChannel() = default;

    // Send binary packet over transport
    [[nodiscard]] virtual Common::Status Send(Common::ByteSpan data) = 0;

    // Asynchronous send
    [[nodiscard]] virtual Common::Status AsyncSend(Common::ByteBuffer data) = 0;

    // Register callback for inbound data stream
    virtual void SetDataCallback(DataReceivedCallback callback) = 0;

    // Register callback for link state changes
    virtual void SetStateCallback(StateChangedCallback callback) = 0;

    // Disconnect and release active transport resources
    virtual void Disconnect() = 0;

    // Current transport connection state
    [[nodiscard]] virtual TransportState GetState() const noexcept = 0;

    // Convenience check for active connection
    [[nodiscard]] virtual bool IsConnected() const noexcept {
        return GetState() == TransportState::Connected;
    }

    // Transport channel underlying physical type
    [[nodiscard]] virtual TransportType GetType() const noexcept = 0;

    // Maximum Transmission Unit (0 if unlimited or stream-oriented)
    [[nodiscard]] virtual size_t GetMtu() const noexcept { return 0; }

    // Remote peer identifier / endpoint description
    [[nodiscard]] virtual std::string GetRemoteEndpoint() const { return {}; }

    // Channel runtime statistics
    [[nodiscard]] virtual TransportStats GetStats() const { return {}; }
};

} // namespace CarPlayStation::Core::Transport
