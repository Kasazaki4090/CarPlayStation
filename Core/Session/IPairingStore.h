#pragma once

#include "Core/Common/Types.h"
#include "Core/Common/Result.h"
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <optional>

namespace CarPlayStation::Core::Session {

/**
 * @brief Represents a paired iPhone record stored on the receiver host.
 *
 * Persists persistent cryptographic identity (public keys) and MAC addresses
 * to facilitate fast, silent reconnections without repeating Bluetooth pairing
 * or full MFi / Pair-Setup handshake every time.
 */
struct PairedDevice {
    std::string device_id{};                          // Primary identifier (e.g. MAC or Apple Device ID)
    std::string device_name{};                        // Human readable display name (e.g. "Kasazaki's iPhone")
    Common::MacAddress bluetooth_mac{};               // Bluetooth adapter MAC
    Common::MacAddress wifi_mac{};                    // Wi-Fi interface MAC
    Common::ByteBuffer public_key{};                  // Pair-Setup Ed25519 / Curve25519 long-term public key
    uint64_t last_connected_timestamp{0};             // Unix timestamp in seconds

    [[nodiscard]] bool IsValid() const noexcept {
        return !device_id.empty();
    }

    [[nodiscard]] bool operator==(const PairedDevice& other) const noexcept {
        return device_id == other.device_id &&
               device_name == other.device_name &&
               bluetooth_mac == other.bluetooth_mac &&
               wifi_mac == other.wifi_mac &&
               public_key == other.public_key &&
               last_connected_timestamp == other.last_connected_timestamp;
    }
};

/**
 * @brief Pure virtual interface for managing and persisting paired device records.
 */
class IPairingStore {
public:
    virtual ~IPairingStore() = default;

    /**
     * @brief Save or update a paired device record.
     */
    [[nodiscard]] virtual Common::Result<void> SaveDevice(const PairedDevice& device) = 0;

    /**
     * @brief Retrieve a paired device by primary identifier.
     */
    [[nodiscard]] virtual Common::Result<PairedDevice> GetDevice(std::string_view device_id) const = 0;

    /**
     * @brief Retrieve a paired device matching either Bluetooth or Wi-Fi MAC address.
     */
    [[nodiscard]] virtual Common::Result<PairedDevice> FindByMac(const Common::MacAddress& mac) const = 0;

    /**
     * @brief List all currently paired devices, ordered from most recently connected to oldest.
     */
    [[nodiscard]] virtual std::vector<PairedDevice> ListDevices() const = 0;

    /**
     * @brief Remove a paired device by primary identifier.
     */
    [[nodiscard]] virtual Common::Result<void> RemoveDevice(std::string_view device_id) = 0;

    /**
     * @brief Clear all paired records.
     */
    virtual void Clear() = 0;

    /**
     * @brief Get count of paired devices.
     */
    [[nodiscard]] virtual size_t Count() const noexcept = 0;

    /**
     * @brief Check if a device is currently recorded in the pairing store.
     */
    [[nodiscard]] virtual bool IsPaired(std::string_view device_id) const noexcept = 0;
};

} // namespace CarPlayStation::Core::Session
