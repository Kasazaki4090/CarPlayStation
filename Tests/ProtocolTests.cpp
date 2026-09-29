#include "Core/Protocol/Iap2LinkEngine.h"
#include "Core/Protocol/Iap2MessageCatalog.h"
#include "Core/Protocol/Iap2SessionManager.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace CarPlayStation::Core;
using namespace CarPlayStation::Core::Protocol;
using namespace CarPlayStation::Core::Common;

// ============================================================================
// Test: Checksum correctness against reference vectors
// ============================================================================
void TestIap2Checksum() {
    // Reference vector from spec: bytes [0x89, 0x4B] -> checksum 0xD6
    const uint8_t data1[] = {0x89, 0x4B};
    assert(Iap2Checksum(ByteSpan(data1, 2)) == 0xD6);

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

    std::cout << "All protocol tests passed.\n";
    return 0;
}
