#include "Core/Transport/BtRfcommTransport.h"
#include <chrono>

namespace CarPlayStation::Core::Transport {

BtRfcommTransport::BtRfcommTransport(uint8_t defaultChannel)
    : m_channel(defaultChannel) {}

BtRfcommTransport::~BtRfcommTransport() {
    StopServer();
}

Common::Status BtRfcommTransport::StartServer(uint8_t channel) {
    if (m_isListening.load()) {
        return Common::Status::Ok();
    }

    m_channel = channel;

    if (m_simulatedMode.load()) {
        m_isListening = true;
        m_state = TransportState::Connecting;
        return Common::Status::Ok();
    }

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return Common::Status::Error(Common::StatusCode::Unavailable, "WSAStartup failed");
    }

    m_serverSocket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (m_serverSocket == INVALID_SOCKET) {
        int err = WSAGetLastError();
        return Common::Status::Error(Common::StatusCode::Unavailable,
            "Failed to create Bluetooth RFCOMM socket (code " + std::to_string(err) + ")");
    }

    SOCKADDR_BTH sa{};
    sa.addressFamily = AF_BTH;
    sa.btAddr = 0; // Local radio
    sa.port = m_channel;

    if (bind(m_serverSocket, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
        return Common::Status::Error(Common::StatusCode::Unavailable,
            "Failed to bind Bluetooth RFCOMM socket to channel " + std::to_string(m_channel) + ": code " + std::to_string(err));
    }

    if (listen(m_serverSocket, 1) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
        return Common::Status::Error(Common::StatusCode::Unavailable,
            "Failed to listen on RFCOMM socket: code " + std::to_string(err));
    }

    // Register SDP service
    HANDLE sdpH = nullptr;
    auto sdpRes = BtSdpHelper::RegisterSdpService(m_serverSocket, m_channel, &sdpH);
    if (sdpRes.IsOk()) {
        m_sdpHandle = sdpH;
    }

    m_running = true;
    m_isListening = true;
    m_state = TransportState::Connecting;
    m_workerThread = std::thread(&BtRfcommTransport::ServerWorkerLoop, this);

    return Common::Status::Ok();
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "Bluetooth RFCOMM only supported on Windows");
#endif
}

void BtRfcommTransport::StopServer() {
    m_running = false;
    m_isListening = false;

#ifdef _WIN32
    if (m_clientSocket != INVALID_SOCKET) {
        shutdown(m_clientSocket, SD_BOTH);
        closesocket(m_clientSocket);
        m_clientSocket = INVALID_SOCKET;
    }

    if (m_serverSocket != INVALID_SOCKET) {
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
    }

    if (m_sdpHandle) {
        BtSdpHelper::UnregisterSdpService(m_sdpHandle);
        m_sdpHandle = nullptr;
    }
#endif

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    m_state = TransportState::Disconnected;
}

void BtRfcommTransport::ServerWorkerLoop() {
#ifdef _WIN32
    while (m_running.load()) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(m_serverSocket, &readFds);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms

        int sel = select(0, &readFds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            continue; // Timeout or signal
        }

        SOCKADDR_BTH clientAddr{};
        int addrLen = sizeof(clientAddr);
        SOCKET client = accept(m_serverSocket, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (client == INVALID_SOCKET) {
            continue;
        }

        std::string remoteMac = BtSdpHelper::FormatBluetoothAddress(clientAddr.btAddr);
        HandleClientIo(client, remoteMac);
    }
#endif
}

void BtRfcommTransport::HandleClientIo(SOCKET clientSock, const std::string& remoteAddr) {
#ifdef _WIN32
    m_clientSocket = clientSock;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remoteEndpoint = remoteAddr + " (RFCOMM ch " + std::to_string(m_channel) + ")";
    }

    m_state = TransportState::Connected;
    StateChangedCallback stateCb;
    DataReceivedCallback dataCb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stateCb = m_stateCallback;
        dataCb = m_dataCallback;
    }
    if (stateCb) {
        stateCb(TransportState::Connected, Common::Status::Ok());
    }

    std::vector<uint8_t> recvBuf(2048);

    while (m_running.load() && m_clientSocket != INVALID_SOCKET) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(m_clientSocket, &readFds);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int sel = select(0, &readFds, nullptr, nullptr, &tv);
        if (sel < 0) {
            break;
        }
        if (sel == 0) {
            continue;
        }

        int bytesRead = recv(m_clientSocket, reinterpret_cast<char*>(recvBuf.data()), static_cast<int>(recvBuf.size()), 0);
        if (bytesRead <= 0) {
            break; // Disconnect or error
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stats.bytes_received += bytesRead;
            m_stats.packets_received++;
            dataCb = m_dataCallback;
        }

        if (dataCb) {
            dataCb(Common::ByteSpan(recvBuf.data(), bytesRead));
        }
    }

    closesocket(m_clientSocket);
    m_clientSocket = INVALID_SOCKET;
    m_state = TransportState::Disconnected;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stateCb = m_stateCallback;
    }
    if (stateCb) {
        stateCb(TransportState::Disconnected, Common::Status::Ok());
    }
