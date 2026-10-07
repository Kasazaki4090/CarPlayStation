// Assertions are the test oracle here, so keep them alive in Release builds.
#undef NDEBUG
#include "Core/Hardware/MockMfiSigner.h"
#include "Core/Hardware/NetworkMfiClient.h"
#include "Core/Hardware/II2cAdapter.h"
#include "Core/Hardware/Ch341Protocol.h"
#include "Core/Hardware/Ch341I2cAdapter.h"
#include "Core/Hardware/Ch347I2cAdapter.h"
#include "Core/Hardware/FtdiI2cAdapter.h"
#include "Core/Hardware/MockI2cAdapter.h"
#include "Core/Hardware/I2cMfiSigner.h"
#include "Core/Hardware/RadioAdapterHelper.h"
#include "Core/Hardware/RemoteMfiHttpClient.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <cstdlib>

using namespace CarPlayStation::Core;
using namespace CarPlayStation::Core::Hardware;

// Test Helper for MockMfiSigner
void TestMockMfiSigner() {
    std::cout << "[Test] Running MockMfiSigner tests...\n";

    MockMfiSigner signer;

    // 1. Default Certificate Fetch
    auto cert_res = signer.FetchCertificate();
    assert(cert_res.IsOk());
    assert(cert_res.Value().size() == 945);
    assert(signer.GetFetchCertCalls() == 1);

    // 2. Custom Certificate
    Common::ByteBuffer custom_cert = {0xDE, 0xAD, 0xBE, 0xEF};
    signer.SetCertificate(custom_cert);
    auto custom_cert_res = signer.FetchCertificate();
    assert(custom_cert_res.IsOk());
    assert(custom_cert_res.Value() == custom_cert);
    assert(signer.GetFetchCertCalls() == 2);

    // 3. Challenge Signing
    uint8_t dummy_challenge[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    auto sign_res = signer.SignChallenge(dummy_challenge);
    assert(sign_res.IsOk());
    assert(sign_res.Value().size() == 128);
    assert(signer.GetSignChallengeCalls() == 1);
    assert(signer.GetLastChallenge().size() == 20);

    // 4. Custom Signer
    signer.SetCustomSigner([](Common::ByteSpan challenge) -> Common::Result<Common::ByteBuffer> {
        Common::ByteBuffer sig;
        sig.push_back(0x42);
        sig.insert(sig.end(), challenge.begin(), challenge.end());
        return sig;
    });
    auto custom_sign_res = signer.SignChallenge(dummy_challenge);
    assert(custom_sign_res.IsOk());
    assert(custom_sign_res.Value().size() == 21);
    assert(custom_sign_res.Value()[0] == 0x42);

    // 5. Error Injection
    signer.SetFailFetchCertificate(true, Common::StatusCode::AuthenticationFailed, "Injected cert failure");
    auto failed_cert_res = signer.FetchCertificate();
    assert(failed_cert_res.IsError());
    assert(failed_cert_res.Error().Code() == Common::StatusCode::AuthenticationFailed);

    signer.SetFailSignChallenge(true, Common::StatusCode::HardwareError, "Injected sign failure");
    auto failed_sign_res = signer.SignChallenge(dummy_challenge);
    assert(failed_sign_res.IsError());
    assert(failed_sign_res.Error().Code() == Common::StatusCode::HardwareError);

    // 6. Chip Info & Shutdown
    auto chip_info = signer.GetChipInfo();
    assert(chip_info.present);
    assert(chip_info.chip_id == 0x000200C0);

    assert(!signer.IsShutdown());
    signer.Shutdown();
    assert(signer.IsShutdown());

    std::cout << "[Test] MockMfiSigner tests passed.\n";
}

// Local in-memory TCP Test Server for NetworkMfiClient verification
class MockTcpMfiServer {
public:
    MockTcpMfiServer() {
#ifdef _WIN32
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
        listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(listen_sock_ != INVALID_SOCKET);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // OS assigns ephemeral port

        int rc = bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        assert(rc == 0);

        sockaddr_in assigned_addr{};
        socklen_t len = sizeof(assigned_addr);
        getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&assigned_addr), &len);
        port_ = ntohs(assigned_addr.sin_port);

        rc = listen(listen_sock_, 5);
        assert(rc == 0);
    }

    ~MockTcpMfiServer() {
        Stop();
    }

    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }

    void Stop() {
        running_ = false;
        if (listen_sock_ != INVALID_SOCKET) {
            closesocket(listen_sock_);
            listen_sock_ = INVALID_SOCKET;
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    template <typename Handler>
    void Start(Handler handler) {
        running_ = true;
        server_thread_ = std::thread([this, handler = std::move(handler)]() {
            while (running_) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(listen_sock_, &rfds);

                timeval tv{0, 100000}; // 100ms
                int sel = select(static_cast<int>(listen_sock_ + 1), &rfds, nullptr, nullptr, &tv);
                if (sel > 0 && FD_ISSET(listen_sock_, &rfds)) {
                    sockaddr_in client_addr{};
                    socklen_t clen = sizeof(client_addr);
                    SOCKET client_sock = accept(listen_sock_, reinterpret_cast<sockaddr*>(&client_addr), &clen);
                    if (client_sock != INVALID_SOCKET) {
                        handler(client_sock);
                        closesocket(client_sock);
                    }
                }
            }
        });
    }

private:
    SOCKET listen_sock_{INVALID_SOCKET};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread server_thread_;
};

