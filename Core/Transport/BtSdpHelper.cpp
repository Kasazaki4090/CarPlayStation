#include "Core/Transport/BtSdpHelper.h"
#include <iomanip>
#include <sstream>
#include <cstring>

namespace CarPlayStation::Core::Transport {

namespace {

// Byte-exact SDP record template from reference live CCPA capture
const uint8_t kSdpRecordTemplate[89] = {
    0x35, 0x57,
    // ServiceRecordHandle = 0x00010000
    0x09, 0x00, 0x00,  0x0a, 0x00, 0x01, 0x00, 0x00,
    // ServiceClassIDList: UUID128 00000000-deca-fade-deca-deafdecacaff
    0x09, 0x00, 0x01,  0x35, 0x11, 0x1c,
        0x00, 0x00, 0x00, 0x00, 0xde, 0xca, 0xfa, 0xde,
        0xde, 0xca, 0xde, 0xaf, 0xde, 0xca, 0xca, 0xff,
    // ProtocolDescriptorList: L2CAP -> RFCOMM channel 0x01 (patched at runtime)
    0x09, 0x00, 0x04,  0x35, 0x0c,
        0x35, 0x03, 0x19, 0x01, 0x00,
        0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x01,
    // BrowseGroupList = PublicBrowseGroup (0x1002)
    0x09, 0x00, 0x05,  0x35, 0x03, 0x19, 0x10, 0x02,
    // BluetoothProfileDescriptorList = SerialPort (0x1101) v1.0
    0x09, 0x00, 0x09,  0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x01, 0x09, 0x01, 0x00,
    // ServiceName (0x0100) = "Wireless iAPv2"
    0x09, 0x01, 0x00,  0x25, 0x0e,
        0x57, 0x69, 0x72, 0x65, 0x6c, 0x65, 0x73, 0x73, 0x20,
        0x69, 0x41, 0x50, 0x76, 0x32,
};

constexpr size_t kUuidSuffixByteOffset = 31; // Offset of 0xFF/0xFE

} // namespace

Common::ByteBuffer BtSdpHelper::BuildIap2SdpRecord(uint8_t rfcommChannel, bool useLegacyFeSuffix) {
    Common::ByteBuffer record(kSdpRecordTemplate, kSdpRecordTemplate + sizeof(kSdpRecordTemplate));
    record[kRfcommChannelByteOffset] = rfcommChannel;
    if (useLegacyFeSuffix) {
        record[kUuidSuffixByteOffset] = 0xfe;
    }
    return record;
}

bool BtSdpHelper::ValidateIap2SdpRecord(Common::ByteSpan record, uint8_t expectedChannel) {
    if (record.size() != kSdpRecordSize) {
        return false;
    }

    // Verify channel byte
    if (record[kRfcommChannelByteOffset] != expectedChannel) {
        return false;
    }

    // Verify header and service name tag
    if (record[0] != 0x35 || record[1] != 0x57) {
        return false;
    }

    // Verify service name "Wireless iAPv2"
    const char expectedName[] = "Wireless iAPv2";
    const size_t nameOffset = kSdpRecordSize - sizeof(expectedName) + 1;
    if (std::memcmp(record.data() + nameOffset, expectedName, sizeof(expectedName) - 1) != 0) {
        return false;
    }

    return true;
}

std::string BtSdpHelper::FormatBluetoothAddress(uint64_t bthAddr) {
    std::ostringstream oss;
    for (int i = 5; i >= 0; --i) {
        uint8_t byte = static_cast<uint8_t>((bthAddr >> (i * 8)) & 0xFF);
        oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        if (i > 0) oss << ":";
    }
    return oss.str();
}

uint64_t BtSdpHelper::ParseBluetoothAddress(std::string_view addrStr) {
    uint64_t addr = 0;
    int shift = 40;
    size_t i = 0;

    while (i < addrStr.size() && shift >= 0) {
        // Skip separators ':' or '-'
        if (addrStr[i] == ':' || addrStr[i] == '-') {
            i++;
            continue;
        }

        if (i + 1 < addrStr.size()) {
            char hexChars[3] = {addrStr[i], addrStr[i + 1], '\0'};
            char* endPtr = nullptr;
            unsigned long val = std::strtoul(hexChars, &endPtr, 16);
            if (endPtr != hexChars + 2) {
                return 0; // Malformed
            }
            addr |= (static_cast<uint64_t>(val) << shift);
            shift -= 8;
            i += 2;
        } else {
            break;
        }
    }

    return addr;
}

#ifdef _WIN32
Common::Status BtSdpHelper::RegisterSdpService(SOCKET socket, uint8_t channel, HANDLE* outHandle) {
    (void)socket;
    if (!outHandle) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "Invalid outHandle");
    }
    *outHandle = nullptr;

    auto sdpRecord = BuildIap2SdpRecord(channel);

    ULONG sdpBlobSize = sizeof(BTH_SET_SERVICE) + static_cast<ULONG>(sdpRecord.size()) - 1;
    std::vector<uint8_t> blobBuffer(sdpBlobSize, 0);
    auto* bthSetService = reinterpret_cast<PBTH_SET_SERVICE>(blobBuffer.data());
    ULONG sdpVersion = BTH_SDP_VERSION;
    HANDLE recordHandle = nullptr;
    bthSetService->pSdpVersion = &sdpVersion;
    bthSetService->pRecordHandle = &recordHandle;
    bthSetService->fSdpVersion = 0;
    bthSetService->fCodService = 0;
    bthSetService->fCopts = 0;
    bthSetService->fPublishRecord = TRUE;
    bthSetService->ulRecordLength = static_cast<ULONG>(sdpRecord.size());
    std::memcpy(bthSetService->pRecord, sdpRecord.data(), sdpRecord.size());

    BLOB blob{};
    blob.cbSize = sdpBlobSize;
    blob.pBlobData = blobBuffer.data();

    WSAQUERYSETW qs{};
    qs.dwSize = sizeof(qs);
    qs.lpszServiceInstanceName = const_cast<LPWSTR>(L"Wireless iAPv2");
    GUID svcGuid = kIap2ServiceClassGuid;
    qs.lpServiceClassId = &svcGuid;
    qs.lpBlob = &blob;
    qs.dwNameSpace = NS_BTH;

    if (SOCKET_ERROR == WSASetServiceW(&qs, RNRSERVICE_REGISTER, 0)) {
        int err = WSAGetLastError();
        return Common::Status::Error(Common::StatusCode::Unavailable,
            "WSASetService failed to register Bluetooth SDP record: code " + std::to_string(err));
    }

    *outHandle = recordHandle ? recordHandle : reinterpret_cast<HANDLE>(1);
    return Common::Status::Ok();
}

void BtSdpHelper::UnregisterSdpService(HANDLE handle) {
    if (!handle) return;

    ULONG sdpBlobSize = sizeof(BTH_SET_SERVICE);
    std::vector<uint8_t> blobBuffer(sdpBlobSize, 0);
    auto* bthSetService = reinterpret_cast<PBTH_SET_SERVICE>(blobBuffer.data());
    ULONG sdpVersion = BTH_SDP_VERSION;
    bthSetService->pSdpVersion = &sdpVersion;
    bthSetService->pRecordHandle = &handle;
    bthSetService->fPublishRecord = FALSE;

    BLOB blob{};
    blob.cbSize = sdpBlobSize;
    blob.pBlobData = blobBuffer.data();

    WSAQUERYSETW qs{};
    qs.dwSize = sizeof(qs);
    GUID svcGuid = kIap2ServiceClassGuid;
    qs.lpServiceClassId = &svcGuid;
    qs.lpBlob = &blob;
    qs.dwNameSpace = NS_BTH;
    WSASetServiceW(&qs, RNRSERVICE_DELETE, 0);
}
#endif

} // namespace CarPlayStation::Core::Transport
