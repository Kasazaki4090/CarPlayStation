#include "Core/Transport/ITransportChannel.h"
#include "Core/Transport/MockTransportChannel.h"
#include "Core/Transport/BtSdpHelper.h"
#include "Core/Transport/BtRfcommTransport.h"
#include "Core/Transport/IWifiSoftApManager.h"
#include "Core/Transport/WifiSoftApManager.h"
#include "Core/Transport/MockWifiSoftApManager.h"
#include "Core/Transport/IMdnsAdvertiser.h"
#include "Core/Transport/MdnsPacketBuilder.h"
#include "Core/Transport/MdnsAdvertiser.h"
#include "Core/Transport/MockMdnsAdvertiser.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <string>

using namespace CarPlayStation::Core;
using namespace CarPlayStation::Core::Transport;
using namespace CarPlayStation::Core::Common;

void TestMockTransportBasics() {
    auto channel = std::make_shared<MockTransportChannel>();
    assert(channel->GetType() == TransportType::MockLoopback);
    assert(channel->GetState() == TransportState::Connected);
    assert(channel->IsConnected());

    channel->SetRemoteEndpoint("mock://test-endpoint");
    assert(channel->GetRemoteEndpoint() == "mock://test-endpoint");

    const uint8_t sampleData[] = {0x01, 0x02, 0x03, 0x04};
    Status status = channel->Send(ByteSpan(sampleData, sizeof(sampleData)));
    assert(status.IsOk());

    assert(channel->GetSentPacketCount() == 1);
    auto lastPkt = channel->GetLastSentPacket();
    assert(lastPkt.has_value());
    assert(lastPkt->size() == 4);
    assert((*lastPkt)[0] == 0x01 && (*lastPkt)[3] == 0x04);

    TransportStats stats = channel->GetStats();
    assert(stats.bytes_sent == 4);
    assert(stats.packets_sent == 1);
    assert(stats.errors_count == 0);

    channel->ClearSentPackets();
    assert(channel->GetSentPacketCount() == 0);

    channel->ResetStats();
    assert(channel->GetStats().bytes_sent == 0);

    std::cout << "[PASS] TestMockTransportBasics\n";
}

void TestMockTransportSimulation() {
    auto channel = std::make_shared<MockTransportChannel>();

    std::vector<uint8_t> receivedData;
    channel->SetDataCallback([&](ByteSpan data) {
        receivedData.assign(data.begin(), data.end());
    });

    TransportState latestState = TransportState::Connected;
    Status latestStatus = Status::Ok();
    channel->SetStateCallback([&](TransportState newState, Status st) {
        latestState = newState;
        latestStatus = st;
    });

    // 1. Simulate inbound data
    const uint8_t inData[] = {0xAA, 0xBB, 0xCC};
    channel->SimulateInboundData(ByteSpan(inData, sizeof(inData)));
    assert(receivedData.size() == 3);
    assert(receivedData[0] == 0xAA && receivedData[2] == 0xCC);
    assert(channel->GetStats().bytes_received == 3);
    assert(channel->GetStats().packets_received == 1);

    // 2. Simulate disconnect
    channel->SimulateDisconnect(Status::Error(StatusCode::ConnectionReset, "Peer closed"));
    assert(channel->GetState() == TransportState::Disconnected);
    assert(!channel->IsConnected());
    assert(latestState == TransportState::Disconnected);
    assert(latestStatus.Code() == StatusCode::ConnectionReset);

    // Cannot send while disconnected
    Status sendRes = channel->Send(ByteSpan(inData, sizeof(inData)));
    assert(sendRes.IsError());
    assert(sendRes.Code() == StatusCode::InvalidState);

    // 3. Simulate reconnect
    channel->SimulateConnect("mock://reconnected");
    assert(channel->IsConnected());
    assert(channel->GetRemoteEndpoint() == "mock://reconnected");
    assert(latestState == TransportState::Connected);

    // 4. Simulate error
    channel->SimulateError(Status::Error(StatusCode::IoError, "Socket error"));
    assert(channel->GetState() == TransportState::Error);
    assert(latestState == TransportState::Error);
    assert(latestStatus.Code() == StatusCode::IoError);
    assert(channel->GetStats().errors_count > 0);

    std::cout << "[PASS] TestMockTransportSimulation\n";
}