void TestNetworkMfiClientSuccess() {
    std::cout << "[Test] Running NetworkMfiClient success transactions...\n";

    MockTcpMfiServer server;
    server.Start([](SOCKET client) {
        // Read 3-byte header
        uint8_t req_hdr[3];
        int n = recv(client, reinterpret_cast<char*>(req_hdr), 3, 0);
        if (n != 3) return;

        uint8_t op = req_hdr[0];
        uint16_t payload_len = (static_cast<uint16_t>(req_hdr[1]) << 8) | static_cast<uint16_t>(req_hdr[2]);

        std::vector<uint8_t> payload(payload_len);
        if (payload_len > 0) {
            recv(client, reinterpret_cast<char*>(payload.data()), payload_len, 0);
        }

        if (op == 0x01) {
            // Op 0x01: Return 945-byte cert
            std::vector<uint8_t> cert(945, 0xAB);
            uint16_t cert_len = static_cast<uint16_t>(cert.size());
            uint8_t resp_hdr[3] = {0x00, static_cast<uint8_t>((cert_len >> 8) & 0xFF), static_cast<uint8_t>(cert_len & 0xFF)};
            send(client, reinterpret_cast<char*>(resp_hdr), 3, 0);
            send(client, reinterpret_cast<char*>(cert.data()), static_cast<int>(cert.size()), 0);
        } else if (op == 0x02) {
            // Op 0x02: Return 128-byte signature
            std::vector<uint8_t> sig(128, 0xCD);
            uint16_t sig_len = static_cast<uint16_t>(sig.size());
            uint8_t resp_hdr[3] = {0x00, static_cast<uint8_t>((sig_len >> 8) & 0xFF), static_cast<uint8_t>(sig_len & 0xFF)};
            send(client, reinterpret_cast<char*>(resp_hdr), 3, 0);
            send(client, reinterpret_cast<char*>(sig.data()), static_cast<int>(sig.size()), 0);
        }
    });

    NetworkMfiClient client("127.0.0.1", server.GetPort());
    assert(client.IsReachable());

    // 1. Fetch Certificate
    auto cert_res = client.FetchCertificate();
    assert(cert_res.IsOk());
    assert(cert_res.Value().size() == 945);
    assert(cert_res.Value()[0] == 0xAB);

    // 2. Sign Challenge
    uint8_t digest[20] = {1, 2, 3, 4};
    auto sign_res = client.SignChallenge(digest);
    assert(sign_res.IsOk());
    assert(sign_res.Value().size() == 128);
    assert(sign_res.Value()[0] == 0xCD);

    server.Stop();
    std::cout << "[Test] NetworkMfiClient success transactions passed.\n";
}

void TestNetworkMfiClientTrickleAndTimeout() {
    std::cout << "[Test] Running NetworkMfiClient trickle stream and timeout tests...\n";

    // Test Trickle: bytes arriving slowly but within deadline
    {
        MockTcpMfiServer server;
        server.Start([](SOCKET client) {
            uint8_t req_hdr[3];
            recv(client, reinterpret_cast<char*>(req_hdr), 3, 0);

            // Send 10-byte response in 1-byte chunks with 20ms delays
            uint8_t resp_hdr[3] = {0x00, 0x00, 0x0A};
            send(client, reinterpret_cast<char*>(resp_hdr), 3, 0);

            for (uint8_t b = 0; b < 10; ++b) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                send(client, reinterpret_cast<char*>(&b), 1, 0);
            }
        });

        NetworkMfiClient::Options opts;
        opts.response_deadline = std::chrono::milliseconds(1500);
        NetworkMfiClient client("127.0.0.1", server.GetPort(), opts);

        auto cert_res = client.FetchCertificate();
        assert(cert_res.IsOk());
        assert(cert_res.Value().size() == 10);
        for (uint8_t i = 0; i < 10; ++i) {
            assert(cert_res.Value()[i] == i);
        }
        server.Stop();
    }

    // Test Timeout: Server goes silent after partial response
    {
        MockTcpMfiServer server;
        server.Start([](SOCKET client) {
            uint8_t req_hdr[3];
            recv(client, reinterpret_cast<char*>(req_hdr), 3, 0);

            // Send header indicating 100 bytes, send 2 bytes then hang
            uint8_t resp_hdr[3] = {0x00, 0x00, 0x64};
            send(client, reinterpret_cast<char*>(resp_hdr), 3, 0);
            uint8_t partial[2] = {0x11, 0x22};
            send(client, reinterpret_cast<char*>(partial), 2, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        });

        NetworkMfiClient::Options opts;
        opts.max_retries = 1;
        opts.response_deadline = std::chrono::milliseconds(300);
        NetworkMfiClient client("127.0.0.1", server.GetPort(), opts);

        auto start = std::chrono::steady_clock::now();
        auto cert_res = client.FetchCertificate();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

        assert(cert_res.IsError());
        assert(cert_res.Error().Code() == Common::StatusCode::Timeout);
        assert(elapsed.count() < 1200); // Bounded near deadline, not hanging

        server.Stop();
    }

    std::cout << "[Test] NetworkMfiClient trickle stream and timeout tests passed.\n";
}

