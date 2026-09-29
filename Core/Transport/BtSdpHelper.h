#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2bth.h>
#endif

namespace CarPlayStation::Core::Transport {

/**
 * @brief Helper utility for Apple Wireless iAP2 SDP (Service Discovery Protocol)
 * records and Bluetooth address formatting.
 */
class BtSdpHelper {
public:
    // Apple Wireless iAP2 SDP record size and channel offset
    static constexpr size_t kSdpRecordSize = 89;
    static constexpr size_t kRfcommChannelByteOffset = 48;

    // GUID: 00000000-deca-fade-deca-deafdecacaff
    static constexpr GUID kIap2ServiceClassGuid = {
        0x00000000, 0xdeca, 0xfade, {0xde, 0xca, 0xde, 0xaf, 0xde, 0xca, 0xca, 0xff}
    };

    // Alternative legacy suffix variant: 00000000-deca-fade-deca-deafdecacafe
    static constexpr GUID kIap2ServiceClassGuidLegacy = {
        0x00000000, 0xdeca, 0xfade, {0xde, 0xca, 0xde, 0xaf, 0xde, 0xca, 0xca, 0xfe}
    };

    /**
     * @brief Builds the exact 89-byte binary SDP service record for Wireless iAPv2.
     * @param rfcommChannel RFCOMM server channel (typically 1).
     * @param useLegacyFeSuffix If true, uses 0xFE suffix instead of 0xFF.
     */
    static Common::ByteBuffer BuildIap2SdpRecord(uint8_t rfcommChannel = 1, bool useLegacyFeSuffix = false);

    /**
     * @brief Validates an SDP record matches the Wireless iAPv2 template.
     */
    static bool ValidateIap2SdpRecord(Common::ByteSpan record, uint8_t expectedChannel = 1);

    /**
     * @brief Formats a 64-bit Bluetooth address into XX:XX:XX:XX:XX:XX standard string.
     */
    static std::string FormatBluetoothAddress(uint64_t bthAddr);

    /**
     * @brief Parses an XX:XX:XX:XX:XX:XX Bluetooth string into a 64-bit integer.
     */
    static uint64_t ParseBluetoothAddress(std::string_view addrStr);

#ifdef _WIN32
    /**
     * @brief Registers the Wireless iAP2 SDP record with the Windows Bluetooth stack.
     * @param socket BTH server socket bound to RFCOMM channel.
     * @param channel RFCOMM channel.
     * @param outHandle Pointer to handle receiving the registration token.
     */
    static Common::Status RegisterSdpService(SOCKET socket, uint8_t channel, HANDLE* outHandle);

    /**
     * @brief Unregisters the SDP record from the Windows Bluetooth stack.
     */
    static void UnregisterSdpService(HANDLE handle);
#endif
};

} // namespace CarPlayStation::Core::Transport