void TestMockTransportFaultInjection() {
    auto channel = std::make_shared<MockTransportChannel>();

    // 1. Injected send failure
    channel->SetFailSends(true, Status::Error(StatusCode::Unavailable, "Radio busy"));
    const uint8_t data[] = {0x11, 0x22};
    Status failStatus = channel->Send(ByteSpan(data, sizeof(data)));
    assert(failStatus.IsError());
    assert(failStatus.Code() == StatusCode::Unavailable);
    assert(failStatus.Message() == "Radio busy");

    channel->SetFailSends(false);
    assert(channel->Send(ByteSpan(data, sizeof(data))).IsOk());

    // 2. MTU enforcement
    channel->SetMtu(4);
    assert(channel->GetMtu() == 4);
    const uint8_t smallData[] = {1, 2, 3};
    const uint8_t largeData[] = {1, 2, 3, 4, 5};
    assert(channel->Send(ByteSpan(smallData, sizeof(smallData))).IsOk());

    Status mtuStatus = channel->Send(ByteSpan(largeData, sizeof(largeData)));
    assert(mtuStatus.IsError());
    assert(mtuStatus.Code() == StatusCode::InvalidArgument);

    std::cout << "[PASS] TestMockTransportFaultInjection\n";
}

void TestMockTransportLoopbackPairing() {
    auto client = std::make_shared<MockTransportChannel>();
    auto server = std::make_shared<MockTransportChannel>();

    MockTransportChannel::Pair(client, server);

    std::vector<uint8_t> clientInbox;
    std::vector<uint8_t> serverInbox;

    client->SetDataCallback([&](ByteSpan data) {
        clientInbox.assign(data.begin(), data.end());
    });

    server->SetDataCallback([&](ByteSpan data) {
        serverInbox.assign(data.begin(), data.end());
    });

    // Client -> Server
    const uint8_t clientMsg[] = {'P', 'I', 'N', 'G'};
    Status s1 = client->Send(ByteSpan(clientMsg, sizeof(clientMsg)));
    assert(s1.IsOk());
    assert(serverInbox.size() == 4);
    assert(serverInbox[0] == 'P' && serverInbox[3] == 'G');

    // Server -> Client
    const uint8_t serverMsg[] = {'P', 'O', 'N', 'G'};
    Status s2 = server->Send(ByteSpan(serverMsg, sizeof(serverMsg)));
    assert(s2.IsOk());
    assert(clientInbox.size() == 4);
    assert(clientInbox[0] == 'P' && clientInbox[3] == 'G');

    // Verify stats
    assert(client->GetStats().bytes_sent == 4);
    assert(client->GetStats().bytes_received == 4);
    assert(server->GetStats().bytes_sent == 4);
    assert(server->GetStats().bytes_received == 4);

    // Break link
    client->ClearPeer();
    serverInbox.clear();
    client->Send(ByteSpan(clientMsg, sizeof(clientMsg)));
    assert(serverInbox.empty()); // No peer delivery

    std::cout << "[PASS] TestMockTransportLoopbackPairing\n";
}

void TestBtSdpRecordGenerationAndValidation() {
    // 1. Build channel 1 record
    auto record1 = BtSdpHelper::BuildIap2SdpRecord(1, false);
    assert(record1.size() == BtSdpHelper::kSdpRecordSize);
    assert(record1[BtSdpHelper::kRfcommChannelByteOffset] == 1);
    assert(record1[31] == 0xff);
    assert(BtSdpHelper::ValidateIap2SdpRecord(ByteSpan(record1.data(), record1.size()), 1));
    assert(!BtSdpHelper::ValidateIap2SdpRecord(ByteSpan(record1.data(), record1.size()), 2)); // Wrong channel

    // 2. Build channel 2 legacy record
    auto record2 = BtSdpHelper::BuildIap2SdpRecord(2, true);
    assert(record2[BtSdpHelper::kRfcommChannelByteOffset] == 2);
    assert(record2[31] == 0xfe);
    assert(BtSdpHelper::ValidateIap2SdpRecord(ByteSpan(record2.data(), record2.size()), 2));

    // 3. Corrupted record check
    auto badRecord = record1;
    badRecord[0] = 0x00; // corrupt header
    assert(!BtSdpHelper::ValidateIap2SdpRecord(ByteSpan(badRecord.data(), badRecord.size()), 1));

    std::cout << "[PASS] TestBtSdpRecordGenerationAndValidation\n";
}

