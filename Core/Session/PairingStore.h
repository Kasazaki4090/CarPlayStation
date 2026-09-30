#pragma once

#include "Core/Session/IPairingStore.h"
#include <mutex>
#include <string>
#include <vector>

namespace CarPlayStation::Core::Session {

/**
 * @brief Thread-safe file-backed implementation of IPairingStore.
 *
 * Persists paired device records to a local JSON file with atomic write semantics
 * (write-to-temp and atomic replace) to guard against corruption during sudden power-off.
 */
class PairingStore : public IPairingStore {
public:
    explicit PairingStore(std::string storage_path);
    ~PairingStore() override = default;

    // --- IPairingStore Implementation ---
    [[nodiscard]] Common::Result<void> SaveDevice(const PairedDevice& device) override;
    [[nodiscard]] Common::Result<PairedDevice> GetDevice(std::string_view device_id) const override;
    [[nodiscard]] Common::Result<PairedDevice> FindByMac(const Common::MacAddress& mac) const override;
    [[nodiscard]] std::vector<PairedDevice> ListDevices() const override;
    [[nodiscard]] Common::Result<void> RemoveDevice(std::string_view device_id) override;
    void Clear() override;
    [[nodiscard]] size_t Count() const noexcept override;
    [[nodiscard]] bool IsPaired(std::string_view device_id) const noexcept override;

    /**
     * @brief Explicitly load records from disk. Automatically invoked upon first access.
     */
    [[nodiscard]] Common::Result<void> Reload();

    /**
     * @brief Returns the absolute or configured storage file path.
     */
    [[nodiscard]] const std::string& GetStoragePath() const noexcept { return storage_path_; }

private:
    [[nodiscard]] Common::Result<void> SaveToDiskLocked();
    [[nodiscard]] Common::Result<void> LoadFromDiskLocked();

    std::string storage_path_;
    mutable std::mutex mutex_;
    std::vector<PairedDevice> cache_;
    bool loaded_{false};
};

} // namespace CarPlayStation::Core::Session