void TestNetworkMfiClientRetryRecovery() {
    std::cout << "[Test] Running NetworkMfiClient retry and recovery tests...\n";

    MockTcpMfiServer server;
    std::atomic<int> attempt_count{0};

    server.Start([&attempt_count](SOCKET client) {
        int attempt = ++attempt_count;
        uint8_t req_hdr[3];
        recv(client, reinterpret_cast<char*>(req_hdr), 3, 0);

        if (attempt <= 2) {
            // First 2 attempts fail with MFi status error 0x01
            uint8_t err_hdr[3] = {0x01, 0x00, 0x00};
            send(client, reinterpret_cast<char*>(err_hdr), 3, 0);
        } else {
            // 3rd attempt succeeds
            std::vector<uint8_t> cert(50, 0x77);
            uint16_t cert_len = static_cast<uint16_t>(cert.size());
            uint8_t resp_hdr[3] = {0x00, static_cast<uint8_t>((cert_len >> 8) & 0xFF), static_cast<uint8_t>(cert_len & 0xFF)};
            send(client, reinterpret_cast<char*>(resp_hdr), 3, 0);
            send(client, reinterpret_cast<char*>(cert.data()), static_cast<int>(cert.size()), 0);
        }
    });

    NetworkMfiClient::Options opts;
    opts.max_retries = 5;
    opts.retry_delay = std::chrono::milliseconds(50);
    NetworkMfiClient client("127.0.0.1", server.GetPort(), opts);

    auto cert_res = client.FetchCertificate();
    assert(cert_res.IsOk());
    assert(cert_res.Value().size() == 50);
    assert(attempt_count.load() == 3);

    server.Stop();
    std::cout << "[Test] NetworkMfiClient retry and recovery tests passed.\n";
}

#include "Core/Hardware/FileMfiSigner.h"

void TestFileMfiSigner() {
    std::cout << "[Test] Running FileMfiSigner tests...\n";

    // 1. Missing file error handling
    FileMfiSigner signer("non_existent_cert.p7b", "non_existent_key.pk8");
    auto cert_res = signer.FetchCertificate();
    assert(cert_res.IsError());
    assert(cert_res.Error().Code() == Common::StatusCode::NotFound);

    // 2. In-memory provisioning with standard NIST P-256 keypair
    std::vector<uint8_t> dummy_cert = {0x30, 0x82, 0x01, 0x00, 0x04, 0x60, 0xDE, 0xAD};
    std::vector<uint8_t> raw_p256_key = {
        0x04, 0x20, // OCTET STRING of 32 bytes
        0xC9, 0xAF, 0xA9, 0xD8, 0x45, 0xBA, 0x75, 0x16,
        0x6B, 0x5C, 0x21, 0x57, 0x67, 0xB1, 0xD6, 0x93,
        0x4E, 0x13, 0xC5, 0x30, 0x47, 0xD4, 0x3B, 0xE7,
        0x89, 0xAE, 0x63, 0x0E, 0xBE, 0x0D, 0x2E, 0x14
    };

    FileMfiSigner mem_signer;
    auto load_res = mem_signer.LoadFromMemory(dummy_cert, raw_p256_key);
    if (load_res.IsOk()) {
        auto mem_cert_res = mem_signer.FetchCertificate();
        assert(mem_cert_res.IsOk());
        assert(mem_cert_res.Value() == dummy_cert);

        // Sign 32-byte MFi v3 challenge
        uint8_t challenge32[32] = {
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
            0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38
        };
        auto sign_res = mem_signer.SignChallenge(challenge32);
        assert(sign_res.IsOk());
        assert(sign_res.Value().size() == 64);

        auto info = mem_signer.GetChipInfo();
        assert(info.present);
        assert(info.chip_id == 0x00030000);
    }

    std::cout << "[Test] FileMfiSigner tests passed.\n";
}

// ----------------------------------------------------------------------------
// Step 2 Tests: I2C & Hardware Adapters (CH341, CH347, FTDI, Mock)
// ----------------------------------------------------------------------------