void TestBluetoothAddressFormattingAndParsing() {
    // 1. Format
    uint64_t addr = 0x001122334455ULL;
    std::string formatted = BtSdpHelper::FormatBluetoothAddress(addr);
    assert(formatted == "00:11:22:33:44:55");

    // 2. Parse colon format
    uint64_t parsed = BtSdpHelper::ParseBluetoothAddress("00:11:22:33:44:55");
    assert(parsed == addr);

    // 3. Parse dash format
    uint64_t parsedDash = BtSdpHelper::ParseBluetoothAddress("00-11-22-33-44-55");
    assert(parsedDash == addr);

    // 4. Parse malformed
    uint64_t bad = BtSdpHelper::ParseBluetoothAddress("invalid");
    assert(bad == 0);

    std::cout << "[PASS] TestBluetoothAddressFormattingAndParsing\n";
}

void TestBtRfcommTransportLifecycleAndSimulation() {
    auto bt = std::make_shared<BtRfcommTransport>(1);
    assert(bt->GetType() == TransportType::BluetoothRfcomm);
    assert(bt->GetMtu() == 1012);
    assert(bt->GetChannel() == 1);
    assert(bt->GetState() == TransportState::Disconnected);
    assert(!bt->IsListening());

    // Enable simulation mode for deterministic test
    bt->EnableSimulatedMode(true);
    assert(bt->IsSimulatedMode());

    Status startRes = bt->StartServer(1);
    assert(startRes.IsOk());
    assert(bt->IsListening());

    std::vector<uint8_t> received;
    bt->SetDataCallback([&](ByteSpan data) {
        received.assign(data.begin(), data.end());
    });

    TransportState latestState = TransportState::Disconnected;
    bt->SetStateCallback([&](TransportState newState, Status) {
        latestState = newState;
    });

    // Simulate iPhone connect
    bt->SimulatePeerConnect("DC:A9:04:12:34:56");
    assert(bt->GetState() == TransportState::Connected);
    assert(latestState == TransportState::Connected);
    assert(bt->GetRemoteEndpoint().find("DC:A9:04:12:34:56") != std::string::npos);

    // Simulate inbound iAP2 packet
    const uint8_t iap2Frame[] = {0xFF, 0x5A, 0x00, 0x0A, 0x01, 0x02};
    bt->SimulatePeerData(ByteSpan(iap2Frame, sizeof(iap2Frame)));
    assert(received.size() == sizeof(iap2Frame));
    assert(received[0] == 0xFF && received[1] == 0x5A);
    assert(bt->GetStats().bytes_received == sizeof(iap2Frame));

    // Outbound send
    const uint8_t replyFrame[] = {0xFF, 0x5A, 0x00, 0x08, 0xAA, 0xBB};
    Status sendRes = bt->Send(ByteSpan(replyFrame, sizeof(replyFrame)));
    assert(sendRes.IsOk());
    assert(bt->GetStats().bytes_sent == sizeof(replyFrame));

    // Simulate iPhone disconnect
    bt->SimulatePeerDisconnect();
    assert(bt->GetState() == TransportState::Disconnected);
    assert(latestState == TransportState::Disconnected);

    // Stop server
    bt->StopServer();
    assert(!bt->IsListening());

    std::cout << "[PASS] TestBtRfcommTransportLifecycleAndSimulation\n";
}

void TestWifiCredentialsGenerationAndValidation() {
    auto creds = WifiSoftApManager::GenerateRandomCredentials("CarPlayStation-", 40);
    assert(creds.ssid.rfind("CarPlayStation-", 0) == 0);
    assert(creds.passphrase.size() == 10);
    assert(creds.band == WifiBand::Band5GHz);
    assert(creds.channel == 40);
    assert(creds.security == WifiSecurityType::Wpa2Personal);
    assert(!creds.gatewayIp.empty());

    assert(WifiBandToString(WifiBand::Band5GHz) == "5GHz");
    assert(WifiBandToString(WifiBand::Band2_4GHz) == "2.4GHz");
    assert(WifiSecurityTypeToString(WifiSecurityType::Wpa2Personal) == "WPA2-Personal");
    assert(ApStateToString(ApState::Running) == "Running");

    std::cout << "[PASS] TestWifiCredentialsGenerationAndValidation\n";
}

