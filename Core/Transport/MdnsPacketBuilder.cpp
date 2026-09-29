#include "Core/Transport/MdnsPacketBuilder.h"
#include <sstream>
#include <cstring>
#include <cstdio>

namespace CarPlayStation::Core::Transport {

namespace {

void AppendU16(Common::ByteBuffer& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

void AppendU32(Common::ByteBuffer& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

} // namespace

Common::ByteBuffer MdnsPacketBuilder::EncodeDnsName(const std::string& name) {
    Common::ByteBuffer encoded;
    std::string clean = name;
    while (!clean.empty() && clean.back() == '.') {
        clean.pop_back();
    }

    std::stringstream ss(clean);
    std::string token;
    while (std::getline(ss, token, '.')) {
        if (token.empty() || token.size() > 63) continue;
        encoded.push_back(static_cast<uint8_t>(token.size()));
        encoded.insert(encoded.end(), token.begin(), token.end());
    }
    encoded.push_back(0x00); // Root label null terminator
    return encoded;
}

Common::ByteBuffer MdnsPacketBuilder::BuildAirPlayAnnouncement(const MdnsServiceConfig& config, uint32_t ttl) {
    Common::ByteBuffer packet;
    packet.reserve(512);

    // 1. DNS Header (12 bytes)
    AppendU16(packet, 0x0000); // Transaction ID (0 for multicast responses)
    AppendU16(packet, 0x8400); // Flags: Standard response, Authoritative Answer
    AppendU16(packet, 0);      // QDCOUNT (Questions: 0)
    AppendU16(packet, 4);      // ANCOUNT (Answers: PTR, SRV, TXT, A)
    AppendU16(packet, 0);      // NSCOUNT (Name Server RRs: 0)
    AppendU16(packet, 0);      // ARCOUNT (Additional RRs: 0)

    std::string fullServiceName = config.instanceName + "." + config.serviceType;

    // --- Record 1: PTR Record ---
    // Maps _airplay._tcp.local. -> CarPlay._airplay._tcp.local.
    auto ptrName = EncodeDnsName(config.serviceType);
    auto ptrTarget = EncodeDnsName(fullServiceName);
    packet.insert(packet.end(), ptrName.begin(), ptrName.end());
    AppendU16(packet, kDnsTypePtr);
    AppendU16(packet, kDnsClassFlushCache);
    AppendU32(packet, ttl);
    AppendU16(packet, static_cast<uint16_t>(ptrTarget.size()));
    packet.insert(packet.end(), ptrTarget.begin(), ptrTarget.end());

    // --- Record 2: SRV Record ---
    // Maps CarPlay._airplay._tcp.local. -> port 5000 on hostname
    auto srvName = EncodeDnsName(fullServiceName);
    auto srvTarget = EncodeDnsName(config.hostname);
    packet.insert(packet.end(), srvName.begin(), srvName.end());
    AppendU16(packet, kDnsTypeSrv);
    AppendU16(packet, kDnsClassFlushCache);
    AppendU32(packet, ttl);
    AppendU16(packet, static_cast<uint16_t>(6 + srvTarget.size())); // Priority(2) + Weight(2) + Port(2) + Target
    AppendU16(packet, 0); // Priority
    AppendU16(packet, 0); // Weight
    AppendU16(packet, config.port);
    packet.insert(packet.end(), srvTarget.begin(), srvTarget.end());

    // --- Record 3: TXT Record ---
    // AirPlay TXT attributes (features, flags, model, deviceid, etc.)
    Common::ByteBuffer txtRdata;
    auto addTxtEntry = [&](const std::string& key, const std::string& val) {
        if (key.empty() || val.empty()) return;
        std::string entry = key + "=" + val;
        if (entry.size() <= 255) {
            txtRdata.push_back(static_cast<uint8_t>(entry.size()));
            txtRdata.insert(txtRdata.end(), entry.begin(), entry.end());
        }
    };

    addTxtEntry("deviceid", config.deviceId);
    addTxtEntry("features", config.features);
    addTxtEntry("flags", config.flags);
    addTxtEntry("model", config.model);
    addTxtEntry("protovers", config.protovers);
    addTxtEntry("pi", config.pi);
    addTxtEntry("srcvers", config.srcvers);

    for (const auto& [k, v] : config.customTxtProps) {
        addTxtEntry(k, v);
    }

    packet.insert(packet.end(), srvName.begin(), srvName.end());
    AppendU16(packet, kDnsTypeTxt);
    AppendU16(packet, kDnsClassFlushCache);
    AppendU32(packet, ttl);
    AppendU16(packet, static_cast<uint16_t>(txtRdata.size()));
    packet.insert(packet.end(), txtRdata.begin(), txtRdata.end());

    // --- Record 4: A Record ---
    // Maps hostname -> IPv4 address
    auto aName = EncodeDnsName(config.hostname);
    packet.insert(packet.end(), aName.begin(), aName.end());
    AppendU16(packet, kDnsTypeA);
    AppendU16(packet, kDnsClassFlushCache);
    AppendU32(packet, ttl);
    AppendU16(packet, 4); // 4-byte IPv4

    uint8_t ipBytes[4] = {192, 168, 137, 1};
    int b0 = 192, b1 = 168, b2 = 137, b3 = 1;
    if (std::sscanf(config.ipv4Address.c_str(), "%d.%d.%d.%d", &b0, &b1, &b2, &b3) == 4) {
        ipBytes[0] = static_cast<uint8_t>(b0);
        ipBytes[1] = static_cast<uint8_t>(b1);
        ipBytes[2] = static_cast<uint8_t>(b2);
        ipBytes[3] = static_cast<uint8_t>(b3);
    }
    packet.push_back(ipBytes[0]);
    packet.push_back(ipBytes[1]);
    packet.push_back(ipBytes[2]);
    packet.push_back(ipBytes[3]);

    return packet;
}

bool MdnsPacketBuilder::ValidateMdnsHeader(Common::ByteSpan packet, uint16_t expectedAnswers) {
    if (packet.size() < 12) {
        return false;
    }

    uint16_t flags = (static_cast<uint16_t>(packet[2]) << 8) | packet[3];
    uint16_t ancount = (static_cast<uint16_t>(packet[6]) << 8) | packet[7];

    if (flags != 0x8400) {
        return false; // Must be standard response + authoritative answer
    }

    if (ancount < expectedAnswers) {
        return false;
    }

    return true;
}

} // namespace CarPlayStation::Core::Transport