void TestCh341Protocol() {
    std::cout << "[Test] Running Ch341Protocol encoding tests...\n";

    // 1. Address byte calculation
    assert(Ch341Protocol::MakeAddressByte(0x10, false) == 0x20); // Write
    assert(Ch341Protocol::MakeAddressByte(0x10, true) == 0x21);  // Read
    assert(Ch341Protocol::MakeAddressByte(0x11, false) == 0x22); // Write
    assert(Ch341Protocol::MakeAddressByte(0x11, true) == 0x23);  // Read

    // 2. Speed configuration packet
    auto speed_pkt = Ch341Protocol::EncodeSpeed(Ch341Protocol::SPEED_100KHZ);
    assert(speed_pkt.size() == 3);
    assert(speed_pkt[0] == Ch341Protocol::CMD_I2C_STREAM);
    assert(speed_pkt[1] == 0x61);
    assert(speed_pkt[2] == Ch341Protocol::CMD_STREAM_END);

    // 3. Write transaction packet
    uint8_t write_payload[] = {0x01, 0x02};
    auto write_stream = Ch341Protocol::EncodeTransaction(0x10, write_payload, 0);
    assert(!write_stream.empty());
    assert(write_stream.front() == Ch341Protocol::CMD_I2C_STREAM);
    assert(write_stream.back() == Ch341Protocol::CMD_STREAM_END);

    // 4. Combined write-then-read transaction (Repeated Start)
    uint8_t reg_addr = 0x30;
    auto txn_stream = Ch341Protocol::EncodeTransaction(0x10, Common::ByteSpan(&reg_addr, 1), 2);
    assert(!txn_stream.empty());
    assert(txn_stream.front() == Ch341Protocol::CMD_I2C_STREAM);
    assert(txn_stream.back() == Ch341Protocol::CMD_STREAM_END);

    // 5. GPIO reset pulse
    auto rst_stream = Ch341Protocol::EncodeResetPulse(0, true);
    assert(!rst_stream.empty());
    assert(rst_stream.front() == Ch341Protocol::CMD_UIO_STREAM);
    assert(rst_stream.back() == Ch341Protocol::CMD_STREAM_END);

    std::cout << "[Test] Ch341Protocol encoding tests passed.\n";
}

void TestMockI2cAdapter() {
    std::cout << "[Test] Running MockI2cAdapter tests...\n";

    auto adapter = std::make_shared<MockI2cAdapter>(0x10);
    assert(adapter->IsOpen());
    assert(adapter->GetAdapterName().find("MFi Coprocessor") != std::string::npos);

    // Speed setting
    assert(adapter->SetSpeed(I2cClockSpeed::Khz400).IsOk());

    // Reset pulse
    assert(adapter->PulseReset(0).IsOk());
    assert(adapter->GetResetCount() == 1);

    // Read major version register 0x02 directly via transaction
    uint8_t reg = 0x02;
    auto read_res = adapter->Transaction(0x10, Common::ByteSpan(&reg, 1), 1);
    assert(read_res.IsOk());
    assert(read_res.Value().size() == 1);
    assert(read_res.Value()[0] == 0x02);

    // Address mismatch probe
    auto bad_addr = adapter->Transaction(0x55, Common::ByteSpan(&reg, 1), 1);
    assert(bad_addr.IsError());

    // Injected NACK
    adapter->SetInjectNack(true);
    auto nack_res = adapter->Transaction(0x10, Common::ByteSpan(&reg, 1), 1);
    assert(nack_res.IsError());
    adapter->SetInjectNack(false);

    // Close and open lifecycle
    adapter->Close();
    assert(!adapter->IsOpen());
    assert(adapter->Transaction(0x10, Common::ByteSpan(&reg, 1), 1).IsError());
    assert(adapter->Open().IsOk());
    assert(adapter->IsOpen());

    std::cout << "[Test] MockI2cAdapter tests passed.\n";
}

void TestI2cMfiSigner() {
    std::cout << "[Test] Running I2cMfiSigner tests with MockI2cAdapter...\n";

    auto adapter = std::make_shared<MockI2cAdapter>(0x10);
    I2cMfiSigner signer(adapter, 0x10);

    // 1. Get Chip Info
    auto chip_info = signer.GetChipInfo();
    assert(chip_info.present);
    assert(chip_info.chip_id == 0x00020002);
    assert(chip_info.firmware_version == "MFi-CP-v2");

    // 2. Fetch Certificate (verifies windowed register reading over I2C)
    auto cert_res = signer.FetchCertificate();
    assert(cert_res.IsOk());
    assert(cert_res.Value().size() == 945);
    // The mock fills the coprocessor certificate with (i * 17 + 33) & 0xFF.
    // Checking every byte proves the 128-byte register windows were stitched in
    // order rather than read as one oversized transaction.
    for (size_t i = 0; i < cert_res.Value().size(); ++i) {
        assert(cert_res.Value()[i] == static_cast<uint8_t>((i * 17 + 33) & 0xFF));
    }

    // 3. Challenge Signing (verifies 0x20 len, 0x21 payload, 0x10 auth trigger, 0x11 sig len, 0x12 sig data)
    uint8_t challenge[20] = {
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x01,
        0x02, 0x03, 0x04, 0x05
    };
    auto sign_res = signer.SignChallenge(challenge);
    assert(sign_res.IsOk());
    assert(sign_res.Value().size() == 128); // 128-byte RSA signature from CP

    // Verify challenge reached the mock coprocessor
    auto last_ch = adapter->GetLastChallenge();
    assert(last_ch.size() == 20);
    assert(std::memcmp(last_ch.data(), challenge, 20) == 0);

    // 4. Invalid challenge argument rejection
    assert(signer.SignChallenge({}).IsError());

    // 5. Shutdown lifecycle
    signer.Shutdown();
    assert(signer.FetchCertificate().IsError());
    assert(signer.SignChallenge(challenge).IsError());

    std::cout << "[Test] I2cMfiSigner tests passed.\n";
}