#else
    (void)clientSock;
    (void)remoteAddr;
#endif
}

Common::Status BtRfcommTransport::Send(Common::ByteSpan data) {
    if (m_state.load() != TransportState::Connected) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.errors_count++;
        return Common::Status::Error(Common::StatusCode::InvalidState, "Transport is not connected");
    }

    if (m_simulatedMode.load()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.bytes_sent += data.size();
        m_stats.packets_sent++;
        return Common::Status::Ok();
    }

#ifdef _WIN32
    SOCKET s = m_clientSocket;
    if (s == INVALID_SOCKET) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.errors_count++;
        return Common::Status::Error(Common::StatusCode::InvalidState, "No active RFCOMM client socket");
    }

    int sent = send(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0);
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.errors_count++;
        return Common::Status::Error(Common::StatusCode::IoError, "RFCOMM send failed: code " + std::to_string(err));
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.bytes_sent += sent;
        m_stats.packets_sent++;
    }

    return Common::Status::Ok();
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "Send not supported on non-Windows");
#endif
}

Common::Status BtRfcommTransport::AsyncSend(Common::ByteBuffer data) {
    return Send(Common::ByteSpan(data.data(), data.size()));
}

void BtRfcommTransport::SetDataCallback(DataReceivedCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dataCallback = std::move(callback);
}

void BtRfcommTransport::SetStateCallback(StateChangedCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stateCallback = std::move(callback);
}

void BtRfcommTransport::Disconnect() {
#ifdef _WIN32
    if (m_clientSocket != INVALID_SOCKET) {
        shutdown(m_clientSocket, SD_BOTH);
        closesocket(m_clientSocket);
        m_clientSocket = INVALID_SOCKET;
    }
#endif
    if (m_state.load() == TransportState::Connected) {
        m_state = TransportState::Disconnected;
        StateChangedCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cb = m_stateCallback;
        }
        if (cb) {
            cb(TransportState::Disconnected, Common::Status::Ok());
        }
    }
}

TransportState BtRfcommTransport::GetState() const noexcept {
    return m_state.load();
}

TransportType BtRfcommTransport::GetType() const noexcept {
    return TransportType::BluetoothRfcomm;
}

size_t BtRfcommTransport::GetMtu() const noexcept {
    return 1012; // Standard Wireless CarPlay RFCOMM MTU
}

std::string BtRfcommTransport::GetRemoteEndpoint() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_remoteEndpoint;
}

TransportStats BtRfcommTransport::GetStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

bool BtRfcommTransport::IsListening() const noexcept {
    return m_isListening.load();
}

uint8_t BtRfcommTransport::GetChannel() const noexcept {
    return m_channel;
}

void BtRfcommTransport::EnableSimulatedMode(bool enable) {
    m_simulatedMode = enable;
}

bool BtRfcommTransport::IsSimulatedMode() const noexcept {
    return m_simulatedMode.load();
}

void BtRfcommTransport::SimulatePeerConnect(std::string_view peerAddress) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remoteEndpoint = std::string(peerAddress) + " (Simulated RFCOMM ch " + std::to_string(m_channel) + ")";
    }
    m_state = TransportState::Connected;
    StateChangedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_stateCallback;
    }
    if (cb) {
        cb(TransportState::Connected, Common::Status::Ok());
    }
}

void BtRfcommTransport::SimulatePeerData(Common::ByteSpan data) {
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

void BtRfcommTransport::SimulatePeerDisconnect() {
    m_state = TransportState::Disconnected;
    StateChangedCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_stateCallback;
    }
    if (cb) {
        cb(TransportState::Disconnected, Common::Status::Ok());
    }
}

} // namespace CarPlayStation::Core::Transport
