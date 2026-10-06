#include "Core/Protocol/Iap2LinkEngine.h"
#include "Core/Protocol/Iap2ControlPlane.h"
#include "Core/Protocol/Iap2MessageCatalog.h"
#include "Core/Protocol/Iap2SessionManager.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace CarPlayStation::Core;
using namespace CarPlayStation::Core::Protocol;
using namespace CarPlayStation::Core::Common;

// ============================================================================
// Test: Checksum correctness against reference vectors
// ============================================================================
void TestIap2Checksum() {
    // Reference vector: bytes [0x89, 0x4B] -> sum=0xD4, (0x100 - 0xD4) & 0xFF = 0x2C
    const uint8_t data1[] = {0x89, 0x4B};
    assert(Iap2Checksum(ByteSpan(data1, 2)) == 0x2C);

    // Empty span: sum=0, (0x100 - 0) & 0xFF = 0x00
    assert(Iap2Checksum(ByteSpan{}) == 0x00);

    // Single byte 0xFF: (0x100 - 0xFF) & 0xFF = 0x01
    const uint8_t data2[] = {0xFF};
    assert(Iap2Checksum(ByteSpan(data2, 1)) == 0x01);

    std::cout << "  [PASS] TestIap2Checksum\n";
}

// ============================================================================
// Test: BuildSyn produces correct 26-byte frame with valid checksums
// ============================================================================
void TestBuildSyn() {
    Iap2LinkEngine engine;
    auto syn = engine.BuildSyn();

    assert(syn.size() == 26);
    assert(syn[0] == 0xFF); // SOP1
    assert(syn[1] == 0x5A); // SOP2
    assert(syn[2] == 0x00); // total high
    assert(syn[3] == 0x1A); // total low (26)
    assert(syn[4] == 0x80); // CTL_SYN
    assert(syn[5] == 0x00); // seq = 0 (first packet)

    // Header checksum valid?
    uint32_t sum = 0;
    for (int i = 0; i < 8; ++i) sum += syn[i];
    assert(syn[8] == static_cast<uint8_t>((0x100 - (sum & 0xFF)) & 0xFF));

    // Payload checksum: last byte must be checksum of bytes [9..24]
    uint32_t psum = 0;
    for (int i = 9; i < 25; ++i) psum += syn[i];
    assert(syn[25] == static_cast<uint8_t>((0x100 - (psum & 0xFF)) & 0xFF));

    // Seq should have advanced to 1 after SYN.
    assert(engine.GetMySeq() == 1);

    std::cout << "  [PASS] TestBuildSyn\n";
}

// ============================================================================
// Test: FeedData parses a valid SYN-ACK and advances peer_seq_
// ============================================================================
void TestFeedDataSynAck() {
    Iap2LinkEngine engine;

    // Build a minimal SYN-ACK frame (9 bytes header only, no payload).
    ByteBuffer frame;
    frame.reserve(9);
    frame.push_back(0xFF);
    frame.push_back(0x5A);
    frame.push_back(0x00);
    frame.push_back(0x09); // total = 9
    frame.push_back(0xC0); // CTL_SYN | CTL_ACK
    frame.push_back(0x42); // peer seq (arbitrary)
    frame.push_back(0x01); // ack (our SYN seq + 1)
    frame.push_back(0x00); // session = 0

    uint32_t sum = 0;
    for (int i = 0; i < 8; ++i) sum += frame[i];
    frame.push_back(static_cast<uint8_t>((0x100 - (sum & 0xFF)) & 0xFF));

    auto status = engine.FeedData(ByteSpan(frame.data(), frame.size()));
    assert(status.IsOk());
    assert(engine.GetPeerSeq() == 0x42);

    std::cout << "  [PASS] TestFeedDataSynAck\n";
}

// ============================================================================
// Test: FeedData with corrupted header checksum is rejected (no crash, resync)
// ============================================================================
void TestFeedDataCorruptedHeader() {
    Iap2LinkEngine engine;

    // Build a valid frame then corrupt one byte in the header.
    ByteBuffer frame;
    frame.push_back(0xFF);
    frame.push_back(0x5A);
    frame.push_back(0x00);
    frame.push_back(0x09);
    frame.push_back(0x40); // CTL_ACK only
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back(0x01);

    uint32_t sum = 0;
    for (int i = 0; i < 8; ++i) sum += frame[i];
    frame.push_back(static_cast<uint8_t>((0x100 - (sum & 0xFF)) & 0xFF));

    // Corrupt the control byte.
    frame[4] ^= 0xFF;

    auto status = engine.FeedData(ByteSpan(frame.data(), frame.size()));
    assert(status.IsOk()); // Should not error, just skip/resync

    std::cout << "  [PASS] TestFeedDataCorruptedHeader\n";
}