void TestHardwareAdaptersContracts() {
    std::cout << "[Test] Running hardware adapters contract tests (CH341, CH347, FTDI)...\n";

    // 1. CH341 Adapter
    {
        Ch341I2cAdapter ch341;
        assert(ch341.GetAdapterName().find("CH341") != std::string::npos);
        assert(!ch341.IsOpen());
        // Probe driver status safely without crashing
        bool has_driver = ch341.IsDriverLoaded();
        (void)has_driver;
        // Non-open operations should return safe error
        assert(ch341.Write(0x10, {}).IsError());
        assert(ch341.Read(0x10, 2).IsError());
        assert(ch341.Transaction(0x10, {}, 2).IsError());
    }

    // 2. CH347 Adapter
    {
        Ch347I2cAdapter ch347;
        assert(ch347.GetAdapterName().find("CH347") != std::string::npos);
        assert(!ch347.IsOpen());
        bool has_driver = ch347.IsDriverLoaded();
        (void)has_driver;
        assert(ch347.Write(0x10, {}).IsError());
        assert(ch347.Read(0x10, 2).IsError());
    }

    // 3. FTDI Adapter
    {
        FtdiI2cAdapter ftdi;
        assert(ftdi.GetAdapterName().find("FTDI") != std::string::npos);
        assert(!ftdi.IsOpen());
        bool has_driver = ftdi.IsDriverLoaded();
        (void)has_driver;
        assert(ftdi.Write(0x10, {}).IsError());
        assert(ftdi.Read(0x10, 2).IsError());
    }

    std::cout << "[Test] Hardware adapters contract tests passed.\n";
}

// ----------------------------------------------------------------------------
// Step 3 Tests: RadioAdapterHelper (Bluetooth & Wi-Fi 5GHz / SoftAP Detection)
// ----------------------------------------------------------------------------

void TestRadioAdapterHelper() {
    std::cout << "[Test] Running RadioAdapterHelper tests...\n";

    RadioAdapterHelper helper;

    // 1. Live system probe (must not crash, returns valid enum and data structure)
    auto live_bt = helper.DetectBluetoothCapabilities();
    auto live_wifi = helper.DetectWifiCapabilities();
    auto live_readiness = helper.EvaluateReadiness();
    (void)live_bt;
    (void)live_wifi;
    assert(!live_readiness.diagnostics.empty());

    RadioState live_bt_state = helper.GetState("bluetooth");
    RadioState live_wifi_state = helper.GetState("wifi");
    assert(RadioStateToString(live_bt_state) != "Unknown");
    assert(RadioStateToString(live_wifi_state) != "Unknown");

    // 2. Simulated Full Readiness (Bluetooth ON + Wi-Fi 5GHz ON + SoftAP)
    helper.SetSimulatedBluetooth(RadioState::On, true, "Intel Wireless Bluetooth");
    helper.SetSimulatedWifi(RadioState::On, true, true, "Intel Wi-Fi 6 AX200");

    assert(helper.GetState("bluetooth") == RadioState::On);
    assert(helper.GetState("wifi") == RadioState::On);

    auto ready_eval = helper.EvaluateReadiness();
    assert(ready_eval.overall_ready);
    assert(ready_eval.bt_ready);
    assert(ready_eval.wifi_ready);
    assert(ready_eval.supports_5ghz);
    assert(ready_eval.supports_softap);
    assert(ready_eval.diagnostics.find("prerequisites for Wireless CarPlay are satisfied") != std::string::npos);

    // 3. Simulated Missing 5GHz capability (2.4GHz only Wi-Fi)
    helper.SetSimulatedWifi(RadioState::On, false, true, "Legacy 802.11bgn Wi-Fi");
    auto no_5g_eval = helper.EvaluateReadiness();
    assert(!no_5g_eval.overall_ready);
    assert(!no_5g_eval.supports_5ghz);
    assert(no_5g_eval.diagnostics.find("Apple Wireless CarPlay requires 5GHz") != std::string::npos);

    // 4. Simulated Bluetooth Off
    helper.SetSimulatedBluetooth(RadioState::Off, true, "Intel Wireless Bluetooth");
    helper.SetSimulatedWifi(RadioState::On, true, true, "Intel Wi-Fi 6 AX200");
    auto bt_off_eval = helper.EvaluateReadiness();
    assert(!bt_off_eval.overall_ready);
    assert(!bt_off_eval.bt_ready);

    // 5. Simulated Radio Toggle (SetEnabled)
    assert(helper.SetEnabled("bluetooth", true).IsOk());
    assert(helper.GetState("bluetooth") == RadioState::On);
    assert(helper.SetEnabled("bluetooth", false).IsOk());
    assert(helper.GetState("bluetooth") == RadioState::Off);

    // 6. Clear simulation and restore
    helper.ClearSimulation();

    std::cout << "[Test] RadioAdapterHelper tests passed.\n";
}

// ---------------------------------------------------------------------------
// RemoteMfiHttpClient (Remote MFi HTTP API) tests
// ---------------------------------------------------------------------------

// Minimal HTTP/1.1 stub for the flat-JSON Remote MFi API. Requests are handled
// one at a time and every reply is closed, which matches what the client asks
// for (Connection: close).
struct StubHttpRequest {
    std::string method;
    std::string path;
    std::string headers;  // request header block, lower-cased names only matter for asserts
    std::string body;
};

