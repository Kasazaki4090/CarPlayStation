#include "Core/Session/IPairingStore.h"
#include "Core/Session/MockPairingStore.h"
#include "Core/Session/PairingStore.h"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <chrono>

using namespace CarPlayStation::Core;
using namespace CarPlayStation::Core::Session;
using namespace CarPlayStation::Core::Common;

// ----------------------------------------------------------------------------
// 1. MockPairingStore Unit Tests
// ----------------------------------------------------------------------------
void TestMockPairingStore() {
    std::cout << "[Test] Running MockPairingStore unit tests...\n";

    MockPairingStore store;
    assert(store.Count() == 0);
    assert(!store.IsPaired("device_1"));

    // 1. Invalid device saving fails
    PairedDevice invalid_dev{};
    assert(store.SaveDevice(invalid_dev).IsError());

    // 2. Add devices
    PairedDevice d1{};
    d1.device_id = "iphone_alice";
    d1.device_name = "Alice's iPhone 15 Pro";
    d1.bluetooth_mac = MacAddress(0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC);
    d1.wifi_mac = MacAddress(0x12, 0x34, 0x56, 0x78, 0x9A, 0xBD);
    d1.public_key = {0xAA, 0xBB, 0xCC, 0xDD};
    d1.last_connected_timestamp = 1700000000;

    PairedDevice d2{};
    d2.device_id = "iphone_bob";
    d2.device_name = "Bob's iPhone 14";
    d2.bluetooth_mac = MacAddress(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    d2.wifi_mac = MacAddress(0x00, 0x11, 0x22, 0x33, 0x44, 0x56);
    d2.public_key = {0x11, 0x22, 0x33, 0x44};
    d2.last_connected_timestamp = 1700005000;

    assert(store.SaveDevice(d1).IsOk());
    assert(store.SaveDevice(d2).IsOk());
    assert(store.Count() == 2);
    assert(store.IsPaired("iphone_alice"));
    assert(store.IsPaired("iphone_bob"));

    // 3. Query by ID
    auto q1 = store.GetDevice("iphone_alice");
    assert(q1.IsOk());
    assert(q1.Value().device_name == "Alice's iPhone 15 Pro");
    assert(q1.Value().bluetooth_mac == d1.bluetooth_mac);

    // 4. Query by MAC (Bluetooth & Wi-Fi)
    auto q_bt = store.FindByMac(d1.bluetooth_mac);
    assert(q_bt.IsOk());
    assert(q_bt.Value().device_id == "iphone_alice");

    auto q_wifi = store.FindByMac(d2.wifi_mac);
    assert(q_wifi.IsOk());
    assert(q_wifi.Value().device_id == "iphone_bob");

    MacAddress unk_mac(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
    assert(store.FindByMac(unk_mac).IsError());

    // 5. Sorted listing (most recent first)
    auto list = store.ListDevices();
    assert(list.size() == 2);
    assert(list[0].device_id == "iphone_bob"); // higher timestamp
    assert(list[1].device_id == "iphone_alice");

    // 6. Update existing record
    d1.device_name = "Alice's iPhone (Renamed)";
    d1.last_connected_timestamp = 1700010000;
    assert(store.SaveDevice(d1).IsOk());
    assert(store.Count() == 2);
    auto q1_upd = store.GetDevice("iphone_alice");
    assert(q1_upd.IsOk());
    assert(q1_upd.Value().device_name == "Alice's iPhone (Renamed)");

    auto list2 = store.ListDevices();
    assert(list2[0].device_id == "iphone_alice"); // Now Alice is most recent

    // 7. Error injection
    store.SetFailSaves(true);
    PairedDevice d3{};
    d3.device_id = "iphone_charlie";
    assert(store.SaveDevice(d3).IsError());
    store.SetFailSaves(false);

    // 8. Removal & Clear
    assert(store.RemoveDevice("iphone_bob").IsOk());
    assert(store.Count() == 1);
    assert(!store.IsPaired("iphone_bob"));
    assert(store.RemoveDevice("non_existent").IsError());

    store.Clear();
    assert(store.Count() == 0);

    std::cout << "  [PASS] TestMockPairingStore\n";
}

// ----------------------------------------------------------------------------
// 2. PairingStore Disk Persistence & Atomic File Replace Tests
// ----------------------------------------------------------------------------
void TestPairingStoreDiskPersistence() {
    std::cout << "[Test] Running PairingStore disk persistence tests...\n";

    namespace fs = std::filesystem;
    fs::path temp_dir = fs::temp_directory_path() / "cps_session_tests";
    fs::create_directories(temp_dir);
    fs::path store_path = temp_dir / "paired_devices_test.json";

    // Clean previous run
    std::error_code ec;
    fs::remove(store_path, ec);

    {
        // 1. Initial cold startup on non-existent file
        PairingStore store(store_path.string());
        assert(store.Count() == 0);
        assert(!fs::exists(store_path)); // shouldn't create until save

        // 2. Save devices
        PairedDevice dev1{};
        dev1.device_id = "test_dev_01";
        dev1.device_name = "Test Receiver Phone";
        dev1.bluetooth_mac = MacAddress(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01);
        dev1.wifi_mac = MacAddress(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02);
        dev1.public_key = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
        dev1.last_connected_timestamp = 1712345678;

        assert(store.SaveDevice(dev1).IsOk());
        assert(store.Count() == 1);
        assert(fs::exists(store_path));
    }

    {
        // 3. Re-instantiate from disk and verify exact data restored
        PairingStore store_reload(store_path.string());
        assert(store_reload.Count() == 1);
        assert(store_reload.IsPaired("test_dev_01"));

        auto res = store_reload.GetDevice("test_dev_01");
        assert(res.IsOk());
        const auto& d = res.Value();
        assert(d.device_name == "Test Receiver Phone");
        assert(d.bluetooth_mac.ToString() == "aa:bb:cc:dd:ee:01");
        assert(d.wifi_mac.ToString() == "aa:bb:cc:dd:ee:02");
        assert(d.public_key.size() == 7);
        assert(d.public_key[0] == 0xDE && d.public_key[1] == 0xAD);
        assert(d.last_connected_timestamp == 1712345678);

        // 4. Append another device and verify
        PairedDevice dev2{};
        dev2.device_id = "test_dev_02";
        dev2.device_name = "Secondary Phone";
        dev2.bluetooth_mac = MacAddress(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
        dev2.wifi_mac = MacAddress(0x11, 0x22, 0x33, 0x44, 0x55, 0x67);
        dev2.public_key = {0x01, 0x02};
        dev2.last_connected_timestamp = 1712350000;

        assert(store_reload.SaveDevice(dev2).IsOk());
        assert(store_reload.Count() == 2);
    }

    {
        // 5. Verify both devices loaded in third instance
        PairingStore store3(store_path.string());
        assert(store3.Count() == 2);
        auto list = store3.ListDevices();
        assert(list.size() == 2);
        assert(list[0].device_id == "test_dev_02"); // Most recent first
        assert(list[1].device_id == "test_dev_01");

        // 6. Removal and persistence
        assert(store3.RemoveDevice("test_dev_01").IsOk());
        assert(store3.Count() == 1);
    }

    {
        // 7. Verify deletion persisted
        PairingStore store4(store_path.string());
        assert(store4.Count() == 1);
        assert(!store4.IsPaired("test_dev_01"));
        assert(store4.IsPaired("test_dev_02"));

        store4.Clear();
        assert(store4.Count() == 0);
    }

    {
        // 8. Verify clear persisted
        PairingStore store5(store_path.string());
        assert(store5.Count() == 0);
    }

    // Cleanup temp test directory
    fs::remove_all(temp_dir, ec);

    std::cout << "  [PASS] TestPairingStoreDiskPersistence\n";
}

// ----------------------------------------------------------------------------
// 3. AirPlay RTSP Codec Tests (Framing, Headers, Body, Slicing)
// ----------------------------------------------------------------------------
#include "Core/Session/AirPlayRtspTypes.h"
#include "Core/Session/AirPlayRtspCodec.h"
#include "Core/Session/AirPlayRtspServer.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#endif

void TestRtspCodecParsingAndFraming() {
    std::cout << "[Test] Running AirPlayRtspCodec parsing and framing tests...\n";

    // 1. Single standalone OPTIONS request
    std::string_view opt_wire = "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: AirPlay/320.20\r\n\r\n";
    auto opt_res = AirPlayRtspCodec::ParseSingle(Common::ByteSpan(reinterpret_cast<const uint8_t*>(opt_wire.data()), opt_wire.size()));
    assert(opt_res.IsOk());
    assert(opt_res.Value().method == RtspMethod::Options);
    assert(opt_res.Value().uri == "*");
    assert(opt_res.Value().GetCSeq() == 1);
    assert(opt_res.Value().GetHeader("user-agent") == "AirPlay/320.20");
    assert(opt_res.Value().body.empty());

    // 2. Request with binary/text body and Content-Length
    std::string body_text = "stream_type=video\r\nwidth=1280\r\nheight=720\r\n";
    std::string setup_wire = "SETUP rtsp://192.168.1.100/stream RTSP/1.0\r\n"
                             "CSeq: 42\r\n"
                             "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;control_port=5001\r\n"
                             "Content-Length: " + std::to_string(body_text.size()) + "\r\n"
                             "\r\n" + body_text;

    auto setup_res = AirPlayRtspCodec::ParseSingle(Common::ByteSpan(reinterpret_cast<const uint8_t*>(setup_wire.data()), setup_wire.size()));
    assert(setup_res.IsOk());
    assert(setup_res.Value().method == RtspMethod::Setup);
    assert(setup_res.Value().Path() == "/stream");
    assert(setup_res.Value().GetCSeq() == 42);
    assert(setup_res.Value().body.size() == body_text.size());
    std::string parsed_body(reinterpret_cast<const char*>(setup_res.Value().body.data()), setup_res.Value().body.size());
    assert(parsed_body == body_text);

    // 3. Streaming chunked feed with partial packets
    AirPlayRtspCodec streaming_codec;
    assert(!streaming_codec.PopRequest().has_value());

    // Send half of the headers
    std::string part1 = "ANNOUNCE rtsp://carplay/stream RTSP/1.0\r\nCSeq: 2\r\nContent-Type:";
    streaming_codec.FeedData(Common::ByteSpan(reinterpret_cast<const uint8_t*>(part1.data()), part1.size()));
    assert(!streaming_codec.PopRequest().has_value());

    // Send rest of headers and half of body
    std::string part2 = " text/parameters\r\nContent-Length: 10\r\n\r\n12345";
    streaming_codec.FeedData(Common::ByteSpan(reinterpret_cast<const uint8_t*>(part2.data()), part2.size()));
    assert(!streaming_codec.PopRequest().has_value()); // Body needs 10 bytes, only 5 arrived

    // Send remaining body bytes
    std::string part3 = "67890";
    streaming_codec.FeedData(Common::ByteSpan(reinterpret_cast<const uint8_t*>(part3.data()), part3.size()));
    auto popped = streaming_codec.PopRequest();
    assert(popped.has_value());
    assert(popped->method == RtspMethod::Announce);
    assert(popped->GetCSeq() == 2);
    assert(popped->body.size() == 10);
    assert(std::string(reinterpret_cast<const char*>(popped->body.data()), 10) == "1234567890");
    assert(!streaming_codec.PopRequest().has_value());

    // 4. Consecutive coalesced requests in single buffer
    std::string two_reqs = "RECORD rtsp://carplay RTSP/1.0\r\nCSeq: 3\r\n\r\n"
                           "TEARDOWN rtsp://carplay RTSP/1.0\r\nCSeq: 4\r\n\r\n";
    streaming_codec.FeedData(Common::ByteSpan(reinterpret_cast<const uint8_t*>(two_reqs.data()), two_reqs.size()));

    auto r1 = streaming_codec.PopRequest();
    assert(r1.has_value() && r1->method == RtspMethod::Record && r1->GetCSeq() == 3);

    auto r2 = streaming_codec.PopRequest();
    assert(r2.has_value() && r2->method == RtspMethod::Teardown && r2->GetCSeq() == 4);

    assert(!streaming_codec.PopRequest().has_value());

    std::cout << "  [PASS] TestRtspCodecParsingAndFraming\n";
}

void TestRtspResponseSerialization() {
    std::cout << "[Test] Running AirPlayRtspCodec response serialization tests...\n";

    RtspRequest req{};
    req.protocol = "RTSP/1.0";
    req.headers["CSeq"] = "99";

    // 1. OPTIONS response
    auto opt_resp = AirPlayRtspCodec::BuildOptionsResponse(req);
    std::string opt_str(reinterpret_cast<const char*>(opt_resp.data()), opt_resp.size());
    assert(opt_str.find("RTSP/1.0 200 OK\r\n") != std::string::npos);
    assert(opt_str.find("CSeq: 99\r\n") != std::string::npos);
    assert(opt_str.find("Public: OPTIONS, ANNOUNCE, SETUP, RECORD") != std::string::npos);
    assert(opt_str.find("Server: AirTunes/220.68\r\n") != std::string::npos);

    // 2. SETUP response
    auto setup_resp = AirPlayRtspCodec::BuildSetupResponse(req, 5000, 5001, "sess_abc");
    std::string setup_str(reinterpret_cast<const char*>(setup_resp.data()), setup_resp.size());
    assert(setup_str.find("server_port=5000") != std::string::npos);
    assert(setup_str.find("control_port=5001") != std::string::npos);
    assert(setup_str.find("Session: sess_abc\r\n") != std::string::npos);

    // 3. Error response
    auto err_resp = AirPlayRtspCodec::BuildErrorResponse(req, 453, "Not Enough Bandwidth");
    std::string err_str(reinterpret_cast<const char*>(err_resp.data()), err_resp.size());
    assert(err_str.find("RTSP/1.0 453 Not Enough Bandwidth\r\n") != std::string::npos);

    std::cout << "  [PASS] TestRtspResponseSerialization\n";
}

// ----------------------------------------------------------------------------
// 4. AirPlay RTSP Server Lifecycle & Socket Verification
// ----------------------------------------------------------------------------
void TestRtspServerLifecycleAndSimulation() {
    std::cout << "[Test] Running AirPlayRtspServer lifecycle and socket verification...\n";

    // 1. In-memory simulation
    {
        AirPlayRtspServer server;
        RtspRequest sim_req{};
        sim_req.method = RtspMethod::Options;
        sim_req.protocol = "RTSP/1.0";
        sim_req.headers["CSeq"] = "5";

        auto sim_resp = server.ProcessSimulatedRequest(sim_req);
        assert(sim_resp.status_code == 200);
        assert(sim_resp.headers.find("Public") != sim_resp.headers.end());

        // Simulated SETUP negotiation
        RtspRequest sim_setup{};
        sim_setup.method = RtspMethod::Setup;
        sim_setup.headers["Transport"] = "RTP/AVP/UDP;unicast;control_port=5001";
        auto setup_resp = server.ProcessSimulatedRequest(sim_setup);
        assert(setup_resp.status_code == 200);
        assert(server.GetNegotiatedParams().video_data_port == 5000);
        assert(server.GetNegotiatedParams().video_control_port == 5001);
    }

    // 2. Real live loopback socket interaction with dynamic ephemeral port
    {
        AirPlayRtspServer::Options opts;
        opts.port = 0; // OS assigns ephemeral port
        opts.bind_address = "127.0.0.1";
        AirPlayRtspServer server(opts);

        assert(!server.IsRunning());
        assert(server.Start().IsOk());
        assert(server.IsRunning());

        uint16_t port = server.GetBoundPort();
        assert(port > 0);

        // Connect client TCP socket
        SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(client != INVALID_SOCKET);

        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

        int c_res = connect(client, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr));
        assert(c_res == 0);

        // Send OPTIONS
        std::string req1 = "OPTIONS * RTSP/1.0\r\nCSeq: 10\r\n\r\n";
        send(client, req1.data(), static_cast<int>(req1.size()), 0);

        char resp_buf[1024] = {0};
        int r = recv(client, resp_buf, sizeof(resp_buf) - 1, 0);
        assert(r > 0);
        std::string r_str(resp_buf, r);
        assert(r_str.find("RTSP/1.0 200 OK") != std::string::npos);
        assert(r_str.find("CSeq: 10") != std::string::npos);
        assert(r_str.find("Public: OPTIONS") != std::string::npos);

        // Send SETUP
        std::string req2 = "SETUP rtsp://127.0.0.1/video RTSP/1.0\r\n"
                           "CSeq: 11\r\n"
                           "Transport: RTP/AVP/UDP;unicast;client_port=6000;control_port=6001\r\n"
                           "\r\n";
        send(client, req2.data(), static_cast<int>(req2.size()), 0);

        std::memset(resp_buf, 0, sizeof(resp_buf));
        r = recv(client, resp_buf, sizeof(resp_buf) - 1, 0);
        assert(r > 0);
        std::string setup_resp_str(resp_buf, r);
        assert(setup_resp_str.find("RTSP/1.0 200 OK") != std::string::npos);
        assert(setup_resp_str.find("CSeq: 11") != std::string::npos);
        assert(setup_resp_str.find("server_port=5000") != std::string::npos);

        closesocket(client);

        // Allow worker cleanup
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        server.Stop();
        assert(!server.IsRunning());
    }

    std::cout << "  [PASS] TestRtspServerLifecycleAndSimulation\n";
}

// ----------------------------------------------------------------------------
// 5. CarPlaySessionStateMachine End-to-End Orchestration & Handoff Tests
// ----------------------------------------------------------------------------
#include "Core/Session/CarPlaySessionStateMachine.h"
#include "Core/Transport/MockWifiSoftApManager.h"
#include "Core/Transport/MockMdnsAdvertiser.h"

void TestCarPlayStateMachineLifecycleAndHandoff() {
    std::cout << "[Test] Running CarPlaySessionStateMachine lifecycle & Wi-Fi handoff tests...\n";

    auto store = std::make_shared<MockPairingStore>();
    auto wifi = std::make_shared<Transport::MockWifiSoftApManager>();
    auto mdns = std::make_shared<Transport::MockMdnsAdvertiser>();
    
    AirPlayRtspServer::Options rtsp_opts;
    rtsp_opts.port = 0; // ephemeral
    auto rtsp = std::make_shared<AirPlayRtspServer>(rtsp_opts);

    SessionConfig config;
    config.ap_ssid = "CarPlay_TestSSID";
    config.ap_passphrase = "SecretPassword123";
    config.ap_channel = 36;
    config.auto_reconnect = true;

    SessionDependencies deps;
    deps.pairing_store = store;
    deps.wifi_manager = wifi;
    deps.mdns_advertiser = mdns;
    deps.rtsp_server = rtsp;

    CarPlaySessionStateMachine sm(config, deps);
    assert(sm.GetState() == SessionState::Idle);

    std::vector<std::pair<SessionState, SessionState>> transitions;
    sm.RegisterStateObserver([&](SessionState old_s, SessionState new_s, Status st) {
        transitions.emplace_back(old_s, new_s);
    });

    // 1. Start session listener
    assert(sm.Start().IsOk());
    assert(sm.GetState() == SessionState::Idle);
    assert(rtsp->IsRunning());

    // 2. Discover Bluetooth iPhone (First-time pairing)
    MacAddress bt_mac(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
    assert(sm.OnBluetoothDeviceDiscovered("iphone_carol", bt_mac, "Carol's iPhone").IsOk());
    assert(sm.GetState() == SessionState::BtDiscovered);
    auto info = sm.GetActiveSessionInfo();
    assert(info.device_id == "iphone_carol");
    assert(info.bluetooth_mac == bt_mac);
    assert(!info.is_fast_reconnect);

    // 3. iAP2 Authentication succeeds
    assert(sm.OnIap2Authenticated(true, "Carol's iPhone 15").IsOk());
    assert(sm.GetState() == SessionState::WifiHandoff);

    // Verify SoftAP was automatically started with 5GHz config
    assert(wifi->IsRunning());
    assert(wifi->GetCredentials().ssid == "CarPlay_TestSSID");
    assert(wifi->GetCredentials().passphrase == "SecretPassword123");
    assert(wifi->GetCredentials().channel == 36);

    // Verify mDNS advertising was started
    assert(mdns->IsRunning());
    assert(mdns->GetConfig().instanceName == "CarPlay_TestSSID");

    // Verify device persisted in PairingStore
    assert(store->IsPaired("iphone_carol"));
    auto stored_dev = store->GetDevice("iphone_carol").Value();
    assert(stored_dev.device_name == "Carol's iPhone 15");

    // 4. Station associates to Wi-Fi SoftAP
    MacAddress wifi_mac(0x11, 0x22, 0x33, 0x44, 0x55, 0x67);
    assert(sm.OnWifiConnected(wifi_mac).IsOk());
    info = sm.GetActiveSessionInfo();
    assert(info.wifi_mac == wifi_mac);

    // 5. AirPlay RTSP negotiation succeeds and streaming starts
    NegotiatedMediaParams params{};
    params.video_width = 1920;
    params.video_height = 1080;
    params.fps = 60;
    params.video_data_port = 5000;
    params.video_control_port = 5001;
    params.main_audio_data_port = 5002;
    params.session_id = "sess_carol_001";

    assert(sm.OnAirPlayStreamingStarted(params).IsOk());
    assert(sm.GetState() == SessionState::AirPlayStreaming);
    info = sm.GetActiveSessionInfo();
    assert(info.media_params.video_width == 1920);
    assert(info.media_params.session_id == "sess_carol_001");

    // 6. Pause and Resume streaming
    assert(sm.OnAirPlayStreamingPaused().IsOk());
    assert(sm.GetState() == SessionState::Paused);

    assert(sm.OnAirPlayStreamingResumed().IsOk());
    assert(sm.GetState() == SessionState::AirPlayStreaming);

    // 7. Teardown
    assert(sm.OnAirPlayTeardown().IsOk());
    assert(sm.GetState() == SessionState::Reconnecting);

    // Reset back to Idle for next test
    sm.Reset();
    assert(sm.GetState() == SessionState::Idle);

    // 8. Second connection: Fast Reconnect detection
    assert(sm.OnBluetoothDeviceDiscovered("iphone_carol", bt_mac).IsOk());
    assert(sm.GetState() == SessionState::BtDiscovered);
    info = sm.GetActiveSessionInfo();
    assert(info.is_fast_reconnect == true);
    assert(info.device_name == "Carol's iPhone 15");
    assert(info.wifi_mac == wifi_mac);

    // Stop session
    sm.Stop();
    assert(sm.GetState() == SessionState::Idle);
    assert(!wifi->IsRunning());
    assert(!mdns->IsRunning());
    assert(!rtsp->IsRunning());

    std::cout << "  [PASS] TestCarPlayStateMachineLifecycleAndHandoff\n";
}

// ----------------------------------------------------------------------------
// 6. CarPlaySessionStateMachine Watchdog, Timeout & Error Recovery Tests
// ----------------------------------------------------------------------------
void TestCarPlayStateMachineWatchdogAndReconnect() {
    std::cout << "[Test] Running CarPlaySessionStateMachine watchdog & reconnect tests...\n";

    SessionConfig config;
    config.bt_auth_timeout = std::chrono::milliseconds(50);
    config.wifi_handoff_timeout = std::chrono::milliseconds(50);
    config.reconnect_interval = std::chrono::milliseconds(30);
    config.max_reconnect_attempts = 2;
    config.auto_reconnect = true;

    CarPlaySessionStateMachine sm(config);
    assert(sm.Start().IsOk());

    // 1. iAP2 Auth Timeout triggers Error state
    MacAddress mac(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF);
    assert(sm.OnBluetoothDeviceDiscovered("dev_timeout", mac).IsOk());
    assert(sm.GetState() == SessionState::BtDiscovered);

    // Tick watchdog before timeout -> no state change
    sm.TickWatchdog(std::chrono::steady_clock::now());
    assert(sm.GetState() == SessionState::BtDiscovered);

    // Advance time past 50ms timeout
    auto future_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
    sm.TickWatchdog(future_time);
    assert(sm.GetState() == SessionState::Error);

    // 2. Authentication failure triggers Error state
    sm.Reset();
    assert(sm.OnBluetoothDeviceDiscovered("dev_fail", mac).IsOk());
    assert(sm.OnIap2Authenticated(false).IsError());
    assert(sm.GetState() == SessionState::Error);

    // 3. Wi-Fi Handoff Timeout triggers Reconnecting and Max Retries
    sm.Reset();
    assert(sm.OnBluetoothDeviceDiscovered("dev_handoff", mac).IsOk());
    assert(sm.OnIap2Authenticated(true).IsOk());
    assert(sm.GetState() == SessionState::WifiHandoff);

    // Tick past handoff timeout -> First retry into Reconnecting
    auto t1 = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
    sm.TickWatchdog(t1);
    assert(sm.GetState() == SessionState::Reconnecting);
    assert(sm.GetActiveSessionInfo().reconnect_attempts == 1);

    // Tick past reconnect interval -> enters WifiHandoff retry
    auto t2 = t1 + std::chrono::milliseconds(40);
    sm.TickWatchdog(t2);
    assert(sm.GetState() == SessionState::WifiHandoff);

    // Second timeout -> Reconnecting with attempt 2
    auto t3 = t2 + std::chrono::milliseconds(60);
    sm.TickWatchdog(t3);
    assert(sm.GetState() == SessionState::Reconnecting);
    assert(sm.GetActiveSessionInfo().reconnect_attempts == 2);

    // Tick past interval with max attempts (2) reached -> Error
    auto t4 = t3 + std::chrono::milliseconds(40);
    sm.TickWatchdog(t4);
    assert(sm.GetState() == SessionState::Error);

    std::cout << "  [PASS] TestCarPlayStateMachineWatchdogAndReconnect\n";
}

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif

    std::cout << "==========================================\n";
    std::cout << "Running CarPlayStation Session Tests (Part 4)\n";
    std::cout << "==========================================\n";

    // Step 1 tests
    TestMockPairingStore();
    TestPairingStoreDiskPersistence();

    // Step 2 tests
    TestRtspCodecParsingAndFraming();
    TestRtspResponseSerialization();
    TestRtspServerLifecycleAndSimulation();

    // Step 3 & 4 tests
    TestCarPlayStateMachineLifecycleAndHandoff();
    TestCarPlayStateMachineWatchdogAndReconnect();

    std::cout << "==========================================\n";
    std::cout << "All Session (Part 4) Steps 1-4 Tests Passed!\n";
    std::cout << "==========================================\n";
    return 0;
}


