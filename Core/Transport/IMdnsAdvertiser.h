#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <string>
#include <vector>
#include <map>

namespace CarPlayStation::Core::Transport {

struct MdnsServiceConfig {
    std::string instanceName{"CarPlay"};
    std::string serviceType{"_airplay._tcp.local."};
    std::string hostname{"CarPlayStation.local."};
    uint16_t port{5000};
    std::string ipv4Address{"192.168.137.1"};
    std::string deviceId{"AA:BB:CC:DD:EE:01"};
    std::string features{"0x44440B80,0x61"};
    std::string flags{"0x4"};
    std::string model{"CarPlayStation-1.0"};
    std::string protovers{"1.0"};
    std::string pi{"dd52a0b0-5435-42ea-99dc-0a6b96bb433b"};
    std::string srcvers{"320.17"};
    std::map<std::string, std::string> customTxtProps{};
};

/**
 * @brief Abstract interface for mDNS (Multicast DNS / DNS-SD RFC 6762/6763)
 * service advertiser publishing AirPlay & Wireless CarPlay records.
 */
class IMdnsAdvertiser {
public:
    virtual ~IMdnsAdvertiser() = default;

    // Start advertising mDNS records on UDP 5353
    [[nodiscard]] virtual Common::Status Start(const MdnsServiceConfig& config) = 0;

    // Stop advertising
    virtual void Stop() = 0;

    // Is currently broadcasting
    [[nodiscard]] virtual bool IsRunning() const noexcept = 0;

    // Active configuration
    [[nodiscard]] virtual MdnsServiceConfig GetConfig() const = 0;

    // Trigger an immediate multicast announcement packet
    [[nodiscard]] virtual Common::Status BroadcastAnnouncement() = 0;

    // Send targeted announcement to specific IP (e.g. unicast to connected iPhone)
    [[nodiscard]] virtual Common::Status AnnounceTo(const std::string& targetIp) = 0;

    // Clear the cache of already-dialed _carplay-ctrl targets so the next
    // session re-dials the (ephemeral) control port. Called at the start of
    // each new session to allow automatic reconnection.
    virtual void ClearDialedTargets() {}
};

} // namespace CarPlayStation::Core::Transport