// ============================================================================
// Test: BuildMessage wraps payload correctly with [40 40] prefix
// ============================================================================
void TestBuildMessage() {
    Iap2LinkEngine engine;

    // First consume a seq via SYN so we start at seq=1.
    (void)engine.BuildSyn();

    const uint8_t body[] = {0x01, 0x02, 0x03};
    auto pkt = engine.BuildMessage(1, Iap2MsgId::AuthCertificate, ByteSpan(body, sizeof(body)));

    // Total size: 9 (header) + (6 + 3) (ctrl payload) + 1 (payload cks) = 19
    assert(pkt.size() == 19);
    assert(pkt[0] == 0xFF);
    assert(pkt[1] == 0x5A);
    assert(pkt[2] == 0x00);
    assert(pkt[3] == 0x13); // 19

    // Control payload starts at offset 9: [40 40][total BE16][msg_id BE16][body...]
    assert(pkt[9] == 0x40);
    assert(pkt[10] == 0x40);
    const uint16_t innerTotal = static_cast<uint16_t>((pkt[11] << 8) | pkt[12]);
    assert(innerTotal == 9); // 6 + 3

    const uint16_t msgId = static_cast<uint16_t>((pkt[13] << 8) | pkt[14]);
    assert(msgId == Iap2MsgId::AuthCertificate);

    std::cout << "  [PASS] TestBuildMessage\n";
}

// ============================================================================
// Test: TlvBuilder produces correct group encoding
// ============================================================================
void TestTlvBuilder() {
    TlvBuilder tlv;
    tlv.Str(0, "Hello");       // len=4+5+1=10, pid=0, "Hello\0"
    tlv.U8(8, 2);              // len=5, pid=8, value=2

    auto& data = tlv.Data();
    // First group: [0x00 0x0A][0x00 0x00]["Hello\0"]
    assert(data[0] == 0x00 && data[1] == 0x0A);
    assert(data[2] == 0x00 && data[3] == 0x00);
    assert(data[4] == 'H' && data[5] == 'e' && data[6] == 'l' && data[7] == 'l' && data[8] == 'o');
    assert(data[9] == 0x00); // NUL terminator

    // Second group: [0x00 0x05][0x00 0x08][0x02]
    assert(data[10] == 0x00 && data[11] == 0x05);
    assert(data[12] == 0x00 && data[13] == 0x08);
    assert(data[14] == 0x02);

    std::cout << "  [PASS] TestTlvBuilder\n";
}

// ============================================================================
// Test: BuildIdentifyInfo produces non-empty TLV groups with expected structure
// ============================================================================
void TestBuildIdentifyInfo() {
    MacAddress mac{};
    mac.bytes = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    auto identBody = Iap2MessageCatalog::BuildIdentifyInfo("TestCar", mac);
    assert(identBody.size() > 0);

    // The body should be a sequence of top-level TLV groups.
    // First group is param 0 (Name), second is param 1, etc.
    size_t offset = 0;
    int groupCount = 0;
    while (offset + 4 <= identBody.size()) {
        const uint16_t len = Endian::ReadBe16(identBody.data() + offset);
        assert(len >= 4);
        const uint16_t pid = Endian::ReadBe16(identBody.data() + offset + 2);
        (void)pid;
        offset += len;
        ++groupCount;
    }

    // Expect at least 8 top-level groups (Name, Model, Mfr, Serial, FW, HW, MsgSent, MsgRecv).
    assert(groupCount >= 8);
    assert(offset == identBody.size()); // All bytes consumed by valid groups.

    std::cout << "  [PASS] TestBuildIdentifyInfo (" << groupCount << " top-level groups)\n";
}

