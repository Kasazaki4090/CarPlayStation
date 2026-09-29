#include "Core/Protocol/Iap2LinkEngine.h"
#include <cstring>

namespace CarPlayStation::Core::Protocol {

Iap2LinkEngine::Iap2LinkEngine() = default;

Common::Status Iap2LinkEngine::FeedData(Common::ByteSpan rawBytes) {
    std::lock_guard lock(mutex_);

    size_t offset = 0;
    while (offset < rawBytes.size()) {
        Iap2RxPacket rx;
        size_t consumed = TryParsePacket(rawBytes.subspan(offset), rx);
        if (consumed == 0) {
            // Not a valid frame at this position — skip one byte and resync.
            ++offset;
            continue;
        }

        offset += consumed;
        peer_seq_ = rx.seq;

        // Dispatch to handler: strip the trailing payload checksum byte for the caller.
        if (handler_) {
            Common::ByteSpan body{};
            if (!rx.payload.empty()) {
                // The last byte is the payload checksum; exclude it from the delivered span.
                body = Common::ByteSpan(rx.payload.data(), rx.payload.size() - 1);
            }
            handler_->OnIap2Message(rx.session_id, body);
        }

        // Send a bare ACK for every valid packet received (matches reference behavior).
        if (send_fn_) {
            auto ack = BuildBareAck();
            send_fn_(Common::ByteSpan(ack.data(), ack.size()));
        }
    }

    return Common::Status::Ok();
}

Common::Status Iap2LinkEngine::SendMessage(uint8_t sessionId, Common::ByteSpan payload) {
    std::lock_guard lock(mutex_);

    // Wrap in link frame: [SOP1][SOP2][len BE16][CTL_ACK][seq][ack][sess][hdr_cks][payload...][payload_cks]
    const uint16_t total = static_cast<uint16_t>(9 + payload.size() + 1);

    Common::ByteBuffer pkt;
    pkt.reserve(total);
    pkt.push_back(Iap2Constants::SOP1);
    pkt.push_back(Iap2Constants::SOP2);
    pkt.push_back(static_cast<uint8_t>((total >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(total & 0xFF));
    pkt.push_back(Iap2Constants::CTL_ACK);
    pkt.push_back(my_seq_);
    pkt.push_back(peer_seq_);
    pkt.push_back(sessionId);
    my_seq_ = static_cast<uint8_t>(my_seq_ + 1);

    // Header checksum over first 8 bytes.
    pkt.push_back(Iap2Checksum(Common::ByteSpan(pkt.data(), 8)));

    // Payload.
    for (uint8_t b : payload) pkt.push_back(b);

    // Payload checksum over the payload region only.
    if (!payload.empty()) {
        pkt.push_back(Iap2Checksum(payload));
    } else {
        pkt.push_back(Iap2Checksum(Common::ByteSpan{}));
    }

    if (send_fn_) {
        return send_fn_(Common::ByteSpan(pkt.data(), pkt.size()));
    }
    return Common::Status::Ok();
}

void Iap2LinkEngine::SetPacketHandler(IIap2PacketHandler* handler) {
    std::lock_guard lock(mutex_);
    handler_ = handler;
}

void Iap2LinkEngine::Reset() {
    std::lock_guard lock(mutex_);
    my_seq_ = 0;
    peer_seq_ = 0;
}

Common::ByteBuffer Iap2LinkEngine::BuildSyn() {
    std::lock_guard lock(mutex_);

    const auto& params = Iap2Constants::SYN_PARAMS;
    const uint16_t total = static_cast<uint16_t>(9 + params.size() + 1); // 26 bytes

    Common::ByteBuffer pkt;
    pkt.reserve(total);
    pkt.push_back(Iap2Constants::SOP1);
    pkt.push_back(Iap2Constants::SOP2);
    pkt.push_back(static_cast<uint8_t>((total >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(total & 0xFF));
    pkt.push_back(Iap2Constants::CTL_SYN);
    pkt.push_back(my_seq_);
    pkt.push_back(0); // ack = 0 for SYN
    pkt.push_back(0); // session = 0 for SYN
    my_seq_ = static_cast<uint8_t>(my_seq_ + 1);

    pkt.push_back(Iap2Checksum(Common::ByteSpan(pkt.data(), 8)));

    for (uint8_t b : params) pkt.push_back(b);
    pkt.push_back(Iap2Checksum(Common::ByteSpan(params.data(), params.size())));

    return pkt;
}

Common::ByteBuffer Iap2LinkEngine::BuildBareAck() const {
    const uint16_t total = 9; // header only

    Common::ByteBuffer a;
    a.reserve(total);
    a.push_back(Iap2Constants::SOP1);
    a.push_back(Iap2Constants::SOP2);
    a.push_back(0x00);
    a.push_back(0x09);
    a.push_back(Iap2Constants::CTL_ACK);
    a.push_back(static_cast<uint8_t>((my_seq_ + 255) % 256)); // wrapping decrement: echo last sent seq
    a.push_back(peer_seq_);
    a.push_back(0); // session = 0

    a.push_back(Iap2Checksum(Common::ByteSpan(a.data(), 8)));
    return a;
}

Common::ByteBuffer Iap2LinkEngine::BuildMessage(uint8_t sessionId, uint16_t messageId, Common::ByteSpan body) {
    std::lock_guard lock(mutex_);

    // Control-session payload: [40 40][total BE16][msg_id BE16][body]
    const uint16_t innerTotal = static_cast<uint16_t>(6 + body.size());
    Common::ByteBuffer ctrlPayload;
    ctrlPayload.reserve(innerTotal);
    ctrlPayload.push_back(Iap2Constants::MSG_PREFIX_0);
    ctrlPayload.push_back(Iap2Constants::MSG_PREFIX_1);
    ctrlPayload.push_back(static_cast<uint8_t>((innerTotal >> 8) & 0xFF));
    ctrlPayload.push_back(static_cast<uint8_t>(innerTotal & 0xFF));
    ctrlPayload.push_back(static_cast<uint8_t>((messageId >> 8) & 0xFF));
    ctrlPayload.push_back(static_cast<uint8_t>(messageId & 0xFF));
    for (uint8_t b : body) ctrlPayload.push_back(b);

    // Now frame it as a link packet.
    const uint16_t total = static_cast<uint16_t>(9 + ctrlPayload.size() + 1);
    Common::ByteBuffer pkt;
    pkt.reserve(total);
    pkt.push_back(Iap2Constants::SOP1);
    pkt.push_back(Iap2Constants::SOP2);
    pkt.push_back(static_cast<uint8_t>((total >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(total & 0xFF));
    pkt.push_back(Iap2Constants::CTL_ACK);
    pkt.push_back(my_seq_);
    pkt.push_back(peer_seq_);
    pkt.push_back(sessionId);
    my_seq_ = static_cast<uint8_t>(my_seq_ + 1);

    pkt.push_back(Iap2Checksum(Common::ByteSpan(pkt.data(), 8)));
    for (uint8_t b : ctrlPayload) pkt.push_back(b);
    pkt.push_back(Iap2Checksum(Common::ByteSpan(ctrlPayload.data(), ctrlPayload.size())));

    return pkt;
}

Common::ByteBuffer Iap2LinkEngine::BuildRaw(uint8_t sessionId, Common::ByteSpan payload) {
    std::lock_guard lock(mutex_);

    const uint16_t total = static_cast<uint16_t>(9 + payload.size() + 1);
    Common::ByteBuffer pkt;
    pkt.reserve(total);
    pkt.push_back(Iap2Constants::SOP1);
    pkt.push_back(Iap2Constants::SOP2);
    pkt.push_back(static_cast<uint8_t>((total >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(total & 0xFF));
    pkt.push_back(Iap2Constants::CTL_ACK);
    pkt.push_back(my_seq_);
    pkt.push_back(peer_seq_);
    pkt.push_back(sessionId);
    my_seq_ = static_cast<uint8_t>(my_seq_ + 1);

    pkt.push_back(Iap2Checksum(Common::ByteSpan(pkt.data(), 8)));
    for (uint8_t b : payload) pkt.push_back(b);
    if (!payload.empty()) {
        pkt.push_back(Iap2Checksum(payload));
    } else {
        pkt.push_back(Iap2Checksum(Common::ByteSpan{}));
    }

    return pkt;
}

size_t Iap2LinkEngine::TryParsePacket(Common::ByteSpan buf, Iap2RxPacket& out) const {
    if (buf.size() < 9 || buf[0] != Iap2Constants::SOP1 || buf[1] != Iap2Constants::SOP2) {
        return 0;
    }

    const uint16_t declaredLen = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
    if (declaredLen < 9 || declaredLen > buf.size()) {
        return 0;
    }

    Common::ByteSpan pkt(buf.data(), declaredLen);

    // Validate header checksum.
    if (Iap2Checksum(Common::ByteSpan(pkt.data(), 8)) != pkt[8]) {
        return 0;
    }

    out.control = pkt[4];
    out.seq = pkt[5];
    out.ack = pkt[6];
    out.session_id = pkt[7];

    // Validate and extract payload (if any).
    if (declaredLen > 9) {
        Common::ByteSpan body(pkt.data() + 9, declaredLen - 9);
        // Last byte is the payload checksum.
        uint8_t sumByte = body[body.size() - 1];
        Common::ByteSpan data(body.data(), body.size() - 1);
        if (Iap2Checksum(data) != sumByte) {
            return 0;
        }
        out.payload.assign(body.begin(), body.end());
    } else {
        out.payload.clear();
    }

    return declaredLen;
}

} // namespace CarPlayStation::Core::Protocol