struct StubHttpResponse {
    int status{200};
    std::string body;
};

class MockHttpMfiServer {
public:
    explicit MockHttpMfiServer(std::function<StubHttpResponse(const StubHttpRequest&)> router)
        : router_(std::move(router)) {
#ifdef _WIN32
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
        listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(listen_sock_ != INVALID_SOCKET);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        assert(bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

        sockaddr_in assigned{};
        socklen_t len = sizeof(assigned);
        getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&assigned), &len);
        port_ = ntohs(assigned.sin_port);

        assert(listen(listen_sock_, 5) == 0);

        running_ = true;
        thread_ = std::thread([this]() {
            while (running_) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(listen_sock_, &rfds);
                timeval tv{0, 100000};
                if (select(static_cast<int>(listen_sock_ + 1), &rfds, nullptr, nullptr, &tv) <= 0) {
                    continue;
                }
                SOCKET client = accept(listen_sock_, nullptr, nullptr);
                if (client == INVALID_SOCKET) {
                    continue;
                }
                StubHttpRequest req;
                if (ReadRequest(client, req)) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        requests_.push_back(req);
                    }
                    const StubHttpResponse resp = router_(req);
                    WriteResponse(client, resp);
                }
                closesocket(client);
            }
        });
    }

    ~MockHttpMfiServer() {
        running_ = false;
        if (listen_sock_ != INVALID_SOCKET) {
            closesocket(listen_sock_);
            listen_sock_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }

    std::vector<StubHttpRequest> Requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    static bool ReadRequest(SOCKET sock, StubHttpRequest& req) {
        std::string raw;
        char buffer[1024];
        std::size_t head_end = std::string::npos;
        while (head_end == std::string::npos) {
            const int n = recv(sock, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return false;
            }
            raw.append(buffer, static_cast<std::size_t>(n));
            head_end = raw.find("\r\n\r\n");
        }
        const std::string head = raw.substr(0, head_end);
        req.body = raw.substr(head_end + 4);

        const std::size_t line_end = head.find("\r\n");
        const std::string request_line = head.substr(0, line_end);
        req.headers = head.substr(line_end == std::string::npos ? 0 : line_end + 2);

        const std::size_t sp1 = request_line.find(' ');
        const std::size_t sp2 = request_line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos) {
            return false;
        }
        req.method = request_line.substr(0, sp1);
        req.path = request_line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);

        const std::size_t cl = req.headers.find("Content-Length:");
        if (cl != std::string::npos) {
            const std::size_t expected = std::strtoul(req.headers.c_str() + cl + 15, nullptr, 10);
            while (req.body.size() < expected) {
                const int n = recv(sock, buffer, sizeof(buffer), 0);
                if (n <= 0) {
                    break;
                }
                req.body.append(buffer, static_cast<std::size_t>(n));
            }
        }
        return true;
    }

    static void WriteResponse(SOCKET sock, const StubHttpResponse& resp) {
        const char* reason = (resp.status == 200) ? "OK"
                         : (resp.status == 400) ? "Bad Request"
                         : (resp.status == 401) ? "Unauthorized"
                         : (resp.status == 404) ? "Not Found"
                         : (resp.status == 500) ? "Internal Server Error"
                         : (resp.status == 503) ? "Service Unavailable"
                                                : "Error";
        std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + reason + "\r\n";
        out += "Content-Type: application/json\r\n";
        out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += resp.body;
        std::size_t sent = 0;
        while (sent < out.size()) {
            const int n = send(sock, out.data() + sent, static_cast<int>(out.size() - sent), 0);
            if (n <= 0) {
                break;
            }
            sent += static_cast<std::size_t>(n);
        }
    }

    std::function<StubHttpResponse(const StubHttpRequest&)> router_;
    SOCKET listen_sock_{INVALID_SOCKET};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<StubHttpRequest> requests_;
};

// Certificate used by the stub: 30 82 01 02 DE AD BE EF.
static const char* kStubCertBase64 = "MIIBAt6tvu8=";
static const Common::ByteBuffer kStubCertDer = {0x30, 0x82, 0x01, 0x02, 0xDE, 0xAD, 0xBE, 0xEF};
// Signature used by the stub: AA BB CC DD.
static const char* kStubSignatureBase64 = "qrvM3Q==";
static const Common::ByteBuffer kStubSignature = {0xAA, 0xBB, 0xCC, 0xDD};