void TestWifiSoftApManagerLifecycleAndWatchdog() {
    auto ap = std::make_shared<WifiSoftApManager>();
    assert(ap->GetState() == ApState::Disabled);
    assert(!ap->IsRunning());

    ap->EnableSimulatedMode(true);
    assert(ap->IsSimulatedMode());

    // Callbacks
    std::string connectedMac;
    std::string disconnectedMac;
    ApState latestApState = ApState::Disabled;

    ap->SetStationConnectedCallback([&](const StationInfo& info) {
        connectedMac = info.macAddress;
    });

    ap->SetStationDisconnectedCallback([&](const std::string& mac) {
        disconnectedMac = mac;
    });

    ap->SetStateCallback([&](ApState state, Status) {
        latestApState = state;
    });

    // Start AP
    auto creds = WifiSoftApManager::GenerateRandomCredentials("TestAP-", 36);
    Status startStatus = ap->StartAccessPoint(creds);
    assert(startStatus.IsOk());
    assert(ap->IsRunning());
    assert(latestApState == ApState::Running);
    assert(ap->GetCredentials().ssid == creds.ssid);

    // Watchdog simulation: iPhone joins
    ap->SimulateStationJoin("AA:BB:CC:11:22:33", "192.168.137.10");
    assert(connectedMac == "AA:BB:CC:11:22:33");
    assert(ap->GetConnectedStationCount() == 1);
    auto stations = ap->GetConnectedStations();
    assert(stations.size() == 1);
    assert(stations[0].ipAddress == "192.168.137.10");

    // Watchdog simulation: iPhone leaves
    ap->SimulateStationLeave("AA:BB:CC:11:22:33");
    assert(disconnectedMac == "AA:BB:CC:11:22:33");
    assert(ap->GetConnectedStationCount() == 0);

    // Stop AP
    ap->StopAccessPoint();
    assert(!ap->IsRunning());
    assert(ap->GetState() == ApState::Disabled);
    assert(latestApState == ApState::Disabled);

    std::cout << "[PASS] TestWifiSoftApManagerLifecycleAndWatchdog\n";
}

void TestMockWifiSoftApManager() {
    auto mockAp = std::make_shared<MockWifiSoftApManager>();
    assert(!mockAp->IsRunning());

    // Injected failure
    mockAp->SetFailStart(true);
    WifiApCredentials creds;
    creds.ssid = "MockSSID";
    creds.passphrase = "MockSecret123";
    Status failStatus = mockAp->StartAccessPoint(creds);
    assert(failStatus.IsError());
    assert(failStatus.Code() == StatusCode::Unavailable);

    // Clear failure and start
    mockAp->SetFailStart(false);
    assert(mockAp->StartAccessPoint(creds).IsOk());
    assert(mockAp->IsRunning());

    bool joined = false;
    mockAp->SetStationConnectedCallback([&](const StationInfo&) {
        joined = true;
    });

    mockAp->SimulateStationJoin("12:34:56:78:9A:BC");
    assert(joined);
    assert(mockAp->GetConnectedStationCount() == 1);

    mockAp->StopAccessPoint();
    assert(!mockAp->IsRunning());
    assert(mockAp->GetConnectedStationCount() == 0);

    std::cout << "[PASS] TestMockWifiSoftApManager\n";
}

void TestMdnsPacketBuilder() {
    // 1. Name encoding
    auto encoded = MdnsPacketBuilder::EncodeDnsName("_airplay._tcp.local.");
    assert(!encoded.empty());
    assert(encoded[0] == 8); // length of "_airplay"
    assert(encoded[encoded.size() - 1] == 0x00); // root label

    // 2. AirPlay announcement packet
    MdnsServiceConfig cfg;
    cfg.instanceName = "CarPlay";
    cfg.serviceType = "_airplay._tcp.local.";
    cfg.hostname = "CarPlayStation.local.";
    cfg.port = 5000;
    cfg.ipv4Address = "192.168.137.1";
    cfg.deviceId = "AA:BB:CC:DD:EE:01";
    cfg.features = "0x44440B80,0x61";
    cfg.flags = "0x4";
    cfg.model = "CarPlayStation-1.0";

    auto packet = MdnsPacketBuilder::BuildAirPlayAnnouncement(cfg);
    assert(packet.size() > 100);
    assert(MdnsPacketBuilder::ValidateMdnsHeader(ByteSpan(packet.data(), packet.size()), 4));

    // Verify presence of feature bit string and device id in raw packet
    std::string packetStr(packet.begin(), packet.end());
    assert(packetStr.find("deviceid=AA:BB:CC:DD:EE:01") != std::string::npos);
    assert(packetStr.find("features=0x44440B80,0x61") != std::string::npos);
    assert(packetStr.find("CarPlay") != std::string::npos);

    std::cout << "[PASS] TestMdnsPacketBuilder\n";
}