// ============================================================================
// Test: ParseMessageId extracts correct message ID from control payload
// ============================================================================
void TestParseMessageId() {
    // Build a valid control-session payload header.
    ByteBuffer payload = Iap2MessageCatalog::BuildControlPayloadHeader(Iap2MsgId::AuthSucceeded, 0);
    assert(payload.size() == 6);

    auto msgId = Iap2MessageCatalog::ParseMessageId(ByteSpan(payload.data(), payload.size()));
    assert(msgId.has_value());
    assert(*msgId == Iap2MsgId::AuthSucceeded);

    // Invalid prefix should return nullopt.
    const uint8_t bad[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    auto none = Iap2MessageCatalog::ParseMessageId(ByteSpan(bad, sizeof(bad)));
    assert(!none.has_value());

    std::cout << "  [PASS] TestParseMessageId\n";
}

// ============================================================================
// Test: Session manager state machine transitions (happy path)
// ============================================================================
void TestSessionManagerHappyPath() {
    Iap2SessionManager::Config cfg;
    cfg.accessoryName = "TestAccessory";
    Iap2SessionManager mgr(cfg);

    assert(mgr.GetState() == Iap2AuthState::Init);

    // 0xAA00 -> SendCert
    auto action = mgr.OnMessage(Iap2MsgId::RequestAuthCertificate, {});
    assert(action == Iap2Action::SendCert);
    assert(mgr.GetState() == Iap2AuthState::CertSent);

    // 0xAA02 -> SignChallenge
    const uint8_t challenge[] = {0xDE, 0xAD, 0xBE, 0xEF};
    action = mgr.OnMessage(Iap2MsgId::RequestAuthChallenge, ByteSpan(challenge, sizeof(challenge)));
    assert(action == Iap2Action::SignChallenge);
    assert(mgr.GetState() == Iap2AuthState::SignSent);

    // 0xAA05 -> Note (Authenticated)
    action = mgr.OnMessage(Iap2MsgId::AuthSucceeded, {});
    assert(action == Iap2Action::Note);
    assert(mgr.GetState() == Iap2AuthState::Authenticated);

    // 0x1D00 -> SendIdentify
    action = mgr.OnMessage(Iap2MsgId::StartIdentification, {});
    assert(action == Iap2Action::SendIdentify);
    assert(mgr.GetState() == Iap2AuthState::IdentSent);

    // 0x1D02 -> Note (Identified)
    action = mgr.OnMessage(Iap2MsgId::IdentifyAccepted, {});
    assert(action == Iap2Action::Note);
    assert(mgr.GetState() == Iap2AuthState::Identified);

    std::cout << "  [PASS] TestSessionManagerHappyPath\n";
}

// ============================================================================
// Test: Session manager rejects out-of-order messages gracefully
// ============================================================================
void TestSessionManagerOutOfOrder() {
    Iap2SessionManager::Config cfg;
    Iap2SessionManager mgr(cfg);

    // 0x1D00 before authentication should be ignored.
    auto action = mgr.OnMessage(Iap2MsgId::StartIdentification, {});
    assert(action == Iap2Action::None);
    assert(mgr.GetState() == Iap2AuthState::Init);

    std::cout << "  [PASS] TestSessionManagerOutOfOrder\n";
}

// ============================================================================
// Test: Session manager handles IdentifyRejected with retry then abort
// ============================================================================
void TestSessionManagerIdentifyRetry() {
    Iap2SessionManager::Config cfg;
    Iap2SessionManager mgr(cfg);

    // Fast-forward to IdentSent state.
    (void)mgr.OnMessage(Iap2MsgId::RequestAuthCertificate, {});
    (void)mgr.OnMessage(Iap2MsgId::RequestAuthChallenge, {});
    (void)mgr.OnMessage(Iap2MsgId::AuthSucceeded, {});
    (void)mgr.OnMessage(Iap2MsgId::StartIdentification, {});
    assert(mgr.GetState() == Iap2AuthState::IdentSent);

    // First rejection -> RetryIdentify
    auto action = mgr.OnMessage(Iap2MsgId::IdentifyRejected, {});
    assert(action == Iap2Action::RetryIdentify);
    assert(mgr.GetState() == Iap2AuthState::IdentRetried);

    // Second rejection -> Abort
    action = mgr.OnMessage(Iap2MsgId::IdentifyRejected, {});
    assert(action == Iap2Action::Abort);

    std::cout << "  [PASS] TestSessionManagerIdentifyRetry\n";
}

// ============================================================================
// Shared TLV walker for the message-body tests
// ============================================================================
struct TestTlvGroup {
    uint16_t pid;
    ByteBuffer value;
};

std::vector<TestTlvGroup> WalkGroups(ByteSpan body) {
    std::vector<TestTlvGroup> groups;
    size_t i = 0;
    while (i + 4 <= body.size()) {
        const uint16_t len = Endian::ReadBe16(body.data() + i);
        const uint16_t pid = Endian::ReadBe16(body.data() + i + 2);
        assert(len >= 4 && static_cast<size_t>(len) <= body.size() - i);
        groups.push_back({pid, ByteBuffer(body.data() + i + 4, body.data() + i + len)});
        i += len;
    }
    assert(i == body.size());  // every byte consumed by a well formed group
    return groups;
}

const TestTlvGroup* FindGroup(const std::vector<TestTlvGroup>& groups, uint16_t pid) {
    for (const auto& g : groups) {
        if (g.pid == pid) return &g;
    }
    return nullptr;
}

std::string ToNulTerminatedString(ByteSpan value) {
    size_t len = value.size();
    if (len > 0 && value[len - 1] == 0) --len;
    return std::string(reinterpret_cast<const char*>(value.data()), len);
}

// ============================================================================
// Test: BuildAccessoryWiFiConfig (0x5703)
// ============================================================================
void TestBuildAccessoryWiFiConfig() {
    // Without a BSSID: SSID, passphrase, security type and channel must be there,
    // and channel 0 has to be emitted (the iPhone accepts it as "unknown").
    auto body = Iap2MessageCatalog::BuildAccessoryWiFiConfig("CarPlay-AP", "secret123", 2, 0);
    auto groups = WalkGroups(body);
    assert(FindGroup(groups, 0) == nullptr);  // BSSID omitted
    assert(ToNulTerminatedString(FindGroup(groups, 1)->value) == "CarPlay-AP");
    assert(ToNulTerminatedString(FindGroup(groups, 2)->value) == "secret123");
    assert(FindGroup(groups, 3)->value.size() == 1 && FindGroup(groups, 3)->value[0] == 2);
    const auto* channel = FindGroup(groups, 4);
    assert(channel && channel->value.size() == 1 && channel->value[0] == 0);

    // With a BSSID it becomes the first group.
    const uint8_t bssid[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    auto withBssid = Iap2MessageCatalog::BuildAccessoryWiFiConfig("AP", "pw", 4, 36, ByteSpan(bssid, 6));
    auto groups2 = WalkGroups(withBssid);
    assert(groups2.front().pid == 0 && groups2.front().value.size() == 6);
    assert(groups2.front().value == ByteBuffer(bssid, bssid + 6));
    assert(FindGroup(groups2, 4)->value[0] == 36);

    std::cout << "  [PASS] TestBuildAccessoryWiFiConfig\n";
}

// ============================================================================
// Test: the five post-identify subscription builders
// ============================================================================
void TestSubscriptionBuilders() {
    const ByteBuffer bodies[] = {
        Iap2MessageCatalog::BuildStartNowPlaying(),
        Iap2MessageCatalog::BuildStartRouteGuidance(),
        Iap2MessageCatalog::BuildStartPowerUpdates(),
        Iap2MessageCatalog::BuildStartCommunicationsUpdates(),
        Iap2MessageCatalog::BuildStartCallState(),
    };
    for (const auto& body : bodies) {
        assert(!body.empty());
        (void)WalkGroups(body);  // asserts well formed, fully consumed framing
    }

    // NowPlaying subscribes through the nested track-attributes (0) and
    // playback-attributes (1) groups.
    auto nowPlaying = WalkGroups(bodies[0]);
    assert(FindGroup(nowPlaying, 0) != nullptr);
    assert(FindGroup(nowPlaying, 1) != nullptr);

    std::cout << "  [PASS] TestSubscriptionBuilders\n";
}

// ============================================================================
// Test: BuildCarPlayStartSession (0x4301) carries the hotspot + receiver identity
// ============================================================================
void TestBuildCarPlayStartSession() {
    Iap2MessageCatalog::CarPlaySessionConfig cfg;
    cfg.airPlayPort = 5000;
    cfg.ssid = "CarPlay-AP";
    cfg.passphrase = "secret123";
    cfg.ipAddress = "192.168.137.1";
    cfg.channel = 0;
    cfg.securityType = 2;
    cfg.deviceIdentifier = "11:22:33:44:55:66";
    cfg.publicKey = std::string(64, 'a');
    cfg.sourceVersion = "320.17";

    auto groups = WalkGroups(Iap2MessageCatalog::BuildCarPlayStartSession(cfg));

    const auto* wireless = FindGroup(groups, 1);
    assert(wireless != nullptr);
    auto inner = WalkGroups(wireless->value);
    assert(ToNulTerminatedString(FindGroup(inner, 0)->value) == "CarPlay-AP");
    assert(ToNulTerminatedString(FindGroup(inner, 1)->value) == "secret123");
    assert(FindGroup(inner, 2)->value[0] == 0);
    assert(ToNulTerminatedString(FindGroup(inner, 3)->value) == "192.168.137.1");
    assert(FindGroup(inner, 4)->value[0] == 2);

    const auto* port = FindGroup(groups, 2);
    assert(port && port->value.size() == 4);
    assert(Endian::ReadBe32(port->value.data()) == 5000u);
    assert(ToNulTerminatedString(FindGroup(groups, 3)->value) == "11:22:33:44:55:66");
    assert(ToNulTerminatedString(FindGroup(groups, 4)->value) == std::string(64, 'a'));
    assert(ToNulTerminatedString(FindGroup(groups, 5)->value) == "320.17");

    std::cout << "  [PASS] TestBuildCarPlayStartSession\n";
}

// ============================================================================
// Test: 0x4300 / 0x4E0D / 0x4E0E parsers round-trip what we can build
// ============================================================================
void TestWirelessControlParsers() {
    // 0x4300: wireless availability section (param 1) with the iPhone's identifier.
    TlvBuilder wireless;
    wireless.Bool(0, true);
    wireless.Str(1, "AA-BB-CC-DD-EE-FF");
    TlvBuilder availability;
    availability.Bytes(1, wireless.Span());
    auto parsed = Iap2MessageCatalog::ParseCarPlayAvailability(availability.Span());
    assert(parsed.has_value());
    assert(!parsed->hasWired);
    assert(parsed->hasWireless);
    assert(parsed->wirelessAvailable);
    assert(parsed->wirelessIdentifier == "AA-BB-CC-DD-EE-FF");

    // A malformed availability byte must be rejected rather than guessed at.
    TlvBuilder badWireless;
    badWireless.U8(0, 7);
    TlvBuilder bad;
    bad.Bytes(1, badWireless.Span());
    assert(!Iap2MessageCatalog::ParseCarPlayAvailability(bad.Span()).has_value());
    assert(!Iap2MessageCatalog::ParseCarPlayAvailability({}).has_value());

    // 0x4E0D
    TlvBuilder avail;
    avail.Bool(0, true);
    auto flag = Iap2MessageCatalog::ParseWirelessCarPlayAvailability(avail.Span());
    assert(flag.has_value() && *flag);
    assert(!Iap2MessageCatalog::ParseWirelessCarPlayAvailability({}).has_value());

    // 0x4E0E
    TlvBuilder transport;
    transport.Str(0, "11:22:33:44:55:66");
    auto ids = Iap2MessageCatalog::ParseDeviceTransportIdentifier(transport.Span());
    assert(ids.has_value());
    assert(ids->bluetoothMac == "11:22:33:44:55:66");
    assert(ids->usbTransportIdentifier.empty());
    assert(!Iap2MessageCatalog::ParseDeviceTransportIdentifier({}).has_value());

    std::cout << "  [PASS] TestWirelessControlParsers\n";
}

// ============================================================================
// Test: the wireless control plane drives subscriptions, 0x5703 and 0x4301
// ============================================================================
void TestWirelessControlPlane() {
    Iap2WirelessControlPlane plane;

    Iap2WirelessControlPlane::WiFiConfig wifi;
    wifi.ssid = "CarPlay-AP";
    wifi.passphrase = "secret123";
    plane.SetWiFiConfig(wifi);

    Iap2WirelessControlPlane::SessionParams params;
    params.airPlayPort = 5000;
    params.ipAddress = "192.168.137.1";
    params.deviceIdentifier = "11:22:33:44:55:66";
    params.publicKey = std::string(64, 'b');
    plane.SetSessionParams(params);

    // Subscriptions in the order the reference sends them, and only once.
    auto subs = plane.OnIdentifyAccepted();
    assert(subs.size() == 5);
    const uint16_t expectedOrder[] = {
        Iap2MsgId::StartNowPlaying, Iap2MsgId::StartRouteGuidance, Iap2MsgId::StartPowerUpdates,
        Iap2MsgId::StartCommunications, Iap2MsgId::StartCallState,
    };
    for (size_t i = 0; i < subs.size(); ++i) {
        assert(subs[i].msgId == expectedOrder[i]);
        assert(!subs[i].body.empty());
    }
    assert(plane.OnIdentifyAccepted().empty());
    assert(plane.SubscriptionsSent());

    // 0x5702 resends 0x5703 up to the pre-transport cap of five.
    for (int i = 1; i <= Iap2WirelessControlPlane::kMaxPreTransportWiFiSends; ++i) {
        auto replies = plane.OnControlMessage(Iap2MsgId::RequestAccessoryWiFi, {});
        assert(replies.size() == 1);
        assert(replies[0].msgId == Iap2MsgId::AccessoryWiFiConfig);
        assert(plane.WiFiConfigSends() == i);
    }
    assert(plane.OnControlMessage(Iap2MsgId::RequestAccessoryWiFi, {}).empty());
    assert(plane.WiFiConfigSends() == Iap2WirelessControlPlane::kMaxPreTransportWiFiSends);

    // 0x4E0D records availability and answers nothing.
    TlvBuilder avail;
    avail.Bool(0, true);
    assert(plane.OnControlMessage(Iap2MsgId::WirelessCarPlayAvailability, avail.Span()).empty());
    assert(plane.WirelessCarPlayAvailable());

    // 0x4E0E switches to the (smaller) post-transport budget.
    TlvBuilder transport;
    transport.Str(0, "AA-BB-CC-DD-EE-FF");
    auto afterTransport = plane.OnControlMessage(Iap2MsgId::DeviceTransportId, transport.Span());
    assert(afterTransport.size() == 1);
    assert(afterTransport[0].msgId == Iap2MsgId::AccessoryWiFiConfig);
    assert(plane.TransportNotified());
    assert(plane.OnControlMessage(Iap2MsgId::RequestAccessoryWiFi, {}).size() == 1);
    assert(plane.OnControlMessage(Iap2MsgId::RequestAccessoryWiFi, {}).empty());
    assert(plane.WiFiConfigSends() == Iap2WirelessControlPlane::kMaxPreTransportWiFiSends
                                    + Iap2WirelessControlPlane::kMaxPostTransportWiFiSends);

    // 0x4300 is answered with 0x4301 carrying our hotspot and receiver identity.
    TlvBuilder wirelessSection;
    wirelessSection.Bool(0, true);
    wirelessSection.Str(1, "AA-BB-CC-DD-EE-FF");
    TlvBuilder availability2;
    availability2.Bytes(1, wirelessSection.Span());
    auto start = plane.OnControlMessage(Iap2MsgId::CarPlayAvailability, availability2.Span());
    assert(start.size() == 1);
    assert(start[0].msgId == Iap2MsgId::CarPlayStartSession);
    assert(plane.CarPlayStartSessionSends() == 1);

    auto groups = WalkGroups(start[0].body);
    const auto* wireless = FindGroup(groups, 1);
    assert(wireless != nullptr);
    auto inner = WalkGroups(wireless->value);
    assert(ToNulTerminatedString(FindGroup(inner, 0)->value) == "CarPlay-AP");
    assert(ToNulTerminatedString(FindGroup(groups, 4)->value) == std::string(64, 'b'));

    // Without a public key we must not emit a half-built 0x4301.
    Iap2WirelessControlPlane keyless;
    keyless.SetWiFiConfig(wifi);
    Iap2WirelessControlPlane::SessionParams noKey;
    noKey.publicKey.clear();
    keyless.SetSessionParams(noKey);
    assert(keyless.OnControlMessage(Iap2MsgId::CarPlayAvailability, availability2.Span()).empty());

    std::cout << "  [PASS] TestWirelessControlPlane\n";
}

// ============================================================================
// Main test runner
// ============================================================================
int main() {
    std::cout << "Running Protocol Tests...\n";

    TestIap2Checksum();
    TestBuildSyn();
    TestFeedDataSynAck();
    TestFeedDataCorruptedHeader();
    TestBuildMessage();
    TestTlvBuilder();
    TestBuildIdentifyInfo();
    TestParseMessageId();
    TestSessionManagerHappyPath();
    TestSessionManagerOutOfOrder();
    TestSessionManagerIdentifyRetry();
    TestBuildAccessoryWiFiConfig();
    TestSubscriptionBuilders();
    TestBuildCarPlayStartSession();
    TestWirelessControlParsers();
    TestWirelessControlPlane();

    std::cout << "All protocol tests passed.\n";
    return 0;
}