void TestRemoteMfiHttpClientSuccess() {
    std::cout << "[Test] Running RemoteMfiHttpClient success transactions...\n";

    MockHttpMfiServer server([](const StubHttpRequest& req) -> StubHttpResponse {
        if (req.path == "/mfi/certificate") {
            return StubHttpResponse{200,
                "{\"type\":\"mfi\",\"protocolMajor\":3,\"certificate\":\"" +
                std::string(kStubCertBase64) +
                "\",\"certificateSha256\":\"00000000000000000000000000000000"
                "0000000000000000000000000000\"}"};
        }
        if (req.path == "/mfi/sign") {
            return StubHttpResponse{200, "{\"signature\":\"" + std::string(kStubSignatureBase64) + "\"}"};
        }
        return StubHttpResponse{404, "{\"detail\":\"unknown path\"}"};
    });

    RemoteMfiHttpClient::Options opt;
    opt.host = "127.0.0.1";
    opt.port = server.GetPort();
    opt.bearer_token = "secret-token";
    RemoteMfiHttpClient client(opt);

    // 1. Certificate fetch decodes base64 and caches.
    auto cert = client.FetchCertificate();
    assert(cert.IsOk());
    assert(cert.Value() == kStubCertDer);

    // 2. protocolMajor=3 must surface as the v3 generation marker so the
    //    existing digest selection keeps working.
    auto info = client.GetChipInfo();
    assert(info.present);
    assert(info.chip_id == 0x00030000u);
    assert(info.firmware_version == "remote-mfi 3.0");

    // The certificate is cached: /mfi/certificate is requested only once.
    auto cert2 = client.FetchCertificate();
    assert(cert2.IsOk());
    assert(server.Requests().size() == 1u);

    // 3. Signing round trip, with bearer auth and a fresh requestId.
    uint8_t challenge[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    auto sig = client.SignChallenge(challenge);
    assert(sig.IsOk());
    assert(sig.Value() == kStubSignature);

    auto requests = server.Requests();
    assert(requests.size() == 2u);
    const StubHttpRequest& sign_req = requests[1];
    assert(sign_req.method == "POST");
    assert(sign_req.path == "/mfi/sign");
    assert(sign_req.headers.find("Authorization: Bearer secret-token") != std::string::npos);
    assert(sign_req.body.find("\"challenge\"") != std::string::npos);
    const std::size_t id_pos = sign_req.body.find("\"requestId\":\"");
    assert(id_pos != std::string::npos);
    assert(sign_req.body.size() >= id_pos + 13u + 36u);
    const std::string id0 = sign_req.body.substr(id_pos + 13, 36);
    assert(id0[8] == '-' && id0[13] == '-' && id0[18] == '-' && id0[23] == '-');
    assert(id0[14] == '4');  // UUID v4

    // 4. A second sign must use a different requestId (server replay cache).
    auto sig2 = client.SignChallenge(challenge);
    assert(sig2.IsOk());
    auto requests2 = server.Requests();
    assert(requests2.size() == 3u);
    const std::size_t p1 = requests[1].body.find("\"requestId\":\"") + 13;
    const std::size_t p2 = requests2[2].body.find("\"requestId\":\"") + 13;
    assert(requests[1].body.compare(p1, 36, requests2[2].body, p2, 36) != 0);

    // 5. InvalidateCache forces a re-fetch.
    client.InvalidateCache();
    auto cert3 = client.FetchCertificate();
    assert(cert3.IsOk());
    assert(server.Requests().size() == 4u);

    // 6. Shutdown makes the signer unusable.
    assert(!client.IsShutdown());
    client.Shutdown();
    assert(client.IsShutdown());
    auto after = client.SignChallenge(challenge);
    assert(after.IsError());
    assert(after.Error().Code() == Common::StatusCode::InvalidState);

    std::cout << "[Test] RemoteMfiHttpClient success transactions passed.\n";
}

void TestRemoteMfiHttpClientErrors() {
    std::cout << "[Test] Running RemoteMfiHttpClient error mapping...\n";

    // 1. 401 -> AuthenticationFailed (no retry).
    {
        MockHttpMfiServer server([](const StubHttpRequest&) -> StubHttpResponse {
            return StubHttpResponse{401, "{\"detail\":\"invalid bearer token\"}"};
        });
        RemoteMfiHttpClient::Options opt;
        opt.host = "127.0.0.1";
        opt.port = server.GetPort();
        RemoteMfiHttpClient client(opt);
        auto cert = client.FetchCertificate();
        assert(cert.IsError());
        assert(cert.Error().Code() == Common::StatusCode::AuthenticationFailed);
        assert(cert.Error().Message().find("invalid bearer token") != std::string::npos);
        assert(server.Requests().size() == 1u);
    }

    // 2. 503 (chip gate busy) is retried, then reported as unavailable.
    {
        MockHttpMfiServer server([](const StubHttpRequest&) -> StubHttpResponse {
            return StubHttpResponse{503, "{\"detail\":\"mfi chip busy\"}"};
        });
        RemoteMfiHttpClient::Options opt;
        opt.host = "127.0.0.1";
        opt.port = server.GetPort();
        opt.max_retries = 1;
        opt.retry_delay = std::chrono::milliseconds(10);
        RemoteMfiHttpClient client(opt);
        auto cert = client.FetchCertificate();
        assert(cert.IsError());
        assert(cert.Error().Code() == Common::StatusCode::DeviceUnavailable);
        assert(server.Requests().size() == 2u);
    }

    // 3. Malformed payloads -> ProtocolError.
    {
        MockHttpMfiServer server([](const StubHttpRequest&) -> StubHttpResponse {
            return StubHttpResponse{200, "{\"type\":\"not-mfi\"}"};
        });
        RemoteMfiHttpClient::Options opt;
        opt.host = "127.0.0.1";
        opt.port = server.GetPort();
        RemoteMfiHttpClient client(opt);
        auto cert = client.FetchCertificate();
        assert(cert.IsError());
        assert(cert.Error().Code() == Common::StatusCode::ProtocolError);
    }
    {
        MockHttpMfiServer server([](const StubHttpRequest&) -> StubHttpResponse {
            return StubHttpResponse{200, "not json at all"};
        });
        RemoteMfiHttpClient::Options opt;
        opt.host = "127.0.0.1";
        opt.port = server.GetPort();
        RemoteMfiHttpClient client(opt);
        auto cert = client.FetchCertificate();
        assert(cert.IsError());
        assert(cert.Error().Code() == Common::StatusCode::ProtocolError);
    }

    // 4. Nothing listening -> DeviceUnavailable.
    {
        RemoteMfiHttpClient::Options opt;
        opt.host = "127.0.0.1";
        opt.port = 1;  // nothing bound here
        opt.max_retries = 0;
        opt.connect_timeout = std::chrono::milliseconds(500);
        RemoteMfiHttpClient client(opt);
        assert(!client.IsReachable());
        auto cert = client.FetchCertificate();
        assert(cert.IsError());
        assert(cert.Error().Code() == Common::StatusCode::DeviceUnavailable);
    }

    // 5. https is not implemented by this client.
    {
        RemoteMfiHttpClient::Options opt;
        opt.use_tls = true;
        RemoteMfiHttpClient client(opt);
        auto cert = client.FetchCertificate();
        assert(cert.IsError());
        assert(cert.Error().Code() == Common::StatusCode::NotSupported);
    }

    // 6. Challenge length is validated client-side before any request.
    {
        RemoteMfiHttpClient::Options opt;
        opt.host = "127.0.0.1";
        opt.port = 1;
        opt.max_retries = 0;
        RemoteMfiHttpClient client(opt);
        Common::ByteBuffer empty;
        auto bad = client.SignChallenge(empty);
        assert(bad.IsError());
        assert(bad.Error().Code() == Common::StatusCode::InvalidArgument);
    }

    std::cout << "[Test] RemoteMfiHttpClient error mapping passed.\n";
}

void TestRemoteMfiHttpClientHealthAndReset() {
    std::cout << "[Test] Running RemoteMfiHttpClient health/reset...\n";

    MockHttpMfiServer server([](const StubHttpRequest& req) -> StubHttpResponse {
        if (req.path == "/healthz") {
            return StubHttpResponse{200, "{\"ok\":true,\"chip\":\"ready\",\"reason\":\"\"}"};
        }
        if (req.path == "/mfi/reset") {
            assert(req.body == "{}");
            return StubHttpResponse{200, "{\"detail\":\"\"}"};
        }
        if (req.path == "/mfi/certificate") {
            return StubHttpResponse{200,
                "{\"type\":\"mfi\",\"protocolMajor\":2,\"certificate\":\"" +
                std::string(kStubCertBase64) + "\"}"};
        }
        return StubHttpResponse{404, "{\"detail\":\"unknown path\"}"};
    });

    RemoteMfiHttpClient::Options opt;
    opt.host = "127.0.0.1";
    opt.port = server.GetPort();
    RemoteMfiHttpClient client(opt);

    assert(client.IsReachable());
    auto health = client.GetHealth();
    assert(health.IsOk());
    assert(health->ok);
    assert(health->chip == "ready");

    // A certificate fetch primes the chip identity; protocolMajor=2 maps to v2.
    auto cert = client.FetchCertificate();
    assert(cert.IsOk());
    auto info = client.GetChipInfo();
    assert(info.present);
    assert(info.chip_id == 0x00020000u);

    // Reset drops the local cache so the next read re-queries the service.
    assert(client.ResetChip().IsOk());
    auto cert2 = client.FetchCertificate();
    assert(cert2.IsOk());
    bool saw_reset = false, saw_certificate_twice = false;
    int cert_count = 0;
    for (const auto& r : server.Requests()) {
        if (r.path == "/mfi/reset") {
            saw_reset = true;
        }
        if (r.path == "/mfi/certificate") {
            ++cert_count;
        }
    }
    saw_certificate_twice = cert_count >= 2;
    assert(saw_reset);
    assert(saw_certificate_twice);

    std::cout << "[Test] RemoteMfiHttpClient health/reset passed.\n";
}

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
    std::cout << "========================================\n";
    std::cout << "CarPlayStation Hardware & MFi Unit Tests\n";
    std::cout << "========================================\n";

    TestMockMfiSigner();
    TestFileMfiSigner();
    TestNetworkMfiClientSuccess();
    TestNetworkMfiClientTrickleAndTimeout();
    TestNetworkMfiClientRetryRecovery();
    TestRemoteMfiHttpClientSuccess();
    TestRemoteMfiHttpClientErrors();
    TestRemoteMfiHttpClientHealthAndReset();

    // Step 2 tests
    TestCh341Protocol();
    TestMockI2cAdapter();
    TestI2cMfiSigner();
    TestHardwareAdaptersContracts();

    // Step 3 tests
    TestRadioAdapterHelper();

    std::cout << "All Hardware & MFi tests passed successfully!\n";
    return 0;
}