void TestMdnsCarPlayControlQueries() {
    // 1. Browse query: one PTR question for _carplay-ctrl._tcp.local.
    auto browse = MdnsPacketBuilder::BuildCarPlayControlQuery();
    assert(browse.size() > 12);
    assert(((browse[4] << 8) | browse[5]) == 1);                 // QDCOUNT
    assert(std::string(browse.begin(), browse.end()).find("_carplay-ctrl") != std::string::npos);

    // 2. Two-step resolution: SRV + A for the instance learned from the PTR answer.
    auto lookup = MdnsPacketBuilder::BuildCarPlayControlLookup("iPhone._carplay-ctrl._tcp.local.");
    assert(((lookup[4] << 8) | lookup[5]) == 2);                 // QDCOUNT = SRV + A
    assert(((lookup[6] << 8) | lookup[7]) == 0);                 // ANCOUNT = 0
    {
        std::string s(lookup.begin(), lookup.end());
        assert(s.find("iPhone._carplay-ctrl") != std::string::npos);
        const uint8_t srv[] = { 0x00, 0x21 };                    // QTYPE = SRV (33)
        const uint8_t a[]   = { 0x00, 0x01 };                    // QTYPE = A (1)
        assert(s.find(std::string(reinterpret_cast<const char*>(srv), 2)) != std::string::npos);
        assert(s.find(std::string(reinterpret_cast<const char*>(a), 2)) != std::string::npos);
    }

    // 3. Empty instance falls back to a browse so we never send an empty question name.
    auto fallback = MdnsPacketBuilder::BuildCarPlayControlLookup("");
    assert(((fallback[4] << 8) | fallback[5]) == 1);
    assert(std::string(fallback.begin(), fallback.end()).find("_carplay-ctrl") != std::string::npos);

    std::cout << "[PASS] TestMdnsCarPlayControlQueries\n";
}

void TestMdnsAdvertiserLifecycle() {
    auto advertiser = std::make_shared<MdnsAdvertiser>();
    assert(!advertiser->IsRunning());

    advertiser->EnableSimulatedMode(true);
    assert(advertiser->IsSimulatedMode());

    MdnsServiceConfig cfg;
    cfg.instanceName = "CarPlay";
    cfg.serviceType = "_airplay._tcp.local.";
    cfg.port = 5000;

    Status startRes = advertiser->Start(cfg);
    assert(startRes.IsOk());
    assert(advertiser->IsRunning());
    assert(advertiser->GetBroadcastCount() >= 1);

    auto lastPkt = advertiser->GetLastBroadcastPacket();
    assert(lastPkt.has_value());
    assert(MdnsPacketBuilder::ValidateMdnsHeader(ByteSpan(lastPkt->data(), lastPkt->size()), 4));

    // Manual burst trigger
    size_t countBefore = advertiser->GetBroadcastCount();
    assert(advertiser->BroadcastAnnouncement().IsOk());
    assert(advertiser->GetBroadcastCount() == countBefore + 1);

    advertiser->Stop();
    assert(!advertiser->IsRunning());

    std::cout << "[PASS] TestMdnsAdvertiserLifecycle\n";
}

void TestMockMdnsAdvertiser() {
    auto mock = std::make_shared<MockMdnsAdvertiser>();
    assert(!mock->IsRunning());

    MdnsServiceConfig cfg;
    assert(mock->Start(cfg).IsOk());
    assert(mock->IsRunning());
    assert(mock->GetBroadcastCount() == 1);

    assert(mock->BroadcastAnnouncement().IsOk());
    assert(mock->GetBroadcastCount() == 2);

    mock->Stop();
    assert(!mock->IsRunning());

    std::cout << "[PASS] TestMockMdnsAdvertiser\n";
}

int main() {
    std::cout << "==========================================\n";
    std::cout << "Running CarPlayStation TransportTests...\n";
    std::cout << "==========================================\n";

    TestMockTransportBasics();
    TestMockTransportSimulation();
    TestMockTransportFaultInjection();
    TestMockTransportLoopbackPairing();

    // Step 2 tests
    TestBtSdpRecordGenerationAndValidation();
    TestBluetoothAddressFormattingAndParsing();
    TestBtRfcommTransportLifecycleAndSimulation();

    // Step 3 tests
    TestWifiCredentialsGenerationAndValidation();
    TestWifiSoftApManagerLifecycleAndWatchdog();
    TestMockWifiSoftApManager();

    // Step 4 tests
    TestMdnsPacketBuilder();
    TestMdnsCarPlayControlQueries();
    TestMdnsAdvertiserLifecycle();
    TestMockMdnsAdvertiser();

    std::cout << "==========================================\n";
    std::cout << "All TransportTests passed successfully!\n";
    std::cout << "==========================================\n";
    return 0;
}
