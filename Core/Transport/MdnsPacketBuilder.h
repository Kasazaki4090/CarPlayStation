#pragma once

#include "Core/Common/Types.h"
#include "Core/Transport/IMdnsAdvertiser.h"
#include <vector>
#include <string>

namespace CarPlayStation::Core::Transport {

/**
 * @brief DNS-SD / mDNS packet serializer compliant with RFC 1035 & RFC 6762.
 */
class MdnsPacketBuilder {
public:
    static constexpr uint16_t kDnsTypeA = 1;
    static constexpr uint16_t kDnsTypePtr = 12;
    static constexpr uint16_t kDnsTypeTxt = 16;
    static constexpr uint16_t kDnsTypeAaaa = 28;
    static constexpr uint16_t kDnsTypeSrv = 33;

    static constexpr uint16_t kDnsClassIn = 1;
    static constexpr uint16_t kDnsClassFlushCache = 0x8001; // IN class with FlushCache bit set

    /**
     * @brief Encodes a dotted domain string (e.g. "_airplay._tcp.local.") into DNS label format.
     */
    static Common::ByteBuffer EncodeDnsName(const std::string& name);

    /**
     * @brief Builds an RFC 6762 unsolicited mDNS response packet containing PTR, SRV, TXT and A records.
     */
    static Common::ByteBuffer BuildAirPlayAnnouncement(const MdnsServiceConfig& config, uint32_t ttl = 4500);

    /**
     * @brief Builds an RFC 6762 mDNS query for _carplay-ctrl._tcp.local. PTR to discover iPhone's control port.
     */
    static Common::ByteBuffer BuildCarPlayControlQuery();

    /**
     * @brief Parses and validates that a raw buffer has a valid mDNS response header.
     */
    static bool ValidateMdnsHeader(Common::ByteSpan packet, uint16_t expectedAnswers = 4);
};

} // namespace CarPlayStation::Core::Transport
